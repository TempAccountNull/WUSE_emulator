#include "emulation_test_utils.hpp"
#include "../windows-emulator/syscall_utils.hpp"
#include "../windows-emulator/cpu_context.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtContinueEx(const syscall_context& c, emulator_object<CONTEXT64> thread_context, uint64_t continue_argument);
    NTSTATUS handle_NtContinue(const syscall_context& c, emulator_object<CONTEXT64> thread_context, BOOLEAN raise_alert);
}

namespace sogen::test
{
    class MemoryFaultContext : public testing::TestWithParam<bool>
    {
      protected:
        windows_emulator win_emu = [&] {
            emulator_settings settings{.disable_logging = true, .use_instruction_precision = GetParam()};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t code{};
        uint64_t target{};
        uint64_t stack{};
        CONTEXT64 context{};
        EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>> record{};

        void SetUp() override
        {
            win_emu.setup_process_if_necessary();
            code = win_emu.memory.allocate_memory(0x2000, memory_permission::all);
            target = win_emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            stack = win_emu.memory.allocate_memory(0x4000, memory_permission::read_write) + 0x2000;
            win_emu.emu().reg(x86_register::rsp, stack);
            win_emu.emu().reg(x86_register::rip, code);
        }

        void capture(const NTSTATUS status = STATUS_ACCESS_VIOLATION)
        {
            bool dispatched = false;
            win_emu.emu().hook_memory_execution(win_emu.process.ki_user_exception_dispatcher, [&](cpu_interface& cpu, uint64_t) {
                dispatched = true;
                cpu.stop();
            });
            for (unsigned attempt = 0; attempt < 4 && !dispatched; ++attempt)
            {
                win_emu.emu().start(win_emu.emu().get_name() == "icicle-emu" ? 16 : 0);
                if (!win_emu.emu().has_violation())
                {
                    break;
                }
            }
            ASSERT_TRUE(dispatched);
            const auto frame = win_emu.emu().reg<uint64_t>(x86_register::rsp);
            context = win_emu.emu().read_memory<CONTEXT64>(frame);
            record = win_emu.emu().read_memory<decltype(record)>(frame + 0x4F0);
            ASSERT_EQ(record.ExceptionCode, static_cast<DWORD>(status));
            ASSERT_EQ(record.NumberParameters, 2U);
        }
    };

    TEST_P(MemoryFaultContext, XrstorGeneralProtectionReachesGuestExceptionDispatcher)
    {
        if (win_emu.emu().get_name() != "icicle-emu")
        {
            GTEST_SKIP();
        }
        const std::array<uint8_t, 3> bytes{0x0F, 0xAE, 0x2B};
        win_emu.emu().write_memory(code, bytes.data(), bytes.size());
        win_emu.emu().reg(x86_register::rbx, target + 8);
        win_emu.emu().reg(x86_register::rax, 3ULL);
        win_emu.emu().reg(x86_register::rdx, 0ULL);
        capture();
        EXPECT_EQ(record.ExceptionInformation[0], 0U);
        EXPECT_EQ(record.ExceptionInformation[1], std::numeric_limits<uint64_t>::max());
        EXPECT_EQ(record.ExceptionAddress, code);
        EXPECT_EQ(context.Rip, code);
        EXPECT_EQ(context.Rsp, stack);
    }

    TEST_P(MemoryFaultContext, ReturnToNonExecutableMemoryPreservesConsumedReturn)
    {
        win_emu.emu().write_memory<uint8_t>(code, 0xC3);
        win_emu.emu().write_memory<uint64_t>(stack, target);
        capture();
        EXPECT_EQ(record.ExceptionInformation[0], 8U);
        EXPECT_EQ(record.ExceptionInformation[1], target);
        EXPECT_EQ(record.ExceptionAddress, target);
        EXPECT_EQ(context.Rip, target);
        EXPECT_EQ(context.Rsp, stack + 8);
    }

    TEST_P(MemoryFaultContext, CallToNonExecutableMemoryPreservesPushedReturn)
    {
        win_emu.emu().write_memory<uint16_t>(code, 0xD0FF);
        win_emu.emu().reg(x86_register::rax, target);
        capture();
        EXPECT_EQ(record.ExceptionInformation[0], 8U);
        EXPECT_EQ(record.ExceptionInformation[1], target);
        EXPECT_EQ(record.ExceptionAddress, target);
        EXPECT_EQ(context.Rip, target);
        EXPECT_EQ(context.Rsp, stack - 8);
        EXPECT_EQ(win_emu.emu().read_memory<uint64_t>(stack - 8), code + 2);
    }

    TEST_P(MemoryFaultContext, ReadFaultUsesInstructionAddressAndUnchangedStack)
    {
        const std::array<uint8_t, 3> bytes{0x48, 0x8B, 0x03};
        win_emu.emu().write_memory(code, bytes.data(), bytes.size());
        win_emu.memory.protect_memory(target, 0x1000, memory_permission::none);
        win_emu.emu().reg(x86_register::rbx, target);
        capture();
        EXPECT_EQ(record.ExceptionInformation[0], 0U);
        EXPECT_EQ(record.ExceptionInformation[1], target);
        EXPECT_EQ(record.ExceptionAddress, code);
        EXPECT_EQ(context.Rip, code);
        EXPECT_EQ(context.Rsp, stack);
    }

    TEST_P(MemoryFaultContext, WriteFaultUsesInstructionAddressAndUnchangedStack)
    {
        const std::array<uint8_t, 3> bytes{0x48, 0x89, 0x03};
        win_emu.emu().write_memory(code, bytes.data(), bytes.size());
        win_emu.memory.protect_memory(target, 0x1000, memory_permission::read);
        win_emu.emu().reg(x86_register::rbx, target);
        capture();
        EXPECT_EQ(record.ExceptionInformation[0], 1U);
        EXPECT_EQ(record.ExceptionInformation[1], target);
        EXPECT_EQ(record.ExceptionAddress, code);
        EXPECT_EQ(context.Rip, code);
        EXPECT_EQ(context.Rsp, stack);
    }

    TEST_P(MemoryFaultContext, CrossPageFetchUsesInstructionStart)
    {
        const std::array<uint8_t, 6> bytes{0x90, 0xB8, 1, 2, 3, 4};
        win_emu.emu().write_memory(code + 0xFFE, bytes.data(), bytes.size());
        win_emu.memory.protect_memory(code + 0x1000, 0x1000, memory_permission::read_write);
        win_emu.emu().reg(x86_register::rip, code + 0xFFE);
        capture();
        EXPECT_EQ(record.ExceptionInformation[0], 8U);
        EXPECT_EQ(record.ExceptionInformation[1], code + 0x1000);
        EXPECT_EQ(record.ExceptionAddress, code + 0xFFF);
        EXPECT_EQ(context.Rip, code + 0xFFF);
        EXPECT_EQ(context.Rsp, stack);
    }

    TEST_P(MemoryFaultContext, ReturnToGuardedExecutableMemoryPreservesTarget)
    {
        win_emu.emu().write_memory<uint8_t>(code, 0xC3);
        win_emu.emu().write_memory<uint64_t>(stack, target);
        win_emu.memory.protect_memory(target, 0x1000, nt_memory_permission{memory_permission::all, memory_permission_ext::guard});
        capture(STATUS_GUARD_PAGE_VIOLATION);
        EXPECT_EQ(record.ExceptionInformation[0], 8U);
        EXPECT_EQ(record.ExceptionInformation[1], target);
        EXPECT_EQ(record.ExceptionAddress, target);
        EXPECT_EQ(context.Rip, target);
        EXPECT_EQ(context.Rsp, stack + 8);
    }

    TEST_P(MemoryFaultContext, ReturnToUnmappedMemoryPreservesTarget)
    {
        win_emu.emu().write_memory<uint8_t>(code, 0xC3);
        win_emu.emu().write_memory<uint64_t>(stack, 0x777700000000);
        capture();
        EXPECT_EQ(record.ExceptionInformation[0], 8U);
        EXPECT_EQ(record.ExceptionInformation[1], 0x777700000000U);
        EXPECT_EQ(record.ExceptionAddress, 0x777700000000U);
        EXPECT_EQ(context.Rip, 0x777700000000U);
        EXPECT_EQ(context.Rsp, stack + 8);
    }

    TEST_P(MemoryFaultContext, Int2dUnknownServiceBuildsBreakpointContextAtNextByte)
    {
        const std::array<uint8_t, 4> bytes{0xCD, 0x2D, 0x90, 0x90};
        win_emu.emu().write_memory(code, bytes.data(), bytes.size());
        win_emu.emu().reg(x86_register::rax, 0x1234ULL);
        win_emu.emu().reg(x86_register::rcx, 0x5678ULL);
        win_emu.emu().reg(x86_register::rdx, 0x9ABCULL);

        bool dispatched = false;
        win_emu.emu().hook_memory_execution(win_emu.process.ki_user_exception_dispatcher, [&](cpu_interface& cpu, uint64_t) {
            dispatched = true;
            cpu.stop();
        });
        win_emu.emu().start(win_emu.emu().get_name() == "icicle-emu" ? 16 : 0);
        ASSERT_TRUE(dispatched);

        const auto frame = win_emu.emu().reg<uint64_t>(x86_register::rsp);
        const auto saved_context = win_emu.emu().read_memory<CONTEXT64>(frame);
        const auto saved_record = win_emu.emu().read_memory<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>(frame + 0x4F0);
        EXPECT_EQ(saved_record.ExceptionCode, static_cast<DWORD>(STATUS_BREAKPOINT));
        EXPECT_EQ(saved_record.ExceptionAddress, code + 2);
        EXPECT_EQ(saved_context.Rip, code + 2);
        EXPECT_EQ(saved_record.NumberParameters, 3U);
        EXPECT_EQ(saved_record.ExceptionInformation[0], 0x1234U);
        EXPECT_EQ(saved_record.ExceptionInformation[1], 0x5678U);
        EXPECT_EQ(saved_record.ExceptionInformation[2], 0x9ABCU);
        const auto traces = win_emu.exception_trace_snapshot();
        ASSERT_EQ(traces.size(), 1U);
        EXPECT_EQ(traces[0].rip, code + 2);
    }

    TEST_P(MemoryFaultContext, Ud2BuildsIllegalInstructionContextAndCanContinue)
    {
        const std::array<uint8_t, 4> bytes{0x0F, 0x0B, 0x90, 0x90};
        win_emu.emu().write_memory(code, bytes.data(), bytes.size());

        bool dispatched = false;
        win_emu.emu().hook_memory_execution(win_emu.process.ki_user_exception_dispatcher, [&](cpu_interface& cpu, uint64_t) {
            dispatched = true;
            cpu.stop();
        });
        win_emu.emu().start(win_emu.emu().get_name() == "icicle-emu" ? 16 : 0);
        ASSERT_TRUE(dispatched);

        const auto frame = win_emu.emu().reg<uint64_t>(x86_register::rsp);
        auto saved_context = win_emu.emu().read_memory<CONTEXT64>(frame);
        const auto saved_record = win_emu.emu().read_memory<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>(frame + 0x4F0);
        EXPECT_EQ(saved_record.ExceptionCode, static_cast<DWORD>(STATUS_ILLEGAL_INSTRUCTION));
        EXPECT_EQ(saved_record.ExceptionAddress, code);
        EXPECT_EQ(saved_context.Rip, code);
        EXPECT_EQ(saved_record.NumberParameters, 0U);
        const auto traces = win_emu.exception_trace_snapshot();
        ASSERT_EQ(traces.size(), 1U);
        EXPECT_EQ(traces[0].rip, code);

        // Model a guest VEH/SEH handler that skips the two-byte UD2 and resumes.
        saved_context.Rip = code + 2;
        saved_context.Rsp = stack;
        saved_context.Rax = 0x1122334455667788ULL;
        win_emu.emu().write_memory(frame, saved_context);
        const syscall_context c{win_emu, win_emu.emu(), win_emu.vcpu(0), win_emu.process};
        // A real argument block with flags=0 avoids the unrelated test-alert path.
        ASSERT_EQ(syscalls::handle_NtContinueEx(c, emulator_object<CONTEXT64>{win_emu.emu(), frame}, target), STATUS_SUCCESS);
        EXPECT_FALSE(c.write_status);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rip), code + 2);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rsp), stack);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rax), 0x1122334455667788ULL);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rflags), saved_context.EFlags);
    }

    TEST_P(MemoryFaultContext, NtContinueFalseRestoresContextWithoutAlertableYield)
    {
        CONTEXT64 saved_context{};
        saved_context.ContextFlags = CONTEXT64_ALL;
        cpu_context::save(win_emu.emu(), saved_context);
        saved_context.Rip = code + 2;
        saved_context.Rsp = stack;
        win_emu.emu().write_memory(target, saved_context);

        auto& vcpu = win_emu.vcpu(0);
        vcpu.switch_thread.store(false);
        ASSERT_NE(vcpu.active_thread, nullptr);
        vcpu.active_thread->apc_alertable = false;
        const syscall_context c{win_emu, win_emu.emu(), vcpu, win_emu.process};
        ASSERT_EQ(syscalls::handle_NtContinue(c, emulator_object<CONTEXT64>{win_emu.emu(), target}, FALSE), STATUS_SUCCESS);

        EXPECT_FALSE(c.write_status);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rip), code + 2);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rsp), stack);
        EXPECT_FALSE(vcpu.switch_thread.load());
        EXPECT_FALSE(vcpu.active_thread->apc_alertable);
    }

    INSTANTIATE_TEST_SUITE_P(InstructionPrecision, MemoryFaultContext, testing::Bool());
}
