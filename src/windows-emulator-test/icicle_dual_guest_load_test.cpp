#define NOMINMAX
#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <memory_manager.hpp>

#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"

namespace sogen::test
{
    namespace
    {
        uint64_t filetime_ticks(const FILETIME& value)
        {
            return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
        }

        uint64_t current_thread_cpu_ticks()
        {
            FILETIME created{};
            FILETIME exited{};
            FILETIME kernel{};
            FILETIME user{};
            if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
            {
                return 0;
            }
            return filetime_ticks(kernel) + filetime_ticks(user);
        }
    } // namespace

    TEST(IcicleSmp, IndependentGuestLoopsMakeConcurrentVcpuProgress)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        ASSERT_EQ(emu->vcpu_count(), 2U);

        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(code, 0U);
        const std::array<uint8_t, 5> loop{0x48, 0xFF, 0xC0, 0xEB, 0xFB};
        emu->write_memory(code, loop.data(), loop.size());

        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        std::atomic<bool> stop{false};
        std::array<std::atomic<bool>, 2> active{};
        std::array<uint64_t, 2> worker_cpu_ticks{};
        std::array<std::string, 2> failures{};

        const auto run = [&](const size_t index) {
            auto& cpu = emu->get_cpu(index);
            cpu.reg(x86_register::rax, 0);
            cpu.reg(x86_register::rip, code);
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            const auto cpu_start = current_thread_cpu_ticks();
            active[index].store(true, std::memory_order_release);
            try
            {
                while (!stop.load(std::memory_order_acquire))
                {
                    cpu.start(100000);
                    emu->sync_worker_context(index);
                }
            }
            catch (const std::exception& error)
            {
                failures[index] = error.what();
            }
            active[index].store(false, std::memory_order_release);
            worker_cpu_ticks[index] = current_thread_cpu_ticks() - cpu_start;
        };

        std::thread vcpu0(run, 0);
        std::thread vcpu1(run, 1);
        while (ready.load(std::memory_order_acquire) != 2)
        {
            std::this_thread::yield();
        }
        const auto wall_start = std::chrono::steady_clock::now();
        go.store(true, std::memory_order_release);

        std::array<uint64_t, 2> prior{};
        std::array<uint64_t, 2> observed{};
        bool concurrent_progress = false;
        const auto deadline = wall_start + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto activity = emu->vcpu_activity();
            if (activity.size() != 2)
            {
                break;
            }
            observed = {activity[0].instructions, activity[1].instructions};
            concurrent_progress |= active[0].load(std::memory_order_acquire) && active[1].load(std::memory_order_acquire) &&
                                   observed[0] > prior[0] && observed[1] > prior[1];
            prior = observed;
        }

        stop.store(true, std::memory_order_release);
        vcpu0.join();
        vcpu1.join();
        const auto wall_end = std::chrono::steady_clock::now();

        EXPECT_TRUE(failures[0].empty()) << failures[0];
        EXPECT_TRUE(failures[1].empty()) << failures[1];
        EXPECT_TRUE(concurrent_progress) << "both per-vCPU instruction counters must "
                                            "advance while both workers are active";
        EXPECT_GT(observed[0], 0U);
        EXPECT_GT(observed[1], 0U);
        EXPECT_GT(emu->get_cpu(0).reg(x86_register::rax), 0U);
        EXPECT_GT(emu->get_cpu(1).reg(x86_register::rax), 0U);

        const double wall_ms = std::chrono::duration<double, std::milli>(wall_end - wall_start).count();
        const double cpu_ms = static_cast<double>(worker_cpu_ticks[0] + worker_cpu_ticks[1]) / 10000.0;
        const double overlap_ratio = cpu_ms / wall_ms;
        std::fprintf(stderr,
                     "[SMP dual-load] vcpu0_icount=%llu vcpu1_icount=%llu vcpu0_rax=%llu "
                     "vcpu1_rax=%llu "
                     "worker_cpu_ms=%.1f wall_ms=%.1f overlap_ratio=%.2f\n",
                     static_cast<unsigned long long>(observed[0]), static_cast<unsigned long long>(observed[1]),
                     static_cast<unsigned long long>(emu->get_cpu(0).reg(x86_register::rax)),
                     static_cast<unsigned long long>(emu->get_cpu(1).reg(x86_register::rax)), cpu_ms, wall_ms, overlap_ratio);
        if (GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) >= 2)
        {
            EXPECT_GT(overlap_ratio, 1.2) << "two CPU-bound guest loops did not overlap on host cores";
        }
    }
} // namespace sogen::test
