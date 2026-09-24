#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <memory_manager.hpp>
#include <utils/finally.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>

namespace sogen::test
{
    TEST(IcicleSmp, SchedulerWorkerUnmapQueuesPeerParkedInHook)
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

        // A direct start() alone must not select the asynchronous owner route.
        // Only the explicit scheduler binding below can do that between quanta.
        auto& owner = emu->get_cpu(0);
        owner.reg(x86_register::rip, code);
        owner.start(1);
        ASSERT_EQ(owner.reg(x86_register::rip), code + 1);

        // Model the scheduler's kernel lock: the owner holds it while releasing a thread's
        // memory, and a peer callback cannot leave its quantum until it acquires the lock.
        std::timed_mutex kernel_lock;
        std::unique_lock owner_lock(kernel_lock);
        std::atomic<bool> in_hook{false};
        std::atomic<bool> unmap_returned{false};
        std::atomic<bool> hook_saw_unmap_return{false};
        emu->hook_memory_execution(code + 0x100, [&](cpu_interface&, uint64_t) {
            in_hook.store(true, std::memory_order_release);
            if (kernel_lock.try_lock_for(std::chrono::seconds(2)))
            {
                hook_saw_unmap_return.store(unmap_returned.load(std::memory_order_acquire),
                                          std::memory_order_release);
                kernel_lock.unlock();
            }
        });

        std::exception_ptr peer_error;
        auto& peer_cpu = emu->get_cpu(1);
        peer_cpu.reg(x86_register::rip, code + 0x100);
        std::thread peer([&] {
            try
            {
                peer_cpu.start(1);
                emu->sync_worker_context(1); // queued unmap drains on the peer's owner thread
            }
            catch (...)
            {
                peer_error = std::current_exception();
            }
        });

        const auto hook_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!in_hook.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < hook_deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!in_hook.load(std::memory_order_acquire))
        {
            owner_lock.unlock();
            peer.join();
            FAIL() << "peer never entered its execution hook";
        }

        emu->set_scheduler_worker_context(0, true);
        {
            const auto clear_worker = utils::finally([&] { emu->set_scheduler_worker_context(0, false); });
            EXPECT_TRUE(memory.release_memory(target, 0));
            unmap_returned.store(true, std::memory_order_release);
        }
        owner_lock.unlock();

        peer.join();
        if (peer_error)
        {
            try
            {
                std::rethrow_exception(peer_error);
            }
            catch (const std::exception& e)
            {
                FAIL() << "peer start failed: " << e.what();
            }
        }
        EXPECT_TRUE(hook_saw_unmap_return.load(std::memory_order_acquire))
            << "scheduler worker waited for a peer blocked in its guest hook";

        const auto issued = emu->smp_op_watermark();
        EXPECT_GT(issued, 0U);
        EXPECT_TRUE(emu->smp_op_applied(issued));
    }

    TEST(IcicleSmp, SchedulerWorkerMapCanReadAndWriteBeforePeerDrain)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        // Bind vCPU 1 before its first guest quantum. No start()-scoped TLS owner
        // exists on this host thread, so only the scheduler binding can select the VM.
        uint64_t target = 0;
        constexpr uint64_t marker = 0x76543210abcdef98ULL;
        emu->set_scheduler_worker_context(1, true);
        {
            const auto clear_worker = utils::finally([&] { emu->set_scheduler_worker_context(1, false); });
            target = memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(target, 0U);
            emu->write_memory(target, &marker, sizeof(marker));
            uint64_t local_read{};
            emu->read_memory(target, &local_read, sizeof(local_read));
            EXPECT_EQ(local_read, marker);
        }

        const auto issued = emu->smp_op_watermark();
        EXPECT_GT(issued, 0U);
        emu->sync_worker_context(0);
        EXPECT_TRUE(emu->smp_op_applied(issued));
        uint64_t master_read{};
        emu->read_memory(target, &master_read, sizeof(master_read));
        EXPECT_EQ(master_read, marker);
    }

    TEST(IcicleSmp, FreshPeerWorkerRetriesQueuedMapReadAndWrite)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        constexpr uint64_t a_value = 0x17aabbccdd001122ULL;
        constexpr uint64_t b_value = 0x29eeff0011334455ULL;
        constexpr uint64_t peer_value = 0x8a77665544332211ULL;

        emu->set_scheduler_worker_context(0, true);
        uint64_t page_a{};
        uint64_t mark_a{};
        {
            const auto clear_issuer = utils::finally([&] { emu->set_scheduler_worker_context(0, false); });
            page_a = memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(page_a, 0U);
            emu->write_memory(page_a, &a_value, sizeof(a_value));
            mark_a = emu->smp_op_watermark();
        }
        ASSERT_GT(mark_a, 0U);
        ASSERT_FALSE(emu->smp_op_applied(mark_a));

        std::atomic<int> phase{0}; // 1: peer read A; 2: issuer mapped B; 3: peer wrote B
        std::atomic<uint64_t> page_b{0};
        std::atomic<uint64_t> mark_b{0};
        bool read_a_ok = false;
        bool wrote_b_ok = false;
        bool read_b_ok = false;
        bool a_applied = false;
        bool b_pending_before_write = false;
        bool b_applied_after_write = false;
        uint64_t observed_a{};
        uint64_t observed_b{};
        std::exception_ptr peer_error;
        std::thread peer([&] {
            emu->set_scheduler_worker_context(1, true); // no start() on this fresh host thread
            const auto clear_peer = utils::finally([&] { emu->set_scheduler_worker_context(1, false); });
            try
            {
                read_a_ok = emu->try_read_memory(page_a, &observed_a, sizeof(observed_a));
                a_applied = emu->smp_op_applied(mark_a);
                phase.store(1, std::memory_order_release);

                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (phase.load(std::memory_order_acquire) == 1 &&
                       std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (phase.load(std::memory_order_acquire) != 2)
                {
                    return;
                }
                const auto b = page_b.load(std::memory_order_acquire);
                const auto watermark = mark_b.load(std::memory_order_acquire);
                b_pending_before_write = !emu->smp_op_applied(watermark);
                wrote_b_ok = emu->try_write_memory(b + 8, &peer_value, sizeof(peer_value));
                b_applied_after_write = emu->smp_op_applied(watermark);
                read_b_ok = emu->try_read_memory(b, &observed_b, sizeof(observed_b));
                phase.store(3, std::memory_order_release);
            }
            catch (...)
            {
                peer_error = std::current_exception();
                phase.store(3, std::memory_order_release);
            }
        });

        const auto read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (phase.load(std::memory_order_acquire) == 0 &&
               std::chrono::steady_clock::now() < read_deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (phase.load(std::memory_order_acquire) != 1)
        {
            phase.store(2, std::memory_order_release);
            peer.join();
            FAIL() << "fresh peer did not finish its queued-map read";
        }

        emu->set_scheduler_worker_context(0, true);
        uint64_t b{};
        uint64_t issued_b{};
        {
            const auto clear_issuer = utils::finally([&] { emu->set_scheduler_worker_context(0, false); });
            b = memory.allocate_memory(0x1000, memory_permission::read_write);
            EXPECT_NE(b, 0U);
            if (b)
            {
                emu->write_memory(b, &b_value, sizeof(b_value));
                issued_b = emu->smp_op_watermark();
            }
        }
        page_b.store(b, std::memory_order_release);
        mark_b.store(issued_b, std::memory_order_release);
        phase.store(2, std::memory_order_release);
        peer.join();

        if (peer_error)
        {
            try
            {
                std::rethrow_exception(peer_error);
            }
            catch (const std::exception& e)
            {
                FAIL() << "fresh peer failed: " << e.what();
            }
        }
        ASSERT_NE(b, 0U);
        EXPECT_EQ(phase.load(std::memory_order_acquire), 3);
        EXPECT_TRUE(read_a_ok);
        EXPECT_EQ(observed_a, a_value);
        EXPECT_TRUE(a_applied);
        EXPECT_GT(issued_b, mark_a);
        EXPECT_TRUE(b_pending_before_write);
        EXPECT_TRUE(wrote_b_ok);
        EXPECT_TRUE(b_applied_after_write);
        EXPECT_TRUE(read_b_ok);
        EXPECT_EQ(observed_b, b_value);
        uint64_t owner_read{};
        emu->read_memory(b + 8, &owner_read, sizeof(owner_read));
        EXPECT_EQ(owner_read, peer_value);
    }

    TEST(IcicleSmp, DirectStartCallerStillMutatesSynchronously)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto target = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0U);
        ASSERT_NE(target, 0U);
        const uint8_t nop = 0x90;
        emu->write_memory(code, &nop, sizeof(nop));

        auto& owner = emu->get_cpu(0);
        owner.reg(x86_register::rip, code);
        owner.start(1);
        emu->set_scheduler_worker_context(0, true);
        emu->set_scheduler_worker_context(0, false);
        const auto before = emu->smp_op_watermark();
        const auto extra = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(extra, 0U);
        EXPECT_EQ(emu->smp_op_watermark(), before) << "external map queued a peer op";
        ASSERT_TRUE(memory.protect_memory(extra, 0x1000, memory_permission::read));
        EXPECT_EQ(emu->smp_op_watermark(), before) << "external protect queued a peer op";
        ASSERT_TRUE(memory.release_memory(target, 0));
        ASSERT_TRUE(memory.release_memory(extra, 0));
        EXPECT_EQ(emu->smp_op_watermark(), before)
            << "a direct start() caller without scheduler binding must retain the synchronous external route";
    }
}
