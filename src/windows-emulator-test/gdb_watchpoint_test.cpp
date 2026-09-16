#include "emulation_test_utils.hpp"
#include <x86_64_gdb_stub_handler.hpp>
#include <win_x86_64_gdb_stub_handler.hpp>
#include <memory_manager.hpp>
#include <array>
#include <limits>

namespace sogen::test
{
    namespace
    {
        class test_watchpoint_handler : public x86_64_gdb_stub_handler
        {
          public:
            using x86_64_gdb_stub_handler::clear_watchpoint_observations;
            using x86_64_gdb_stub_handler::record_watchpoint;
            using x86_64_gdb_stub_handler::x86_64_gdb_stub_handler;

            bool is_32_bit() const override
            {
                return false;
            }

            bool switch_to_thread(uint32_t id) override
            {
                return id == 1;
            }

            std::optional<uint32_t> get_exit_code() override
            {
                return {};
            }
        };

        class test_windows_watchpoint_handler : public win_x86_64_gdb_stub_handler
        {
          public:
            using win_x86_64_gdb_stub_handler::win_x86_64_gdb_stub_handler;
            using x86_64_gdb_stub_handler::record_watchpoint;
        };
    }

    class IcicleWatchpoints : public testing::Test
    {
      protected:
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};
        uint64_t data{};

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x1000, memory_permission::all);
            data = memory->allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(code, 0U);
            ASSERT_NE(data, 0U);
            // mov [rdi],rax; nop; nop. The following marker may service a deferred stop.
            const std::array<uint8_t, 5> program{0x48, 0x89, 0x07, 0x90, 0x90};
            emu->write_memory(code, program.data(), program.size());
            emu->reg(x86_register::rip, code);
            emu->reg(x86_register::rdi, data);
            emu->reg(x86_register::rax, 0x8877665544332211ull);
        }
    };

    TEST_F(IcicleWatchpoints, EnclosingStoreKeepsCallbackPcAndCopiedValueAfterDeferredStop)
    {
        test_watchpoint_handler handler{*emu};
        ASSERT_TRUE(handler.set_breakpoint(gdb_stub::breakpoint_type::hardware_write, data + 4, 2));
        // Ensure that a following instruction hook exists, as in the analyzer.
        size_t instruction_callbacks{};
        auto hook = scoped_hook{*emu, emu->hook_memory_execution([&](cpu_interface&, uint64_t) { ++instruction_callbacks; })};
        emu->start(3);
        const auto stop = handler.get_watchpoint_observations();
        ASSERT_EQ(stop.count, 1U);
        const auto& access = stop.observations[0];
        EXPECT_EQ(access.address, data);
        EXPECT_EQ(access.size, 8U);
        EXPECT_EQ(access.watched_address, data + 4);
        EXPECT_EQ(access.watched_size, 2U);
        EXPECT_EQ(access.outcome, gdb_stub::watchpoint_outcome::completed);
        EXPECT_FALSE(access.host_write);
        EXPECT_EQ(access.backend_error, 0U);
        EXPECT_TRUE(access.pc_valid);
        EXPECT_EQ(access.callback_pc, code);
        EXPECT_EQ(access.cpu_index, 0U);
        EXPECT_EQ(access.thread_id, 1U);
        EXPECT_EQ(access.captured_size, 8U);
        EXPECT_EQ(access.value[0], 0x11);
        EXPECT_EQ(access.value[7], 0x88);
        EXPECT_EQ(emu->read_memory<uint64_t>(data), 0x8877665544332211ull);
        EXPECT_GE(instruction_callbacks, 1U);
        EXPECT_LE(instruction_callbacks, 2U);
        // A later debugger register/memory read must not reconstruct or overwrite callback evidence.
        emu->reg(x86_register::rip, code + 4);
        const uint64_t debugger_value{};
        ASSERT_TRUE(handler.write_memory(data, &debugger_value, sizeof(debugger_value)));
        const auto again = handler.get_watchpoint_observations();
        EXPECT_EQ(again.observations[0].callback_pc, code);
        EXPECT_EQ(again.observations[0].value, access.value);
        EXPECT_EQ(again.count, 1U);
        const auto xml = gdb_stub::format_watchpoint_observations(again);
        EXPECT_NE(xml.find("outcome=\"completed\""), std::string::npos);
        EXPECT_NE(xml.find("source=\"guest\""), std::string::npos);
        EXPECT_NE(xml.find("value=\"1122334455667788\""), std::string::npos);
    }

    TEST_F(IcicleWatchpoints, HostEmulationWriteIsObservedAndDebuggerWriteIsSuppressed)
    {
        test_watchpoint_handler handler{*emu};
        ASSERT_TRUE(handler.set_breakpoint(gdb_stub::breakpoint_type::hardware_write, data + 4, 2));
        emu->reg(x86_register::rip, code + 3);
        const uint64_t host_value = 0x8877665544332211ull;
        emu->write_memory(data, &host_value, sizeof(host_value));
        auto stop = handler.get_watchpoint_observations();
        ASSERT_EQ(stop.count, 1U);
        EXPECT_TRUE(stop.observations[0].host_write);
        EXPECT_EQ(stop.observations[0].callback_pc, code + 3);
        EXPECT_EQ(stop.observations[0].address, data);
        EXPECT_EQ(stop.observations[0].size, sizeof(host_value));
        EXPECT_EQ(stop.observations[0].captured_address, data + 4);
        EXPECT_EQ(stop.observations[0].captured_size, 2U);
        EXPECT_EQ(stop.observations[0].value[0], 0x55);
        EXPECT_EQ(stop.observations[0].value[1], 0x66);
        const auto xml = gdb_stub::format_watchpoint_observations(stop);
        EXPECT_NE(xml.find("source=\"host\""), std::string::npos);
        EXPECT_NE(xml.find("captured-address=\""), std::string::npos);
        EXPECT_NE(xml.find("value-kind=\"attempted-overlap\""), std::string::npos);
        EXPECT_NE(xml.find("value=\"5566\""), std::string::npos);

        const uint64_t debugger_value{};
        ASSERT_TRUE(handler.write_memory(data, &debugger_value, sizeof(debugger_value)));
        stop = handler.get_watchpoint_observations();
        EXPECT_EQ(stop.count, 1U);
    }

    TEST_F(IcicleWatchpoints, ProtectedStoreReportsAttemptWithoutClaimingCommit)
    {
        emu->write_memory<uint64_t>(data, 0xAABBCCDDEEFF0011ull);
        ASSERT_TRUE(memory->protect_memory(data, 0x1000, memory_permission::read));
        test_watchpoint_handler handler{*emu};
        ASSERT_TRUE(handler.set_breakpoint(gdb_stub::breakpoint_type::hardware_write, data + 4, 2));
        // The violation keeps its ordinary stop/error path; the observation is supplementary.
        auto violation =
            scoped_hook{*emu, emu->hook_memory_violation([](cpu_interface&, uint64_t, size_t, memory_operation, memory_violation_type) {
                            return memory_violation_continuation::stop;
                        })};
        try
        {
            emu->start(1);
        }
        catch (const std::runtime_error&)
        {
        }
        const auto stop = handler.get_watchpoint_observations();
        ASSERT_EQ(stop.count, 1U);
        EXPECT_EQ(stop.observations[0].outcome, gdb_stub::watchpoint_outcome::failed);
        EXPECT_NE(stop.observations[0].backend_error, 0U);
        EXPECT_EQ(stop.observations[0].callback_pc, code);
        EXPECT_EQ(stop.observations[0].value[0], 0x11);
        EXPECT_EQ(stop.observations[0].value[7], 0x88);
        EXPECT_EQ(emu->read_memory<uint64_t>(data), 0xAABBCCDDEEFF0011ull);
        EXPECT_NE(gdb_stub::format_watchpoint_observations(stop).find("outcome=\"failed\""), std::string::npos);
    }

    TEST_F(IcicleWatchpoints, HalfOpenBoundariesAndInvalidRegistration)
    {
        test_watchpoint_handler handler{*emu};
        ASSERT_TRUE(handler.set_breakpoint(gdb_stub::breakpoint_type::hardware_write, data + 8, 2));
        emu->start(1);
        EXPECT_EQ(handler.get_watchpoint_observations().count, 0U);
        emu->reg(x86_register::rip, code);
        emu->reg(x86_register::rdi, data + 10);
        emu->start(1);
        EXPECT_EQ(handler.get_watchpoint_observations().count, 0U);
        EXPECT_FALSE(handler.set_breakpoint(gdb_stub::breakpoint_type::hardware_write, data, 0));
        EXPECT_FALSE(handler.set_breakpoint(gdb_stub::breakpoint_type::hardware_write, UINT64_MAX - 3, 8));
        EXPECT_THROW(emu->hook_memory_write(UINT64_MAX - 3, 8, [](cpu_interface&, uint64_t, const void*, size_t) {}),
                     std::invalid_argument);
        EXPECT_THROW(emu->hook_memory_read(data, 0, [](cpu_interface&, uint64_t, const void*, size_t) {}), std::invalid_argument);
    }

    TEST_F(IcicleWatchpoints, BoundedCaptureMarksTruncationDropsAndUnknownOutcome)
    {
        test_watchpoint_handler handler{*emu};
        std::array<uint8_t, 80> bytes{};
        bytes.fill(0xCA);
        handler.record_watchpoint(*emu, data, 4, data, bytes.data(), bytes.size(), true, {});
        bytes.fill(0);
        for (size_t i = 0; i < 128; ++i)
        {
            handler.record_watchpoint(*emu, data, 4, data, bytes.data(), 4, true, {});
        }
        const auto stop = handler.get_watchpoint_observations();
        ASSERT_EQ(stop.count, 128U);
        EXPECT_EQ(stop.dropped, 1U);
        EXPECT_EQ(stop.observations[0].outcome, gdb_stub::watchpoint_outcome::unknown);
        EXPECT_EQ(stop.observations[0].captured_size, 64U);
        EXPECT_EQ(stop.observations[0].size, 80U);
        EXPECT_EQ(stop.observations[0].value[63], 0xCA);
        const auto xml = gdb_stub::format_watchpoint_observations(stop);
        EXPECT_NE(xml.find("dropped=\"1\""), std::string::npos);
        EXPECT_NE(xml.find("truncated=\"1\""), std::string::npos);
        EXPECT_NE(xml.find("outcome=\"unknown\""), std::string::npos);
        handler.clear_watchpoint_observations();
        EXPECT_EQ(handler.get_watchpoint_observations().count, 0U);
    }

    TEST(GdbWatchpointLifecycle, PendingOutputAndInterruptRetainCaptureUntilExecution)
    {
        emulator_settings settings{.disable_logging = true};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        auto win = create_sample_emulator(std::move(settings));
        win.setup_process_if_necessary();
        const auto address = win.mod_manager.executable->entry_point;
        win.emu().write_memory<uint8_t>(address, 0x90);
        win.emu().reg(x86_register::rip, address);
        test_windows_watchpoint_handler handler{win};
        const std::array<uint8_t, 4> value{1, 2, 3, 4};
        handler.record_watchpoint(win.emu(), 0x1000, 4, 0x1000, value.data(), value.size(), true,
                                  {.outcome = memory_access_outcome::completed, .backend_error = 0});
        win.callbacks.on_debug_string("output before the watchpoint stop");
        handler.on_interrupt();
        const auto before = win.get_executed_instructions();
        EXPECT_EQ(handler.run(), gdb_stub::action::output);
        EXPECT_EQ(handler.get_watchpoint_observations().count, 1U);
        EXPECT_EQ(handler.consume_debug_output(), "output before the watchpoint stop");
        EXPECT_EQ(handler.run(), gdb_stub::action::resume);
        EXPECT_EQ(handler.get_watchpoint_observations().count, 1U);
        EXPECT_EQ(win.get_executed_instructions(), before);
        EXPECT_EQ(handler.singlestep(), gdb_stub::action::resume);
        EXPECT_EQ(handler.get_watchpoint_observations().count, 0U);
        EXPECT_EQ(win.get_executed_instructions(), before + 1);
    }
}
