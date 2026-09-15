#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <algorithm>
#include <array>
#include <vector>

namespace sogen::test
{
    class IciclePmulhuw : public testing::Test
    {
      protected:
        using vector = std::array<uint16_t, 32>;
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};
        uint64_t data{};
        size_t cursor{};
        int interrupt{};

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x10000, memory_permission::all);
            data = memory->allocate_memory(0x2000, memory_permission::read_write);
            ASSERT_NE(code, 0U);
            ASSERT_NE(data, 0U);
            emu->hook_interrupt([&](cpu_interface& cpu, const int value) {
                interrupt = value;
                cpu.stop();
            });
            xcr0(0xE7);
        }

        uint64_t load(std::vector<uint8_t> bytes)
        {
            const auto address = code + cursor;
            cursor += 32;
            bytes.push_back(0x90);
            emu->write_memory(address, bytes.data(), bytes.size());
            emu->reg(x86_register::rip, address);
            interrupt = 0;
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

        static x86_register zmm(const size_t index)
        {
            return static_cast<x86_register>(static_cast<int>(x86_register::zmm0) + static_cast<int>(index));
        }

        static vector words(const size_t shift = 0)
        {
            constexpr std::array<uint16_t, 8> values{0, 1, 32767, 32768, 65534, 65535, 256, 257};
            vector result{};
            for (size_t i = 0; i < result.size(); ++i)
            {
                result[i] = values[(i + shift) % values.size()];
            }
            return result;
        }

        static vector multiply(const vector& a, const vector& b, const size_t width)
        {
            vector result{};
            for (size_t i = 0; i < width / 2; ++i)
            {
                result[i] = static_cast<uint16_t>((static_cast<uint64_t>(a[i]) * b[i]) / 65536);
            }
            return result;
        }

        static std::vector<uint8_t> encoding(const int family, const size_t width, const size_t destination, const size_t first,
                                             const size_t second, const bool mem = false, const uint8_t mask = 0, const bool zero = false)
        {
            std::vector<uint8_t> bytes;
            if (family == 0)
            {
                if (width != 8)
                {
                    bytes.push_back(0x66);
                }
                if ((destination | second) & 8)
                {
                    bytes.push_back(static_cast<uint8_t>(0x40 | ((destination & 8) >> 1) | ((second & 8) >> 3)));
                }
                bytes.push_back(0x0F);
            }
            else if (family == 1)
            {
                bytes = {0xC4, static_cast<uint8_t>(0xE1 ^ ((destination & 8) << 4) ^ ((second & 8) << 2)),
                         static_cast<uint8_t>(((~first & 15) << 3) | 1 | (width == 32 ? 4 : 0))};
            }
            else
            {
                const auto length = width == 64 ? 0x40 : 0;
                bytes = {
                    0x62,
                    static_cast<uint8_t>(0xF1 ^ ((destination & 8) << 4) ^ (destination & 16) ^ ((second & 8) << 2) ^ ((second & 16) << 2)),
                    static_cast<uint8_t>(5 | ((~first & 15) << 3)),
                    static_cast<uint8_t>((width == 32 ? 0x20 : length) | ((first & 16) ? 0 : 8) | mask | (zero ? 0x80 : 0))};
            }
            bytes.push_back(0xE4);
            bytes.push_back(static_cast<uint8_t>((mem ? 0 : 0xC0) | ((destination & 7) << 3) | (second & 7)));
            return bytes;
        }

        void expect_fault(const std::vector<uint8_t>& bytes, const int expected)
        {
            const auto address = load(bytes);
            const auto original = emu->save_registers();
            emu->start(1);
            EXPECT_EQ(interrupt, expected);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address);
            EXPECT_EQ(emu->save_registers(), original);
        }
    };

    TEST_F(IciclePmulhuw, CapturedWarpInstruction)
    {
        vector a{0, 0, 2, 0, 4, 0, 6, 0};
        vector b{20, 0, 20, 0, 20, 0, 20, 0};
        std::fill(a.begin() + 8, a.end(), 0xABCD);
        emu->reg<vector>(zmm(7), a);
        emu->reg<vector>(zmm(4), b);
        const auto ip = load({0x66, 0x0F, 0xE4, 0xFC});
        emu->reg(x86_register::eflags, 0xCD5U);
        emu->reg(x86_register::mxcsr, 0xFFFFU);
        const auto flags = emu->reg<uint32_t>(x86_register::eflags);
        emu->start(1);
        std::fill_n(a.begin(), 8, 0);
        EXPECT_EQ(emu->reg<vector>(zmm(7)), a);
        EXPECT_EQ(emu->reg<vector>(zmm(4)), b);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + 4);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), flags);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0xFFFFU);
    }

    TEST_F(IciclePmulhuw, SixVectorFormsMemoryAndRegisterSources)
    {
        for (const int family : {0, 1, 2})
        {
            for (const size_t width : {16U, 32U, 64U})
            {
                for (const bool mem : {false, true})
                {
                    if ((family == 0 && width != 16) || (family == 1 && width == 64))
                    {
                        continue;
                    }
                    SCOPED_TRACE(family);
                    SCOPED_TRACE(width);
                    SCOPED_TRACE(mem);
                    const auto first = words();
                    const auto second = words(3);
                    const size_t target = std::array<size_t, 3>{1, 9, 25}[static_cast<size_t>(family)];
                    const size_t other = family == 2 ? 31 : 15;
                    const auto address = data + 0x2000 - width;
                    emu->write_memory(address, second.data(), width);
                    emu->reg(x86_register::rbx, address);
                    emu->reg<vector>(zmm(1), first);
                    emu->reg<vector>(zmm(other), second);
                    auto expected = multiply(first, second, width);
                    if (family == 0)
                    {
                        std::copy(first.begin() + 8, first.end(), expected.begin() + 8);
                    }
                    const auto bytes = encoding(family, width, target, 1, mem ? 3 : other, mem);
                    const auto ip = load(bytes);
                    emu->start(1);
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
                    EXPECT_EQ(emu->reg<vector>(zmm(target)), expected);
                    EXPECT_EQ(interrupt, 0);
                }
            }
        }
    }

    TEST_F(IciclePmulhuw, WordMasksAndBothWigValues)
    {
        for (const size_t width : {16U, 32U, 64U})
        {
            for (const bool zero : {false, true})
            {
                for (const bool w : {false, true})
                {
                    for (const uint64_t mask : {0ULL, 1ULL, 0xAAAAAAAAULL, 0x80000000ULL, 0xFFFFFFFFULL})
                    {
                        auto original = words(1);
                        const auto a = words();
                        const auto b = words(3);
                        emu->reg<vector>(zmm(25), original);
                        emu->reg<vector>(zmm(18), a);
                        emu->reg<vector>(zmm(31), b);
                        emu->reg(x86_register::k3, mask);
                        auto bytes = encoding(2, width, 25, 18, 31, false, 3, zero);
                        if (w)
                        {
                            bytes[2] |= 0x80;
                        }
                        const auto ip = load(bytes);
                        emu->start(1);
                        const auto products = multiply(a, b, width);
                        for (size_t i = 0; i < width / 2; ++i)
                        {
                            if (mask & (1ULL << i))
                            {
                                original[i] = products[i];
                            }
                            else if (zero)
                            {
                                original[i] = 0;
                            }
                        }
                        std::fill(original.begin() + static_cast<ptrdiff_t>(width / 2), original.end(), 0);
                        EXPECT_EQ(emu->reg<vector>(zmm(25)), original);
                        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
                        EXPECT_EQ(interrupt, 0);
                    }
                }
            }
        }
    }

    TEST_F(IciclePmulhuw, SourcesMayAliasDestinationAndTwoByteVex)
    {
        for (const bool alias_first : {false, true})
        {
            for (const bool two_byte : {false, true})
            {
                const auto a = words();
                const auto b = words(5);
                emu->reg<vector>(zmm(1), a);
                emu->reg<vector>(zmm(2), b);
                const auto target = alias_first ? 1U : 2U;
                auto bytes = encoding(1, 32, target, 1, 2);
                if (two_byte)
                {
                    bytes.erase(bytes.begin() + 1);
                    bytes[0] = 0xC5;
                    bytes[1] |= 0x80;
                }
                else
                {
                    bytes[2] |= 0x80;
                }
                load(bytes);
                emu->start(1);
                EXPECT_EQ(emu->reg<vector>(zmm(target)), multiply(a, b, 32));
                EXPECT_EQ(interrupt, 0);
            }
        }
    }

    TEST_F(IciclePmulhuw, MmxPhysicalAliasesMemoryAndState)
    {
        using extended = std::array<uint8_t, 10>;
        constexpr uint64_t left = 0xFFFF80007FFF0100;
        constexpr uint64_t right = 0xFFFF800000020100;
        constexpr uint64_t result = 0xFFFE400000000001;
        const extended source{0, 1, 2, 0, 0, 0x80, 0xFF, 0xFF, 0x34, 0x12};
        for (const bool mem : {false, true})
        {
            emu->reg(x86_register::fpsw, uint16_t{0});
            emu->reg(x86_register::mm2, left);
            emu->reg<extended>(x86_register::st4, source);
            emu->write_memory(data, right);
            emu->reg(x86_register::rbx, data);
            emu->reg(x86_register::fptag, uint16_t{0xFFFF});
            emu->reg(x86_register::fpsw, uint16_t{0x100});
            const auto ip = load(encoding(0, 8, 2, 2, mem ? 3 : 4, mem));
            emu->start(1);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::mm2), result);
            EXPECT_EQ(emu->reg<extended>(x86_register::st4), source);
            EXPECT_EQ(emu->reg<uint16_t>(x86_register::fptag), 0U);
            EXPECT_EQ(emu->reg<uint16_t>(x86_register::fpsw), 0x100U);
            EXPECT_EQ(emu->reg<extended>(x86_register::st2)[9], 0xFFU);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + 3);
        }
        emu->reg<extended>(x86_register::st3, extended{0, 1, 0xFF, 0x7F, 0, 0x80, 0xFF, 0xFF, 0x11, 0x22});
        emu->reg<extended>(x86_register::st5, source);
        emu->reg(x86_register::fpsw, uint16_t{0x3900});
        load({0x0F, 0xE4, 0xD4});
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::mm2), result);
        EXPECT_EQ(emu->reg<extended>(x86_register::st4), source);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fpsw), 0x100U);
    }

    TEST_F(IciclePmulhuw, MaskedMemoryDoesNotReadInactiveWords)
    {
        const auto a = words(4);
        const auto b = words(3);
        const auto address = data + 0x1FFE;
        emu->write_memory<uint16_t>(address, b[0]);
        emu->reg(x86_register::rbx, address);
        emu->reg<vector>(zmm(18), a);
        emu->reg(x86_register::k1, 1ULL);
        auto bytes = encoding(2, 64, 25, 18, 3, true, 1, true);
        load(bytes);
        emu->start(1);
        EXPECT_EQ(emu->reg<vector>(zmm(25)), multiply(a, b, 2));
        for (const uint64_t address_bad : {0x800000000001ULL, 0x700000000001ULL})
        {
            emu->reg(x86_register::rbx, address_bad);
            emu->reg(x86_register::k1, 0ULL);
            const auto ip = load(bytes);
            emu->start(1);
            EXPECT_EQ(interrupt, 0);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
            EXPECT_EQ(emu->reg<vector>(zmm(25)), vector{});
        }
    }

    TEST_F(IciclePmulhuw, AlignmentAndCanonicalAddressFaults)
    {
        emu->reg(x86_register::rbx, data + 1);
        expect_fault(encoding(0, 16, 9, 9, 3, true), 13);
        for (const bool stack : {false, true})
        {
            emu->reg(x86_register::rbx, 0x800000000000ULL);
            emu->reg(x86_register::rbp, 0x800000000000ULL);
            auto bytes = encoding(2, 64, 25, 18, stack ? 5 : 3, true);
            if (stack)
            {
                bytes.back() |= 0x40;
                bytes.push_back(0);
            }
            expect_fault(bytes, stack ? 12 : 13);
        }
        emu->reg(x86_register::rbx, data + 1);
        emu->reg(x86_register::cr0, 0x80050033ULL);
        emu->reg(x86_register::cs, uint16_t{0x33});
        emu->reg(x86_register::eflags, 0x40002U);
        expect_fault(encoding(0, 8, 2, 2, 3, true), 17);
        expect_fault(encoding(2, 64, 25, 18, 3, true), 17);
        for (const size_t width : {16U, 32U})
        {
            const auto bytes = encoding(1, width, 9, 1, 3, true);
            const auto ip = load(bytes);
            emu->start(1);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
            EXPECT_EQ(interrupt, 0);
        }
    }

    TEST_F(IciclePmulhuw, FailedMemoryReadPreservesAllRegisterState)
    {
        uint64_t fault{};
        emu->hook_memory_violation([&](cpu_interface&, const uint64_t address, size_t, memory_operation, memory_violation_type) {
            fault = address;
            return memory_violation_continuation::stop;
        });
        for (const int family : {0, 1, 2})
        {
            const auto bytes = encoding(family, family == 0 ? 8 : 32, family == 0 ? 2 : 9, 1, 3, true);
            emu->reg(x86_register::rbx, data + 0x1FFC);
            emu->reg(x86_register::fpsw, uint16_t{0x3900});
            emu->reg(x86_register::fptag, uint16_t{0xFFFF});
            load(bytes);
            const auto original = emu->save_registers();
            EXPECT_THROW(emu->start(1), std::runtime_error);
            EXPECT_EQ(fault, data + 0x2000);
            EXPECT_EQ(emu->save_registers(), original);
        }
    }

    TEST_F(IciclePmulhuw, DisabledStateAndPendingX87FaultBeforeMemoryAccess)
    {
        emu->reg(x86_register::rbx, 0x700000000000ULL);
        emu->reg(x86_register::cr0, 0x80010037ULL);
        expect_fault(encoding(0, 8, 2, 2, 3, true), 6);
        expect_fault(encoding(0, 16, 9, 9, 3, true), 6);
        emu->reg(x86_register::cr0, 0x8001003BULL);
        for (const int family : {0, 1, 2})
        {
            expect_fault(encoding(family, 16, 9, 1, 3, true), 7);
        }
        emu->reg(x86_register::cr0, 0x80010033ULL);
        emu->reg(x86_register::cr4, 0x40420ULL);
        expect_fault(encoding(0, 16, 9, 9, 3, true), 6);
        emu->reg(x86_register::cr4, 0x40620ULL);
        xcr0(3);
        expect_fault(encoding(1, 32, 9, 1, 3, true), 6);
        expect_fault(encoding(2, 64, 9, 1, 3, true), 6);
        xcr0(7);
        expect_fault(encoding(2, 64, 9, 1, 3, true), 6);
        emu->reg(x86_register::fpsw, uint16_t{1});
        emu->reg(x86_register::fpcw, uint16_t{0x37E});
        expect_fault(encoding(0, 8, 2, 2, 3, true), 16);
    }

    TEST_F(IciclePmulhuw, RelativeAndCompressedDisplacement)
    {
        const auto a = words(4);
        const auto b = words(3);
        emu->write_memory(data, b);
        constexpr uint64_t low_data = 0x200000;
        ASSERT_TRUE(memory->allocate_memory(low_data, 0x1000, memory_permission::read_write));
        emu->write_memory(low_data, b);
        for (const size_t width : {16U, 32U, 64U})
        {
            for (const int mode : {0, 1, 2})
            {
                emu->reg<vector>(zmm(18), a);
                auto bytes = encoding(2, width, 25, 18, mode == 2 ? 3 : 5, true);
                if (mode == 2)
                {
                    emu->reg(x86_register::rbx, data + 2 * width);
                    bytes.back() |= 0x40;
                    bytes.push_back(0xFE);
                }
                else
                {
                    if (mode == 1)
                    {
                        bytes.insert(bytes.begin(), 0x67);
                    }
                    const auto displacement = static_cast<uint32_t>((mode == 1 ? low_data : data) - (code + cursor + bytes.size() + 4));
                    for (size_t i = 0; i < 4; ++i)
                    {
                        bytes.push_back(static_cast<uint8_t>(displacement >> (8 * i)));
                    }
                }
                const auto ip = load(bytes);
                emu->start(1);
                EXPECT_EQ(interrupt, 0);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
                EXPECT_EQ(emu->reg<vector>(zmm(25)), multiply(a, b, width));
            }
        }
    }

    TEST_F(IciclePmulhuw, ExtendedSibAndSegmentBases)
    {
        const auto a = words(4);
        const auto b = words(3);
        emu->write_memory(data + 32, b);
        for (const uint8_t segment : std::array<uint8_t, 3>{0, 0x64, 0x65})
        {
            emu->reg<vector>(zmm(18), a);
            emu->reg(x86_register::r12, segment == 0 ? data : 0ULL);
            emu->reg(x86_register::r13, 4ULL);
            if (segment != 0)
            {
                emu->reg(segment == 0x64 ? x86_register::fs_base : x86_register::gs_base, data);
            }
            auto bytes = encoding(2, 64, 25, 18, 12, true);
            bytes[1] &= ~0x40;
            bytes.push_back(0xEC);
            if (segment != 0)
            {
                bytes.insert(bytes.begin(), segment);
            }
            const auto ip = load(bytes);
            emu->start(1);
            EXPECT_EQ(interrupt, 0);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
            EXPECT_EQ(emu->reg<vector>(zmm(25)), multiply(a, b, 64));
        }
    }

    TEST_F(IciclePmulhuw, InvalidPrefixesAndReservedEvexFields)
    {
        const auto valid = encoding(2, 64, 25, 18, 31);
        for (const uint8_t prefix : std::array<uint8_t, 5>{0xF0, 0x66, 0xF2, 0xF3, 0x40})
        {
            auto bytes = valid;
            bytes.insert(bytes.begin(), prefix);
            expect_fault(bytes, 6);
        }
        for (const size_t index : {1U, 2U, 3U})
        {
            auto bytes = valid;
            bytes[index] ^= std::array<uint8_t, 3>{8, 4, 0x10}[index - 1];
            expect_fault(bytes, 6);
        }
        auto bytes = valid;
        bytes[3] |= 0x80;
        expect_fault(bytes, 6);
        bytes = valid;
        bytes[3] |= 0x60;
        expect_fault(bytes, 6);
        bytes = encoding(0, 8, 2, 2, 4);
        bytes.insert(bytes.begin(), 0xF0);
        expect_fault(bytes, 6);
    }
}
