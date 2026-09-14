#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <array>

namespace sogen::test
{
    class IcicleBootSupport : public testing::Test
    {
      protected:
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x2000, memory_permission::all);
            ASSERT_NE(code, 0u);
        }

        template <size_t N>
        void load(const std::array<uint8_t, N>& bytes)
        {
            emu->write_memory(code, bytes.data(), bytes.size());
            emu->reg(x86_register::rip, code);
        }
    };

    TEST_F(IcicleBootSupport, TimestampHooksPreserveOutputsAndInstructionLength)
    {
        load(std::array<uint8_t, 6>{0x0F, 0x31, 0x0F, 0x01, 0xF9, 0x90});
        size_t rdtsc_count{};
        size_t rdtscp_count{};
        auto* first = emu->hook_instruction(x86_hookable_instructions::rdtsc, [&](cpu_interface&, uint64_t) {
            ++rdtsc_count;
            emu->reg(x86_register::rax, 0x12345678ULL);
            emu->reg(x86_register::rdx, 0xABCDEF01ULL);
            return instruction_hook_continuation::skip_instruction;
        });
        auto* second = emu->hook_instruction(x86_hookable_instructions::rdtscp, [&](cpu_interface&, uint64_t) {
            ++rdtscp_count;
            emu->reg(x86_register::rax, 0x98765432ULL);
            emu->reg(x86_register::rcx, 7ULL);
            return instruction_hook_continuation::skip_instruction;
        });
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 2);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 0x12345678ULL);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rdx), 0xABCDEF01ULL);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 5);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 0x98765432ULL);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rcx), 7ULL);
        EXPECT_EQ(rdtsc_count, 1u);
        EXPECT_EQ(rdtscp_count, 1u);
        emu->delete_hook(first);
        emu->delete_hook(second);
        emu->reg(x86_register::rip, code);
        emu->start(1);
        EXPECT_EQ(rdtsc_count, 1u);
    }

    TEST_F(IcicleBootSupport, TimestampHookCanFinalizeInstructionPointer)
    {
        load(std::array<uint8_t, 5>{0x0F, 0x01, 0xF9, 0x90, 0x90});
        ASSERT_NE(emu->hook_instruction(x86_hookable_instructions::rdtscp,
                                        [&](cpu_interface&, uint64_t) {
                                            emu->reg(x86_register::rip, code + 4);
                                            return instruction_hook_continuation::finalized_instruction_pointer;
                                        }),
                  nullptr);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 4);
    }

    TEST_F(IcicleBootSupport, HostPatchInvalidatesPreviouslyExecutedCode)
    {
        load(std::array<uint8_t, 6>{0xB8, 1, 0, 0, 0, 0x90});
        emu->start(1);
        ASSERT_EQ(emu->reg<uint64_t>(x86_register::rax), 1u);
        emu->write_memory<uint8_t>(code + 1, 2);
        emu->reg(x86_register::rip, code);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 2u);
    }

    TEST_F(IcicleBootSupport, GuestPatchChangesNextInstructionInSameBlock)
    {
        load(std::array<uint8_t, 13>{0xC6, 0x05, 0x01, 0, 0, 0, 2, 0xB8, 1, 0, 0, 0, 0x90});
        emu->hook_memory_execution(code + 12, [&](cpu_interface& cpu, uint64_t) { cpu.stop(); });
        emu->start(64);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 2u);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 12);
    }

    TEST_F(IcicleBootSupport, RejectedStackStoreDoesNotReplayStackDecrement)
    {
        load(std::array<uint8_t, 2>{0x50, 0x90});
        emu->write_memory<uint64_t>(code + 0x108, 0x9090909090909090ULL);
        emu->reg(x86_register::rip, code + 0x108);
        emu->start(1);
        emu->reg(x86_register::rip, code);
        emu->reg(x86_register::rsp, code + 0x110);
        emu->reg(x86_register::rax, 0x123456789ABCDEF0ULL);
        emu->hook_memory_execution(code + 1, [&](cpu_interface& cpu, uint64_t) { cpu.stop(); });
        emu->start(64);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rsp), code + 0x108);
        EXPECT_EQ(emu->read_memory<uint64_t>(code + 0x108), 0x123456789ABCDEF0ULL);
    }

    TEST_F(IcicleBootSupport, TimestampOpcodeInsideImmediateIsNotAnInstruction)
    {
        load(std::array<uint8_t, 6>{0xB8, 0x00, 0x00, 0x0F, 0x31, 0x90});
        size_t count{};
        emu->hook_instruction(x86_hookable_instructions::rdtsc, [&](cpu_interface&, uint64_t) {
            ++count;
            return instruction_hook_continuation::skip_instruction;
        });
        emu->start(1);
        EXPECT_EQ(count, 0u);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 0x310F0000ULL);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 5);
    }

    TEST_F(IcicleBootSupport, CodePatchAndStopFromSameHookPreserveStop)
    {
        load(std::array<uint8_t, 6>{0xB8, 1, 0, 0, 0, 0x90});
        auto* hook = emu->hook_memory_execution(code, [&](cpu_interface& cpu, uint64_t) {
            emu->write_memory<uint8_t>(code + 1, 2);
            cpu.stop();
        });
        emu->start(64);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
        emu->delete_hook(hook);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), 2u);
    }

    TEST_F(IcicleBootSupport, GuestStoreSpanningTwoExecutedPages)
    {
        load(std::array<uint8_t, 4>{0x48, 0x89, 0x03, 0x90});
        emu->write_memory<uint64_t>(code + 0xFFC, 0x9090909090909090ULL);
        for (const auto address : {code + 0xFFC, code + 0x1000})
        {
            emu->reg(x86_register::rip, address);
            emu->start(1);
        }
        emu->reg(x86_register::rip, code);
        emu->reg(x86_register::rbx, code + 0xFFC);
        emu->reg(x86_register::rax, 0x123456789ABCDEF0ULL);
        emu->hook_memory_execution(code + 3, [&](cpu_interface& cpu, uint64_t) { cpu.stop(); });
        emu->start(64);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 3);
        EXPECT_EQ(emu->read_memory<uint64_t>(code + 0xFFC), 0x123456789ABCDEF0ULL);
    }

    TEST(EmulationTest, SectionFirstExecutionSurvivesSnapshotRestore)
    {
        std::vector<std::tuple<std::string, std::string, uint64_t>> events;
        emulator_callbacks callbacks;
        callbacks.on_section_first_execution.add([&](const mapped_module& mod, const mapped_section& section, const uint64_t address) {
            events.emplace_back(mod.name, section.name, address);
        });
        emulator_settings settings{.use_relative_time = true};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        auto emu = create_sample_emulator(settings, {}, std::move(callbacks));
        emu.setup_process_if_necessary();
        emu.save_snapshot();
        emu.start(200000);
        ASSERT_FALSE(events.empty());
        const auto expected = events;
        events.clear();
        emu.restore_snapshot();
        emu.start(200000);
        EXPECT_EQ(events, expected);
    }

    TEST_F(IcicleBootSupport, HostWritesToReadOnlyZeroPagesDoNotAlias)
    {
        const auto first = memory->allocate_memory(0x2000, memory_permission::read);
        const auto second = memory->allocate_memory(0x1000, memory_permission::read);
        EXPECT_EQ(emu->read_memory<uint64_t>(first), 0u);
        EXPECT_EQ(emu->read_memory<uint64_t>(first + 0x1000), 0u);
        EXPECT_EQ(emu->read_memory<uint64_t>(second), 0u);
        emu->write_memory<uint64_t>(first, 0x123456789abcdef0);
        EXPECT_EQ(emu->read_memory<uint64_t>(first), 0x123456789abcdef0u);
        EXPECT_EQ(emu->read_memory<uint64_t>(first + 0x1000), 0u);
        EXPECT_EQ(emu->read_memory<uint64_t>(second), 0u);
        const std::array<uint8_t, 32> bytes{1, 2, 3, 4};
        emu->write_memory(first + 0xff8, bytes.data(), bytes.size());
        EXPECT_EQ(emu->read_memory<uint64_t>(second + 0xff8), 0u);
        EXPECT_EQ(emu->read_memory<uint64_t>(second), 0u);
        const auto third = memory->allocate_memory(0x1000, memory_permission::read);
        EXPECT_EQ(emu->read_memory<uint64_t>(third), 0u);
    }

}
