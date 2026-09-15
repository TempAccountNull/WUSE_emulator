#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>

#include <array>
#include <bit>
#include <limits>
#include <utility>
#include <vector>

namespace sogen::test
{
    class IcicleAddressSize : public testing::Test
    {
      protected:
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        static constexpr uint64_t code_base = 0x180010000;
        static constexpr uint64_t data_base = 0x80020000;
        size_t cursor{};

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            ASSERT_TRUE(memory->allocate_memory(code_base, 0x1000, memory_permission::all));
            ASSERT_TRUE(memory->allocate_memory(data_base, 0x1000, memory_permission::read_write));
            emu->reg(x86_register::eflags, 0xCD5U);
        }

        uint64_t load(std::vector<uint8_t> bytes)
        {
            const auto address = code_base + cursor;
            cursor += 32;
            bytes.push_back(0x90);
            emu->write_memory(address, bytes.data(), bytes.size());
            emu->reg(x86_register::rip, address);
            return address;
        }

        void expect_lea(const std::vector<uint8_t>& bytes, const uint64_t expected)
        {
            const auto address = load(bytes);
            const auto flags = emu->reg<uint32_t>(x86_register::eflags);
            const auto base = emu->reg<uint64_t>(x86_register::rbx);
            emu->start(1);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), expected);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
            EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), flags);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rbx), base);
        }
    };

    TEST_F(IcicleAddressSize, BaseDisplacementsZeroExtendAndWrap)
    {
        for (const auto base : {0x80020000ULL, 0xFFFFFFFFULL, 0x1234567880020000ULL})
        {
            emu->reg(x86_register::rbx, base);
            expect_lea({0x67, 0x48, 0x8D, 0x03}, static_cast<uint32_t>(base));
            expect_lea({0x67, 0x48, 0x8D, 0x43, 0xF0}, static_cast<uint32_t>(base - 16));
            expect_lea({0x67, 0x48, 0x8D, 0x83, 0x20, 0, 0, 0}, static_cast<uint32_t>(base + 32));
        }
    }

    TEST_F(IcicleAddressSize, ScaledExtendedRegistersWrapAt32Bits)
    {
        emu->reg(x86_register::r12, 0x1234567880020000ULL);
        emu->reg(x86_register::r9, 0xABCD000000000004ULL);
        expect_lea({0x67, 0x4B, 0x8D, 0x04, 0x8C}, 0x80020010);
        emu->reg(x86_register::r12, 0xFFFFFFFFFFFFFFF0ULL);
        emu->reg(x86_register::r9, 8ULL);
        expect_lea({0x67, 0x4B, 0x8D, 0x04, 0x8C}, 0x10);
        expect_lea({0x67, 0x48, 0x8D, 0x04, 0x25, 0, 0, 2, 0x80}, data_base);
    }

    TEST_F(IcicleAddressSize, InstructionRelativeAddressUsesTruncatedNextRip)
    {
        const std::vector<uint8_t> bytes{0x67, 0x48, 0x8D, 0x05, 0, 0, 1, 0};
        expect_lea(bytes, static_cast<uint32_t>(code_base + cursor + bytes.size() + 0x10000));
        const std::vector<uint8_t> negative{0x67, 0x48, 0x8D, 0x05, 0, 0, 0xFF, 0xFF};
        expect_lea(negative, static_cast<uint32_t>(code_base + cursor + negative.size() - 0x10000));
    }

    TEST_F(IcicleAddressSize, Normal64BitAddressingRetainsHighBits)
    {
        emu->reg(x86_register::rbx, 0x1234567880020000ULL);
        expect_lea({0x48, 0x8D, 0x03}, 0x1234567880020000ULL);
        const std::vector<uint8_t> bytes{0x48, 0x8D, 0x05, 0, 0, 1, 0};
        expect_lea(bytes, code_base + cursor + bytes.size() + 0x10000);
    }

    TEST_F(IcicleAddressSize, LoadsAndStoresUseZeroExtendedAddress)
    {
        emu->reg(x86_register::rbx, 0xAAAA000080020000ULL);
        emu->write_memory<uint32_t>(data_base, 0xC1234567);
        const auto read = load({0x67, 0x8B, 0x03});
        ASSERT_NO_THROW(emu->start(1));
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 0xC1234567ULL);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), read + 3);
        const auto write = load({0x67, 0x89, 0x43, 4});
        ASSERT_NO_THROW(emu->start(1));
        EXPECT_EQ(emu->read_memory<uint32_t>(data_base + 4), 0xC1234567U);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), write + 4);
    }

    TEST_F(IcicleAddressSize, SegmentBaseIsAddedAfterAddressTruncation)
    {
        constexpr uint64_t segment_base = 0x100000000;
        ASSERT_TRUE(memory->allocate_memory(segment_base + data_base, 0x1000, memory_permission::read_write));
        emu->write_memory<uint32_t>(segment_base + data_base, 0xC1234567);
        emu->reg(x86_register::rbx, 0xAAAA000080020000ULL);
        for (const auto prefix : {0x64, 0x65})
        {
            emu->reg(prefix == 0x64 ? x86_register::fs_base : x86_register::gs_base, segment_base);
            const auto address = load({static_cast<uint8_t>(prefix), 0x67, 0x8B, 0x03});
            ASSERT_NO_THROW(emu->start(1));
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 0xC1234567ULL);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + 4);
        }
    }

    TEST_F(IcicleAddressSize, UnmappedLoadReportsZeroExtendedAddressAndPreservesDestination)
    {
        uint64_t fault{};
        emu->hook_memory_violation([&](cpu_interface&, const uint64_t address, size_t, memory_operation, memory_violation_type) {
            fault = address;
            return memory_violation_continuation::stop;
        });
        emu->reg(x86_register::rbx, 0xABCD000080030000ULL);
        emu->reg(x86_register::rax, 0x123456789ABCDEF0ULL);
        const auto address = load({0x67, 0x8B, 0x03});
        const auto flags = emu->reg<uint32_t>(x86_register::eflags);
        EXPECT_THROW(emu->start(1), std::runtime_error);
        EXPECT_EQ(fault, 0x80030000ULL);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 0x123456789ABCDEF0ULL);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), flags);
    }

    TEST_F(IcicleAddressSize, ExtendedIndexSurvivesEverySibDisplacement)
    {
        emu->reg(x86_register::rbx, 0xABCD000080020000ULL);
        emu->reg(x86_register::r12, 0xABCD000000000004ULL);
        expect_lea({0x67, 0x4A, 0x8D, 0x04, 0xA3}, data_base + 16);
        expect_lea({0x67, 0x4A, 0x8D, 0x44, 0xA3, 0}, data_base + 16);
        expect_lea({0x67, 0x4A, 0x8D, 0x44, 0xA3, 0xFC}, data_base + 12);
        expect_lea({0x67, 0x4A, 0x8D, 0x84, 0xA3, 0, 0, 0, 0}, data_base + 16);
        expect_lea({0x67, 0x4A, 0x8D, 0x84, 0xA3, 0xFC, 0xFF, 0xFF, 0xFF}, data_base + 12);
        expect_lea({0x67, 0x4A, 0x8D, 0x04, 0xA5, 0, 0, 2, 0x80}, data_base + 16);
        emu->write_memory<uint32_t>(data_base + 16, 0xB1234567);
        const auto address = load({0x67, 0x42, 0x8B, 0x04, 0xA3});
        ASSERT_NO_THROW(emu->start(1));
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 0xB1234567ULL);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + 5);
    }

    TEST_F(IcicleAddressSize, InstructionRelativeCompareIncludesTrailingImmediate)
    {
        const auto displacement = static_cast<uint32_t>(data_base - (code_base + cursor + 8));
        const std::vector<uint8_t> bytes{0x67,
                                         0x80,
                                         0x3D,
                                         static_cast<uint8_t>(displacement),
                                         static_cast<uint8_t>(displacement >> 8),
                                         static_cast<uint8_t>(displacement >> 16),
                                         static_cast<uint8_t>(displacement >> 24),
                                         0x5A};
        emu->write_memory<uint8_t>(data_base, 0x5A);
        emu->reg(x86_register::rax, 0x123456789ABCDEF0ULL);
        const auto address = load(bytes);
        ASSERT_NO_THROW(emu->start(1));
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags) & 0x8D5U, 0x44U);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 0x123456789ABCDEF0ULL);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
        EXPECT_EQ(emu->read_memory<uint8_t>(data_base), 0x5A);
    }

    TEST_F(IcicleAddressSize, InstructionRelativeOffsetWrapsAndIgnoresRexBase)
    {
        expect_lea({0x67, 0x48, 0x8D, 0x05, 0, 0, 0xFF, 0x7F}, 8);
        emu->reg(x86_register::r13, 0xAAAAAAAAAAAAAAAAULL);
        const std::vector<uint8_t> bytes{0x67, 0x49, 0x8D, 0x05, 0, 0, 1, 0};
        expect_lea(bytes, static_cast<uint32_t>(code_base + cursor + bytes.size() + 0x10000));
    }

    TEST_F(IcicleAddressSize, NoBaseSibDependsOnAddressWidthAndIgnoresRexBase)
    {
        emu->reg(x86_register::r13, 0xAAAAAAAAAAAAAAAAULL);
        for (const auto rex : {0x48, 0x49})
        {
            expect_lea({static_cast<uint8_t>(rex), 0x8D, 0x04, 0x25, 0, 0, 2, 0x80}, 0xFFFFFFFF80020000ULL);
            expect_lea({0x67, static_cast<uint8_t>(rex), 0x8D, 0x04, 0x25, 0, 0, 2, 0x80}, data_base);
        }
    }

    TEST_F(IcicleAddressSize, LeaUsesDestinationWidthAndIgnoresSegmentBase)
    {
        constexpr uint64_t sentinel = 0xFEDCBA9876543210ULL;
        for (const auto segment : {0x64, 0x65})
        {
            emu->reg(segment == 0x64 ? x86_register::fs_base : x86_register::gs_base, 0x100000000ULL);
            const std::vector<uint8_t> word{static_cast<uint8_t>(segment), 0x67, 0x66, 0x8D, 0x05, 0, 0, 0, 0};
            emu->reg(x86_register::rax, sentinel);
            expect_lea(word, (sentinel & ~0xFFFFULL) | static_cast<uint16_t>(code_base + cursor + word.size()));
            const std::vector<uint8_t> dword{static_cast<uint8_t>(segment), 0x67, 0x8D, 0x05, 0, 0, 0, 0};
            emu->reg(x86_register::rax, sentinel);
            expect_lea(dword, static_cast<uint32_t>(code_base + cursor + dword.size()));
            const std::vector<uint8_t> qword{static_cast<uint8_t>(segment), 0x67, 0x48, 0x8D, 0x05, 0, 0, 0, 0};
            emu->reg(x86_register::rax, sentinel);
            expect_lea(qword, static_cast<uint32_t>(code_base + cursor + qword.size()));
        }
    }

    TEST_F(IcicleAddressSize, UnmappedStoreReportsZeroExtendedAddressAndPreservesSource)
    {
        uint64_t fault{};
        emu->hook_memory_violation([&](cpu_interface&, const uint64_t address, size_t, memory_operation, memory_violation_type) {
            fault = address;
            return memory_violation_continuation::stop;
        });
        emu->reg(x86_register::rbx, 0xABCD000080030000ULL);
        emu->reg(x86_register::rax, 0x123456789ABCDEF0ULL);
        const auto address = load({0x67, 0x89, 0x03});
        const auto flags = emu->reg<uint32_t>(x86_register::eflags);
        EXPECT_THROW(emu->start(1), std::runtime_error);
        EXPECT_EQ(fault, 0x80030000ULL);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 0x123456789ABCDEF0ULL);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), flags);
    }

    TEST_F(IcicleAddressSize, CompareAndSubtractUpdateAllArithmeticFlags)
    {
        for (const auto width : {8, 16, 32, 64})
        {
            const uint64_t mask = std::numeric_limits<uint64_t>::max() >> (64 - width);
            const uint64_t sign = 1ULL << (width - 1);
            const std::array<std::pair<uint64_t, uint64_t>, 6> operands{
                {{0x5A, 0x5A}, {0x10, 1}, {0, 1}, {sign - 1, mask}, {sign, 1}, {0x1F, 0x10}}};
            for (const auto subtract : {false, true})
            {
                for (const auto& [left, right] : operands)
                {
                    SCOPED_TRACE(testing::Message() << "width=" << width << " sub=" << subtract << " left=" << left << " right=" << right);
                    std::vector<uint8_t> bytes;
                    if (width == 16)
                    {
                        bytes.push_back(0x66);
                    }
                    else if (width == 64)
                    {
                        bytes.push_back(0x48);
                    }
                    bytes.push_back(static_cast<uint8_t>((subtract ? 0x28 : 0x38) + (width == 8 ? 0 : 1)));
                    bytes.push_back(0xD8);
                    emu->reg(x86_register::rax, left);
                    emu->reg(x86_register::rbx, right);
                    emu->reg(x86_register::eflags, 0xCD5U);
                    const auto address = load(bytes);
                    emu->start(1);
                    const auto result = (left - right) & mask;
                    const auto overflow = ((left ^ right) & (left ^ result) & sign) != 0;
                    const uint32_t expected = (left < right ? 1U : 0U) | (std::popcount(result & 0xFF) % 2 == 0 ? 4U : 0U) |
                                              ((left & 15) < (right & 15) ? 0x10U : 0U) | (result == 0 ? 0x40U : 0U) |
                                              ((result & sign) != 0 ? 0x80U : 0U) | (overflow ? 0x800U : 0U);
                    EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags) & 0x8D5U, expected);
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), subtract ? result : left);
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rbx), right);
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
                }
            }
        }
    }
}
