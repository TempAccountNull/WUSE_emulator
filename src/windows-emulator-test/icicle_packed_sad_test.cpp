#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <array>
#include <vector>

namespace sogen::test
{
    class IciclePackedSad : public testing::Test
    {
      protected:
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};
        using wide = std::array<uint64_t, 4>;

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x2000, memory_permission::all);
            ASSERT_NE(code, 0u);
        }

        void load(const std::vector<uint8_t>& bytes, const uint64_t offset = 0)
        {
            emu->write_memory(code + offset, bytes.data(), bytes.size());
            emu->reg(x86_register::rip, code + offset);
        }
    };

    TEST_F(IciclePackedSad, CapturedGameOperandsAndUnchangedUpperYmm)
    {
        load({0x66, 0x0F, 0xF6, 0xD4, 0x90});
        const wide left{0xFFFFFF01, 0, 0x123456789ABCDEF0, 0x87654321};
        const wide right{};
        emu->reg<wide>(x86_register::ymm2, left);
        emu->reg<wide>(x86_register::ymm4, right);
        emu->reg(x86_register::eflags, 0xCD5u);
        const auto flags = emu->reg<uint32_t>(x86_register::eflags);
        emu->reg(x86_register::mxcsr, 0xFFFFu);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 4);
        EXPECT_EQ(emu->reg<wide>(x86_register::ymm2), (wide{766, 0, left[2], left[3]}));
        EXPECT_EQ(emu->reg<wide>(x86_register::ymm4), right);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), flags);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0xFFFFu);
    }

    TEST_F(IciclePackedSad, ExtendedRegistersAndAliasedSources)
    {
        load({0x66, 0x45, 0x0F, 0xF6, 0xCF, 0x90});
        emu->reg<wide>(x86_register::ymm9, wide{0, UINT64_MAX, 3, 4});
        emu->reg<wide>(x86_register::ymm15, wide{UINT64_MAX, 0, 5, 6});
        emu->start(1);
        EXPECT_EQ(emu->reg<wide>(x86_register::ymm9), (wide{2040, 2040, 3, 4}));
        load({0x66, 0x45, 0x0F, 0xF6, 0xC9, 0x90}, 0x10);
        emu->start(1);
        EXPECT_EQ(emu->reg<wide>(x86_register::ymm9), (wide{0, 0, 3, 4}));
    }

    TEST_F(IciclePackedSad, RipRelativeMemoryReadsTwoIndependentGroups)
    {
        load({0x66, 0x0F, 0xF6, 0x15, 0xF8, 0, 0, 0, 0x90});
        const std::array<uint64_t, 2> input{UINT64_MAX, 0x0101010101010101};
        emu->write_memory(code + 0x100, input.data(), sizeof(input));
        emu->reg<wide>(x86_register::ymm2, wide{0, 0, 3, 4});
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 8);
        EXPECT_EQ(emu->reg<wide>(x86_register::ymm2), (wide{2040, 8, 3, 4}));
        EXPECT_EQ((emu->read_memory<std::array<uint64_t, 2>>(code + 0x100)), input);
    }

    TEST_F(IciclePackedSad, LegacyMisalignmentDoesNotModifyDestination)
    {
        load({0x66, 0x0F, 0xF6, 0x13, 0x90});
        emu->reg(x86_register::rbx, code + 0x101);
        const wide original{1, 2, 3, 4};
        emu->reg<wide>(x86_register::ymm2, original);
        int interrupt{};
        emu->hook_interrupt([&](cpu_interface& cpu, const int value) {
            interrupt = value;
            cpu.stop();
        });
        emu->start(1);
        EXPECT_EQ(interrupt, 13);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
        EXPECT_EQ(emu->reg<wide>(x86_register::ymm2), original);
    }

    TEST_F(IciclePackedSad, UnmappedAndUnreadableOperandsDoNotModifyDestination)
    {
        uint64_t fault{};
        emu->hook_memory_violation(
            [&](cpu_interface&, const uint64_t address, size_t, const memory_operation operation, memory_violation_type) {
                fault = address;
                EXPECT_EQ(operation, memory_operation::read);
                return memory_violation_continuation::stop;
            });
        const auto unreadable = memory->allocate_memory(0x1000, memory_permission::none);
        for (const auto source : {code + 0x3000, unreadable})
        {
            load({0x66, 0x0F, 0xF6, 0x13, 0x90});
            const wide original{1, 2, 3, 4};
            emu->reg<wide>(x86_register::ymm2, original);
            emu->reg(x86_register::rbx, source);
            EXPECT_THROW(emu->start(1), std::runtime_error);
            EXPECT_EQ(fault, source);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
            EXPECT_EQ(emu->reg<wide>(x86_register::ymm2), original);
        }
    }

    TEST_F(IciclePackedSad, MmxUnsignedSumAndTagState)
    {
        load({0x0F, 0xF6, 0xD4, 0x90});
        emu->reg(x86_register::mm2, UINT64_MAX);
        emu->reg(x86_register::mm4, 0ULL);
        emu->reg(x86_register::fptag, uint16_t{0xFFFF});
        emu->reg(x86_register::fpsw, uint16_t{0x100});
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::mm2), 2040u);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fptag), 0u);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fpsw), 0x100u);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 3);
    }

    TEST_F(IciclePackedSad, VexRegisterSourcesAndUpperZeroing)
    {
        load({0x0F, 0x01, 0xD1, 0x90}, 0x40);
        emu->reg(x86_register::rax, 7ULL);
        emu->reg(x86_register::rdx, 0ULL);
        emu->reg(x86_register::rcx, 0ULL);
        emu->start(1);
        load({0xC5, 0xE9, 0xF6, 0xCC, 0x90});
        emu->reg<wide>(x86_register::ymm1, wide{1, 2, 3, 4});
        emu->reg<wide>(x86_register::ymm2, wide{0, UINT64_MAX, UINT64_MAX, 0});
        emu->reg<wide>(x86_register::ymm4, wide{UINT64_MAX, 0, 0, UINT64_MAX});
        emu->start(1);
        EXPECT_EQ(emu->reg<wide>(x86_register::ymm1), (wide{2040, 2040, 0, 0}));
        load({0xC5, 0xED, 0xF6, 0xCC, 0x90}, 0x10);
        emu->start(1);
        EXPECT_EQ(emu->reg<wide>(x86_register::ymm1), (wide{2040, 2040, 2040, 2040}));
    }

    TEST_F(IciclePackedSad, MmxAliasesTheLowSignificandAndSetsTheExponent)
    {
        using extended = std::array<uint8_t, 10>;
        load({0x0F, 0xF6, 0xD4, 0x90});
        const extended physical2{255, 255, 255, 255, 255, 255, 255, 255, 0x34, 0x12};
        const extended physical4{0, 0, 0, 0, 0, 0, 0, 0, 0x78, 0x56};
        // With TOP=7, logical ST3 and ST5 contain physical R2 and R4.
        emu->reg<extended>(x86_register::st3, physical2);
        emu->reg<extended>(x86_register::st5, physical4);
        emu->reg(x86_register::fpsw, uint16_t{0x3900});
        emu->reg(x86_register::fpcw, uint16_t{0x37F});
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::mm2), 2040u);
        EXPECT_EQ(emu->reg<extended>(x86_register::st2), (extended{0xF8, 7, 0, 0, 0, 0, 0, 0, 255, 255}));
        EXPECT_EQ(emu->reg<extended>(x86_register::st4), physical4);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fpsw), 0x100u);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fpcw), 0x37Fu);
        const auto saved = emu->save_registers();
        emu->reg(x86_register::mm2, 0ULL);
        emu->restore_registers(saved);
        EXPECT_EQ(emu->reg<extended>(x86_register::st2), (extended{0xF8, 7, 0, 0, 0, 0, 0, 0, 255, 255}));
    }

    TEST_F(IciclePackedSad, DisabledStateFaultsBeforeReadingMemory)
    {
        struct scenario
        {
            uint8_t family;
            uint64_t cr0;
            uint64_t cr4;
            uint16_t status;
            uint16_t control;
            int vector;
        };

        const std::array cases{scenario{.family = 0, .cr0 = 0x80010037, .cr4 = 0x40620, .status = 0, .control = 0x37F, .vector = 6},
                               scenario{.family = 0, .cr0 = 0x8001003B, .cr4 = 0x40620, .status = 0, .control = 0x37F, .vector = 7},
                               scenario{.family = 0, .cr0 = 0x80010033, .cr4 = 0x40620, .status = 1, .control = 0x37E, .vector = 16},
                               scenario{.family = 1, .cr0 = 0x80010037, .cr4 = 0x40620, .status = 0, .control = 0x37F, .vector = 6},
                               scenario{.family = 1, .cr0 = 0x80010033, .cr4 = 0x40420, .status = 0, .control = 0x37F, .vector = 6},
                               scenario{.family = 1, .cr0 = 0x8001003B, .cr4 = 0x40620, .status = 0, .control = 0x37F, .vector = 7},
                               scenario{.family = 2, .cr0 = 0x80010033, .cr4 = 0x40620, .status = 0, .control = 0x37F, .vector = 6}};
        size_t faults{};
        int interrupt{};
        emu->hook_interrupt([&](cpu_interface& cpu, const int value) {
            interrupt = value;
            cpu.stop();
        });
        emu->hook_memory_violation([&](cpu_interface&, uint64_t, size_t, memory_operation, memory_violation_type) {
            ++faults;
            return memory_violation_continuation::stop;
        });
        for (const auto& item : cases)
        {
            SCOPED_TRACE(item.family);
            SCOPED_TRACE(item.vector);
            const std::array<std::vector<uint8_t>, 3> encodings{std::vector<uint8_t>{0x0F, 0xF6, 0x13, 0x90},
                                                                std::vector<uint8_t>{0x66, 0x0F, 0xF6, 0x13, 0x90},
                                                                std::vector<uint8_t>{0xC5, 0xE9, 0xF6, 0x0B, 0x90}};
            load(encodings.at(item.family));
            emu->reg(x86_register::cr0, item.cr0);
            emu->reg(x86_register::cr4, item.cr4);
            emu->reg(x86_register::fpsw, item.status);
            emu->reg(x86_register::fpcw, item.control);
            emu->reg(x86_register::rbx, 0x700000000000ULL);
            const auto before = emu->save_registers();
            interrupt = 0;
            emu->start(1);
            EXPECT_EQ(interrupt, item.vector);
            EXPECT_EQ(faults, 0u);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
            EXPECT_EQ(emu->reg<uint16_t>(x86_register::fpsw), item.status);
            EXPECT_EQ(emu->save_registers(), before);
        }
    }

    TEST_F(IciclePackedSad, NoncanonicalStackAndDataAddressesSelectTheirFaults)
    {
        int interrupt{};
        emu->hook_interrupt([&](cpu_interface& cpu, const int value) {
            interrupt = value;
            cpu.stop();
        });
        for (const bool stack : {false, true})
        {
            load(stack ? std::vector<uint8_t>{0x66, 0x0F, 0xF6, 0x55, 0, 0x90} : std::vector<uint8_t>{0x66, 0x0F, 0xF6, 0x13, 0x90});
            emu->reg(x86_register::rbp, 0x800000000000ULL);
            emu->reg(x86_register::rbx, 0x800000000000ULL);
            emu->reg<wide>(x86_register::ymm2, wide{1, 2, 3, 4});
            interrupt = 0;
            emu->start(1);
            EXPECT_EQ(interrupt, stack ? 12 : 13);
            EXPECT_EQ(emu->reg<wide>(x86_register::ymm2), (wide{1, 2, 3, 4}));
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
        }
    }

    TEST_F(IciclePackedSad, MmxAlignmentCheckAndOrdinaryUnalignedReads)
    {
        load({0x0F, 0xF6, 0x13, 0x90});
        emu->reg(x86_register::rbx, code + 0x101);
        emu->write_memory<uint64_t>(code + 0x101, UINT64_MAX);
        emu->reg(x86_register::mm2, 0ULL);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::mm2), 2040u);
        emu->reg(x86_register::rip, code);
        emu->reg(x86_register::cr0, 0x80050033ULL);
        emu->reg(x86_register::cs, uint16_t{0x33});
        emu->reg(x86_register::eflags, 0x40002u);
        int interrupt{};
        emu->hook_interrupt([&](cpu_interface& cpu, const int value) {
            interrupt = value;
            cpu.stop();
        });
        emu->start(1);
        EXPECT_EQ(interrupt, 17);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::mm2), 2040u);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
    }

    TEST_F(IciclePackedSad, VexUnalignedMemoryAndPageFaultAtomicity)
    {
        load({0x0F, 0x01, 0xD1, 0x90}, 0x40);
        emu->reg(x86_register::rax, 7ULL);
        emu->reg(x86_register::rdx, 0ULL);
        emu->reg(x86_register::rcx, 0ULL);
        emu->start(1);
        for (const bool wide_source : {false, true})
        {
            load({0xC5, static_cast<uint8_t>(wide_source ? 0xED : 0xE9), 0xF6, 0x0B, 0x90});
            emu->reg<wide>(x86_register::ymm2, wide{});
            const wide input{UINT64_MAX, 0x0101010101010101, 0x0202020202020202, 0x0303030303030303};
            emu->write_memory(code + 0x101, input.data(), sizeof(input));
            emu->reg(x86_register::rbx, code + 0x101);
            emu->start(1);
            EXPECT_EQ(emu->reg<wide>(x86_register::ymm1), (wide{2040, 8, wide_source ? 16u : 0u, wide_source ? 24u : 0u}));
        }
        uint64_t fault{};
        emu->hook_memory_violation([&](cpu_interface&, const uint64_t address, size_t, memory_operation, memory_violation_type) {
            fault = address;
            return memory_violation_continuation::stop;
        });
        load({0xC5, 0xED, 0xF6, 0x0B, 0x90});
        emu->reg<wide>(x86_register::ymm1, wide{1, 2, 3, 4});
        emu->reg(x86_register::rbx, code + 0x1FF0);
        EXPECT_THROW(emu->start(1), std::runtime_error);
        EXPECT_EQ(fault, code + 0x2000);
        EXPECT_EQ(emu->reg<wide>(x86_register::ymm1), (wide{1, 2, 3, 4}));
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
    }

    TEST_F(IciclePackedSad, FlagsAboveIoplKeepTheirArchitecturalBitPositions)
    {
        load({0x9C, 0x90});
        const uint32_t flags = (3u << 12) | (1u << 14) | (15u << 18) | 0x202;
        emu->reg(x86_register::eflags, flags);
        emu->reg(x86_register::rsp, code + 0x1F00);
        emu->start(1);
        EXPECT_EQ(emu->read_memory<uint64_t>(code + 0x1EF8), flags);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), flags);
    }

}
