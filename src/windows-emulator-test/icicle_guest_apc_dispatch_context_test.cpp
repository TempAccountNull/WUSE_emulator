#define NOMINMAX

#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include "../windows-analyzer/analysis.hpp"
#include "../windows-analyzer/analysis_reporter.hpp"
#include <utils/finally.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace sogen::test
{
    namespace
    {
        struct apc_context_reporter final : analysis_reporter
        {
            windows_emulator* win{};
            unsigned total_dispatches{};
            unsigned second_vcpu_dispatches{};
            unsigned mismatches{};

            void report(const analysis_event& event) override
            {
                const auto* activity = std::get_if<generic_activity_event>(&event);
                if (!activity || activity->details != "APC Dispatch")
                {
                    return;
                }

                ++total_dispatches;
                auto& second = win->vcpu(1);
                const auto* target = second.active_thread;
                if (!target || !target->apc_alertable || target->pending_apcs.empty())
                {
                    return;
                }

                ++second_vcpu_dispatches;
                if (activity->execution.thread_id != target->id || activity->execution.rip != second.cpu.read_instruction_pointer())
                {
                    ++mismatches;
                }
            }
        };
    }

    TEST(IcicleSmp, SchedulerApcActivityReportsSecondVcpuThreadAndRip)
    {
        const auto* previous_jit = std::getenv("SOGEN_ICICLE_JIT");
        const auto* previous_hook = std::getenv("SOGEN_ICICLE_INSTRUCTION_HOOK");
        const std::string saved_jit = previous_jit ? previous_jit : "";
        const std::string saved_hook = previous_hook ? previous_hook : "";
        ASSERT_EQ(_putenv_s("SOGEN_ICICLE_JIT", "1"), 0);
        const auto restore_jit = utils::finally([&] { _putenv_s("SOGEN_ICICLE_JIT", saved_jit.c_str()); });
        ASSERT_EQ(_putenv_s("SOGEN_ICICLE_INSTRUCTION_HOOK", "0"), 0);
        const auto restore_hook = utils::finally([&] { _putenv_s("SOGEN_ICICLE_INSTRUCTION_HOOK", saved_hook.c_str()); });

        emulator_settings settings{};
        settings.use_relative_time = false;
        settings.use_instruction_precision = false;
        settings.emulation_root = get_emulator_root();
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";

        emulator_interfaces interfaces{};
        interfaces.socket_factory = network::create_static_socket_factory();
        interfaces.ui = std::make_unique<null_ui_backend>();
        auto app_settings = get_sample_app_settings({});
        app_settings.arguments.emplace_back(u"-smp-apc-context");
        windows_emulator emu{icicle::create_x86_64_emulator(2), std::move(app_settings), settings, {}, std::move(interfaces)};

        apc_context_reporter reporter{};
        reporter.win = &emu;
        analysis_settings logging_settings{};
        analysis_context analysis{.settings = &logging_settings, .win_emu = &emu, .reporters = {&reporter}};
        register_analysis_callbacks(analysis);

        std::mutex watchdog_mutex;
        std::condition_variable watchdog_cv;
        bool finished{};
        std::atomic_bool timed_out{false};
        std::thread watchdog([&] {
            std::unique_lock lock(watchdog_mutex);
            if (!watchdog_cv.wait_for(lock, std::chrono::seconds(30), [&] { return finished; }))
            {
                timed_out.store(true, std::memory_order_release);
                lock.unlock();
                emu.stop();
            }
        });
        const auto finish_watchdog = utils::finally([&] {
            {
                std::lock_guard lock(watchdog_mutex);
                finished = true;
            }
            watchdog_cv.notify_one();
            watchdog.join();
        });

        emu.start();
        EXPECT_FALSE(timed_out.load(std::memory_order_acquire));
        ASSERT_TERMINATED_SUCCESSFULLY(emu);
        EXPECT_GT(reporter.second_vcpu_dispatches, 0U) << "total APC dispatches=" << reporter.total_dispatches
                                                   << " vcpu0 ic=" << emu.emu().vcpu_activity()[0].instructions
                                                   << " vcpu1 ic=" << emu.emu().vcpu_activity()[1].instructions;
        EXPECT_EQ(reporter.mismatches, 0U);
    }
}
