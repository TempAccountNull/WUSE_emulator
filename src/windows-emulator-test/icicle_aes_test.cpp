#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <array>

namespace sogen::test
{
    class IcicleAes : public testing::Test
    {
      protected:
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};
        using vector = std::array<uint32_t, 4>;
        using wide_vector = std::array<uint32_t, 8>;

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x2000, memory_permission::all);
            ASSERT_NE(code, 0u);
            emu->reg(x86_register::mxcsr, 0x1F80u);
            emu->reg(x86_register::eflags, 0x85u);
        }

        template <size_t N>
        void load(const std::array<uint8_t, N>& bytes, const uint64_t offset = 0)
        {
            emu->write_memory(code + offset, bytes.data(), bytes.size());
            emu->reg(x86_register::rip, code + offset);
        }
    };

    TEST_F(IcicleAes, CapturedKeyAssistPreservesFlagsSourceAndUpperYmm)
    {
        load(std::array<uint8_t, 7>{0x66, 0x0F, 0x3A, 0xDF, 0xC8, 1, 0x90});
        emu->reg<vector>(x86_register::xmm0, vector{});
        const wide_vector original{0, 0, 0, 0, 11, 22, 33, 44};
        emu->reg<wide_vector>(x86_register::ymm1, original);
        emu->reg(x86_register::rcx, 0xB902B6F9A0ULL);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 6);
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm0), vector{});
        EXPECT_EQ(emu->reg<wide_vector>(x86_register::ymm1), (wide_vector{0x63636363, 0x63636362, 0x63636363, 0x63636362, 11, 22, 33, 44}));
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rcx), 0xB902B6F9A0ULL);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), 0x85u);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1F80u);
    }

    TEST_F(IcicleAes, AliasedSourceSupportsEveryRoundConstant)
    {
        for (uint32_t constant = 0; constant < 256; ++constant)
        {
            load(std::array<uint8_t, 7>{0x66, 0x0F, 0x3A, 0xDF, 0xC0, static_cast<uint8_t>(constant), 0x90}, 16 * constant);
            emu->reg<vector>(x86_register::xmm0, vector{});
            emu->start(1);
            EXPECT_EQ(emu->reg<vector>(x86_register::xmm0), (vector{0x63636363, 0x63636363 ^ constant, 0x63636363, 0x63636363 ^ constant}));
        }
    }

    TEST_F(IcicleAes, VexKeyAssistClearsUpperYmm)
    {
        load(std::array<uint8_t, 7>{0xC4, 0xE3, 0x79, 0xDF, 0xC8, 0x1B, 0x90});
        emu->reg<vector>(x86_register::xmm0, vector{});
        emu->reg<wide_vector>(x86_register::ymm1, wide_vector{1, 2, 3, 4, 11, 22, 33, 44});
        emu->start(1);
        EXPECT_EQ(emu->reg<wide_vector>(x86_register::ymm1), (wide_vector{0x63636363, 0x63636378, 0x63636363, 0x63636378, 0, 0, 0, 0}));
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), 0x85u);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1F80u);
    }

    TEST_F(IcicleAes, LegacyAndVexEncryptionAndDecryptionRounds)
    {
        for (const bool vex : {false, true})
        {
            for (uint8_t opcode = 0xDC; opcode <= 0xDF; ++opcode)
            {
                const uint64_t offset = 16ULL * (opcode - 0xDC) + (vex ? 0x100 : 0);
                if (vex)
                {
                    load(std::array<uint8_t, 6>{0xC4, 0xE2, 0x69, opcode, 0xC8, 0x90}, offset);
                }
                else
                {
                    load(std::array<uint8_t, 6>{0x66, 0x0F, 0x38, opcode, 0xC8, 0x90}, offset);
                }
                emu->reg<vector>(x86_register::xmm0, vector{1, 2, 3, 4});
                emu->reg<vector>(x86_register::xmm2, vector{});
                emu->reg<wide_vector>(x86_register::ymm1, wide_vector{0, 0, 0, 0, 11, 22, 33, 44});
                emu->start(1);
                const uint32_t fill = opcode < 0xDE ? 0x63636363 : 0x52525252;
                EXPECT_EQ(emu->reg<wide_vector>(x86_register::ymm1), (wide_vector{fill ^ 1, fill ^ 2, fill ^ 3, fill ^ 4, vex ? 0u : 11u,
                                                                                  vex ? 0u : 22u, vex ? 0u : 33u, vex ? 0u : 44u}));
                EXPECT_EQ(emu->reg<vector>(x86_register::xmm0), (vector{1, 2, 3, 4}));
                EXPECT_EQ(emu->reg<vector>(x86_register::xmm2), vector{});
                EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), 0x85u);
                EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1F80u);
            }
        }
    }

    TEST_F(IcicleAes, InverseMixColumnsAndVexUpperLanes)
    {
        for (const bool vex : {false, true})
        {
            if (vex)
            {
                load(std::array<uint8_t, 6>{0xC4, 0xE2, 0x79, 0xDB, 0xC8, 0x90}, 16);
            }
            else
            {
                load(std::array<uint8_t, 6>{0x66, 0x0F, 0x38, 0xDB, 0xC8, 0x90});
            }
            emu->reg<vector>(x86_register::xmm0, vector{0xE5816604, 0, 0, 0});
            emu->reg<wide_vector>(x86_register::ymm1, wide_vector{1, 2, 3, 4, 11, 22, 33, 44});
            emu->start(1);
            EXPECT_EQ(emu->reg<wide_vector>(x86_register::ymm1),
                      (wide_vector{0x305DBFD4, 0, 0, 0, vex ? 0u : 11u, vex ? 0u : 22u, vex ? 0u : 33u, vex ? 0u : 44u}));
        }
    }

    TEST_F(IcicleAes, AlignedLegacyAndUnalignedVexMemory)
    {
        emu->hook_interrupt([&](cpu_interface& cpu, const int number) {
            ADD_FAILURE() << "Unexpected interrupt " << number;
            cpu.stop();
        });
        emu->hook_memory_violation([&](cpu_interface&, uint64_t address, size_t, memory_operation, memory_violation_type) {
            ADD_FAILURE() << "Unexpected memory fault " << std::hex << address;
            return memory_violation_continuation::stop;
        });
        for (const bool vex : {false, true})
        {
            if (vex)
            {
                load(std::array<uint8_t, 7>{0xC4, 0xE3, 0x79, 0xDF, 0x0B, 1, 0x90}, 16);
            }
            else
            {
                load(std::array<uint8_t, 7>{0x66, 0x0F, 0x3A, 0xDF, 0x0B, 1, 0x90});
            }
            const auto address = code + 0x100 + (vex ? 1 : 0);
            emu->reg(x86_register::rbx, address);
            emu->write_memory(address, vector{});
            emu->start(1);
            EXPECT_EQ(emu->reg<vector>(x86_register::xmm1), (vector{0x63636363, 0x63636362, 0x63636363, 0x63636362}));
            EXPECT_EQ(emu->read_memory<vector>(address), vector{});
        }
    }

    TEST_F(IcicleAes, EveryLegacyMemoryOperationRequiresAlignment)
    {
        int interrupt{};
        emu->hook_interrupt([&](cpu_interface& cpu, const int number) {
            interrupt = number;
            cpu.stop();
        });
        for (uint8_t index = 0; index < 6; ++index)
        {
            const auto map = static_cast<uint8_t>(index == 5 ? 0x3A : 0x38);
            const auto opcode = static_cast<uint8_t>(index == 5 ? 0xDF : 0xDB + index);
            load(std::array<uint8_t, 7>{0x66, 0x0F, map, opcode, 0x0B, 1, 0x90}, 16ULL * index);
            emu->reg(x86_register::rbx, code + 0x101);
            const wide_vector original{1, 2, 3, 4, 5, 6, 7, 8};
            emu->reg<wide_vector>(x86_register::ymm1, original);
            interrupt = 0;
            emu->start(1);
            EXPECT_EQ(interrupt, 13);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 16ULL * index);
            EXPECT_EQ(emu->reg<wide_vector>(x86_register::ymm1), original);
        }
    }

    TEST_F(IcicleAes, UnmappedLegacyAndPageCrossingVexPreserveDestination)
    {
        uint64_t fault{};
        emu->hook_memory_violation([&](cpu_interface&, uint64_t address, size_t, memory_operation operation, memory_violation_type) {
            fault = address;
            EXPECT_EQ(operation, memory_operation::read);
            return memory_violation_continuation::stop;
        });
        for (const bool vex : {false, true})
        {
            if (vex)
            {
                load(std::array<uint8_t, 7>{0xC4, 0xE3, 0x79, 0xDF, 0x0B, 1, 0x90}, 16);
            }
            else
            {
                load(std::array<uint8_t, 7>{0x66, 0x0F, 0x3A, 0xDF, 0x0B, 1, 0x90});
            }
            const auto address = code + (vex ? 0x1FF8 : 0x3000);
            emu->reg(x86_register::rbx, address);
            const wide_vector original{1, 2, 3, 4, 5, 6, 7, 8};
            emu->reg<wide_vector>(x86_register::ymm1, original);
            EXPECT_THROW(emu->start(1), std::runtime_error);
            EXPECT_GE(fault, address);
            EXPECT_LT(fault, address + 16);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + (vex ? 16 : 0));
            EXPECT_EQ(emu->reg<wide_vector>(x86_register::ymm1), original);
        }
    }
}
