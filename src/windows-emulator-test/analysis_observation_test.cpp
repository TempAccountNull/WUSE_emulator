#include "emulation_test_utils.hpp"
#include "../windows-analyzer/analysis.hpp"
#include "../windows-analyzer/analysis_reporter.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtTerminateThread(const syscall_context&, handle, NTSTATUS);
}

namespace sogen::test
{
    class AnalysisObservation : public testing::Test, public analysis_reporter
    {
      protected:
        windows_emulator win_emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        analysis_settings logging_settings{.verbose_logging = true};
        analysis_context analysis{.settings = &logging_settings, .win_emu = &win_emu, .reporters = {this}};
        std::vector<function_execution_event> calls{};
        std::vector<execution_progress_event> progress{};
        std::vector<entry_point_execution_event> entries{};
        std::vector<foreign_code_transition_event> transitions{};
        std::vector<thread_terminated_event> terminated{};
        uint64_t caller{};
        uint64_t callee{};
        uint64_t stack{};

        void report(const analysis_event& event) override
        {
            if (const auto* call = std::get_if<function_execution_event>(&event))
            {
                calls.push_back(*call);
            }
            if (const auto* entry = std::get_if<entry_point_execution_event>(&event))
            {
                entries.push_back(*entry);
            }
            if (const auto* transition = std::get_if<foreign_code_transition_event>(&event))
            {
                transitions.push_back(*transition);
            }
            if (const auto* exit = std::get_if<thread_terminated_event>(&event))
            {
                terminated.push_back(*exit);
            }
            if (const auto* update = std::get_if<execution_progress_event>(&event))
            {
                progress.push_back(*update);
            }
        }

        void observe(const uint64_t previous, const uint64_t current)
        {
            auto& thread = win_emu.current_thread();
            thread.previous_ip = previous;
            thread.current_ip = current;
            thread.executed_instructions = 2;
            win_emu.emu().reg(x86_register::rip, current);
            win_emu.callbacks.on_instruction(current);
        }

        void SetUp() override
        {
            win_emu.setup_process_if_necessary();
            caller = win_emu.mod_manager.executable->entry_point + 0x100;
            callee = caller + 0x40;
            stack = win_emu.memory.allocate_memory(0x4000, memory_permission::read_write) + 0x2000;
            win_emu.mod_manager.executable->address_names[callee] = "WinVerifyTrust";
            win_emu.emu().reg(x86_register::rip, callee);
            win_emu.emu().reg(x86_register::rsp, stack);
            win_emu.emu().reg(x86_register::rax, 0x123456789ABCDEF0ULL);
            win_emu.emu().write_memory<uint64_t>(stack, caller + 2);
            register_analysis_callbacks(analysis);
        }
    };

    TEST_F(AnalysisObservation, FunctionDetailsPreserveGuestRegistersAndReturnSlot)
    {
        const auto before = win_emu.emu().save_registers();
        win_emu.callbacks.on_instruction(callee);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        EXPECT_EQ(win_emu.emu().read_memory<uint64_t>(stack), caller + 2);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_EQ(calls.front().function_name, "WinVerifyTrust");
        EXPECT_EQ(calls.front().execution.rip, callee);
    }

    TEST_F(AnalysisObservation, TracedCallReturnsWithGuestResultAndBalancedStack)
    {
        const std::array<uint8_t, 3> call{0xFF, 0xD0, 0x90};
        const std::array<uint8_t, 6> body{0xB8, 0x00, 0x01, 0x0B, 0x80, 0xC3};
        win_emu.emu().write_memory(caller, call.data(), call.size());
        win_emu.emu().write_memory(callee, body.data(), body.size());
        win_emu.emu().write_memory<uint64_t>(stack, caller + 0x80);
        win_emu.emu().reg(x86_register::rip, caller);
        win_emu.emu().reg(x86_register::rax, callee);
        win_emu.emu().start(3);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rip), caller + 2);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rsp), stack);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rax), 0x800B0100U);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_EQ(calls.front().execution.rip, callee);
    }

    TEST_F(AnalysisObservation, VerboseIncludesUnselectedModuleCalls)
    {
        const auto target = win_emu.mod_manager.ntdll->image_base + 0x500;
        win_emu.mod_manager.ntdll->address_names[target] = "ObservedFunction";
        observe(target + 0x40, target);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_FALSE(calls.front().interesting);
        EXPECT_EQ(calls.front().function_name, "ObservedFunction");
        calls.clear();
        logging_settings.verbose_logging = false;
        observe(target + 0x40, target);
        EXPECT_TRUE(calls.empty());
    }

    TEST_F(AnalysisObservation, SelectedModulesRetainIncomingAndOutgoingCalls)
    {
        logging_settings.verbose_logging = false;
        const auto target = win_emu.mod_manager.ntdll->image_base + 0x500;
        win_emu.mod_manager.ntdll->address_names[target] = "ObservedFunction";
        logging_settings.modules.insert(win_emu.mod_manager.ntdll->name);
        observe(target + 0x40, target);
        observe(target + 0x40, callee);
        observe(caller, target);
        ASSERT_EQ(calls.size(), 3U);
        for (const auto& call : calls)
        {
            EXPECT_TRUE(call.interesting);
        }
    }

    TEST_F(AnalysisObservation, AnonymousCallerIsInteresting)
    {
        logging_settings.verbose_logging = false;
        const auto target = win_emu.mod_manager.ntdll->image_base + 0x500;
        win_emu.mod_manager.ntdll->address_names[target] = "ObservedFunction";
        ASSERT_EQ(win_emu.mod_manager.find_by_address(stack), nullptr);
        observe(stack, target);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_TRUE(calls.front().interesting);
    }

    TEST_F(AnalysisObservation, EntryPointPreservesCallClassification)
    {
        auto& module = *win_emu.mod_manager.ntdll;
        const auto target = module.image_base + 0x500;
        module.entry_point = target;
        module.address_names.erase(target);
        observe(target + 0x40, target);
        observe(caller, target);
        ASSERT_EQ(entries.size(), 2U);
        EXPECT_FALSE(entries.front().interesting);
        EXPECT_TRUE(entries.back().interesting);
        EXPECT_TRUE(calls.empty());
    }

    TEST_F(AnalysisObservation, ForeignTransitionDistinguishesBranchFromReturn)
    {
        const auto target = win_emu.mod_manager.ntdll->image_base + 0x500;
        win_emu.mod_manager.ntdll->address_names[target] = "ObservedFunction";
        win_emu.mod_manager.ntdll->address_names.erase(target + 3);
        const std::array<uint8_t, 5> branch{0xE9, 0, 0, 0, 0};
        win_emu.emu().write_memory(caller, branch.data(), branch.size());
        observe(caller, target + 3);
        ASSERT_EQ(transitions.size(), 1U);
        EXPECT_TRUE(transitions.front().interesting);
        EXPECT_EQ(transitions.front().function_name, "ObservedFunction");
        EXPECT_EQ(transitions.front().function_offset, 3U);
        win_emu.emu().write_memory<uint8_t>(caller, 0xC3);
        observe(caller, target + 3);
        EXPECT_EQ(transitions.size(), 1U);
    }

    TEST_F(AnalysisObservation, ExportBoundsKeepNamedCallsAndForeignTransitions)
    {
        auto& module = *win_emu.mod_manager.ntdll;
        const auto first = module.image_base + 0x500;
        const auto last = first + 0x100;
        module.address_names.clear();
        module.address_names[first] = "FirstExport";
        module.address_names[last] = "LastExport";
        const auto same_module_caller = last + 0x100;
        observe(same_module_caller, first - 1);
        observe(same_module_caller, last + 1);
        observe(same_module_caller, first + 1);
        EXPECT_TRUE(calls.empty());
        observe(same_module_caller, first);
        observe(same_module_caller, last);
        ASSERT_EQ(calls.size(), 2U);
        EXPECT_EQ(calls[0].function_name, "FirstExport");
        EXPECT_EQ(calls[1].function_name, "LastExport");
        const std::array<uint8_t, 5> branch{0xE9, 0, 0, 0, 0};
        win_emu.emu().write_memory(caller, branch.data(), branch.size());
        observe(caller, last + 3);
        ASSERT_EQ(transitions.size(), 1U);
        EXPECT_EQ(transitions[0].function_name, "LastExport");
        EXPECT_EQ(transitions[0].function_offset, 3U);
    }

    TEST_F(AnalysisObservation, IgnoredFunctionsRemainAbsentInVerboseMode)
    {
        logging_settings.ignored_functions.insert("WinVerifyTrust");
        observe(caller, callee);
        EXPECT_TRUE(calls.empty());
    }

    TEST_F(AnalysisObservation, UnnamedInstructionsStillContributeToSelectedSummaries)
    {
        auto& module = *win_emu.mod_manager.ntdll;
        const auto target = module.image_base + 0x500;
        module.address_names.erase(target);
        win_emu.emu().write_memory<uint8_t>(target, 0x90);
        logging_settings.instruction_summary = true;
        observe(target + 0x40, target);
        EXPECT_TRUE(analysis.instructions.empty());
        logging_settings.modules.insert(module.name);
        observe(target + 0x40, target);
        ASSERT_EQ(analysis.instructions.size(), 1U);
        EXPECT_EQ(analysis.instructions.begin()->second, 1U);
        EXPECT_TRUE(calls.empty());
        EXPECT_TRUE(entries.empty());
        EXPECT_TRUE(transitions.empty());
    }

    TEST_F(AnalysisObservation, FirstAnonymousInstructionDoesNotMakeAnUnselectedCallInteresting)
    {
        const auto target = win_emu.mod_manager.ntdll->image_base + 0x500;
        win_emu.mod_manager.ntdll->address_names[target] = "ObservedFunction";
        auto& thread = win_emu.current_thread();
        thread.previous_ip = stack;
        thread.executed_instructions = 1;
        win_emu.emu().reg(x86_register::rip, target);
        win_emu.callbacks.on_instruction(target);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_FALSE(calls.front().interesting);
        calls.clear();
        logging_settings.verbose_logging = false;
        win_emu.callbacks.on_instruction(target);
        EXPECT_TRUE(calls.empty());
    }

    TEST_F(AnalysisObservation, ThreadExitReportsTargetAndFullStatusWithoutExitingProcess)
    {
        for (const auto status : std::array<NTSTATUS, 2>{STATUS_SUCCESS, STATUS_ACCESS_VIOLATION})
        {
            const auto target = win_emu.process.create_thread(win_emu.memory, caller, 0, 0x10000, 0);
            const auto id = win_emu.process.threads.get(target)->id;
            auto& vcpu = win_emu.vcpu(0);
            const syscall_context context{.win_emu = win_emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = win_emu.process};
            ASSERT_NE(id, vcpu.active_thread->id);
            ASSERT_EQ(syscalls::handle_NtTerminateThread(context, target, status), STATUS_SUCCESS);
            ASSERT_FALSE(terminated.empty());
            EXPECT_EQ(terminated.back().terminated_thread_id, id);
            EXPECT_EQ(terminated.back().exit_status, static_cast<uint32_t>(status));
            EXPECT_FALSE(vcpu.active_thread->exit_status.has_value());
            EXPECT_FALSE(win_emu.process.exit_status.has_value());
        }
        EXPECT_EQ(terminated.size(), 2U);
    }

    TEST_F(AnalysisObservation, ProgressExcludesInstructionsFromRestoredSnapshot)
    {
        const std::array<uint8_t, 2> loop{0xEB, 0xFE};
        win_emu.emu().write_memory(caller, loop.data(), loop.size());
        win_emu.emu().reg(x86_register::rip, caller);
        win_emu.emu().start(0x20000);
        win_emu.emu().start(0x20000);
        const auto count = win_emu.get_executed_instructions();
        ASSERT_EQ(count, 0x40000U);
        utils::buffer_serializer output{};
        win_emu.serialize(output);
        utils::buffer_deserializer input{output.get_buffer()};
        win_emu.deserialize(input);
        analysis_context restored{.settings = &logging_settings, .win_emu = &win_emu, .reporters = {this}};
        register_analysis_callbacks(restored);
        restored.progress_last -= std::chrono::seconds(6);
        progress.clear();
        win_emu.callbacks.on_instruction(caller);
        ASSERT_EQ(progress.size(), 1U);
        EXPECT_EQ(progress.front().header.instruction_count, count);
        EXPECT_EQ(progress.front().instructions_per_second, 0U);
    }

}
