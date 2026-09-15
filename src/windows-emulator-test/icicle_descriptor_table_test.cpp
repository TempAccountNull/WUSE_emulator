#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <segment_utils.hpp>

#include <array>
#include <cstring>

namespace sogen::test
{
    namespace
    {
        std::array<uint8_t, 12> table_register(const uint64_t base, const uint32_t limit)
        {
            std::array<uint8_t, 12> bytes{};
            std::memcpy(bytes.data(), &limit, sizeof(limit));
            std::memcpy(bytes.data() + sizeof(limit), &base, sizeof(base));
            return bytes;
        }
    }

    TEST(IcicleDescriptorTable, ReadsIndependentGdtAndIdt)
    {
        const auto emu = create_x86_64_emulator(backend_type::icicle);
        constexpr uint64_t gdt_base = 0x12345678ABCDE000;
        constexpr uint64_t idt_base = 0x56789ABCDEFFF000;
        constexpr uint32_t gdt_limit = 0x1234;
        constexpr uint32_t idt_limit = 0x5678;
        emu->load_gdt(gdt_base, gdt_limit);
        const auto idt = table_register(idt_base, idt_limit);
        ASSERT_EQ(emu->write_register(x86_register::idtr, idt.data(), idt.size()), idt.size());

        cpu_interface::descriptor_table_register table{};
        ASSERT_TRUE(emu->read_descriptor_table(static_cast<int>(x86_register::gdtr), table));
        EXPECT_EQ(table.base, gdt_base);
        EXPECT_EQ(table.limit, gdt_limit);
        ASSERT_TRUE(emu->read_descriptor_table(static_cast<int>(x86_register::idtr), table));
        EXPECT_EQ(table.base, idt_base);
        EXPECT_EQ(table.limit, idt_limit);
        EXPECT_FALSE(emu->read_descriptor_table(static_cast<int>(x86_register::rax), table));
        EXPECT_EQ(table.base, idt_base);
        EXPECT_EQ(table.limit, idt_limit);
    }

    TEST(IcicleDescriptorTable, RawReadPreservesFullValueAndCallerBounds)
    {
        const auto emu = create_x86_64_emulator(backend_type::icicle);
        const auto expected = table_register(0xFEDCBA9876543210, 0x7654);
        for (const auto reg : {x86_register::gdtr, x86_register::idtr})
        {
            ASSERT_EQ(emu->write_register(reg, expected.data(), expected.size()), expected.size());
            for (size_t size = 0; size <= expected.size(); ++size)
            {
                std::array<uint8_t, 14> buffer{};
                buffer.fill(0xA5);
                ASSERT_EQ(emu->read_register(reg, buffer.data() + 1, size), size ? expected.size() : 1U);
                EXPECT_EQ(buffer.front(), 0xA5);
                for (size_t index = 0; index < size; ++index)
                {
                    EXPECT_EQ(buffer[index + 1], expected[index]);
                }
                for (size_t index = size + 1; index < buffer.size(); ++index)
                {
                    EXPECT_EQ(buffer[index], 0xA5);
                }
            }
        }
    }

    TEST(IcicleDescriptorTable, ResolvesGuestCodeSegmentBitness)
    {
        const auto emu = create_x86_64_emulator(backend_type::icicle);
        memory_manager memory{*emu};
        const auto gdt = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(gdt, 0U);
        emu->write_memory<uint64_t>(gdt + 0x20, 0x00CFFB000000FFFF);
        emu->write_memory<uint64_t>(gdt + 0x30, 0x00AFFB000000FFFF);
        emu->write_memory<uint64_t>(gdt + 0x40, 0x00009B000000FFFF);
        emu->load_gdt(gdt, 0x47);
        EXPECT_EQ(segment_utils::get_segment_bitness(*emu, 0x23), segment_utils::segment_bitness::bit32);
        EXPECT_EQ(segment_utils::get_segment_bitness(*emu, 0x33), segment_utils::segment_bitness::bit64);
        EXPECT_EQ(segment_utils::get_segment_bitness(*emu, 0x40), segment_utils::segment_bitness::bit16);
        EXPECT_FALSE(segment_utils::get_segment_bitness(*emu, 0));
        EXPECT_FALSE(segment_utils::get_segment_bitness(*emu, 0x48));
    }
}
