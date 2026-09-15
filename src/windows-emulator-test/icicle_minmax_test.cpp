#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <array>
#include <vector>
#include <algorithm>

namespace sogen::test
{
    class IcicleMinMax : public testing::Test
    {
      protected:
        using vector = std::array<uint8_t, 64>;
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
            ASSERT_NE(code, 0u);
            ASSERT_NE(data, 0u);
            emu->hook_interrupt([&](cpu_interface& cpu, const int value) {
                interrupt = value;
                cpu.stop();
            });
            xcr0(0xE7);
            emu->reg(x86_register::mxcsr, 0x1F80u);
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

        static vector fill(const size_t element, const uint64_t value)
        {
            vector result{};
            for (size_t byte = 0; byte < result.size(); ++byte)
            {
                result[byte] = static_cast<uint8_t>(value >> ((byte % element) * 8));
            }
            return result;
        }

        static uint64_t one(const size_t element)
        {
            return element == 4 ? 0x3F800000ULL : 0x3FF0000000000000ULL;
        }

        static uint64_t two(const size_t element)
        {
            return element == 4 ? 0x40000000ULL : 0x4000000000000000ULL;
        }

        static size_t element(const uint8_t pp)
        {
            return pp == 0 ? 4 : 8;
        }

        static std::vector<uint8_t> encoding(const bool maximum, const uint8_t pp, const int family, const size_t width,
                                             const size_t destination, const size_t first, const size_t second, const bool mem = false,
                                             const uint8_t mask = 0, const bool zero = false, const bool special = false)
        {
            std::vector<uint8_t> bytes;
            if (family == 0)
            {
                if (pp != 0)
                {
                    bytes.push_back(pp == 1 ? 0x66 : 0xF2);
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
                         static_cast<uint8_t>(((~first & 15) << 3) | pp | (width == 32 ? 4 : 0))};
            }
            else
            {
                const auto length = (width == 64 ? 0x40 : 0) | (width == 32 ? 0x20 : 0);
                bytes = {
                    0x62,
                    static_cast<uint8_t>(0xF1 ^ ((destination & 8) << 4) ^ (destination & 16) ^ ((second & 8) << 2) ^ ((second & 16) << 2)),
                    static_cast<uint8_t>((pp == 0 ? 0 : 0x80) | 4 | ((~first & 15) << 3) | pp),
                    static_cast<uint8_t>(length | ((first & 16) ? 0 : 8) | mask | (zero ? 0x80 : 0) | (special ? 0x10 : 0))};
            }
            bytes.push_back(maximum ? 0x5F : 0x5D);
            bytes.push_back(static_cast<uint8_t>((mem ? 0 : 0xC0) | ((destination & 7) << 3) | (second & 7)));
            return bytes;
        }

        void expect_fault(const std::vector<uint8_t>& bytes, const int expected)
        {
            const auto address = load(bytes);
            const auto original = emu->reg<vector>(zmm(9));
            emu->start(1);
            EXPECT_EQ(interrupt, expected);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address);
            EXPECT_EQ(emu->reg<vector>(zmm(9)), original);
        }
    };

    TEST_F(IcicleMinMax, AllThirtyFormsAndTheirUpperRegisterRules)
    {
        for (const bool maximum : {false, true})
        {
            for (const uint8_t pp : std::array<uint8_t, 3>{0, 1, 3})
            {
                for (const int family : {0, 1, 2})
                {
                    for (const size_t width : {16u, 32u, 64u})
                    {
                        if ((family == 0 && width != 16) || (family == 1 && width == 64) || (pp == 3 && width != 16))
                        {
                            continue;
                        }
                        SCOPED_TRACE(maximum);
                        SCOPED_TRACE(pp);
                        SCOPED_TRACE(family);
                        SCOPED_TRACE(width);
                        const auto size = element(pp);
                        const auto first = fill(size, two(size));
                        const auto second = fill(size, one(size));
                        const size_t target = std::array<size_t, 3>{1, 9, 25}.at(family);
                        const size_t other = family == 2 ? 31 : 15;
                        vector sentinel{};
                        sentinel.fill(0x5A);
                        emu->reg<vector>(zmm(target), sentinel);
                        emu->reg<vector>(zmm(1), first);
                        emu->reg<vector>(zmm(other), second);
                        const auto bytes = encoding(maximum, pp, family, width, target, 1, other);
                        const auto address = load(bytes);
                        emu->reg(x86_register::eflags, 0xCD5u);
                        const auto flags = emu->reg<uint32_t>(x86_register::eflags);
                        emu->start(1);
                        vector expected = family == 0 ? first : vector{};
                        const auto selected = maximum ? first : second;
                        std::copy_n(selected.begin(), pp == 3 ? 8 : width, expected.begin());
                        if (pp == 3 && family != 0)
                        {
                            std::copy(first.begin() + 8, first.begin() + 16, expected.begin() + 8);
                        }
                        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
                        EXPECT_EQ(interrupt, 0);
                        EXPECT_EQ(emu->reg<vector>(zmm(target)), expected);
                        EXPECT_EQ(emu->reg<vector>(zmm(other)), second);
                        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), flags);
                        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1F80u);
                    }
                }
            }
        }
    }

    TEST_F(IcicleMinMax, CapturedMinpsWithExtendedDestination)
    {
        const auto address = load({0x44, 0x0F, 0x5D, 0xE9});
        emu->reg<vector>(zmm(13), fill(4, one(4)));
        emu->reg<vector>(zmm(1), fill(4, one(4)));
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + 4);
        EXPECT_EQ(emu->reg<vector>(zmm(13)), fill(4, one(4)));
    }

    TEST_F(IcicleMinMax, MasksMergeOrZeroAndScalarUpperHalfComesFromFirstSource)
    {
        for (const bool maximum : {false, true})
        {
            for (const uint8_t pp : std::array<uint8_t, 3>{0, 1, 3})
            {
                for (const bool zero : {false, true})
                {
                    for (const uint64_t mask : {0ULL, 1ULL, 0xAAAAULL, 0xFFFFULL})
                    {
                        const auto size = element(pp);
                        const auto width = pp == 3 ? 16u : 64u;
                        const auto first = fill(size, two(size));
                        const auto second = fill(size, one(size));
                        vector sentinel{};
                        sentinel.fill(0x55);
                        emu->reg<vector>(zmm(25), sentinel);
                        emu->reg<vector>(zmm(18), first);
                        emu->reg<vector>(zmm(31), second);
                        emu->reg(x86_register::k3, mask);
                        load(encoding(maximum, pp, 2, width, 25, 18, 31, false, 3, zero));
                        emu->start(1);
                        auto expected = sentinel;
                        for (size_t lane = 0; lane < (pp == 3 ? 1 : width / size); ++lane)
                        {
                            for (size_t byte = 0; byte < size; ++byte)
                            {
                                if (mask & (1ULL << lane))
                                {
                                    expected[lane * size + byte] = (maximum ? first : second)[lane * size + byte];
                                }
                                else if (zero)
                                {
                                    expected[lane * size + byte] = 0;
                                }
                            }
                        }
                        if (pp == 3)
                        {
                            std::copy(first.begin() + 8, first.begin() + 16, expected.begin() + 8);
                            std::fill(expected.begin() + 16, expected.end(), 0);
                        }
                        EXPECT_EQ(emu->reg<vector>(zmm(25)), expected);
                        EXPECT_EQ(interrupt, 0);
                    }
                }
            }
        }
    }

    TEST_F(IcicleMinMax, NanSignedZeroDenormalAndFtzDazForAllFamilies)
    {
        for (const bool maximum : {false, true})
        {
            for (const uint8_t pp : std::array<uint8_t, 3>{0, 1, 3})
            {
                for (const int family : {0, 1, 2})
                {
                    const auto size = element(pp);
                    const uint64_t sign = 1ULL << (size * 8 - 1);
                    const uint64_t infinity = size == 4 ? 0x7F800000ULL : 0x7FF0000000000000ULL;
                    const uint64_t quiet = size == 4 ? 0x400000ULL : 0x8000000000000ULL;

                    struct example
                    {
                        uint64_t first;
                        uint64_t second;
                        uint64_t expected;
                        uint32_t flags;
                        bool daz;
                    };

                    const std::array cases{
                        example{.first = 0, .second = sign, .expected = sign, .flags = 0, .daz = false},
                        example{.first = infinity | quiet | 13, .second = one(size), .expected = one(size), .flags = 1, .daz = false},
                        example{.first = one(size), .second = infinity | 15, .expected = infinity | 15, .flags = 1, .daz = false},
                        example{.first = 1, .second = 0, .expected = maximum ? 1ULL : 0ULL, .flags = 2, .daz = false},
                        example{.first = 1, .second = sign | 1, .expected = sign, .flags = 0, .daz = true}};
                    for (const auto& item : cases)
                    {
                        const auto target = family == 0 ? 1 : 9;
                        emu->reg<vector>(zmm(1), fill(size, item.first));
                        emu->reg<vector>(zmm(2), fill(size, item.second));
                        const auto bytes = encoding(maximum, pp, family, 16, target, 1, 2);
                        const auto address = load(bytes);
                        const uint32_t control = 0x9F80u | (item.daz ? 0x40u : 0);
                        emu->reg(x86_register::mxcsr, control);
                        emu->start(1);
                        const auto result = emu->reg<vector>(zmm(target));
                        const auto expected = fill(size, item.expected);
                        EXPECT_TRUE(std::equal(expected.begin(), expected.begin() + size, result.begin()));
                        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
                        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), control | item.flags);
                        EXPECT_EQ(interrupt, 0);
                    }
                }
            }
        }
    }

    TEST_F(IcicleMinMax, UnmaskedFaultsPreserveDestinationAndSaePreservesMxcsr)
    {
        for (const bool maximum : {false, true})
        {
            for (const uint8_t pp : std::array<uint8_t, 3>{0, 1, 3})
            {
                const auto size = element(pp);
                const uint64_t nan = size == 4 ? 0x7FC12345ULL : 0x7FF8123456789ABCULL;
                for (const uint64_t value : {1ULL, nan})
                {
                    for (const bool enabled : {false, true})
                    {
                        emu->reg<vector>(zmm(1), fill(size, value));
                        emu->reg<vector>(zmm(2), fill(size, one(size)));
                        emu->reg<vector>(zmm(9), fill(size, two(size)));
                        emu->reg(x86_register::mxcsr, 0x1E00u);
                        emu->reg(x86_register::cr4, enabled ? 0x40620ULL : 0x40220ULL);
                        expect_fault(encoding(maximum, pp, 2, pp == 3 ? 16 : 64, 9, 1, 2), enabled ? 19 : 6);
                        emu->reg(x86_register::mxcsr, 0x1E00u);
                        const auto bytes = encoding(maximum, pp, 2, pp == 3 ? 16 : 64, 9, 1, 2, false, 0, false, true);
                        const auto address = load(bytes);
                        emu->start(1);
                        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
                        EXPECT_EQ(interrupt, 0);
                        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1E00u);
                    }
                }
                emu->reg(x86_register::cr4, 0x40620ULL);
            }
        }
    }

    TEST_F(IcicleMinMax, RegisterAliasesAndTwoByteVex)
    {
        for (const bool maximum : {false, true})
        {
            for (const uint8_t pp : std::array<uint8_t, 3>{0, 1, 3})
            {
                const auto size = element(pp);
                emu->reg<vector>(zmm(1), fill(size, two(size)));
                emu->reg<vector>(zmm(2), fill(size, one(size)));
                auto bytes = encoding(maximum, pp, 1, 16, 2, 1, 2);
                bytes.erase(bytes.begin() + 1);
                bytes[0] = 0xC5;
                bytes[1] |= 0x80;
                const auto address = load(bytes);
                emu->start(1);
                auto expected = fill(size, maximum ? two(size) : one(size));
                if (pp == 3)
                {
                    const auto first = fill(size, two(size));
                    std::copy(first.begin() + 8, first.begin() + 16, expected.begin() + 8);
                }
                std::fill(expected.begin() + 16, expected.end(), 0);
                EXPECT_EQ(emu->reg<vector>(zmm(2)), expected);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
            }
        }
    }

    TEST_F(IcicleMinMax, FullMemoryAndScalarAccessWidths)
    {
        for (const bool maximum : {false, true})
        {
            for (const uint8_t pp : std::array<uint8_t, 3>{0, 1, 3})
            {
                for (const int family : {0, 1, 2})
                {
                    const auto size = element(pp);
                    const size_t width = pp == 3 ? 16 : (16ULL << family);
                    const size_t bytes_read = pp == 3 ? 8 : width;
                    const auto address = data + 0x2000 - bytes_read;
                    const auto source = fill(size, one(size));
                    emu->write_memory(address, source.data(), bytes_read);
                    emu->reg(x86_register::rbx, address);
                    emu->reg<vector>(zmm(1), fill(size, two(size)));
                    const auto target = family == 0 ? 1 : 9;
                    const auto bytes = encoding(maximum, pp, family, width, target, 1, 3, true);
                    const auto ip = load(bytes);
                    emu->start(1);
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
                    const auto actual = emu->reg<vector>(zmm(target));
                    const auto expected = fill(size, maximum ? two(size) : one(size));
                    EXPECT_TRUE(std::equal(expected.begin(), expected.begin() + bytes_read, actual.begin()));
                    EXPECT_EQ(interrupt, 0);
                }
            }
        }
    }

    TEST_F(IcicleMinMax, BroadcastReadsOneElementAndDisp8ScalesWithTuple)
    {
        for (const bool maximum : {false, true})
        {
            for (const uint8_t pp : std::array<uint8_t, 2>{0, 1})
            {
                for (const size_t width : {16u, 32u, 64u})
                {
                    const auto size = element(pp);
                    const auto address = data + 0x2000 - size;
                    const auto source = fill(size, one(size));
                    emu->write_memory(address, source.data(), size);
                    emu->reg(x86_register::rbx, address - 2 * size);
                    emu->reg<vector>(zmm(1), fill(size, two(size)));
                    auto bytes = encoding(maximum, pp, 2, width, 9, 1, 3, true, 0, false, true);
                    bytes.back() |= 0x40;
                    bytes.push_back(2);
                    const auto ip = load(bytes);
                    emu->start(1);
                    auto expected = fill(size, maximum ? two(size) : one(size));
                    std::fill(expected.begin() + static_cast<ptrdiff_t>(width), expected.end(), 0);
                    EXPECT_EQ(emu->reg<vector>(zmm(9)), expected);
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
                    EXPECT_EQ(interrupt, 0);
                }
            }
        }
    }

    TEST_F(IcicleMinMax, MaskedOffMemoryAndFpLanesDoNotFault)
    {
        for (const uint8_t pp : std::array<uint8_t, 3>{0, 1, 3})
        {
            const auto size = element(pp);
            emu->reg(x86_register::rbx, 0x800000000001ULL);
            emu->reg(x86_register::k1, 0ULL);
            emu->reg(x86_register::mxcsr, 0u);
            emu->reg<vector>(zmm(1), fill(size, UINT64_MAX));
            auto bytes = encoding(false, pp, 2, pp == 3 ? 16 : 64, 9, 1, 3, true, 1, true);
            const auto ip = load(bytes);
            emu->start(1);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
            EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0u);
            EXPECT_EQ(interrupt, 0);
        }
    }

    TEST_F(IcicleMinMax, PartialMasksSuppressUnreadablePagesAndFaultsDoNotCommit)
    {
        const auto address = data + 0x1FFC;
        emu->write_memory(address, uint32_t{0x3F800000});
        emu->reg(x86_register::rbx, address);
        emu->reg<vector>(zmm(1), fill(4, two(4)));
        emu->reg<vector>(zmm(9), fill(4, two(4)));
        emu->reg(x86_register::k1, 1ULL);
        const auto bytes = encoding(false, 0, 2, 64, 9, 1, 3, true, 1);
        auto ip = load(bytes);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
        const auto before = emu->reg<vector>(zmm(9));
        uint64_t fault{};
        emu->hook_memory_violation([&](cpu_interface&, const uint64_t location, size_t, memory_operation, memory_violation_type) {
            fault = location;
            return memory_violation_continuation::stop;
        });
        emu->reg(x86_register::k1, 3ULL);
        ip = load(bytes);
        EXPECT_THROW(emu->start(1), std::runtime_error);
        EXPECT_EQ(fault, address + 4);
        EXPECT_EQ(emu->reg<vector>(zmm(9)), before);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip);
    }

    TEST_F(IcicleMinMax, AlignmentAndNoncanonicalAddresses)
    {
        emu->reg(x86_register::rbx, data + 1);
        expect_fault(encoding(false, 0, 0, 16, 9, 9, 3, true), 13);
        emu->write_memory(data + 1, fill(4, one(4)));
        emu->reg<vector>(zmm(1), fill(4, two(4)));
        const auto bytes = encoding(false, 0, 1, 16, 9, 1, 3, true);
        const auto ip = load(bytes);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
        emu->reg(x86_register::rbx, 0x800000000000ULL);
        expect_fault(bytes, 13);
        auto stack = bytes;
        stack.insert(stack.begin(), 0x36);
        expect_fault(stack, 12);
        emu->reg(x86_register::rbx, data + 1);
        emu->reg(x86_register::cr0, 0x80050033ULL);
        emu->reg(x86_register::eflags, 0x40000u);
        emu->reg(x86_register::cs, uint16_t{0x33});
        expect_fault(encoding(false, 3, 1, 16, 9, 1, 3, true), 17);
        expect_fault(encoding(false, 0, 2, 64, 9, 1, 3, true, 0, false, true), 17);
    }

    TEST_F(IcicleMinMax, StateAndIllegalEncodings)
    {
        for (const int family : {0, 1, 2})
        {
            const auto bytes = encoding(false, 0, family, 16, 9, 1, 2);
            auto locked = bytes;
            locked.insert(locked.begin(), 0xF0);
            expect_fault(locked, 6);
            emu->reg(x86_register::cr0, 0x8001003BULL);
            expect_fault(bytes, 7);
            emu->reg(x86_register::cr0, 0x80010033ULL);
            if (family == 0)
            {
                emu->reg(x86_register::cr0, 0x80010037ULL);
                expect_fault(bytes, 6);
                emu->reg(x86_register::cr0, 0x80010033ULL);
            }
            else
            {
                xcr0(3);
                expect_fault(bytes, 6);
                xcr0(0xE7);
                for (const uint8_t prefix : std::array<uint8_t, 4>{0x48, 0x66, 0xF2, 0xF3})
                {
                    auto invalid = bytes;
                    invalid.insert(invalid.begin(), prefix);
                    expect_fault(invalid, 6);
                }
            }
        }
        auto invalid = encoding(false, 0, 2, 64, 9, 1, 2, false, 0, true);
        expect_fault(invalid, 6);
        invalid = encoding(false, 0, 2, 64, 9, 1, 2);
        invalid[3] |= 0x60;
        expect_fault(invalid, 6);
        invalid = encoding(false, 0, 2, 64, 9, 1, 2);
        invalid[2] ^= 0x80;
        expect_fault(invalid, 6);
        invalid = encoding(false, 3, 2, 16, 9, 1, 3, true, 0, false, true);
        expect_fault(invalid, 6);
    }

    TEST_F(IcicleMinMax, ScalarLigAndPackedSaeIgnoreLengthBits)
    {
        for (const bool maximum : {false, true})
        {
            for (const int family : {1, 2})
            {
                for (const uint8_t length : std::array<uint8_t, 4>{0, 1, 2, 3})
                {
                    if (family == 1 && length > 1)
                    {
                        continue;
                    }
                    emu->reg<vector>(zmm(18), fill(8, two(8)));
                    emu->reg<vector>(zmm(1), fill(8, two(8)));
                    emu->reg<vector>(zmm(2), fill(8, one(8)));
                    auto bytes = encoding(maximum, 3, family, 16, 9, family == 2 ? 18 : 1, 2);
                    if (family == 1)
                    {
                        bytes[2] |= length << 2;
                    }
                    else
                    {
                        bytes[3] |= length << 5;
                    }
                    const auto ip = load(bytes);
                    emu->start(1);
                    auto expected = fill(8, maximum ? two(8) : one(8));
                    const auto first = fill(8, two(8));
                    std::copy(first.begin() + 8, first.begin() + 16, expected.begin() + 8);
                    std::fill(expected.begin() + 16, expected.end(), 0);
                    EXPECT_EQ(emu->reg<vector>(zmm(9)), expected);
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
                    EXPECT_EQ(interrupt, 0);
                }
            }
            for (const uint8_t pp : std::array<uint8_t, 3>{0, 1, 3})
            {
                for (const uint8_t length : std::array<uint8_t, 4>{0, 1, 2, 3})
                {
                    const auto size = element(pp);
                    emu->reg<vector>(zmm(18), fill(size, two(size)));
                    emu->reg<vector>(zmm(31), fill(size, one(size)));
                    emu->reg(x86_register::mxcsr, 0x1E35u);
                    auto bytes = encoding(maximum, pp, 2, 16, 25, 18, 31, false, 0, false, true);
                    bytes[3] |= length << 5;
                    const auto ip = load(bytes);
                    emu->start(1);
                    auto expected = fill(size, maximum ? two(size) : one(size));
                    if (pp == 3)
                    {
                        const auto first = fill(size, two(size));
                        std::copy(first.begin() + 8, first.begin() + 16, expected.begin() + 8);
                        std::fill(expected.begin() + 16, expected.end(), 0);
                    }
                    EXPECT_EQ(emu->reg<vector>(zmm(25)), expected);
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
                    EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1E35u);
                    EXPECT_EQ(interrupt, 0);
                }
            }
        }
    }

    TEST_F(IcicleMinMax, RipRelativeAndFullVectorCompressedDisplacement)
    {
        for (const bool maximum : {false, true})
        {
            for (const uint8_t pp : std::array<uint8_t, 3>{0, 1, 3})
            {
                const auto size = element(pp);
                const auto source = fill(size, one(size));
                emu->write_memory(data, source);
                for (const bool relative : {false, true})
                {
                    for (const size_t width : {16u, 32u, 64u})
                    {
                        if (pp == 3 && width != 16)
                        {
                            continue;
                        }
                        emu->reg<vector>(zmm(18), fill(size, two(size)));
                        auto bytes = encoding(maximum, pp, 2, width, 25, 18, relative ? 5 : 3, true);
                        if (relative)
                        {
                            const auto displacement = static_cast<uint32_t>(data - (code + cursor + bytes.size() + 4));
                            for (size_t byte = 0; byte < 4; ++byte)
                            {
                                bytes.push_back(static_cast<uint8_t>(displacement >> (byte * 8)));
                            }
                        }
                        else
                        {
                            emu->reg(x86_register::rbx, data + 2 * (pp == 3 ? 8 : width));
                            bytes.back() |= 0x40;
                            bytes.push_back(0xFE);
                        }
                        const auto ip = load(bytes);
                        emu->start(1);
                        auto expected = fill(size, maximum ? two(size) : one(size));
                        if (pp == 3)
                        {
                            const auto first = fill(size, two(size));
                            std::copy(first.begin() + 8, first.begin() + 16, expected.begin() + 8);
                        }
                        std::fill(expected.begin() + static_cast<ptrdiff_t>(width), expected.end(), 0);
                        EXPECT_EQ(emu->reg<vector>(zmm(25)), expected);
                        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + bytes.size());
                        EXPECT_EQ(interrupt, 0);
                    }
                }
            }
        }
    }

}
