#include "emulation_test_utils.hpp"

#include <memory_manager.hpp>

#include <array>

namespace sogen::test
{
    class BackendCpuFeatureCoherence : public testing::TestWithParam<backend_type>
    {
      protected:
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};
        size_t next_slot{};

        void SetUp() override
        {
            emu = create_x86_64_emulator(GetParam());
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x1000, memory_permission::all);
            ASSERT_NE(code, 0u);
        }

        template <size_t N>
        void execute(const std::array<uint8_t, N>& instruction)
        {
            const auto address = code + next_slot;
            next_slot += 0x20;
            emu->write_memory(address, instruction.data(), instruction.size());
            emu->reg(x86_register::rip, address);
            emu->start(GetParam() == backend_type::whp && N == 2 ? 0 : 1);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + instruction.size());
        }

        std::array<uint32_t, 4> cpuid(const uint32_t leaf, const uint32_t subleaf)
        {
            emu->reg(x86_register::rax, static_cast<uint64_t>(leaf));
            emu->reg(x86_register::rcx, static_cast<uint64_t>(subleaf));
            execute(std::array<uint8_t, 2>{0x0F, 0xA2});
            return {emu->reg<uint32_t>(x86_register::eax), emu->reg<uint32_t>(x86_register::ebx), emu->reg<uint32_t>(x86_register::ecx),
                    emu->reg<uint32_t>(x86_register::edx)};
        }
    };

    TEST_P(BackendCpuFeatureCoherence, CpuidXstateAndXgetbvAgree)
    {
        const auto basic = cpuid(0, 0);
        ASSERT_GE(basic[0], 1u);
        const auto version = cpuid(1, 0);
        const bool xsave = (version[2] & (1u << 26)) != 0;
        const bool osxsave = (version[2] & (1u << 27)) != 0;
        const bool avx = (version[2] & (1u << 28)) != 0;

        SCOPED_TRACE(emu->get_name());
        EXPECT_FALSE(osxsave && !xsave);
        EXPECT_FALSE(avx && !xsave);
        if (!osxsave)
        {
            return;
        }

        ASSERT_GE(basic[0], 0xDu);
        const auto xstate = cpuid(0xD, 0);
        emu->reg(x86_register::rcx, uint64_t{0});
        execute(std::array<uint8_t, 3>{0x0F, 0x01, 0xD0});
        const uint64_t xcr0 = emu->reg<uint32_t>(x86_register::eax) | (static_cast<uint64_t>(emu->reg<uint32_t>(x86_register::edx)) << 32);

        EXPECT_NE(xcr0 & 1u, 0u);
        EXPECT_EQ(xcr0 & ~((static_cast<uint64_t>(xstate[3]) << 32) | xstate[0]), 0u);
        EXPECT_GE(xstate[1], 512u);
        if (avx)
        {
            EXPECT_NE(xstate[0] & (1u << 2), 0u);
        }
    }

    INSTANTIATE_TEST_SUITE_P(WhpAndIcicle, BackendCpuFeatureCoherence, testing::Values(backend_type::whp, backend_type::icicle));
}
