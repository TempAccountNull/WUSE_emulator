#include "emulation_test_utils.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace sogen::test
{
    class InstructionExecutionObservation : public testing::Test
    {
      protected:
        windows_emulator win = [] {
            emulator_settings settings{.disable_logging = true, .use_relative_time = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t code{};

        void SetUp() override
        {
            if (!win.uses_instruction_precision() || !win.emu().supports_global_memory_execution_hooks())
            {
                GTEST_SKIP() << "Requires global instruction hooks";
            }
            win.setup_process_if_necessary();
            code = win.mod_manager.executable->entry_point;
            std::array<uint8_t, 32> bytes{};
            bytes.fill(0x90);
            win.emu().write_memory(code, bytes.data(), bytes.size());
            win.vcpu(0).switch_thread = false;
        }

        void step(const uint64_t address)
        {
            win.emu().reg(x86_register::rip, address);
            win.emu().start(1);
        }
    };

    TEST_F(InstructionExecutionObservation, BoundaryKeepsCaptureCountsYieldAndObserverOrder)
    {
        auto& thread = win.current_thread();
        constexpr uint64_t quantum = 0x20000;
        constexpr uint64_t return_value = 0x123456789ABCDEF0ULL;
        thread.executed_instructions = quantum - 2;
        thread.current_ip = code - 1;
        thread.apc_alertable = true;
        thread.callback_stack.emplace_back();
        thread.callback_return_rax.reset();
        win.process.zw_callback_return = code + 1;
        win.emu().reg(x86_register::rax, return_value);
        const auto before = win.get_executed_instructions();
        std::vector<uint64_t> observed;
        win.callbacks.on_instruction = [&](const uint64_t address) {
            observed.push_back(address);
            EXPECT_EQ(&win.current_thread(), &thread);
            EXPECT_EQ(&win.active_cpu(), &win.vcpu(0).cpu);
            EXPECT_EQ(thread.current_ip, address);
            EXPECT_EQ(thread.previous_ip, address - 1);
            EXPECT_EQ(win.get_executed_instructions(), before + observed.size());
            EXPECT_EQ(thread.executed_instructions, quantum - 2 + observed.size());
            if (observed.size() == 1)
            {
                EXPECT_FALSE(thread.callback_return_rax.has_value());
                EXPECT_FALSE(win.vcpu(0).switch_thread);
                EXPECT_TRUE(thread.apc_alertable);
            }
            else
            {
                EXPECT_EQ(thread.callback_return_rax, return_value);
                EXPECT_TRUE(win.vcpu(0).switch_thread);
                EXPECT_FALSE(thread.apc_alertable);
            }
        };
        step(code);
        step(code + 1);
        EXPECT_EQ(observed, (std::vector<uint64_t>{code, code + 1}));
        EXPECT_EQ(win.get_executed_instructions(), before + 2);
        EXPECT_EQ(thread.executed_instructions, quantum);
    }

    TEST_F(InstructionExecutionObservation, MatchingAddressWithoutCallbackFrameDoesNotCaptureReturn)
    {
        auto& thread = win.current_thread();
        ASSERT_TRUE(thread.callback_stack.empty());
        thread.callback_return_rax.reset();
        win.process.zw_callback_return = code;
        win.emu().reg(x86_register::rax, 0xFEDCBA9876543210ULL);
        size_t observations{};
        win.callbacks.on_instruction = [&](uint64_t) {
            ++observations;
            EXPECT_FALSE(thread.callback_return_rax.has_value());
        };
        step(code);
        EXPECT_EQ(observations, 1U);
        EXPECT_FALSE(thread.callback_return_rax.has_value());
    }

    TEST_F(InstructionExecutionObservation, SectionCacheHitStillDeliversEveryInstruction)
    {
        auto& module = *win.mod_manager.executable;
        auto section = std::ranges::find_if(
            module.sections, [&](const mapped_section& candidate) { return code - candidate.region.start < candidate.region.length; });
        ASSERT_NE(section, module.sections.end());
        ASSERT_FALSE(section->first_execute.has_value());
        const auto private_code = win.memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(private_code, 0U);
        win.emu().write_memory<uint8_t>(private_code, 0x90);
        std::vector<uint64_t> first;
        std::vector<uint64_t> instructions;
        win.callbacks.on_section_first_execution.add(
            [&](const mapped_module& seen_module, const mapped_section& seen_section, const uint64_t address) {
                EXPECT_EQ(&seen_module, &module);
                EXPECT_EQ(&seen_section, &*section);
                EXPECT_EQ(seen_section.first_execute, address);
                EXPECT_EQ(win.current_thread().current_ip, address);
                first.push_back(address);
            });
        win.callbacks.on_instruction = [&](const uint64_t address) {
            EXPECT_EQ(first, (std::vector<uint64_t>{code}));
            instructions.push_back(address);
        };
        step(code);
        step(code + 1);
        step(private_code);
        step(code + 2);
        EXPECT_EQ(first, (std::vector<uint64_t>{code}));
        EXPECT_EQ(instructions, (std::vector<uint64_t>{code, code + 1, private_code, code + 2}));
    }

    TEST_F(InstructionExecutionObservation, FirstSectionCallbackCanReplaceInstructionObserver)
    {
        size_t original{};
        std::vector<uint64_t> replacement;
        win.callbacks.on_instruction = [&](uint64_t) { ++original; };
        win.callbacks.on_section_first_execution.add([&](const mapped_module&, const mapped_section&, uint64_t) {
            win.callbacks.on_instruction = [&](const uint64_t address) { replacement.push_back(address); };
        });
        step(code);
        step(code + 1);
        EXPECT_EQ(original, 0U);
        EXPECT_EQ(replacement, (std::vector<uint64_t>{code, code + 1}));
    }

    TEST_F(InstructionExecutionObservation, UnloadAndReloadAtSameAddressResetTheSectionCache)
    {
        const auto sample = std::filesystem::current_path() / "test-sample.exe";
        auto* module = win.mod_manager.map_local_module(sample, R"(C:\observer-reload.exe)", win.log, false, true);
        ASSERT_NE(module, nullptr);
        const auto base = module->image_base;
        const auto entry = module->entry_point;
        ASSERT_NE(base, win.mod_manager.executable->image_base);
        std::vector<uint64_t> first;
        size_t instructions{};
        win.callbacks.on_section_first_execution.add([&](const mapped_module& seen, const mapped_section&, const uint64_t address) {
            if (seen.image_base == base)
            {
                first.push_back(address);
            }
        });
        win.callbacks.on_instruction = [&](uint64_t) { ++instructions; };
        win.emu().write_memory<uint8_t>(entry, 0x90);
        step(entry);
        step(entry);
        EXPECT_EQ(first, (std::vector<uint64_t>{entry}));
        const auto image_size = static_cast<size_t>(module->size_of_image);
        std::vector<std::byte> image(image_size);
        win.memory.read_memory(base, image.data(), image.size());
        ASSERT_TRUE(win.mod_manager.unmap(base));
        ASSERT_EQ(win.mod_manager.find_by_address(entry), nullptr);
        ASSERT_TRUE(win.memory.allocate_memory(base, image.size(), memory_permission::all));
        win.memory.write_memory(base, image.data(), image.size());
        module = win.mod_manager.map_memory_module(base, image.size(), R"(C:\observer-reload.exe)", win.log, false, true);
        ASSERT_NE(module, nullptr);
        ASSERT_EQ(module->image_base, base);
        ASSERT_EQ(module->entry_point, entry);
        win.emu().write_memory<uint8_t>(entry, 0x90);
        step(entry);
        EXPECT_EQ(first, (std::vector<uint64_t>{entry, entry}));
        EXPECT_EQ(instructions, 3U);
    }

}
