#include "emulation_test_utils.hpp"

namespace sogen::test
{
    class NativeStackGuard : public testing::Test
    {
      protected:
        windows_emulator win = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();

        void SetUp() override
        {
            win.setup_process_if_necessary();
        }

        uint64_t code_page()
        {
            return win.memory.allocate_memory(0x1000, memory_permission::all);
        }

        void start_until_hook(bool& reached)
        {
            for (unsigned attempt = 0; attempt < 4 && !reached; ++attempt)
            {
                win.emu().start(win.emu().get_name() == "icicle-emu" ? 16 : 0);
                if (!win.emu().has_violation())
                {
                    break;
                }
            }
        }

        bool execute_probe(const uint64_t address, const uint64_t rsp)
        {
            const auto code = code_page();
            const std::array<uint8_t, 5> bytes{0x41, 0xC6, 0x03, 0x00, 0x90};
            win.emu().write_memory(code, bytes.data(), bytes.size());
            win.emu().reg(x86_register::r11, address);
            win.emu().reg(x86_register::rsp, rsp);
            win.emu().reg(x86_register::rip, code);

            bool reached_next_instruction = false;
            win.emu().hook_memory_execution(code + 4, [&](cpu_interface& cpu, uint64_t) {
                reached_next_instruction = true;
                cpu.stop();
            });
            if (win.emu().get_name() == "icicle-emu")
            {
                // A recoverable memory fault can end a bounded Icicle quantum before the
                // faulting instruction retires. Give the restarted access its own quantum.
                for (unsigned attempt = 0; attempt < 4; ++attempt)
                {
                    win.emu().start(1);
                    if (reached_next_instruction || win.emu().reg<uint64_t>(x86_register::rip) == code + 4)
                    {
                        return true;
                    }
                }
                return false;
            }
            for (unsigned attempt = 0; attempt < 4 && !reached_next_instruction; ++attempt)
            {
                win.emu().start(0);
                if (!win.emu().has_violation())
                {
                    break;
                }
            }
            return reached_next_instruction;
        }
    };

    TEST_F(NativeStackGuard, NewThreadHasReservedStackAndOneGuardPage)
    {
        const auto& thread = win.vcpu(0).thread();
        const auto low = win.memory.get_region_info(thread.stack_base);
        const auto guard = win.memory.get_region_info(thread.stack_guard_page);
        const auto high = win.memory.get_region_info(thread.stack_base + thread.stack_size - 0x1000);
        const auto teb = thread.teb64->read();

        EXPECT_TRUE(low.is_reserved);
        EXPECT_FALSE(low.is_committed);
        EXPECT_TRUE(guard.is_committed);
        EXPECT_TRUE(guard.permissions.is_guarded());
        EXPECT_TRUE(high.is_committed);
        EXPECT_FALSE(high.permissions.is_guarded());
        EXPECT_EQ(teb.DeallocationStack, thread.stack_base);
        EXPECT_EQ(teb.NtTib.StackLimit, thread.stack_guard_page + 0x1000);
        EXPECT_EQ(teb.NtTib.StackBase, thread.stack_base + thread.stack_size);
        EXPECT_EQ(teb.GuaranteedStackBytes, thread.stack_guarantee_size);
        EXPECT_LT(thread.stack_guarantee_size, thread.stack_size);
    }

    TEST_F(NativeStackGuard, EqualReserveAndCommitExpandsReserveForGuardAndGuarantee)
    {
        constexpr uint64_t requested_commit = 0x40000;
        const auto handle =
            win.process.create_thread(win.memory, win.mod_manager.executable->entry_point, 0, requested_commit, 0, false, requested_commit);
        const auto* thread = win.process.threads.get(handle);
        ASSERT_NE(thread, nullptr);
        EXPECT_GE(thread->stack_size, requested_commit + thread->stack_guarantee_size + 0x1000);
        EXPECT_EQ(thread->stack_guard_page, thread->stack_base + thread->stack_size - requested_commit - 0x1000);
        EXPECT_TRUE(win.memory.get_region_info(thread->stack_guard_page).permissions.is_guarded());
        EXPECT_TRUE(win.memory.get_region_info(thread->stack_base + thread->stack_size - requested_commit).is_committed);
    }

    TEST_F(NativeStackGuard, GuestChkstkFirstProbeGrowsStackAndRetriesInstruction)
    {
        auto& thread = win.vcpu(0).thread();
        const auto old_guard = thread.stack_guard_page;
        const auto rsp = thread.stack_base + thread.stack_size - 0x1000;

        const auto first_probe = thread.teb64->read().NtTib.StackLimit - 0x1000;
        ASSERT_EQ(first_probe, old_guard);
        ASSERT_TRUE(execute_probe(first_probe, rsp));
        EXPECT_EQ(thread.stack_guard_page, old_guard - 0x1000);
        EXPECT_FALSE(win.memory.get_region_info(old_guard).permissions.is_guarded());
        EXPECT_TRUE(win.memory.get_region_info(old_guard - 0x1000).permissions.is_guarded());
        EXPECT_EQ(thread.teb64->read().NtTib.StackLimit, old_guard);
        EXPECT_EQ(win.exception_trace_non_debug_count(), 0U);
    }

    TEST_F(NativeStackGuard, OlderV5SnapshotRestoresStackLimitAboveGuard)
    {
        const auto guard = win.vcpu(0).thread().stack_guard_page;
        win.vcpu(0).thread().teb64->access([&](TEB64& teb) { teb.NtTib.StackLimit = guard; });
        win.save_snapshot();
        win.restore_snapshot();

        const auto& restored = win.vcpu(0).thread();
        EXPECT_EQ(restored.stack_guard_page, guard);
        EXPECT_EQ(restored.teb64->read().NtTib.StackLimit, guard + 0x1000);
    }

    TEST_F(NativeStackGuard, SnapshotPreservesGuestAdjustedStackLimit)
    {
        const auto guard = win.vcpu(0).thread().stack_guard_page;
        const auto adjusted_limit = guard + 0x2000;
        win.vcpu(0).thread().teb64->access([&](TEB64& teb) { teb.NtTib.StackLimit = adjusted_limit; });
        win.save_snapshot();
        win.restore_snapshot();

        EXPECT_EQ(win.vcpu(0).thread().teb64->read().NtTib.StackLimit, adjusted_limit);
    }

    TEST_F(NativeStackGuard, GuestTebCanIncreaseStackGuarantee)
    {
        auto& thread = win.vcpu(0).thread();
        const auto first_guard = thread.stack_guard_page;
        thread.teb64->access([](TEB64& teb) { teb.GuaranteedStackBytes = 0x8000; });

        ASSERT_EQ(thread.handle_stack_guard(first_guard), emulator_thread::stack_guard_result::grown);
        EXPECT_EQ(thread.stack_guarantee_size, 0x8000U);
        EXPECT_EQ(thread.teb64->read().GuaranteedStackBytes, 0x8000U);
    }

    TEST_F(NativeStackGuard, FinalGuardDeliversStackOverflowWithWritableExceptionFrame)
    {
        auto& thread = win.vcpu(0).thread();
        while (thread.stack_guard_page > thread.stack_base + thread.stack_guarantee_size)
        {
            ASSERT_EQ(thread.handle_stack_guard(thread.stack_guard_page), emulator_thread::stack_guard_result::grown);
        }

        const auto code = code_page();
        const std::array<uint8_t, 3> bytes{0x45, 0x84, 0x1B};
        win.emu().write_memory(code, bytes.data(), bytes.size());
        const auto initial_rsp = thread.stack_guard_page + 0x400;
        win.emu().reg(x86_register::r11, thread.stack_guard_page);
        win.emu().reg(x86_register::rsp, initial_rsp);
        win.emu().reg(x86_register::rip, code);

        bool dispatched = false;
        win.emu().hook_memory_execution(win.process.ki_user_exception_dispatcher, [&](cpu_interface& cpu, uint64_t) {
            dispatched = true;
            cpu.stop();
        });
        ASSERT_NO_THROW(start_until_hook(dispatched));
        ASSERT_TRUE(dispatched);
        const auto frame = win.emu().reg<uint64_t>(x86_register::rsp);
        EXPECT_GE(frame, thread.stack_base);
        const auto context = win.emu().read_memory<CONTEXT64>(frame);
        const auto record = win.emu().read_memory<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>(frame + 0x4F0);
        EXPECT_EQ(record.ExceptionCode, static_cast<DWORD>(STATUS_STACK_OVERFLOW));
        EXPECT_EQ(record.ExceptionAddress, code);
        EXPECT_EQ(context.Rip, code);
        EXPECT_EQ(context.Rsp, initial_rsp);
        EXPECT_EQ(thread.stack_guard_page, 0U);
        EXPECT_TRUE(win.memory.get_region_info(thread.stack_base).is_committed);
    }

    TEST_F(NativeStackGuard, ExceptionFrameConsumesFinalGuardAndDeliversStackOverflow)
    {
        auto& thread = win.vcpu(0).thread();
        while (thread.stack_guard_page > thread.stack_base + thread.stack_guarantee_size)
        {
            ASSERT_EQ(thread.handle_stack_guard(thread.stack_guard_page), emulator_thread::stack_guard_result::grown);
        }

        const auto code = code_page();
        win.emu().write_memory<uint16_t>(code, 0x0B0F);
        const auto initial_rsp = thread.stack_guard_page + 0x500;
        win.emu().reg(x86_register::rsp, initial_rsp);
        win.emu().reg(x86_register::rip, code);

        bool dispatched = false;
        win.emu().hook_memory_execution(win.process.ki_user_exception_dispatcher, [&](cpu_interface& cpu, uint64_t) {
            dispatched = true;
            cpu.stop();
        });
        ASSERT_NO_THROW(start_until_hook(dispatched));
        ASSERT_TRUE(dispatched);
        const auto frame = win.emu().reg<uint64_t>(x86_register::rsp);
        const auto context = win.emu().read_memory<CONTEXT64>(frame);
        const auto record = win.emu().read_memory<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>(frame + 0x4F0);
        EXPECT_EQ(record.ExceptionCode, static_cast<DWORD>(STATUS_STACK_OVERFLOW));
        EXPECT_EQ(record.ExceptionAddress, code);
        EXPECT_EQ(record.NumberParameters, 0U);
        EXPECT_EQ(context.Rip, code);
        EXPECT_EQ(context.Rsp, initial_rsp);
        EXPECT_EQ(thread.stack_guard_page, 0U);
        EXPECT_EQ(thread.teb64->read().NtTib.StackLimit, thread.stack_base);
        EXPECT_TRUE(win.memory.get_region_info(thread.stack_base).is_committed);
    }

    TEST_F(NativeStackGuard, WritableAlternateStackReceivesGuestExceptionFrame)
    {
        const auto alternate_stack = win.memory.allocate_memory(0x4000, memory_permission::read_write);
        const auto initial_rsp = alternate_stack + 0x2000;
        const auto code = code_page();
        win.emu().write_memory<uint16_t>(code, 0x0B0F);
        win.emu().reg(x86_register::rsp, initial_rsp);
        win.emu().reg(x86_register::rip, code);

        bool dispatched = false;
        win.emu().hook_memory_execution(win.process.ki_user_exception_dispatcher, [&](cpu_interface& cpu, uint64_t) {
            dispatched = true;
            cpu.stop();
        });
        ASSERT_NO_THROW(start_until_hook(dispatched));
        ASSERT_TRUE(dispatched);
        const auto frame = win.emu().reg<uint64_t>(x86_register::rsp);
        const auto context = win.emu().read_memory<CONTEXT64>(frame);
        const auto record = win.emu().read_memory<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>(frame + 0x4F0);
        EXPECT_GE(frame, alternate_stack);
        EXPECT_EQ(record.ExceptionCode, static_cast<DWORD>(STATUS_ILLEGAL_INSTRUCTION));
        EXPECT_EQ(context.Rsp, initial_rsp);
    }

    TEST_F(NativeStackGuard, UnmappedAlternateStackFrameTerminatesWithoutHostWrite)
    {
        const auto code = code_page();
        win.emu().write_memory<uint16_t>(code, 0x0B0F);
        win.emu().reg(x86_register::rsp, 0x2000);
        win.emu().reg(x86_register::rip, code);

        ASSERT_NO_THROW(win.emu().start(win.emu().get_name() == "icicle-emu" ? 16 : 0));
        ASSERT_TRUE(win.process.exit_status.has_value());
        EXPECT_EQ(*win.process.exit_status, STATUS_STACK_OVERFLOW);
    }

    TEST_F(NativeStackGuard, ExceptionFrameUnderflowEndsAsGuestStackOverflow)
    {
        auto& thread = win.vcpu(0).thread();
        const auto code = code_page();
        win.emu().write_memory<uint16_t>(code, 0x0B0F);
        win.emu().reg(x86_register::rsp, thread.stack_base + 0x4D8);
        win.emu().reg(x86_register::rip, code);

        ASSERT_NO_THROW(win.emu().start(win.emu().get_name() == "icicle-emu" ? 16 : 0));
        ASSERT_TRUE(win.process.exit_status.has_value());
        EXPECT_EQ(*win.process.exit_status, STATUS_STACK_OVERFLOW);
    }
}
