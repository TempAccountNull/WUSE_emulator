#include <host_wait_idle_policy.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace sogen::test
{
    // Exercise the same idle backoff called by the Windows scheduler with eight
    // lock-contending workers and a continuously progressing simulated guest.
    TEST(HostWaitIdleScheduler, EightIdleWorkersBoundLockPollingAndWakePromptly)
    {
        std::mutex kernel_lock;
        std::atomic<bool> completed{false};
        std::atomic<uint64_t> polls{0};
        std::atomic<uint64_t> simulated_guest_progress{0};
        std::atomic<uint32_t> awakened{0};
        std::vector<std::thread> workers;
        workers.reserve(8);
        for (unsigned i = 0; i < 8; ++i)
        {
            workers.emplace_back([&] {
                while (!completed.load(std::memory_order_acquire))
                {
                    {
                        std::scoped_lock lock(kernel_lock);
                        polls.fetch_add(1, std::memory_order_relaxed);
                    }
                    detail::sleep_for_pending_host_wait();
                }
                awakened.fetch_add(1, std::memory_order_relaxed);
            });
        }

        std::thread guest([&] {
            while (!completed.load(std::memory_order_acquire))
            {
                {
                    std::scoped_lock lock(kernel_lock);
                    simulated_guest_progress.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::yield();
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto polls_before_wake = polls.load(std::memory_order_relaxed);
        const auto progress_before_wake = simulated_guest_progress.load(std::memory_order_relaxed);
        const auto wake_start = std::chrono::steady_clock::now();
        completed.store(true, std::memory_order_release);
        for (auto& worker : workers)
        {
            worker.join();
        }
        guest.join();
        const auto wake_time = std::chrono::steady_clock::now() - wake_start;

        std::cout << "HOSTWAIT 8 idle workers: polls=" << polls_before_wake
                  << " guest-progress=" << progress_before_wake
                  << " wake-ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(wake_time).count() << "\n";
        EXPECT_GT(polls_before_wake, 0U);
        EXPECT_LT(polls_before_wake, 2000U) << "eight idle workers spun on the shared lock";
        EXPECT_GT(progress_before_wake, 25U) << "lock contention starved the simulated guest";
        EXPECT_EQ(awakened.load(std::memory_order_relaxed), 8U);
        EXPECT_LT(wake_time, std::chrono::milliseconds(250));
    }
}