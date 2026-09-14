#include "emulation_test_utils.hpp"
#include "../windows-analyzer/analysis.hpp"
#include "../windows-analyzer/analysis_reporter.hpp"

namespace sogen::test
{
    class AnalysisObservation : public testing::Test, public analysis_reporter
    {
      protected:
        windows_emulator win_emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        analysis_settings logging_settings{.verbose_logging = true};
        analysis_context analysis{.settings = &logging_settings, .win_emu = &win_emu, .reporters = {this}};
        std::vector<function_execution_event> calls{};
        std::vector<execution_progress_event> progress{};
        uint64_t caller{};
        uint64_t callee{};
        uint64_t stack{};

        void report(const analysis_event& event) override
        {
            if (const auto* call = std::get_if<function_execution_event>(&event))
            {
                calls.push_back(*call);
            }
            if (const auto* update = std::get_if<execution_progress_event>(&event))
            {
                progress.push_back(*update);
            }
        }

        void SetUp() override
        {
            win_emu.setup_process_if_necessary();
            caller = win_emu.mod_manager.executable->entry_point + 0x100;
            callee = caller + 0x40;
            stack = win_emu.memory.allocate_memory(0x4000, memory_permission::read_write) + 0x2000;
            win_emu.mod_manager.executable->address_names[callee] = "WinVerifyTrust";
            win_emu.emu().reg(x86_register::rip, callee);
            win_emu.emu().reg(x86_register::rsp, stack);
            win_emu.emu().reg(x86_register::rax, 0x123456789ABCDEF0ULL);
            win_emu.emu().write_memory<uint64_t>(stack, caller + 2);
            register_analysis_callbacks(analysis);
        }
    };

    TEST_F(AnalysisObservation, FunctionDetailsPreserveGuestRegistersAndReturnSlot)
    {
        const auto before = win_emu.emu().save_registers();
        win_emu.callbacks.on_instruction(callee);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        EXPECT_EQ(win_emu.emu().read_memory<uint64_t>(stack), caller + 2);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_EQ(calls.front().function_name, "WinVerifyTrust");
        EXPECT_EQ(calls.front().execution.rip, callee);
    }

    TEST_F(AnalysisObservation, TracedCallReturnsWithGuestResultAndBalancedStack)
    {
        const std::array<uint8_t, 3> call{0xFF, 0xD0, 0x90};
        const std::array<uint8_t, 6> body{0xB8, 0x00, 0x01, 0x0B, 0x80, 0xC3};
        win_emu.emu().write_memory(caller, call.data(), call.size());
        win_emu.emu().write_memory(callee, body.data(), body.size());
        win_emu.emu().write_memory<uint64_t>(stack, caller + 0x80);
        win_emu.emu().reg(x86_register::rip, caller);
        win_emu.emu().reg(x86_register::rax, callee);
        win_emu.emu().start(3);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rip), caller + 2);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rsp), stack);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rax), 0x800B0100U);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_EQ(calls.front().execution.rip, callee);
    }

    TEST_F(AnalysisObservation, ProgressExcludesInstructionsFromRestoredSnapshot)
    {
        const std::array<uint8_t, 2> loop{0xEB, 0xFE};
        win_emu.emu().write_memory(caller, loop.data(), loop.size());
        win_emu.emu().reg(x86_register::rip, caller);
        win_emu.emu().start(0x20000);
        win_emu.emu().start(0x20000);
        const auto count = win_emu.get_executed_instructions();
        ASSERT_EQ(count, 0x40000U);
        utils::buffer_serializer output{};
        win_emu.serialize(output);
        utils::buffer_deserializer input{output.get_buffer()};
        win_emu.deserialize(input);
        analysis_context restored{.settings = &logging_settings, .win_emu = &win_emu, .reporters = {this}};
        register_analysis_callbacks(restored);
        restored.progress_last -= std::chrono::seconds(6);
        progress.clear();
        win_emu.callbacks.on_instruction(caller);
        ASSERT_EQ(progress.size(), 1U);
        EXPECT_EQ(progress.front().header.instruction_count, count);
        EXPECT_EQ(progress.front().instructions_per_second, 0U);
    }

}
