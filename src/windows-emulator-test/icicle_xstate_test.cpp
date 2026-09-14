#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <array>

namespace sogen::test
{
    class IcicleXstate : public testing::TestWithParam<bool>
    {
      protected:
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};
        uint64_t state{};
        using vector = std::array<uint64_t, 2>;
        using extended = std::array<uint8_t, 10>;

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x1000, memory_permission::all);
            state = memory->allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(code, 0u);
            ASSERT_NE(state, 0u);
            emu->reg(x86_register::rbx, state);
            emu->reg(x86_register::rsp, state + 0xF00);
            emu->reg(x86_register::eflags, 0x246u);
        }

        void execute(const uint8_t modrm, const uint64_t mask)
        {
            const std::array<uint8_t, 5> wide{0x48, 0x0F, 0xAE, modrm, 0x90};
            emu->write_memory(code, wide.data() + (GetParam() ? 0 : 1), GetParam() ? 5 : 4);
            emu->reg(x86_register::rip, code);
            emu->reg(x86_register::rax, mask & 0xFFFFFFFF);
            emu->reg(x86_register::rdx, mask >> 32);
            emu->start(1);
        }

        void expect_completed()
        {
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + (GetParam() ? 4 : 3));
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rsp), state + 0xF00);
            EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), 0x246u);
        }

        void populate()
        {
            emu->write_memory<uint16_t>(state, 0x027F);
            emu->write_memory<uint16_t>(state + 2, 0x5D00);
            emu->write_memory<uint8_t>(state + 4, 0x38);
            emu->write_memory<uint16_t>(state + 6, 0x123);
            emu->write_memory<uint64_t>(state + 8, 0x1234567887654321);
            emu->write_memory<uint64_t>(state + 16, 0x2345678976543210);
            emu->write_memory<uint32_t>(state + 24, 0x1FA0);
            emu->write_memory<extended>(state + 32, {0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0x3F});
            emu->write_memory<extended>(state + 64, {1, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0x7F});
            for (size_t index = 0; index < 16; ++index)
            {
                emu->write_memory<vector>(state + 160 + index * 16, {index + 1, 0x8877665544332200 + index});
            }
            emu->write_memory<uint64_t>(state + 512, 3);
        }
    };

    TEST_P(IcicleXstate, RestoresNonzeroLegacyStateAndPreservesUpperYmm)
    {
        populate();
        const std::array<uint64_t, 4> upper{0, 0, 0x12345678, 0xABCDEF};
        emu->reg<std::array<uint64_t, 4>>(x86_register::ymm15, upper);
        execute(0x2B, 3);
        expect_completed();
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fpcw), 0x027F);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fpsw), 0x5D00);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fptag), 0xF93F);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fop), 0x123);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::fip), GetParam() ? 0x1234567887654321ULL : 0x87654321ULL);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::fdp), GetParam() ? 0x2345678976543210ULL : 0x76543210ULL);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1FA0u);
        EXPECT_EQ(emu->reg<extended>(x86_register::fp0), (extended{0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0x3F}));
        for (size_t index = 0; index < 16; ++index)
        {
            EXPECT_EQ(emu->reg<vector>(static_cast<x86_register>(static_cast<int>(x86_register::xmm0) + index)),
                      (vector{index + 1, 0x8877665544332200 + index}));
        }
        const auto result = emu->reg<std::array<uint64_t, 4>>(x86_register::ymm15);
        EXPECT_EQ(result[2], upper[2]);
        EXPECT_EQ(result[3], upper[3]);
    }

    TEST_P(IcicleXstate, SelectiveSseInitializationStillLoadsMxcsr)
    {
        populate();
        emu->write_memory<uint64_t>(state + 512, 0);
        emu->reg(x86_register::fpcw, uint16_t{0x123});
        emu->reg<vector>(x86_register::xmm0, vector{11, 22});
        emu->reg<vector>(x86_register::xmm15, vector{33, 44});
        execute(0x2B, 2);
        expect_completed();
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm0), vector{});
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm15), vector{});
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1FA0u);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fpcw), 0x123);
    }

    TEST_P(IcicleXstate, SelectiveX87InitializationPreservesSse)
    {
        populate();
        emu->write_memory<uint64_t>(state + 512, 0);
        emu->reg<vector>(x86_register::xmm0, vector{11, 22});
        emu->reg(x86_register::mxcsr, 0x1F80u);
        execute(0x2B, 1);
        expect_completed();
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fpcw), 0x37F);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fpsw), 0);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::fptag), 0xFFFF);
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm0), (vector{11, 22}));
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1F80u);
    }

    TEST_P(IcicleXstate, DisabledComponentsAreMaskedWithoutReadingTheirPayload)
    {
        emu->write_memory<uint32_t>(state + 24, 0xFFFFFFFF);
        emu->reg<vector>(x86_register::xmm0, vector{11, 22});
        emu->reg(x86_register::mxcsr, 0x1F80u);
        execute(0x2B, 0xC);
        expect_completed();
        EXPECT_EQ(emu->reg<vector>(x86_register::xmm0), (vector{11, 22}));
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0x1F80u);
    }

    TEST_P(IcicleXstate, SavesSelectedComponentsAndPreservesOtherBytes)
    {
        populate();
        execute(0x2B, 3);
        expect_completed();
        std::array<uint8_t, 576> sentinel;
        sentinel.fill(0xA5);
        emu->write_memory(state + 0x400, sentinel.data(), sentinel.size());
        emu->reg(x86_register::rbx, state + 0x400);
        execute(0x23, 2);
        expect_completed();
        EXPECT_EQ(emu->read_memory<uint64_t>(state + 0x400), 0xA5A5A5A5A5A5A5A5ULL);
        EXPECT_EQ(emu->read_memory<uint32_t>(state + 0x418), 0x1FA0u);
        EXPECT_EQ(emu->read_memory<vector>(state + 0x4A0), (vector{1, 0x8877665544332200}));
        EXPECT_EQ(emu->read_memory<uint64_t>(state + 0x600), 0xA5A5A5A5A5A5A5A7ULL);
        EXPECT_EQ(emu->read_memory<uint64_t>(state + 0x608), 0xA5A5A5A5A5A5A5A5ULL);
        EXPECT_EQ(emu->read_memory<uint64_t>(state + 0x5F8), 0xA5A5A5A5A5A5A5A5ULL);
        execute(0x23, 1);
        expect_completed();
        EXPECT_EQ(emu->read_memory<uint16_t>(state + 0x400), 0x27F);
        EXPECT_EQ(emu->read_memory<uint8_t>(state + 0x404), 0x38);
        EXPECT_EQ(emu->read_memory<extended>(state + 0x420), (extended{0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0x3F}));
        EXPECT_EQ(emu->read_memory<uint64_t>(state + 0x408), GetParam() ? 0x1234567887654321ULL : 0xA5A5567887654321ULL);
    }

    TEST_P(IcicleXstate, InvalidHeaderAndMxcsrRaiseGeneralProtectionBeforeRegisterWrites)
    {
        int interrupt{};
        emu->hook_interrupt([&](cpu_interface& cpu, const int vector) {
            interrupt = vector;
            cpu.stop();
        });
        for (const auto offset : {512u, 520u, 528u, 24u})
        {
            populate();
            emu->write_memory<uint64_t>(state + 520, 0);
            emu->write_memory<uint64_t>(state + 528, 0);
            if (offset == 24)
            {
                emu->write_memory<uint32_t>(state + offset, 0x10000);
            }
            else
            {
                emu->write_memory<uint64_t>(state + offset, 4);
            }
            emu->reg<vector>(x86_register::xmm0, vector{11, 22});
            emu->reg(x86_register::fpcw, uint16_t{0x123});
            interrupt = 0;
            execute(0x2B, 3);
            EXPECT_EQ(interrupt, 13);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
            EXPECT_EQ(emu->reg<vector>(x86_register::xmm0), (vector{11, 22}));
            EXPECT_EQ(emu->reg<uint16_t>(x86_register::fpcw), 0x123);
        }
    }

    TEST_P(IcicleXstate, MisalignedSaveAndRestoreRaiseGeneralProtection)
    {
        int interrupt{};
        emu->hook_interrupt([&](cpu_interface& cpu, const int vector) {
            interrupt = vector;
            cpu.stop();
        });
        emu->reg(x86_register::rbx, state + 8);
        for (const auto modrm : {0x23, 0x2B})
        {
            interrupt = 0;
            execute(static_cast<uint8_t>(modrm), 3);
            EXPECT_EQ(interrupt, 13);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
        }
    }

    TEST_P(IcicleXstate, UnmappedHeaderReportsActualFaultAddress)
    {
        uint64_t fault{};
        emu->hook_memory_violation([&](cpu_interface&, uint64_t address, size_t, memory_operation, memory_violation_type) {
            fault = address;
            return memory_violation_continuation::stop;
        });
        emu->reg(x86_register::rbx, state + 0xE00);
        EXPECT_THROW(execute(0x2B, 3), std::runtime_error);
        EXPECT_EQ(fault, state + 0x1000);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
    }

    TEST_P(IcicleXstate, CpuidAndXgetbvDescribeImplementedState)
    {
        const std::array<uint8_t, 3> query{0x0F, 0xA2, 0x90};
        emu->write_memory(code, query.data(), query.size());
        for (const auto leaf : {0u, 1u, 0xDu})
        {
            emu->reg(x86_register::rip, code);
            emu->reg(x86_register::rax, static_cast<uint64_t>(leaf));
            emu->reg(x86_register::rcx, 0ULL);
            emu->start(1);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 2);
            if (leaf == 0)
            {
                EXPECT_EQ(emu->reg<uint32_t>(x86_register::eax), 0xDu);
            }
            if (leaf == 1)
            {
                EXPECT_EQ(emu->reg<uint32_t>(x86_register::ecx) & 0x1C000000, 0x0C000000u);
            }
            if (leaf == 0xD)
            {
                EXPECT_EQ(emu->reg<uint32_t>(x86_register::eax), 3u);
                EXPECT_EQ(emu->reg<uint32_t>(x86_register::ebx), 576u);
                EXPECT_EQ(emu->reg<uint32_t>(x86_register::ecx), 576u);
                EXPECT_EQ(emu->reg<uint32_t>(x86_register::edx), 0u);
            }
        }
        const std::array<uint8_t, 4> xgetbv{0x0F, 0x01, 0xD0, 0x90};
        emu->write_memory(code, xgetbv.data(), xgetbv.size());
        emu->reg(x86_register::rcx, 0ULL);
        emu->reg(x86_register::rip, code);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eax), 3u);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::edx), 0u);
    }

    INSTANTIATE_TEST_SUITE_P(Encoding, IcicleXstate, testing::Bool());

    TEST(EmulationTest, IcicleSharedXstateMatchesCpuAfterSetupAndSnapshotRestore)
    {
        emulator_settings settings{};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        auto emu = create_sample_emulator(std::move(settings));
        if (emu.emu().get_name() != "icicle-emu")
        {
            GTEST_SKIP();
        }
        emu.setup_process_if_necessary();
        const auto check = [&] {
            emu.process.kusd.access([](const KUSER_SHARED_DATA64& data) {
                EXPECT_EQ(data.XState.EnabledFeatures, 3u);
                EXPECT_EQ(data.XState.EnabledVolatileFeatures, 3u);
                EXPECT_EQ(data.XState.Size, 576u);
                EXPECT_EQ(data.XState.Features[1].Offset, 160u);
                EXPECT_EQ(data.XState.Features[1].Size, 256u);
                EXPECT_EQ(data.XState.ControlFlags, 0u);
            });
        };
        check();
        emu.process.kusd.access([](KUSER_SHARED_DATA64& data) {
            data.XState.EnabledFeatures = 0x1F;
            data.XState.EnabledVolatileFeatures = 0xF;
            data.XState.Size = 960;
        });
        emu.save_snapshot();
        emu.restore_snapshot();
        check();
    }
}
