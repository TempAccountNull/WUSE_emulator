#include "emulation_test_utils.hpp"

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
            win_emu.emu().start(16);
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

    INSTANTIATE_TEST_SUITE_P(InstructionPrecision, MemoryFaultContext, testing::Bool());
}
