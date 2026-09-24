#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <memory_manager.hpp>
#include "../windows-emulator/scheduler_vm_gate.hpp"
#include <utils/finally.hpp>

#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>

namespace sogen::test
{
    namespace
    {
        template <typename Predicate>
        bool wait_for_pause_condition(Predicate&& condition)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!condition() && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return condition();
        }

        bool external_pause_started(const x86_64_emulator& emu)
        {
            return emu.smp_gate_debug().find("quiescing=1") != std::string::npos;
        }
    }

    TEST(IcicleSmp, ExternalPauseWaitsForParkedPeerQueueDrain)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto target = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0U);
        ASSERT_NE(target, 0U);
        const std::array<uint8_t, 0x101> nops = [] {
            std::array<uint8_t, 0x101> bytes{};
            bytes.fill(0x90);
            return bytes;
        }();
        emu->write_memory(code, nops.data(), nops.size());

        // Queue a peer unmap without draining it yet.
        emu->set_scheduler_worker_context(0, true);
        {
            const auto clear = utils::finally([&] { emu->set_scheduler_worker_context(0, false); });
            ASSERT_TRUE(memory.release_memory(target, 0));
        }
        const auto issued = emu->smp_op_watermark();
        ASSERT_GT(issued, 0U);
        ASSERT_FALSE(emu->smp_op_applied(issued));

        std::atomic_bool peer_parked{false};
        std::atomic_bool begin_drain{false};
        std::atomic_bool external_done{false};
        std::exception_ptr peer_error;
        std::exception_ptr external_error;
        std::thread peer([&] {
            try
            {
                emu->set_scheduler_worker_context(1, true);
                emu->set_scheduler_vm_parked(1, true);
                const auto clear = utils::finally([&] {
                    emu->set_scheduler_vm_parked(1, false);
                    emu->set_scheduler_worker_context(1, false);
                });
                peer_parked.store(true, std::memory_order_release);
                while (!begin_drain.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                emu->sync_worker_context(1);
            }
            catch (...)
            {
                peer_error = std::current_exception();
            }
        });
        const bool parked = wait_for_pause_condition([&] { return peer_parked.load(std::memory_order_acquire); });
        std::thread external([&] {
            try
            {
                emu->hook_memory_execution(code + 0x100, [](cpu_interface&, uint64_t) {});
                external_done.store(true, std::memory_order_release);
            }
            catch (...)
            {
                external_error = std::current_exception();
            }
        });

        const bool in_pause = wait_for_pause_condition([&] { return external_pause_started(*emu); });
        if (parked && in_pause)
        {
            EXPECT_FALSE(external_done.load(std::memory_order_acquire));
            EXPECT_FALSE(emu->smp_op_applied(issued));
        }
        begin_drain.store(true, std::memory_order_release);
        peer.join();
        external.join();
        EXPECT_TRUE(parked);
        EXPECT_TRUE(in_pause);
        EXPECT_EQ(peer_error, nullptr);
        EXPECT_EQ(external_error, nullptr);
        EXPECT_TRUE(external_done.load(std::memory_order_acquire));
        EXPECT_TRUE(emu->smp_op_applied(issued));
    }

    TEST(IcicleSmp, ExternalPauseDoesNotBlockParkedOwnerLocalUnmap)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto target = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0U);
        ASSERT_NE(target, 0U);
        const std::array<uint8_t, 0x101> nops = [] {
            std::array<uint8_t, 0x101> bytes{};
            bytes.fill(0x90);
            return bytes;
        }();
        emu->write_memory(code, nops.data(), nops.size());

        std::atomic_bool owner_parked{false};
        std::atomic_bool begin_unmap{false};
        std::atomic_bool owner_done{false};
        std::atomic_bool external_done{false};
        std::exception_ptr owner_error;
        std::exception_ptr external_error;
        std::thread owner([&] {
            try
            {
                emu->set_scheduler_worker_context(0, true);
                emu->set_scheduler_vm_parked(0, true);
                const auto clear = utils::finally([&] {
                    emu->set_scheduler_vm_parked(0, false);
                    emu->set_scheduler_worker_context(0, false);
                });
                owner_parked.store(true, std::memory_order_release);
                while (!begin_unmap.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                if (!memory.release_memory(target, 0))
                {
                    throw std::runtime_error("owner unmap failed");
                }
                owner_done.store(true, std::memory_order_release);
            }
            catch (...)
            {
                owner_error = std::current_exception();
            }
        });
        const bool parked = wait_for_pause_condition([&] { return owner_parked.load(std::memory_order_acquire); });
        std::thread external([&] {
            try
            {
                emu->hook_memory_execution(code + 0x100, [](cpu_interface&, uint64_t) {});
                external_done.store(true, std::memory_order_release);
            }
            catch (...)
            {
                external_error = std::current_exception();
            }
        });

        const bool in_pause = wait_for_pause_condition([&] { return external_pause_started(*emu); });
        if (parked && in_pause)
        {
            EXPECT_FALSE(external_done.load(std::memory_order_acquire));
        }
        begin_unmap.store(true, std::memory_order_release);
        owner.join();
        external.join();
        EXPECT_TRUE(parked);
        EXPECT_TRUE(in_pause);
        EXPECT_EQ(owner_error, nullptr);
        EXPECT_EQ(external_error, nullptr);
        EXPECT_TRUE(owner_done.load(std::memory_order_acquire));
        EXPECT_TRUE(external_done.load(std::memory_order_acquire));

        const auto issued = emu->smp_op_watermark();
        ASSERT_GT(issued, 0U);
        ASSERT_NO_THROW(emu->sync_worker_context(1));
        EXPECT_TRUE(emu->smp_op_applied(issued));
        uint64_t value{};
        EXPECT_FALSE(emu->try_read_memory(target, &value, sizeof(value)));
    }

    TEST(IcicleSmp, ExternalHookDestructorSeekingKernelLockDoesNotInvertParkedGate)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(code, 0U);

        struct timed_kernel_lock
        {
            std::timed_mutex mutex{};
            std::atomic_bool worker_acquired{false};

            void lock()
            {
                mutex.lock();
                worker_acquired.store(true, std::memory_order_release);
            }

            void unlock()
            {
                mutex.unlock();
            }
        } kernel;

        std::atomic_bool destructor_entered{false};
        std::atomic_bool destructor_timed_out{false};
        std::atomic_bool destructor_finished{false};
        auto lifetime = std::shared_ptr<int>(new int(1), [&](int* value) {
            destructor_entered.store(true, std::memory_order_release);
            if (!wait_for_pause_condition([&] { return kernel.worker_acquired.load(std::memory_order_acquire); }) ||
                !kernel.mutex.try_lock_for(std::chrono::seconds(1)))
            {
                destructor_timed_out.store(true, std::memory_order_release);
            }
            else
            {
                kernel.mutex.unlock();
            }
            delete value;
            destructor_finished.store(true, std::memory_order_release);
        });
        auto* hook = emu->hook_memory_execution(code, [hold = std::move(lifetime)](cpu_interface&, uint64_t) {});
        ASSERT_NE(hook, nullptr);
        ASSERT_FALSE(destructor_entered.load(std::memory_order_acquire));

        std::exception_ptr external_error;
        std::thread external([&] {
            try
            {
                emu->delete_hook(hook); // destroys the final callback capture under the paused VM gates
            }
            catch (...)
            {
                external_error = std::current_exception();
            }
        });
        const bool in_destructor = wait_for_pause_condition([&] { return destructor_entered.load(std::memory_order_acquire); });

        std::atomic_bool worker_finished{false};
        std::exception_ptr worker_error;
        std::thread worker([&] {
            try
            {
                emu->set_scheduler_worker_context(1, true);
                const auto clear = utils::finally([&] { emu->set_scheduler_worker_context(1, false); });
                std::unique_lock<timed_kernel_lock> lock(kernel, std::defer_lock);
                acquire_scheduler_vm_parked(*emu, 1, lock);
                emu->set_scheduler_vm_parked(1, false);
                lock.unlock();
                worker_finished.store(true, std::memory_order_release);
            }
            catch (...)
            {
                worker_error = std::current_exception();
            }
        });
        worker.join();
        external.join();
        EXPECT_TRUE(in_destructor);
        EXPECT_TRUE(destructor_finished.load(std::memory_order_acquire));
        EXPECT_FALSE(destructor_timed_out.load(std::memory_order_acquire));
        EXPECT_TRUE(worker_finished.load(std::memory_order_acquire));
        EXPECT_EQ(worker_error, nullptr);
        EXPECT_EQ(external_error, nullptr);
    }
}
