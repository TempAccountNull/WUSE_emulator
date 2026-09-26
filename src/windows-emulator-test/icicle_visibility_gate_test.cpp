#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <memory_manager.hpp>
#include <utils/finally.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <utility>

namespace sogen::test
{
    TEST(IcicleSmp, EightVcpuLongJitVisibilityGateDrainsBeforePeerReads)
    {
        auto emu = icicle::create_x86_64_emulator(8);
        ASSERT_EQ(emu->vcpu_count(), 8U);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(code, 0U);

        std::array<uint8_t, 0x203> program{};
        program.fill(0x90);
        program[0] = 0xeb;
        program[1] = 0xfe;
        program[0x101] = 0xeb;
        program[0x102] = 0xfe;
        program[0x200] = 0x48;
        program[0x201] = 0x8b;
        program[0x202] = 0x01;
        emu->write_memory(code, program.data(), program.size());

        std::atomic<bool> hook_entered{false};
        std::atomic<bool> release_hook{false};
        std::atomic<bool> finish_workers{false};
        std::atomic<unsigned> workers_started{0};
        emu->hook_memory_execution(code + 0x100, [&](cpu_interface& cpu, uint64_t) {
            if (cpu.index() != 4)
            {
                return;
            }
            hook_entered.store(true, std::memory_order_release);
            while (!release_hook.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        std::array<std::thread, 7> workers{};
        std::array<std::exception_ptr, 7> worker_errors{};
        const auto stop_workers = [&] {
            release_hook.store(true, std::memory_order_release);
            finish_workers.store(true, std::memory_order_release);
            for (size_t i = 1; i < 8; ++i)
            {
                emu->get_cpu(i).stop();
            }
            for (auto& worker : workers)
            {
                if (worker.joinable())
                {
                    worker.join();
                }
            }
        };

        uint64_t first_page{};
        uint64_t second_page{};
        constexpr uint64_t first_marker = 0x41872c009a11b365ULL;
        constexpr uint64_t second_marker = 0x5a29e30074b8c612ULL;
        uint64_t first_mark{};
        uint64_t second_mark{};
        bool blocked_mark_stayed_closed = false;
        bool first_mark_applied = false;
        bool second_mark_applied = false;
        std::string first_failure_state{};
        std::string second_failure_state{};

        {
            const auto cleanup = utils::finally(stop_workers);
            for (size_t i = 1; i < 8; ++i)
            {
                auto& cpu = emu->get_cpu(i);
                cpu.reg(x86_register::rip, i == 4 ? code + 0x100 : code);
                workers[i - 1] = std::thread([&, i] {
                    try
                    {
                        emu->set_scheduler_worker_context(i, true);
                        const auto clear_worker = utils::finally([&] { emu->set_scheduler_worker_context(i, false); });
                        workers_started.fetch_add(1, std::memory_order_release);
                        while (!finish_workers.load(std::memory_order_acquire))
                        {
                            emu->get_cpu(i).start(50'000'000);
                        }
                    }
                    catch (...)
                    {
                        worker_errors[i - 1] = std::current_exception();
                    }
                });
            }

            const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while ((workers_started.load(std::memory_order_acquire) != 7 || !hook_entered.load(std::memory_order_acquire)) &&
                   std::chrono::steady_clock::now() < entered_deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            ASSERT_EQ(workers_started.load(std::memory_order_acquire), 7U);
            ASSERT_TRUE(hook_entered.load(std::memory_order_acquire));

            emu->set_scheduler_worker_context(0, true);
            const auto clear_issuer = utils::finally([&] { emu->set_scheduler_worker_context(0, false); });
            first_page = memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(first_page, 0U);
            emu->write_memory(first_page, &first_marker, sizeof(first_marker));
            first_mark = emu->smp_op_watermark();
            ASSERT_GT(first_mark, 0U);
            blocked_mark_stayed_closed = !emu->smp_op_applied(first_mark);
            const auto blocked_state = emu->smp_gate_debug();
            EXPECT_NE(blocked_state.find("q4="), std::string::npos);
            EXPECT_NE(blocked_state.find("oldest="), std::string::npos);
            EXPECT_NE(blocked_state.find("completed="), std::string::npos);
            EXPECT_NE(blocked_state.find("v4=RUN/k1/age="), std::string::npos);

            release_hook.store(true, std::memory_order_release);
            const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!emu->smp_op_applied(first_mark) && std::chrono::steady_clock::now() < first_deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            first_mark_applied = emu->smp_op_applied(first_mark);
            if (!first_mark_applied)
            {
                first_failure_state = emu->smp_gate_debug();
            }

            second_page = memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(second_page, 0U);
            emu->write_memory(second_page, &second_marker, sizeof(second_marker));
            second_mark = emu->smp_op_watermark();
            const auto second_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!emu->smp_op_applied(second_mark) && std::chrono::steady_clock::now() < second_deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            second_mark_applied = emu->smp_op_applied(second_mark);
            if (!second_mark_applied)
            {
                second_failure_state = emu->smp_gate_debug();
            }
        }

        EXPECT_TRUE(blocked_mark_stayed_closed) << "visibility gate opened while vCPU 4 held its pending map";
        EXPECT_TRUE(first_mark_applied) << first_failure_state;
        EXPECT_TRUE(second_mark_applied) << second_failure_state;
        for (const auto& error : worker_errors)
        {
            if (!error)
            {
                continue;
            }
            try
            {
                std::rethrow_exception(error);
            }
            catch (const std::exception& e)
            {
                ADD_FAILURE() << "vCPU worker failed: " << e.what();
            }
        }
        if (!first_mark_applied || !second_mark_applied)
        {
            return;
        }

        for (size_t i = 0; i < 8; ++i)
        {
            auto& cpu = emu->get_cpu(i);
            cpu.acknowledge_stop();
            for (const auto& [address, expected] :
                 std::array<std::pair<uint64_t, uint64_t>, 2>{{{first_page, first_marker}, {second_page, second_marker}}})
            {
                cpu.reg(x86_register::rcx, address);
                cpu.reg(x86_register::rip, code + 0x200);
                cpu.start(1);
                EXPECT_EQ(cpu.reg(x86_register::rax), expected) << "vCPU " << i << " read a stale peer map";
            }
        }
    }
} // namespace sogen::test
