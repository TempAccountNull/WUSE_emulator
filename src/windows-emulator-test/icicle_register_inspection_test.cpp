#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include "../windows-emulator/guest_inspection_query.hpp"
#include <memory_manager.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <vector>

namespace sogen::test
{
    class IcicleRegisterInspection : public testing::Test
    {
      protected:
        static constexpr uint64_t gpr_marker = 0x123456789ABCDEF0ULL;
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};
        uint64_t data{};

        void SetUp() override
        {
            emu = icicle::create_x86_64_emulator(2);
            ASSERT_EQ(emu->vcpu_count(), 2U);
            ASSERT_NE(&emu->get_cpu(0), &emu->get_cpu(1));
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x1000, memory_permission::all);
            data = memory->allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(code, 0U);
            ASSERT_NE(data, 0U);
            constexpr std::array<uint8_t, 4> nops{0x90, 0x90, 0x90, 0x90};
            emu->write_memory(code, nops.data(), nops.size());
            for (size_t index = 0; index < emu->vcpu_count(); ++index)
            {
                auto& cpu = emu->get_cpu(index);
                ASSERT_EQ(cpu.index(), index);
                ASSERT_TRUE(cpu.has_guest_cpu_context());
                cpu.reg(x86_register::rax, gpr_marker + index);
                cpu.reg(x86_register::rip, code);
                cpu.start(1);
                ASSERT_EQ(cpu.reg(x86_register::rip), code + 1);
                uint64_t value{};
                ASSERT_EQ(cpu.read_register(x86_register::rax, &value, sizeof(value)), sizeof(value));
                ASSERT_EQ(value, gpr_marker + index);
            }
        }
    };

    TEST_F(IcicleRegisterInspection, UnsupportedMsrReadPreservesBufferOnEveryInitializedVcpu)
    {
        for (size_t index = 0; index < emu->vcpu_count(); ++index)
        {
            SCOPED_TRACE(index);
            auto& cpu = emu->get_cpu(index);
            std::array<uint8_t, 18> sentinel{};
            sentinel.fill(0xA5);
            for (size_t width = 0; width <= 16; ++width)
            {
                SCOPED_TRACE(width);
                auto buffer = sentinel;
                EXPECT_EQ(cpu.read_register(x86_register::msr, buffer.data() + 1, width), 0U);
                EXPECT_EQ(buffer, sentinel);
            }
            EXPECT_EQ(cpu.reg(x86_register::rax), gpr_marker + index);
        }

        constexpr uint64_t sentinel = 0xA5A5A5A5A5A5A5A5ULL;
        uint64_t value = sentinel;
        EXPECT_EQ(emu->read_register(x86_register::msr, &value, sizeof(value)), 0U);
        EXPECT_EQ(value, sentinel);
        EXPECT_EQ(emu->reg(x86_register::rax), gpr_marker);
    }

    TEST_F(IcicleRegisterInspection, FullCollectorMarksEferUnavailableAndReadsLaterRegistersFromSelectedVcpu)
    {
        auto& cpu = emu->get_cpu(1);
        constexpr uint64_t rflags = 0x246;
        constexpr uint32_t mxcsr = 0x1FA0;
        constexpr std::array<uint8_t, 16> vector = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                                                    0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE};
        constexpr uint64_t mmx = 0x8877665544332211ULL;
        ASSERT_EQ(cpu.write_register(x86_register::rflags, &rflags, sizeof(rflags)), sizeof(rflags));
        ASSERT_EQ(cpu.write_register(x86_register::mxcsr, &mxcsr, sizeof(mxcsr)), sizeof(mxcsr));
        ASSERT_EQ(cpu.write_register(x86_register::xmm15, vector.data(), vector.size()), vector.size());
        ASSERT_EQ(cpu.write_register(x86_register::mm7, &mmx, sizeof(mmx)), sizeof(mmx));

        constexpr uint64_t efer_sentinel = 0xD15EA5ED12345678ULL;
        detail::inspection_context context{};
        context.efer = efer_sentinel;
        std::vector<x86_register> seen;
        detail::inspection_registers(
            [&](const x86_register reg, void* value, const size_t width) {
                seen.push_back(reg);
                return cpu.read_register(reg, value, width);
            },
            context);

        ASSERT_EQ(seen.size(), 78U);
        EXPECT_EQ(seen[37], x86_register::msr);
        EXPECT_EQ(seen.back(), x86_register::mm7);
        constexpr uint64_t efer_mask = uint64_t{1} << 37;
        EXPECT_EQ(context.available[0] & efer_mask, 0U);
        EXPECT_EQ(context.read_errors[0] & efer_mask, efer_mask);
        EXPECT_EQ(context.efer, efer_sentinel);
        EXPECT_EQ(context.gpr[0], gpr_marker + 1);
        EXPECT_EQ(context.rip, code + 1);
        EXPECT_EQ(context.rflags, rflags);
        EXPECT_EQ(context.mxcsr, mxcsr);
        EXPECT_EQ(context.xmm[15], vector);
        EXPECT_EQ(context.mmx[7], mmx);
        constexpr uint64_t before_msr_mask = (uint64_t{1} << 0) | (uint64_t{1} << 16) | (uint64_t{1} << 17) | (uint64_t{1} << 36);
        constexpr uint64_t xmm15_mask = uint64_t{1} << 53;
        constexpr uint64_t mm7_mask = uint64_t{1} << 13;
        EXPECT_EQ(context.available[0] & before_msr_mask, before_msr_mask);
        EXPECT_EQ(context.read_errors[0] & before_msr_mask, 0U);
        EXPECT_EQ(context.available[0] & xmm15_mask, xmm15_mask);
        EXPECT_EQ(context.read_errors[0] & xmm15_mask, 0U);
        EXPECT_EQ(context.available[1] & mm7_mask, mm7_mask);
        EXPECT_EQ(context.read_errors[1] & mm7_mask, 0U);
        EXPECT_EQ(emu->get_cpu(0).reg(x86_register::rax), gpr_marker);
    }

    TEST_F(IcicleRegisterInspection, HostViewMsrReadStillRejectsMissingGuestContext)
    {
        bool observed = false;
        constexpr uint64_t sentinel = 0xA5A5A5A5A5A5A5A5ULL;
        uint64_t register_value = sentinel;
        auto* hook = emu->hook_memory_write_observed(
            data, sizeof(uint64_t), [&](cpu_interface& cpu, uint64_t, const void*, size_t, memory_write_result result) {
                if (result.origin == memory_write_origin::host)
                {
                    observed = true;
                    EXPECT_FALSE(cpu.has_guest_cpu_context());
                    EXPECT_THROW((void)cpu.read_raw_register(static_cast<int>(x86_register::msr), &register_value, sizeof(register_value)),
                                 std::logic_error);
                }
            });
        ASSERT_NE(hook, nullptr);
        constexpr uint64_t memory_value = 0x0123456789ABCDEFULL;
        EXPECT_TRUE(emu->try_write_memory(data, &memory_value, sizeof(memory_value)));
        emu->delete_hook(hook);
        EXPECT_TRUE(observed);
        EXPECT_EQ(register_value, sentinel);
    }
}
