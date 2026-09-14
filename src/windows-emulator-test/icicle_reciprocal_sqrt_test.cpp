#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <array>
#include <bit>
#include <cmath>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace sogen::test
{
    class IcicleReciprocalSqrt : public testing::Test
    {
      protected:
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};
        using lanes = std::array<uint32_t, 4>;

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x2000, memory_permission::all);
            ASSERT_NE(code, 0u);
            emu->reg(x86_register::rip, code);
        }

        lanes execute(const lanes& source, const lanes& destination = {}, const uint32_t mxcsr = 0x1F80)
        {
            constexpr std::array<uint8_t, 4> bytes{0x0F, 0x52, 0xE2, 0x90};
            emu->write_memory(code, bytes.data(), bytes.size());
            emu->write_register(x86_register::xmm2, source.data(), sizeof(source));
            emu->write_register(x86_register::xmm4, destination.data(), sizeof(destination));
            emu->reg(x86_register::mxcsr, mxcsr);
            emu->start(1);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 3);
            EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), mxcsr);
            EXPECT_EQ(emu->reg<lanes>(x86_register::xmm2), source);
            return emu->reg<lanes>(x86_register::xmm4);
        }

        static void check_estimate(const lanes& source, const lanes& result)
        {
            for (size_t lane = 0; lane < 4; ++lane)
            {
                const double value = std::bit_cast<float>(source[lane]);
                const double estimate = std::bit_cast<float>(result[lane]);
                EXPECT_LE(std::abs(estimate * std::sqrt(value) - 1.0), 1.5 / 4096.0);
            }
#if defined(_M_X64) || defined(__x86_64__)
            const auto expected = std::bit_cast<lanes>(_mm_rsqrt_ps(std::bit_cast<__m128>(source)));
            EXPECT_EQ(result, expected);
#endif
        }
    };

    TEST_F(IcicleReciprocalSqrt, YmmRegisterRoundTripsBothHalves)
    {
        constexpr std::array<uint32_t, 8> original{0x12345678, 0x9ABCDEF0, 1, 2, 0x87654321, 0xFEDCBA98, 3, 4};
        emu->write_register(x86_register::ymm4, original.data(), sizeof(original));
        EXPECT_EQ((emu->reg<std::array<uint32_t, 8>>(x86_register::ymm4)), original);
        EXPECT_EQ(emu->reg<lanes>(x86_register::xmm4), (lanes{original[0], original[1], 1, 2}));
    }

    TEST_F(IcicleReciprocalSqrt, ActualGameRegisterFormPreservesUpperYmm)
    {
        constexpr lanes source{0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000};
        constexpr std::array<uint32_t, 8> original{0xCF72D000, 0x3E6777A5, 0, 0, 1, 2, 3, 4};
        emu->write_register(x86_register::ymm4, original.data(), sizeof(original));
        const auto result = execute(source, {original[0], original[1], 0, 0});
        check_estimate(source, result);
        const auto full = emu->reg<std::array<uint32_t, 8>>(x86_register::ymm4);
        for (size_t lane = 4; lane < 8; ++lane)
        {
            EXPECT_EQ(full[lane], original[lane]);
        }
        RecordProperty("observed_lane_bits", std::to_string(result[0]));
    }

    TEST_F(IcicleReciprocalSqrt, RipRelativeMemoryReadsAllFourLanes)
    {
        constexpr std::array<uint8_t, 8> bytes{0x0F, 0x52, 0x25, 0xF9, 0, 0, 0, 0x90};
        constexpr lanes source{0x3F800000, 0x40000000, 0x40800000, 0x41100000};
        emu->write_memory(code, bytes.data(), bytes.size());
        emu->write_memory(code + 0x100, source.data(), sizeof(source));
        emu->start(1);
        ASSERT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 7);
        check_estimate(source, emu->reg<lanes>(x86_register::xmm4));
        EXPECT_EQ(emu->read_memory<lanes>(code + 0x100), source);
    }

    TEST_F(IcicleReciprocalSqrt, AliasedPackedOperandsKeepIndependentLanes)
    {
        constexpr std::array<uint8_t, 4> bytes{0x0F, 0x52, 0xE4, 0x90};
        constexpr lanes source{0x00800000, 0x3DCCCCCD, 0x7F7FFFFF, 0x42C80000};
        emu->write_memory(code, bytes.data(), bytes.size());
        emu->write_register(x86_register::xmm4, source.data(), sizeof(source));
        emu->start(1);
        ASSERT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 3);
        check_estimate(source, emu->reg<lanes>(x86_register::xmm4));
    }

    TEST_F(IcicleReciprocalSqrt, SignedZeroAndDenormalsProduceSignedInfinity)
    {
        constexpr lanes source{0, 0x80000000, 1, 0x807FFFFF};
        constexpr lanes expected{0x7F800000, 0xFF800000, 0x7F800000, 0xFF800000};
        EXPECT_EQ(execute(source), expected);
    }

    TEST_F(IcicleReciprocalSqrt, NegativeValuesAndInfinitiesFollowArchitecturalRules)
    {
        constexpr lanes source{0xBF800000, 0xFF800000, 0x7F800000, 0xC0800000};
        constexpr lanes expected{0xFFC00000, 0xFFC00000, 0, 0xFFC00000};
        EXPECT_EQ(execute(source), expected);
    }

    TEST_F(IcicleReciprocalSqrt, NanPayloadsAndSignsArePreservedAndQuieted)
    {
        constexpr lanes source{0x7FC12345, 0xFFC54321, 0x7F812345, 0xFF854321};
        constexpr lanes expected{0x7FC12345, 0xFFC54321, 0x7FC12345, 0xFFC54321};
        EXPECT_EQ(execute(source), expected);
    }

    TEST_F(IcicleReciprocalSqrt, RoundingAndDenormalModesDoNotAlterEstimateOrFlags)
    {
        constexpr lanes source{0x40000000, 1, 0x80000001, 0x7F812345};
        const auto baseline = execute(source);
        for (const auto mode : {0x0000u, 0x1FC0u, 0x3F80u, 0x5F80u, 0x7F80u, 0xFFFFu})
        {
            emu->reg(x86_register::rip, code);
            EXPECT_EQ(execute(source, {}, mode), baseline);
        }
    }

    TEST_F(IcicleReciprocalSqrt, ScalarRegisterPreservesUpperDestinationLanes)
    {
        constexpr std::array<uint8_t, 5> bytes{0xF3, 0x0F, 0x52, 0xE2, 0x90};
        constexpr lanes source{0x40800000, 0x7F812345, 1, 0xBF800000};
        constexpr std::array<uint32_t, 8> original{0xDEADBEEF, 1, 2, 3, 4, 5, 6, 7};
        emu->write_memory(code, bytes.data(), bytes.size());
        emu->write_register(x86_register::xmm2, source.data(), sizeof(source));
        emu->write_register(x86_register::ymm4, original.data(), sizeof(original));
        emu->start(1);
        ASSERT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 4);
        const auto result = emu->reg<std::array<uint32_t, 8>>(x86_register::ymm4);
        EXPECT_NEAR(std::bit_cast<float>(result[0]), 0.5, 0.5 * 1.5 / 4096.0);
        for (size_t lane = 1; lane < 8; ++lane)
        {
            EXPECT_EQ(result[lane], original[lane]);
        }
        EXPECT_EQ(emu->reg<lanes>(x86_register::xmm2), source);
    }

    TEST_F(IcicleReciprocalSqrt, ScalarMemoryReadsOnlyFourBytes)
    {
        constexpr std::array<uint8_t, 9> bytes{0xF3, 0x0F, 0x52, 0x25, 0xF4, 0x1F, 0, 0, 0x90};
        constexpr lanes original{0, 0x11223344, 0x55667788, 0x99AABBCC};
        constexpr uint32_t source = 0x80000000;
        emu->write_memory(code, bytes.data(), bytes.size());
        emu->write_memory(code + 0x1FFC, &source, sizeof(source));
        emu->write_register(x86_register::xmm4, original.data(), sizeof(original));
        emu->start(1);
        ASSERT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 8);
        EXPECT_EQ(emu->reg<lanes>(x86_register::xmm4), (lanes{0xFF800000, original[1], original[2], original[3]}));
    }
}
