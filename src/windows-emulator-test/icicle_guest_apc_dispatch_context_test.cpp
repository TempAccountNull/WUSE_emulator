#define NOMINMAX

#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include "../windows-analyzer/analysis.hpp"
#include "../windows-analyzer/analysis_reporter.hpp"

namespace sogen::test
{
    namespace
    {
        struct apc_context_reporter final : analysis_reporter
        {
            windows_emulator* win{};
            unsigned dispatches{};
            unsigned mismatches{};

            void report(const analysis_event& event) override
            {
                const auto* activity = std::get_if<generic_activity_event>(&event);
                if (!activity || activity->details != "APC Dispatch")
                {
                    return;
                }

                ++dispatches;
                const auto& second = win->vcpu(1);
                const auto* target = second.active_thread;
                if (!target || activity->execution.thread_id != target->id ||
                    activity->execution.rip != second.cpu.read_instruction_pointer())
                {
                    ++mismatches;
                }
            }
        };
    }

    TEST(IcicleSmp, SchedulerApcActivityReportsSecondVcpuThreadAndRip)
    {
        emulator_settings settings{.disable_logging = true, .use_relative_time = false, .use_instruction_precision = false};
        settings.emulation_root = get_emulator_root();
        settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";

        emulator_interfaces interfaces{};
        interfaces.socket_factory = network::create_static_socket_factory();
        interfaces.ui = std::make_unique<null_ui_backend>();
        windows_emulator emu{icicle::create_x86_64_emulator(2), get_sample_app_settings({}), settings, {}, std::move(interfaces)};
        emu.setup_process_if_necessary();

        auto& first = emu.vcpu(0);
        auto& second = emu.vcpu(1);
        const auto first_thread_id = first.active_thread->id;
        const auto code = emu.mod_manager.executable->entry_point;
        const auto target_handle = emu.process.create_thread(emu.memory, code + 16, 0, 0x10000, 0);
        auto& target = *emu.process.threads.get(target_handle);
        const auto original = first.cpu.save_registers();
        first.cpu.reg(x86_register::rip, code + 16);
        first.cpu.reg(x86_register::rsp, target.stack_base + target.stack_size - 0x100);
        target.last_registers = first.cpu.save_registers();
        target.setup_done = true;
        first.cpu.restore_registers(original);
        target.apc_alertable = true;
        target.pending_apcs.push_back({.apc_routine = code + 32});

        apc_context_reporter reporter{};
        reporter.win = &emu;
        analysis_settings logging_settings{};
        analysis_context analysis{.settings = &logging_settings, .win_emu = &emu, .reporters = {&reporter}};
        register_analysis_callbacks(analysis);

        ASSERT_TRUE(emu.perform_thread_switch(second));
        EXPECT_EQ(second.active_thread, &target);
        EXPECT_EQ(reporter.dispatches, 1U);
        EXPECT_EQ(reporter.mismatches, 0U);
        EXPECT_EQ(emu.active_cpu().index(), 0U);
        EXPECT_EQ(emu.current_thread().id, first_thread_id);
    }
}
