#define NOMINMAX

#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <utils/finally.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <set>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>

namespace sogen::test
{
    namespace
    {
        uint64_t filetime_ticks(const FILETIME& value)
        {
            return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
        }

        uint64_t process_cpu_ticks()
        {
            FILETIME created{};
            FILETIME exited{};
            FILETIME kernel{};
            FILETIME user{};
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            {
                return 0;
            }
            return filetime_ticks(kernel) + filetime_ticks(user);
        }
    }

    TEST(IcicleSmp, GuestCreatedCpuBoundThreadsOverlapOnTwoVcpus)
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

        struct observation
        {
            std::mutex mutex;
            std::string output;
            bool started{};
            bool ended{};
            std::chrono::steady_clock::time_point start_wall;
            std::chrono::steady_clock::time_point end_wall;
            uint64_t start_cpu{};
            uint64_t end_cpu{};
            std::array<uint64_t, 2> start_instructions{};
            std::array<uint64_t, 2> end_instructions{};
        } observed;

        auto app_settings = get_sample_app_settings({});
        app_settings.arguments.emplace_back(u"-smp-load");
        windows_emulator* running{};
        emulator_callbacks callbacks{};
        callbacks.on_stdout = [&](const std::string_view data) {
            std::lock_guard lock(observed.mutex);
            observed.output.append(data);
            const auto record = [&](const bool start) {
                const auto activity = running->emu().vcpu_activity();
                if (activity.size() != 2)
                {
                    return;
                }
                const std::array<uint64_t, 2> counts{activity[0].instructions, activity[1].instructions};
                if (start)
                {
                    observed.start_wall = std::chrono::steady_clock::now();
                    observed.start_cpu = process_cpu_ticks();
                    observed.start_instructions = counts;
                    observed.started = true;
                }
                else
                {
                    observed.end_wall = std::chrono::steady_clock::now();
                    observed.end_cpu = process_cpu_ticks();
                    observed.end_instructions = counts;
                    observed.ended = true;
                }
            };
            if (!observed.started && observed.output.find("SOGEN_SMP_LOAD_START") != std::string::npos)
            {
                record(true);
            }
            if (!observed.ended && observed.output.find("SOGEN_SMP_LOAD_END") != std::string::npos)
            {
                record(false);
            }
        };

        windows_emulator emu{icicle::create_x86_64_emulator(2), std::move(app_settings), settings, std::move(callbacks),
                             std::move(interfaces)};
        running = &emu;
        emu.start();
        ASSERT_TERMINATED_SUCCESSFULLY(emu);
        ASSERT_TRUE(observed.started) << observed.output;
        ASSERT_TRUE(observed.ended) << observed.output;
        ASSERT_GT(observed.end_cpu, observed.start_cpu);
        ASSERT_GT(observed.end_wall, observed.start_wall);
        EXPECT_GT(observed.end_instructions[0] - observed.start_instructions[0], 1000000U);
        EXPECT_GT(observed.end_instructions[1] - observed.start_instructions[1], 1000000U);

        const double wall_ms = std::chrono::duration<double, std::milli>(observed.end_wall - observed.start_wall).count();
        const double cpu_ms = static_cast<double>(observed.end_cpu - observed.start_cpu) / 10000.0;
        const double overlap_ratio = cpu_ms / wall_ms;
        std::fprintf(stderr, "[SMP guest scheduler] vcpu0_delta=%llu vcpu1_delta=%llu cpu_ms=%.1f wall_ms=%.1f overlap_ratio=%.2f\n",
                     static_cast<unsigned long long>(observed.end_instructions[0] - observed.start_instructions[0]),
                     static_cast<unsigned long long>(observed.end_instructions[1] - observed.start_instructions[1]), cpu_ms, wall_ms,
                     overlap_ratio);
        if (GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) >= 2)
        {
            EXPECT_GT(overlap_ratio, 1.2);
        }
    }

    TEST(IcicleSmp, FourReadyGuestThreadsEachRunOnTwoVcpus)
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

        struct observation
        {
            std::mutex mutex;
            std::string output;
            bool started{};
            bool ended{};
            std::array<uint64_t, 2> start_instructions{};
            std::array<uint64_t, 2> end_instructions{};
        } observed;

        auto app_settings = get_sample_app_settings({});
        app_settings.arguments.emplace_back(u"-smp-fair");
        windows_emulator* running{};
        emulator_callbacks callbacks{};
        callbacks.on_stdout = [&](const std::string_view data) {
            std::lock_guard lock(observed.mutex);
            observed.output.append(data);
            const auto record = [&](const bool start) {
                const auto activity = running->emu().vcpu_activity();
                if (activity.size() != 2)
                {
                    return;
                }
                const std::array<uint64_t, 2> counts{activity[0].instructions, activity[1].instructions};
                if (start)
                {
                    observed.start_instructions = counts;
                    observed.started = true;
                }
                else
                {
                    observed.end_instructions = counts;
                    observed.ended = true;
                }
            };
            if (!observed.started && observed.output.find("SOGEN_SMP_FAIR_START") != std::string::npos)
            {
                record(true);
            }
            if (!observed.ended && observed.output.find("SOGEN_SMP_FAIR_END") != std::string::npos)
            {
                record(false);
            }
        };

        windows_emulator emu{icicle::create_x86_64_emulator(2), std::move(app_settings), settings, std::move(callbacks),
                             std::move(interfaces)};
        running = &emu;

        // A scheduler regression must fail the test instead of leaving the suite running forever.
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
        EXPECT_FALSE(timed_out.load(std::memory_order_acquire)) << "Four guest workers did not finish within 30 seconds";
        ASSERT_TERMINATED_SUCCESSFULLY(emu);
        ASSERT_TRUE(observed.started) << observed.output;
        ASSERT_TRUE(observed.ended) << observed.output;
        EXPECT_GT(observed.end_instructions[0] - observed.start_instructions[0], 100000U);
        EXPECT_GT(observed.end_instructions[1] - observed.start_instructions[1], 100000U);

        std::array<bool, 4> seen{};
        std::set<unsigned> tids;
        constexpr std::string_view marker = "SOGEN_SMP_FAIR_WORKER ";
        for (size_t pos = 0; (pos = observed.output.find(marker, pos)) != std::string::npos; pos += marker.size())
        {
            unsigned index{};
            unsigned tid{};
            unsigned long long progress{};
            unsigned long long result{};
            const auto parsed = std::sscanf(observed.output.c_str() + pos, "SOGEN_SMP_FAIR_WORKER %u %u %llu %llu",
                                            &index, &tid, &progress, &result);
            ASSERT_EQ(parsed, 4);
            ASSERT_LT(index, seen.size());
            EXPECT_FALSE(seen[index]) << "Duplicate worker record for index " << index;
            seen[index] = true;
            EXPECT_NE(tid, 0U);
            tids.insert(tid);
            EXPECT_EQ(progress, 500000U);
        }
        for (size_t i = 0; i < seen.size(); ++i)
        {
            EXPECT_TRUE(seen[i]) << "Guest worker " << i << " never completed";
        }
        EXPECT_EQ(tids.size(), seen.size()) << "Workers did not receive distinct guest TIDs";
    }
}
