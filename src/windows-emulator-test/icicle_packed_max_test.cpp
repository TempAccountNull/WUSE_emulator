#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <cpu_context.hpp>
#include <array>

namespace sogen::test
{
    class IciclePackedMax : public testing::Test
    {
      protected:
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};
        using vector = std::array<uint32_t, 4>;

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x2000, memory_permission::all);
            ASSERT_NE(code, 0u);
            emu->reg(x86_register::mxcsr, 0x1F80u);
            emu->reg(x86_register::eflags, 0x246u);
        }

        template <size_t N>
        void load(const std::array<uint8_t, N>& bytes)
        {
            emu->write_memory(code, bytes.data(), bytes.size());
            emu->reg(x86_register::rip, code);
        }
    };

    TEST_F(IciclePackedMax, CapturedGameOperandsPreserveOtherRegistersAndUpperYmm)
    {
        load(std::array<uint8_t, 4>{0x0F, 0x5F, 0xD0, 0x90});
        const vector source{0x3F7FFFFE, 0x3F7FFFFE, 0x3F7FFFFE, 0x3F7FF000};
        const std::array<uint32_t, 8> target{0xBF7FFFFE, 0xBF7FFFFE, 0xBF7FFFFE, 0xBF7FF000, 11, 22, 33, 44};
        emu->reg<vector>(x86_register::xmm0, source);
        emu->reg<std::array<uint32_t, 8>>(x86_register::ymm2, target);
        emu->reg(x86_register::rcx, 0xBA5CD8F210ULL);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 3);
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm2), source);
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm0), source);
        const auto result = emu->reg<std::array<uint32_t, 8>>(x86_register::ymm2);
        for (size_t i = 4; i < result.size(); ++i)
        {
            EXPECT_EQ(result[i], target[i]);
        }
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rcx), 0xBA5CD8F210ULL);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), 0x246u);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1F80u);
    }

    TEST_F(IciclePackedMax, MaskedSpecialValuesSetFlagsAndPreserveSignalingNan)
    {
        load(std::array<uint8_t, 4>{0x0F, 0x5F, 0xD0, 0x90});
        emu->reg<vector>(x86_register::xmm2, vector{0, 0x7FC12345, 0x3F800000, 1});
        const vector source{0x80000000, 0x3F800000, 0x7F812345, 0};
        emu->reg<vector>(x86_register::xmm0, source);
        emu->reg(x86_register::mxcsr, 0x1FA0u);
        emu->start(1);
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm2), (vector{0x80000000, 0x3F800000, 0x7F812345, 1}));
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm0), source);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1FA3u);
    }

    TEST_F(IciclePackedMax, DazSuppressesDenormalFaultAndFtzDoesNotFlushMaximum)
    {
        load(std::array<uint8_t, 4>{0x0F, 0x5F, 0xD0, 0x90});
        for (const uint32_t control : {0x9FC0u, 0x9F80u})
        {
            emu->reg(x86_register::rip, code);
            emu->reg<vector>(x86_register::xmm2, vector{1, 0x80000001, 1, 0});
            emu->reg<vector>(x86_register::xmm0, vector{0, 0x80000002, 0x80000001, 1});
            emu->reg(x86_register::mxcsr, control);
            emu->start(1);
            const auto expected = control & 0x40 ? vector{0, 0x80000000, 0x80000000, 0} : vector{1, 0x80000001, 1, 1};
            EXPECT_EQ(emu->reg<vector>(x86_register::xmm2), expected);
            EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), control | (control & 0x40 ? 0 : 2));
        }
    }

    TEST_F(IciclePackedMax, UnmaskedExceptionsPreserveOperandsAndFaultingIp)
    {
        load(std::array<uint8_t, 4>{0x0F, 0x5F, 0xD0, 0x90});
        int interrupt{};
        emu->hook_interrupt([&](cpu_interface& cpu, const int number) {
            interrupt = number;
            cpu.stop();
        });
        const vector first{0x7FC12345, 1, 0, 0};
        const vector second{0x3F800000, 0, 0, 0};
        for (const uint32_t control : {0x1F00u, 0x1E80u, 0x1E00u})
        {
            interrupt = 0;
            emu->reg(x86_register::rip, code);
            emu->reg(x86_register::mxcsr, control);
            emu->reg<vector>(x86_register::xmm2, first);
            emu->reg<vector>(x86_register::xmm0, second);
            emu->start(1);
            EXPECT_EQ(interrupt, 19);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
            EXPECT_EQ(emu->reg<vector>(x86_register::xmm2), first);
            EXPECT_EQ(emu->reg<vector>(x86_register::xmm0), second);
            EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), control | 3);
        }
    }

    TEST_F(IciclePackedMax, RipRelativeMemoryReadsFourLanes)
    {
        load(std::array<uint8_t, 8>{0x0F, 0x5F, 0x15, 0xF9, 0, 0, 0, 0x90});
        emu->reg<vector>(x86_register::xmm2, vector{0xBF800000, 0x40800000, 0, 0x7F800000});
        const vector source{0x40000000, 0x3F800000, 0x80000000, 0xFF800000};
        emu->hook_interrupt([&](cpu_interface& cpu, const int number) {
            ADD_FAILURE() << "Unexpected interrupt " << number << " at " << std::hex << code;
            cpu.stop();
        });
        emu->write_memory(code + 0x100, source);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 7);
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm2), (vector{0x40000000, 0x40800000, 0x80000000, 0x7F800000}));
        EXPECT_EQ(emu->read_memory<vector>(code + 0x100), source);
    }

    TEST_F(IciclePackedMax, UnalignedMemoryRaisesGeneralProtectionBeforeWritingDestination)
    {
        load(std::array<uint8_t, 8>{0x0F, 0x5F, 0x15, 0xFA, 0, 0, 0, 0x90});
        int interrupt{};
        emu->hook_interrupt([&](cpu_interface& cpu, const int number) {
            interrupt = number;
            cpu.stop();
        });
        const vector original{1, 2, 3, 4};
        emu->reg<vector>(x86_register::xmm2, original);
        emu->start(1);
        EXPECT_EQ(interrupt, 13);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm2), original);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1F80u);
    }

    TEST_F(IciclePackedMax, UnmappedMemoryReportsTheOperandWithoutChangingRegisters)
    {
        load(std::array<uint8_t, 4>{0x0F, 0x5F, 0x13, 0x90});
        const auto address = code + 0x3000;
        emu->reg(x86_register::rbx, address);
        const vector original{1, 2, 3, 4};
        emu->reg<vector>(x86_register::xmm2, original);
        uint64_t fault{};
        emu->hook_memory_violation([&](cpu_interface&, uint64_t value, size_t, memory_operation operation, memory_violation_type) {
            fault = value;
            EXPECT_EQ(operation, memory_operation::read);
            return memory_violation_continuation::stop;
        });
        EXPECT_THROW(emu->start(1), std::runtime_error);
        EXPECT_EQ(fault, address);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm2), original);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1F80u);
    }

    TEST_F(IciclePackedMax, FloatingPointContextSavesAndRestoresAllXmmRegisters)
    {
        CONTEXT64 context{};
        context.ContextFlags = CONTEXT_FLOATING_POINT_64;
        const vector expected{0x7FC12345, 1, 0x80000000, 0x3F800000};
        for (int i = 0; i < 16; ++i)
        {
            emu->reg<vector>(static_cast<x86_register>(static_cast<int>(x86_register::xmm0) + i), expected);
        }
        emu->reg(x86_register::mxcsr, 0x1E03u);
        cpu_context::save(*emu, context);
        EXPECT_EQ(context.MxCsr, 0x1E03u);
        EXPECT_EQ(context.FltSave.MxCsr, 0x1E03u);
        for (int i = 0; i < 16; ++i)
        {
            EXPECT_EQ(memcmp(&(&context.Xmm0)[i], expected.data(), sizeof(expected)), 0);
            emu->reg<vector>(static_cast<x86_register>(static_cast<int>(x86_register::xmm0) + i), vector{});
        }
        emu->reg(x86_register::mxcsr, 0x1F80u);
        cpu_context::restore(*emu, context);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1E03u);
        for (int i = 0; i < 16; ++i)
        {
            EXPECT_EQ(emu->reg<vector>(static_cast<x86_register>(static_cast<int>(x86_register::xmm0) + i)), expected);
        }
        context.ContextFlags = CONTEXT_INTEGER_64;
        context.MxCsr = 0;
        context.Xmm2 = {};
        cpu_context::restore(*emu, context);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1E03u);
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm2), expected);
    }

    TEST(IciclePackedMaxWindows, UnmaskedInvalidAndDenormalReachGuestExceptionDispatcher)
    {
        emulator_settings settings{};
        settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
        auto win = create_sample_emulator(std::move(settings));
        win.start(1);
        auto& vcpu = win.vcpu(0);
        auto& cpu = vcpu.cpu;
        const auto code = win.memory.allocate_memory(0x1000, memory_permission::all);
        const std::array<uint8_t, 4> bytes{0x0F, 0x5F, 0xD0, 0x90};
        cpu.write_memory(code, bytes.data(), bytes.size());
        const auto stack = cpu.reg<uint64_t>(x86_register::rsp);
        for (const auto value : {0x7FC12345u, 1u})
        {
            cpu.reg(x86_register::rip, code);
            cpu.reg(x86_register::rsp, stack);
            cpu.reg(x86_register::mxcsr, 0x1E00u);
            cpu.reg<std::array<uint32_t, 4>>(x86_register::xmm2, std::array<uint32_t, 4>{value, 0, 0, 0});
            cpu.reg<std::array<uint32_t, 4>>(x86_register::xmm0, std::array<uint32_t, 4>{0, 0, 0, 0});
            vcpu.thread().current_ip = code;
            win.start_cpu(vcpu, 1);
            EXPECT_EQ(cpu.reg<uint64_t>(x86_register::rip), win.process.ki_user_exception_dispatcher);
            const auto saved_stack = cpu.reg<uint64_t>(x86_register::rsp);
            const auto context = cpu.read_memory<CONTEXT64>(saved_stack);
            const auto record = cpu.read_memory<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>(saved_stack + 0x4F0);
            EXPECT_EQ(context.Rip, code);
            EXPECT_EQ(record.ExceptionCode, static_cast<DWORD>(STATUS_FLOAT_INVALID_OPERATION));
            EXPECT_EQ(record.ExceptionAddress, code);
            EXPECT_EQ(record.NumberParameters, 2u);
            EXPECT_EQ(record.ExceptionInformation[0], 0u);
            EXPECT_EQ(record.ExceptionInformation[1], context.MxCsr);
            EXPECT_EQ(context.MxCsr, value == 1 ? 0x1E02u : 0x1E01u);
            EXPECT_EQ(context.Xmm2.Low, value);
            EXPECT_EQ(context.Xmm2.High, 0);
            EXPECT_EQ(context.Xmm0.Low, 0u);
        }
    }
}
