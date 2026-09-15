#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <array>
#include <vector>

namespace sogen::test
{
    class IcicleMoveMask : public testing::Test
    {
      protected:
        using vector = std::array<uint8_t, 64>;
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};
        size_t cursor{};

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x4000, memory_permission::all);
            ASSERT_NE(code, 0u);
            emu->hook_interrupt([](cpu_interface& cpu, int) { cpu.stop(); });
            xcr0(7);
        }

        uint64_t load(std::vector<uint8_t> bytes)
        {
            const auto address = code + cursor;
            cursor += 32;
            bytes.push_back(0x90);
            emu->write_memory(address, bytes.data(), bytes.size());
            emu->reg(x86_register::rip, address);
            return address;
        }

        void xcr0(const uint64_t value)
        {
            load({0x0F, 0x01, 0xD1});
            emu->reg(x86_register::rax, value);
            emu->reg(x86_register::rcx, 0ULL);
            emu->reg(x86_register::rdx, 0ULL);
            emu->start(1);
        }

        static std::vector<uint8_t> encoding(const bool packed_double, const size_t width, const bool vex)
        {
            if (vex)
            {
                return {0xC5, static_cast<uint8_t>(0xF8 | (packed_double ? 1 : 0) | (width == 32 ? 4 : 0)), 0x50, 0xC4};
            }
            return packed_double ? std::vector<uint8_t>{0x66, 0x0F, 0x50, 0xC4} : std::vector<uint8_t>{0x0F, 0x50, 0xC4};
        }

        static vector signs(const size_t element, const uint32_t mask)
        {
            vector result{};
            result.fill(0x5A);
            for (size_t i = 0; i < 32 / element; ++i)
            {
                result[(i + 1) * element - 1] = static_cast<uint8_t>(0x7F | (((mask >> i) & 1) << 7));
            }
            return result;
        }

        void expect_fault(const std::vector<uint8_t>& bytes, const int expected)
        {
            int received{};
            auto* const hook = emu->hook_interrupt([&](cpu_interface& cpu, const int value) {
                received = value;
                cpu.stop();
            });
            const auto address = load(bytes);
            emu->reg(x86_register::rax, UINT64_MAX);
            const auto source = signs(4, 0xA5);
            emu->reg<vector>(x86_register::zmm4, source);
            const auto flags = emu->reg<uint32_t>(x86_register::eflags);
            const auto mxcsr = emu->reg<uint32_t>(x86_register::mxcsr);
            emu->start(1);
            EXPECT_EQ(received, expected);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), UINT64_MAX);
            EXPECT_EQ(emu->reg<vector>(x86_register::zmm4), source);
            EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), flags);
            EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), mxcsr);
            emu->delete_hook(hook);
        }
    };

    TEST_F(IcicleMoveMask, CapturedGameOperandZerosTheEntireRax)
    {
        const auto address = load({0x0F, 0x50, 0xC4});
        vector source{};
        for (size_t i = 12; i < 16; ++i)
        {
            source[i] = 0xFF;
        }
        source[40] = 0xA5;
        emu->reg<vector>(x86_register::zmm4, source);
        emu->reg(x86_register::rax, 0xBACD14E780ULL);
        emu->reg(x86_register::eflags, 0x44u);
        emu->reg(x86_register::mxcsr, 0x1F80u);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + 3);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 8u);
        EXPECT_EQ(emu->reg<vector>(x86_register::zmm4), source);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), 0x44u);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1F80u);
    }

    TEST_F(IcicleMoveMask, EverySignMaskInAllSixForms)
    {
        for (const bool packed_double : {false, true})
        {
            for (const auto family : {0, 1, 2})
            {
                const size_t width = family == 2 ? 32 : 16;
                const size_t element = packed_double ? 8 : 4;
                const auto bytes = encoding(packed_double, width, family != 0);
                const auto address = load(bytes);
                for (uint32_t mask = 0; mask < (1u << (width / element)); ++mask)
                {
                    SCOPED_TRACE(packed_double);
                    SCOPED_TRACE(family);
                    SCOPED_TRACE(mask);
                    const auto source = signs(element, mask);
                    emu->reg<vector>(x86_register::zmm4, source);
                    emu->reg(x86_register::rax, UINT64_MAX);
                    emu->reg(x86_register::rip, address);
                    emu->reg(x86_register::eflags, 0xCD5u);
                    const auto flags = emu->reg<uint32_t>(x86_register::eflags);
                    emu->reg(x86_register::mxcsr, 0xFFFFu);
                    emu->start(1);
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), mask);
                    EXPECT_EQ(emu->reg<vector>(x86_register::zmm4), source);
                    EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), flags);
                    EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0xFFFFu);
                }
            }
        }
    }

    TEST_F(IcicleMoveMask, ExtendedRegistersAndIgnoredWBit)
    {
        for (const bool packed_double : {false, true})
        {
            for (const bool vex : {false, true})
            {
                for (const bool w : {false, true})
                {
                    std::vector<uint8_t> bytes;
                    if (vex)
                    {
                        bytes = {0xC4, 0x41, static_cast<uint8_t>(0x7C | (packed_double ? 1 : 0) | (w ? 0x80 : 0)), 0x50, 0xCF};
                    }
                    else
                    {
                        if (packed_double)
                        {
                            bytes.push_back(0x66);
                        }
                        bytes.insert(bytes.end(), {static_cast<uint8_t>(w ? 0x4D : 0x45), 0x0F, 0x50, 0xCF});
                    }
                    const auto source = signs(packed_double ? 8 : 4, 0xA5);
                    emu->reg<vector>(x86_register::zmm15, source);
                    emu->reg(x86_register::r9, UINT64_MAX);
                    const auto address = load(bytes);
                    emu->start(1);
                    const auto bits = (vex ? 32 : 16) / (packed_double ? 8 : 4);
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::r9), 0xA5u & ((1u << bits) - 1));
                    EXPECT_EQ(emu->reg<vector>(x86_register::zmm15), source);
                }
            }
        }
    }

    TEST_F(IcicleMoveMask, NanInfinityZeroAndDenormalSignsDoNotRaiseFpExceptions)
    {
        for (const bool packed_double : {false, true})
        {
            const std::array<uint64_t, 6> magnitudes =
                packed_double
                    ? std::array<uint64_t, 6>{0, 1, 0x7FF0000000000000, 0x7FF0000000000001, 0x7FF8000000000000, 0x3FF0000000000000}
                    : std::array<uint64_t, 6>{0, 1, 0x7F800000, 0x7F800001, 0x7FC00000, 0x3F800000};
            for (const auto family : {0, 1, 2})
            {
                const size_t element = packed_double ? 8 : 4;
                const size_t width = family == 2 ? 32 : 16;
                const auto bytes = encoding(packed_double, width, family != 0);
                const auto address = load(bytes);
                for (const auto magnitude : magnitudes)
                {
                    vector source{};
                    for (size_t lane = 0; lane < width / element; ++lane)
                    {
                        const uint64_t value = magnitude | ((lane & 1ULL) << (element * 8 - 1));
                        for (size_t byte = 0; byte < element; ++byte)
                        {
                            source[lane * element + byte] = static_cast<uint8_t>(value >> (byte * 8));
                        }
                    }
                    emu->reg<vector>(x86_register::zmm4, source);
                    emu->reg(x86_register::rax, UINT64_MAX);
                    emu->reg(x86_register::rip, address);
                    emu->reg(x86_register::mxcsr, 0u);
                    emu->start(1);
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 0xAAu & ((1u << (width / element)) - 1));
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
                    EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0u);
                    EXPECT_EQ(emu->reg<vector>(x86_register::zmm4), source);
                }
            }
        }
    }

    TEST_F(IcicleMoveMask, ReservedVvvvMemoryAndIllegalPrefixesRaiseInvalidOpcode)
    {
        for (const bool packed_double : {false, true})
        {
            for (const auto family : {0, 1, 2})
            {
                const auto valid = encoding(packed_double, family == 2 ? 32 : 16, family != 0);
                for (const uint8_t modrm : std::array<uint8_t, 3>{0x04, 0x44, 0x84})
                {
                    auto bytes = valid;
                    bytes.back() = modrm;
                    bytes.insert(bytes.end(), {0x24, 0, 0, 0, 0});
                    expect_fault(bytes, 6);
                }
                auto locked = valid;
                locked.insert(locked.begin(), 0xF0);
                expect_fault(locked, 6);
                if (family != 0)
                {
                    for (uint8_t vvvv = 0; vvvv < 15; ++vvvv)
                    {
                        auto bytes = valid;
                        bytes[1] = static_cast<uint8_t>((bytes[1] & 0x87) | (vvvv << 3));
                        expect_fault(bytes, 6);
                    }
                    for (const uint8_t prefix : std::array<uint8_t, 5>{0x40, 0x48, 0x66, 0xF2, 0xF3})
                    {
                        auto bytes = valid;
                        bytes.insert(bytes.begin(), prefix);
                        expect_fault(bytes, 6);
                    }
                }
            }
        }
    }

    TEST_F(IcicleMoveMask, DisabledSseAndAvxStatePreservesTheDestination)
    {
        for (const bool packed_double : {false, true})
        {
            for (const auto family : {0, 1, 2})
            {
                const auto bytes = encoding(packed_double, family == 2 ? 32 : 16, family != 0);
                if (family == 0)
                {
                    emu->reg(x86_register::cr0, 0x80010037ULL);
                    expect_fault(bytes, 6);
                    emu->reg(x86_register::cr0, 0x80010033ULL);
                    emu->reg(x86_register::cr4, 0x40420ULL);
                    expect_fault(bytes, 6);
                }
                else
                {
                    for (const auto value : {1ULL, 3ULL})
                    {
                        xcr0(value);
                        expect_fault(bytes, 6);
                    }
                    xcr0(7);
                    emu->reg(x86_register::cr4, 0x620ULL);
                    expect_fault(bytes, 6);
                }
                emu->reg(x86_register::cr4, 0x40620ULL);
                emu->reg(x86_register::cr0, 0x8001003BULL);
                expect_fault(bytes, 7);
                emu->reg(x86_register::cr0, 0x80010033ULL);
            }
        }
    }

    TEST_F(IcicleMoveMask, ConsecutiveIntegerAndVectorInstructionsObserveTheMask)
    {
        const auto address = load({0x66, 0x0F, 0xEF, 0xE4, 0x0F, 0x50, 0xC4, 0x83, 0xF0, 0x08});
        emu->reg<vector>(x86_register::zmm4, signs(4, 0xFF));
        emu->reg(x86_register::rax, UINT64_MAX);
        emu->start(3);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + 10);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 8u);
        EXPECT_EQ((emu->reg<std::array<uint64_t, 2>>(x86_register::xmm4)), (std::array<uint64_t, 2>{}));
    }
}
