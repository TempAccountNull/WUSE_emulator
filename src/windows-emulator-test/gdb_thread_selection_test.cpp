#include "emulation_test_utils.hpp"
#include <win_x86_64_gdb_stub_handler.hpp>
#include <utils/finally.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace sogen::test
{
    class GdbThreadSelectionTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true, .use_relative_time = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        handle target_handle{};
        uint64_t code{};
        uint64_t scratch{};
        size_t switches{};

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            code = emu.mod_manager.executable->entry_point;
            std::array<uint8_t, 64> instructions{};
            instructions.fill(0x90);
            emu.memory.write_memory(code, instructions.data(), instructions.size());
            emu.emu().reg(x86_register::rip, code);
            emu.emu().reg(x86_register::rax, 0x1020304050607080ull);
            emu.vcpu(0).switch_thread = false;
            scratch = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            std::array<uint8_t, 0x1000> canary{};
            canary.fill(0xa5);
            emu.memory.write_memory(scratch, canary.data(), canary.size());
            target_handle = emu.process.create_thread(emu.memory, code + 16, 0, 0x10000, 0);
            seed_target_context();
            emu.callbacks.on_thread_switch = [&](emulator_thread&, emulator_thread&) { ++switches; };
        }

        emulator_thread& target()
        {
            return *emu.process.threads.get(target_handle);
        }

        void seed_target_context()
        {
            auto& cpu = emu.emu();
            const auto original = cpu.save_registers();
            const auto restore = utils::finally([&] { cpu.restore_registers(original); });
            cpu.reg(x86_register::rip, code + 16);
            cpu.reg(x86_register::rax, 0x8877665544332211ull);
            cpu.reg(x86_register::r12, 0x123456789abcdef0ull);
            cpu.reg(x86_register::rsp, target().stack_base + target().stack_size - 0x100);
            const std::array<uint8_t, 16> xmm{0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
                                              0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f};
            cpu.write_register(x86_register::xmm0, xmm.data(), xmm.size());
            target().last_registers = cpu.save_registers();
            target().setup_done = true;
        }

        std::vector<std::byte> memory_bytes(const uint64_t address, const size_t size) const
        {
            std::vector<std::byte> result(size);
            emu.memory.read_memory(address, result.data(), result.size());
            return result;
        }

        std::vector<std::byte> state()
        {
            utils::buffer_serializer data;
            data.write(emu.vcpu(0).active_thread->id);
            data.write(emu.vcpu(0).switch_thread.load());
            data.write(emu.get_executed_instructions());
            data.write(emu.last_stop_reason());
            data.write(emu.last_stop_detail());
            data.write(switches);
            data.write_vector(emu.emu().save_registers());
            emu.process.threads.serialize(data);
            emu.process.events.serialize(data);
            emu.process.semaphores.serialize(data);
            emu.process.mutants.serialize(data);
            emu.process.io_completions.serialize(data);
            data.write_vector(memory_bytes(scratch, 0x1000));
            data.write_vector(memory_bytes(gdt_base_for_vcpu(0), GDT_LIMIT));
            for (const auto& [index, thread] : emu.process.threads)
            {
                (void)index;
                data.write(static_cast<bool>(thread.await_host_condition));
                data.write_vector(memory_bytes(thread.stack_base, static_cast<size_t>(thread.stack_size)));
                if (thread.teb64)
                {
                    data.write_vector(memory_bytes(thread.teb64->value(), sizeof(TEB64)));
                }
            }
            return data.move_buffer();
        }

        static void read_every_register(win_x86_64_gdb_stub_handler& debugger)
        {
            std::vector<std::byte> bytes(debugger.get_max_register_size());
            for (size_t index = 0; index < debugger.get_register_count(); ++index)
            {
                SCOPED_TRACE(index);
                const auto size = debugger.read_register(index, bytes.data(), bytes.size());
                EXPECT_LE(size, bytes.size());
            }
        }

        void inspect_without_changes(const uint32_t id)
        {
            for (const auto architecture : {gdb_target_architecture::bits_64, gdb_target_architecture::bits_32})
            {
                SCOPED_TRACE(static_cast<int>(architecture));
                const auto before = state();
                const auto active_id = emu.vcpu(0).active_thread->id;
                win_x86_64_gdb_stub_handler debugger{emu, {}, architecture};
                for (size_t pass = 0; pass < 2; ++pass)
                {
                    SCOPED_TRACE(pass);
                    ASSERT_TRUE(debugger.switch_to_thread(id));
                    EXPECT_EQ(debugger.get_current_thread_id(), active_id);
                    read_every_register(debugger);
                    EXPECT_EQ(state(), before);
                }
            }
        }

        template <typename T>
        T target_register(const x86_register reg)
        {
            auto& cpu = emu.emu();
            const auto original = cpu.save_registers();
            const auto restore = utils::finally([&] { cpu.restore_registers(original); });
            cpu.restore_registers(target().last_registers);
            return cpu.reg<T>(reg);
        }
    };

    TEST_F(GdbThreadSelectionTest, ExpiredSleepAndPendingStatusSurviveActiveAndInactiveInspection)
    {
        target().await_time = emu.clock().steady_now() - 1s;
        target().pending_status = STATUS_PENDING;
        emu.vcpu(0).switch_thread = true;
        inspect_without_changes(target().id);

        auto& active = emu.current_thread();
        active.await_time = emu.clock().steady_now() - 1s;
        active.pending_status = STATUS_TIMEOUT;
        inspect_without_changes(active.id);
    }

    TEST_F(GdbThreadSelectionTest, SignaledAutoResetEventAndSemaphoreAreNotConsumed)
    {
        event signal{};
        signal.type = SynchronizationEvent;
        signal.signaled = true;
        const auto event_handle = emu.process.events.store(std::move(signal));
        semaphore semaphore_entry{};
        semaphore_entry.current_count = 1;
        semaphore_entry.max_count = 3;
        const auto semaphore_handle = emu.process.semaphores.store(std::move(semaphore_entry));

        for (const bool any : {true, false})
        {
            SCOPED_TRACE(any);
            target().await_any = any;
            target().await_objects = any ? std::vector{event_handle} : std::vector{event_handle, semaphore_handle};
            inspect_without_changes(target().id);
            EXPECT_TRUE(emu.process.events.get(event_handle)->signaled);
            EXPECT_EQ(emu.process.semaphores.get(semaphore_handle)->current_count, 1u);
        }
        target().await_any = true;
        target().await_objects = {semaphore_handle};
        inspect_without_changes(target().id);
    }

    TEST_F(GdbThreadSelectionTest, InspectionDoesNotAcquireAbandonedMutant)
    {
        mutant entry{};
        entry.abandoned = true;
        const auto lock = emu.process.mutants.store(std::move(entry));
        target().await_any = true;
        target().await_objects = {lock};
        inspect_without_changes(target().id);
        EXPECT_TRUE(emu.process.mutants.get(lock)->abandoned);
        EXPECT_EQ(emu.process.mutants.get(lock)->locked_count, 0u);
    }

    TEST_F(GdbThreadSelectionTest, QueuedIoCompletionAndGuestOutputBuffersArePreserved)
    {
        io_completion port{};
        port.enqueue({.key_context = 0x11223344, .apc_context = 0x55667788});
        const auto completion = emu.process.io_completions.store(std::move(port));
        for (const auto kind : {io_completion_wait_type::remove_single, io_completion_wait_type::remove_multiple})
        {
            SCOPED_TRACE(static_cast<int>(kind));
            target().await_io_completion = pending_io_completion_wait{
                .io_completion_handle = completion,
                .type = kind,
                .key_context_ptr = scratch,
                .apc_context_ptr = scratch + 8,
                .io_status_block_ptr = scratch + 16,
                .completion_entries_ptr = scratch + 0x100,
                .entries_removed_ptr = scratch + 0x200,
                .max_entries = 2,
                .timeout = emu.clock().steady_now() - 1s,
            };
            inspect_without_changes(target().id);
            ASSERT_EQ(emu.process.io_completions.get(completion)->queue.size(), 1u);
        }
    }

    TEST_F(GdbThreadSelectionTest, MessageWaitDoesNotDequeueOrWriteGuestMessage)
    {
        target().post_message(emu, msg{.message = 0x8001, .wParam = 0x1234, .lParam = 0x5678, .time = 42});
        target().await_msg.emplace(emulator_object<msg>{emu.memory, scratch}, 0, 0, 0);
        inspect_without_changes(target().id);
        ASSERT_EQ(target().message_queue.size(), 1u);
        EXPECT_EQ(target().message_queue.front().message, 0x8001u);

        target().await_msg.reset();
        target().await_any = true;
        target().await_msg_mask = QS_POSTMESSAGE;
        inspect_without_changes(target().id);
    }

    TEST_F(GdbThreadSelectionTest, AlertsAndApcsDoNotDispatchDuringInspection)
    {
        target().waiting_for_alert = true;
        target().alerted = true;
        inspect_without_changes(target().id);

        target().waiting_for_alert = false;
        target().alerted = false;
        target().apc_alertable = true;
        target().pending_apcs.push_back({.apc_routine = code + 32,
                                         .apc_argument1 = 0x1234,
                                         .apc_argument2 = scratch,
                                         .restamp_io_status_block = true,
                                         .io_status = STATUS_SUCCESS,
                                         .io_information = 0x5678});
        inspect_without_changes(target().id);
        ASSERT_EQ(target().pending_apcs.size(), 1u);
        EXPECT_TRUE(target().apc_alertable);
    }

    TEST_F(GdbThreadSelectionTest, HostWaitPredicateIsNotPolled)
    {
        size_t calls{};
        target().await_host_condition = [&] {
            ++calls;
            return true;
        };
        inspect_without_changes(target().id);
        EXPECT_EQ(calls, 0u);
        EXPECT_TRUE(static_cast<bool>(target().await_host_condition));
    }

    TEST_F(GdbThreadSelectionTest, NeverRunSuspendedContextAndSyntheticWow64DescriptorArePreserved)
    {
        target().setup_done = false;
        target().suspended = 2;
        target().pending_status = STATUS_TIMEOUT;
        target().teb32.emplace(emu.memory, scratch + 0x300);
        const uint64_t descriptor = 0x123456789abcdef0ull;
        emu.memory.write_memory(gdt_base_for_vcpu(0) + 10 * sizeof(uint64_t), &descriptor, sizeof(descriptor));
        inspect_without_changes(target().id);
        EXPECT_FALSE(target().setup_done);
        EXPECT_EQ(target().suspended, 2u);
    }

    TEST_F(GdbThreadSelectionTest, InactiveRegisterWriteChangesOnlyTheTargetRegisterContext)
    {
        target().pending_status = STATUS_PENDING;
        target().await_time = emu.clock().steady_now() + 1h;
        auto* const active = emu.vcpu(0).active_thread;
        const auto actual_registers = emu.emu().save_registers();
        const auto before = state();
        const auto original_target = target().last_registers;
        const uint64_t value = 0xfedcba9876543210ull;
        std::vector<std::byte> expected;
        {
            const auto restore = utils::finally([&] { emu.emu().restore_registers(actual_registers); });
            emu.emu().restore_registers(original_target);
            emu.emu().reg(x86_register::r12, value);
            expected = emu.emu().save_registers();
        }

        win_x86_64_gdb_stub_handler debugger{emu};
        ASSERT_TRUE(debugger.switch_to_thread(target().id));
        const auto entry = std::ranges::find(x64_gdb_registers, x86_register::r12, &register_entry::reg);
        ASSERT_NE(entry, x64_gdb_registers.end());
        const auto index = static_cast<size_t>(entry - x64_gdb_registers.begin());
        ASSERT_EQ(debugger.write_register(index, &value, sizeof(value)), sizeof(value));
        EXPECT_EQ(target().last_registers, expected);
        EXPECT_EQ(target_register<uint64_t>(x86_register::r12), value);
        EXPECT_EQ(emu.vcpu(0).active_thread, active);
        EXPECT_EQ(emu.emu().save_registers(), actual_registers);
        EXPECT_EQ(target().pending_status, STATUS_PENDING);

        const auto edited = std::exchange(target().last_registers, original_target);
        EXPECT_EQ(state(), before);
        target().last_registers = edited;
        ASSERT_TRUE(debugger.switch_to_thread(active->id));
        ASSERT_TRUE(debugger.switch_to_thread(target().id));
        uint64_t observed{};
        ASSERT_EQ(debugger.read_register(index, &observed, sizeof(observed)), sizeof(observed));
        EXPECT_EQ(observed, value);
    }

    TEST_F(GdbThreadSelectionTest, X86RegisterViewReadsAndWritesOnlySelectedSavedContext)
    {
        const auto actual = emu.emu().save_registers();
        win_x86_64_gdb_stub_handler debugger{emu, {}, gdb_target_architecture::bits_32};
        ASSERT_TRUE(debugger.switch_to_thread(target().id));
        uint32_t eax{};
        ASSERT_EQ(debugger.read_register(0, &eax, sizeof(eax)), sizeof(eax));
        EXPECT_EQ(eax, 0x44332211u);
        const uint32_t replacement = 0xaabbccddu;
        ASSERT_EQ(debugger.write_register(0, &replacement, sizeof(replacement)), sizeof(replacement));
        EXPECT_EQ(target_register<uint32_t>(x86_register::eax), replacement);
        EXPECT_EQ(emu.emu().save_registers(), actual);
    }

    TEST_F(GdbThreadSelectionTest, BothRegisterViewsReadSelectedVectorRegisters)
    {
        const std::array<uint8_t, 16> expected{0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
                                               0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f};
        const auto before = state();
        for (const auto architecture : {gdb_target_architecture::bits_64, gdb_target_architecture::bits_32})
        {
            win_x86_64_gdb_stub_handler debugger{emu, {}, architecture};
            const auto& mapping = architecture == gdb_target_architecture::bits_64 ? x64_gdb_registers : x86_gdb_registers;
            const auto entry = std::ranges::find(mapping, x86_register::xmm0, &register_entry::reg);
            ASSERT_NE(entry, mapping.end());
            ASSERT_TRUE(debugger.select_general_thread(target().id));
            std::array<uint8_t, 16> value{};
            ASSERT_EQ(debugger.read_register(static_cast<size_t>(entry - mapping.begin()), value.data(), value.size()), value.size());
            EXPECT_EQ(value, expected);
            EXPECT_EQ(state(), before);
        }
    }

    TEST_F(GdbThreadSelectionTest, InvalidAndTerminatedSelectionsDoNotChangeGuestOrPriorSelection)
    {
        const auto dead_handle = emu.process.create_thread(emu.memory, code + 32, 0, 0x10000, 0);
        auto& dead = *emu.process.threads.get(dead_handle);
        dead.exit_status = STATUS_SUCCESS;
        win_x86_64_gdb_stub_handler debugger{emu};
        ASSERT_TRUE(debugger.switch_to_thread(target().id));
        const auto before = state();
        EXPECT_FALSE(debugger.switch_to_thread(std::numeric_limits<uint32_t>::max() - 1));
        EXPECT_FALSE(debugger.switch_to_thread(dead.id));
        uint64_t rax{};
        ASSERT_EQ(debugger.read_register(0, &rax, sizeof(rax)), sizeof(rax));
        EXPECT_EQ(rax, 0x8877665544332211ull);
        EXPECT_EQ(state(), before);
    }

    TEST_F(GdbThreadSelectionTest, InvalidRegisterRequestRestoresLiveCpuAndKeepsTargetContext)
    {
        win_x86_64_gdb_stub_handler debugger{emu};
        ASSERT_TRUE(debugger.switch_to_thread(target().id));
        const auto before = state();
        std::array<std::byte, 64> bytes{};
        const auto invalid = debugger.get_register_count();
        EXPECT_EQ(debugger.read_register(invalid, bytes.data(), bytes.size()), 0u);
        EXPECT_EQ(debugger.write_register(invalid, bytes.data(), bytes.size()), 0u);
        EXPECT_EQ(state(), before);
    }

    TEST_F(GdbThreadSelectionTest, GeneralZeroAndAllResetInspectionToActualLiveContext)
    {
        win_x86_64_gdb_stub_handler debugger{emu};
        const auto before = state();
        for (const uint32_t id : {0u, std::numeric_limits<uint32_t>::max()})
        {
            ASSERT_TRUE(debugger.select_general_thread(target().id));
            ASSERT_TRUE(debugger.select_general_thread(id));
            uint64_t rax{};
            ASSERT_EQ(debugger.read_register(0, &rax, sizeof(rax)), sizeof(rax));
            EXPECT_EQ(rax, 0x1020304050607080ull);
            EXPECT_EQ(debugger.get_current_thread_id(), emu.current_thread().id);
            EXPECT_EQ(state(), before);
        }
    }

    TEST_F(GdbThreadSelectionTest, InvalidContinuationDoesNotChangeInspectionOrGuestState)
    {
        const auto dead_handle = emu.process.create_thread(emu.memory, code + 32, 0, 0x10000, 0);
        auto& dead = *emu.process.threads.get(dead_handle);
        dead.exit_status = STATUS_SUCCESS;
        win_x86_64_gdb_stub_handler debugger{emu};
        ASSERT_TRUE(debugger.select_general_thread(target().id));
        const auto before = state();
        EXPECT_FALSE(debugger.select_continuation_thread(std::numeric_limits<uint32_t>::max() - 1));
        EXPECT_FALSE(debugger.select_continuation_thread(dead.id));
        uint64_t rax{};
        ASSERT_EQ(debugger.read_register(0, &rax, sizeof(rax)), sizeof(rax));
        EXPECT_EQ(rax, 0x8877665544332211ull);
        EXPECT_EQ(state(), before);
    }

    TEST_F(GdbThreadSelectionTest, DefaultContinueIgnoresGeneralInspectionSelection)
    {
        auto* const active = emu.vcpu(0).active_thread;
        uint32_t executed_thread{};
        emu.callbacks.on_instruction = [&](uint64_t) {
            executed_thread = emu.current_thread().id;
            emu.stop();
        };
        win_x86_64_gdb_stub_handler debugger{emu};
        ASSERT_TRUE(debugger.switch_to_thread(target().id));
        const auto target_before = target().last_registers;
        EXPECT_EQ(debugger.run(), gdb_stub::action::resume);
        EXPECT_EQ(executed_thread, active->id);
        EXPECT_EQ(emu.vcpu(0).active_thread, active);
        EXPECT_EQ(target().last_registers, target_before);
        EXPECT_EQ(target().executed_instructions, 0u);
    }

    TEST_F(GdbThreadSelectionTest, SingleStepUsesGeneralSelectionAndReportsActualExecutionThread)
    {
        const auto active_id = emu.current_thread().id;
        const auto before = emu.get_executed_instructions();
        win_x86_64_gdb_stub_handler debugger{emu};
        ASSERT_TRUE(debugger.switch_to_thread(target().id));
        EXPECT_EQ(debugger.get_current_thread_id(), active_id);
        EXPECT_EQ(debugger.singlestep(), gdb_stub::action::resume);
        EXPECT_EQ(debugger.get_current_thread_id(), target().id);
        EXPECT_EQ(emu.emu().read_instruction_pointer(), code + 17);
        EXPECT_EQ(emu.get_executed_instructions(), before + 1);
        EXPECT_EQ(target().executed_instructions, 1u);
    }

    TEST_F(GdbThreadSelectionTest, ExplicitContinuationOverridesGeneralSelectionForStep)
    {
        const auto active_id = emu.current_thread().id;
        win_x86_64_gdb_stub_handler debugger{emu};
        ASSERT_TRUE(debugger.switch_to_thread(target().id));
        const auto before = state();
        ASSERT_TRUE(debugger.select_continuation_thread(active_id));
        EXPECT_EQ(state(), before);
        EXPECT_EQ(debugger.singlestep(), gdb_stub::action::resume);
        EXPECT_EQ(debugger.get_current_thread_id(), active_id);
        EXPECT_EQ(emu.emu().read_instruction_pointer(), code + 1);
        EXPECT_EQ(target().executed_instructions, 0u);
    }

    TEST_F(GdbThreadSelectionTest, ExplicitContinuationSelectsTargetOnlyWhenRunIsRequested)
    {
        uint32_t executed_thread{};
        emu.callbacks.on_instruction = [&](uint64_t) {
            executed_thread = emu.current_thread().id;
            emu.stop();
        };
        win_x86_64_gdb_stub_handler debugger{emu};
        const auto before = state();
        ASSERT_TRUE(debugger.select_continuation_thread(target().id));
        EXPECT_EQ(state(), before);
        EXPECT_EQ(debugger.run(), gdb_stub::action::resume);
        EXPECT_EQ(executed_thread, target().id);
        EXPECT_EQ(debugger.get_current_thread_id(), target().id);
    }

    TEST_F(GdbThreadSelectionTest, ExplicitAnyContinuationOverridesInspectionForStep)
    {
        const auto actual_id = emu.current_thread().id;
        win_x86_64_gdb_stub_handler debugger{emu};
        ASSERT_TRUE(debugger.select_general_thread(target().id));
        const auto before = state();
        ASSERT_TRUE(debugger.select_continuation_thread(0));
        EXPECT_EQ(state(), before);
        EXPECT_EQ(debugger.singlestep(), gdb_stub::action::resume);
        EXPECT_EQ(debugger.get_current_thread_id(), actual_id);
        EXPECT_EQ(emu.emu().read_instruction_pointer(), code + 1);
        EXPECT_EQ(target().executed_instructions, 0u);
    }
}
