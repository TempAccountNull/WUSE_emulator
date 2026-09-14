#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <array>
#include <cmath>
#include <limits>

namespace sogen::test
{
    class IciclePackedDivide : public testing::Test
    {
      protected:
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x2000, memory_permission::all);
            ASSERT_NE(code, 0u);
            emu->reg(x86_register::rip, code);
        }
    };

    TEST_F(IciclePackedDivide, RipRelativeSinglePrecisionReadsAllFourLanes)
    {
        constexpr std::array<uint8_t, 8> bytes{0x0F, 0x5E, 0x05, 0xF9, 0, 0, 0, 0x90};
        constexpr std::array<float, 4> dividend{6, -9, 0, 25};
        constexpr std::array<float, 4> divisor{2, 3, 5, 4};
        constexpr std::array<float, 4> expected{3, -3, 0, 6.25F};
        emu->write_memory(code, bytes.data(), bytes.size());
        emu->write_memory(code + 0x100, divisor.data(), sizeof(divisor));
        emu->write_register(x86_register::xmm0, dividend.data(), sizeof(dividend));
        emu->start(1);
        ASSERT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 7);
        EXPECT_EQ((emu->reg<std::array<float, 4>>(x86_register::xmm0)), expected);
        EXPECT_EQ((emu->read_memory<std::array<float, 4>>(code + 0x100)), divisor);
    }

    TEST_F(IciclePackedDivide, RegisterSinglePrecisionPreservesSourceAndSpecialValues)
    {
        constexpr std::array<uint8_t, 4> bytes{0x0F, 0x5E, 0xC1, 0x90};
        constexpr std::array<float, 4> dividend{1, -0.0F, 0, std::numeric_limits<float>::infinity()};
        constexpr std::array<float, 4> divisor{0, 2, 0, 2};
        emu->write_memory(code, bytes.data(), bytes.size());
        emu->write_register(x86_register::xmm0, dividend.data(), sizeof(dividend));
        emu->write_register(x86_register::xmm1, divisor.data(), sizeof(divisor));
        emu->start(1);
        ASSERT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 3);
        const auto result = emu->reg<std::array<float, 4>>(x86_register::xmm0);
        EXPECT_TRUE(std::isinf(result[0]) && !std::signbit(result[0]));
        EXPECT_EQ(result[1], 0.0F);
        EXPECT_TRUE(std::signbit(result[1]));
        EXPECT_TRUE(std::isnan(result[2]));
        EXPECT_TRUE(std::isinf(result[3]) && !std::signbit(result[3]));
        EXPECT_EQ((emu->reg<std::array<float, 4>>(x86_register::xmm1)), divisor);
    }

    TEST_F(IciclePackedDivide, RegisterDoublePrecisionSupportsAliasedOperands)
    {
        constexpr std::array<uint8_t, 5> bytes{0x66, 0x0F, 0x5E, 0xC0, 0x90};
        constexpr std::array<double, 2> dividend{6.25, -9.5};
        constexpr std::array<double, 2> expected{1, 1};
        emu->write_memory(code, bytes.data(), bytes.size());
        emu->write_register(x86_register::xmm0, dividend.data(), sizeof(dividend));
        emu->start(1);
        ASSERT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 4);
        EXPECT_EQ((emu->reg<std::array<double, 2>>(x86_register::xmm0)), expected);
    }
}
