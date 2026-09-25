#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <utils/finally.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
namespace sogen::test
{
    namespace
    {
        constexpr size_t scaling_worker_count = 8;
        constexpr uint64_t scaling_iterations = 250'000;
        constexpr std::string_view scaling_start_marker = "SOGEN_SMP_SCALING_START";
        constexpr std::string_view scaling_end_marker = "SOGEN_SMP_SCALING_END";
        constexpr std::string_view scaling_worker_marker = "SOGEN_SMP_SCALING_WORKER ";

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
            const auto ticks = [](const FILETIME& value) {
                return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
            };
            return ticks(kernel) + ticks(user);
        }

        uint64_t expected_result(const size_t worker)
        {
            uint64_t value = worker + 1;
            for (uint64_t iteration = 0; iteration < scaling_iterations; ++iteration)
            {
                value = value * 6364136223846793005ULL + iteration;
            }
            return value;
        }

        class IcicleGuestScaling : public ::testing::TestWithParam<size_t>
        {
        };
    }

    TEST_P(IcicleGuestScaling, EightRunnableGuestThreads)
    {
        const auto* enabled = std::getenv("SOGEN_SMP_GUEST_SCALING_BENCH");
        if (!enabled || enabled[0] != '1' || enabled[1] != '\0')
        {
            GTEST_SKIP() << "Set SOGEN_SMP_GUEST_SCALING_BENCH=1 to run the guest scaling benchmark";
        }

        const auto vcpu_count = GetParam();
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
            std::vector<uint64_t> start_instructions;
            std::vector<uint64_t> end_instructions;
        } observed;

        auto app_settings = get_sample_app_settings({});
        app_settings.arguments.emplace_back(u"-smp-scaling");
        windows_emulator* running{};
        emulator_callbacks callbacks{};
        callbacks.on_stdout = [&](const std::string_view data) {
            std::lock_guard lock(observed.mutex);
            observed.output.append(data);
            const auto record = [&](const bool start) {
                const auto activity = running->emu().vcpu_activity();
                if (activity.size() != vcpu_count)
                {
                    return;
                }
                std::vector<uint64_t> counts;
                counts.reserve(activity.size());
                for (const auto& vcpu : activity)
                {
                    counts.push_back(vcpu.instructions);
                }
                if (start)
                {
                    observed.start_wall = std::chrono::steady_clock::now();
                    observed.start_cpu = process_cpu_ticks();
                    observed.start_instructions = std::move(counts);
                    observed.started = true;
                }
                else
                {
                    observed.end_wall = std::chrono::steady_clock::now();
                    observed.end_cpu = process_cpu_ticks();
                    observed.end_instructions = std::move(counts);
                    observed.ended = true;
                }
            };
            if (!observed.started && observed.output.find(scaling_start_marker) != std::string::npos)
            {
                record(true);
            }
            if (observed.started && !observed.ended && observed.output.find(scaling_end_marker) != std::string::npos)
            {
                record(false);
            }
        };

        windows_emulator emu{icicle::create_x86_64_emulator(vcpu_count), std::move(app_settings), settings, std::move(callbacks),
                             std::move(interfaces)};
        running = &emu;

        std::mutex watchdog_mutex;
        std::condition_variable watchdog_cv;
        bool finished{};
        std::atomic_bool timed_out{false};
        std::thread watchdog([&] {
            std::unique_lock lock(watchdog_mutex);
            if (!watchdog_cv.wait_for(lock, std::chrono::seconds(60), [&] { return finished; }))
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

        std::lock_guard lock(observed.mutex);
        ASSERT_TRUE(observed.started) << observed.output;
        ASSERT_TRUE(observed.ended) << observed.output;
        ASSERT_EQ(observed.start_instructions.size(), vcpu_count);
        ASSERT_EQ(observed.end_instructions.size(), vcpu_count);
        ASSERT_GT(observed.end_wall, observed.start_wall);
        ASSERT_GE(observed.end_cpu, observed.start_cpu);

        std::array<bool, scaling_worker_count> seen{};
        std::set<unsigned> tids;
        for (size_t pos = 0; (pos = observed.output.find(scaling_worker_marker, pos)) != std::string::npos;
             pos += scaling_worker_marker.size())
        {
            unsigned index{};
            unsigned tid{};
            unsigned long long progress{};
            unsigned long long result{};
            const auto parsed =
                std::sscanf(observed.output.c_str() + pos, "SOGEN_SMP_SCALING_WORKER %u %u %llu %llu", &index, &tid, &progress, &result);
            ASSERT_EQ(parsed, 4);
            ASSERT_LT(index, seen.size());
            EXPECT_FALSE(seen[index]) << "Duplicate worker record " << index;
            seen[index] = true;
            EXPECT_NE(tid, 0U);
            tids.insert(tid);
            EXPECT_EQ(progress, scaling_iterations);
            EXPECT_EQ(result, expected_result(index));
        }
        for (size_t index = 0; index < seen.size(); ++index)
        {
            EXPECT_TRUE(seen[index]) << "Guest worker " << index << " never completed";
        }
        EXPECT_EQ(tids.size(), scaling_worker_count);

        const double wall_seconds = std::chrono::duration<double>(observed.end_wall - observed.start_wall).count();
        const double cpu_seconds = static_cast<double>(observed.end_cpu - observed.start_cpu) / 10'000'000.0;
        uint64_t total_instructions{};
        std::fprintf(stderr,
                     "[SMP guest scaling] vcpus=%zu workers=%zu iterations_per_worker=%llu wall_ms=%.1f "
                     "cpu_ms=%.1f overlap=%.2f work_miter_per_s=%.3f",
                     vcpu_count, scaling_worker_count, static_cast<unsigned long long>(scaling_iterations), wall_seconds * 1000.0,
                     cpu_seconds * 1000.0, cpu_seconds / wall_seconds,
                     static_cast<double>(scaling_worker_count * scaling_iterations) / wall_seconds / 1e6);
        for (size_t index = 0; index < vcpu_count; ++index)
        {
            ASSERT_GE(observed.end_instructions[index], observed.start_instructions[index]);
            const auto delta = observed.end_instructions[index] - observed.start_instructions[index];
            EXPECT_GT(delta, 0U) << "vCPU " << index << " did not execute the workload";
            total_instructions += delta;
            std::fprintf(stderr, " vcpu%zu_inst=%llu", index, static_cast<unsigned long long>(delta));
        }
        std::fprintf(stderr, " aggregate_mips=%.3f\n", static_cast<double>(total_instructions) / wall_seconds / 1e6);
    }

    INSTANTIATE_TEST_SUITE_P(VcpuCounts, IcicleGuestScaling, ::testing::Values(1U, 2U, 4U, 8U));
}
#endif
