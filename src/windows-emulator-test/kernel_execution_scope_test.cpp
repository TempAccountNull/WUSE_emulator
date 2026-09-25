#include <gtest/gtest.h>
#include <kernel_lock.hpp>
#include <atomic>
#include <chrono>
#include <thread>

namespace sogen::test
{
    TEST(KernelExecutionScope, CallbackBorrowKeepsOtherThreadsExcluded)
    {
        kernel_lock lock;
        std::atomic<bool> tried{};
        std::atomic<bool> acquired{};
        {
            const kernel_lock::guest_execution_scope scope(lock, true);
            {
                const std::scoped_lock callback(lock);
                EXPECT_TRUE(lock.is_held_by_current_thread());
            }
            std::thread contender([&] {
                acquired = lock.try_lock();
                if (acquired)
                {
                    lock.unlock();
                }
                tried = true;
            });
            contender.join();
            EXPECT_TRUE(tried);
            EXPECT_FALSE(acquired);
            EXPECT_TRUE(lock.try_lock());
            lock.unlock();
        }
        std::thread next([&] {
            acquired = lock.try_lock();
            if (acquired)
            {
                lock.unlock();
            }
        });
        next.join();
        EXPECT_TRUE(acquired);
    }

    TEST(KernelExecutionScope, ExceptionReleasesCallbackAndQuantumOwnership)
    {
        kernel_lock lock;
        try
        {
            const kernel_lock::guest_execution_scope scope(lock, true);
            const std::scoped_lock callback(lock);
            throw 1;
        }
        catch (int)
        {
        }
        EXPECT_TRUE(lock.try_lock());
        lock.unlock();
    }

    TEST(KernelExecutionScope, DisabledScopeAllowsIndependentHostThreads)
    {
        kernel_lock lock;
        const kernel_lock::guest_execution_scope scope(lock, false);
        std::atomic<bool> acquired{};
        std::thread contender([&] {
            acquired = lock.try_lock();
            if (acquired)
            {
                lock.unlock();
            }
        });
        contender.join();
        EXPECT_TRUE(acquired);
    }

    TEST(KernelExecutionScope, CallbackExceptionLeavesQuantumBorrowAvailable)
    {
        kernel_lock lock;
        {
            const kernel_lock::guest_execution_scope scope(lock, true);
            try
            {
                const std::scoped_lock callback(lock);
                throw 1;
            }
            catch (int)
            {
            }
            {
                const std::scoped_lock next_callback(lock);
                EXPECT_TRUE(lock.is_held_by_current_thread());
            }
            std::atomic<bool> acquired{};
            std::thread contender([&] {
                acquired = lock.try_lock();
                if (acquired)
                {
                    lock.unlock();
                }
            });
            contender.join();
            EXPECT_FALSE(acquired);
        }
        EXPECT_TRUE(lock.try_lock());
        lock.unlock();
    }

    TEST(KernelExecutionScope, AnotherLockUsesItsOwnMutexDuringBorrowedCallback)
    {
        kernel_lock quantum_lock;
        kernel_lock other_lock;
        const kernel_lock::guest_execution_scope scope(quantum_lock, true);
        const std::scoped_lock callback(quantum_lock);
        {
            const std::scoped_lock independent(other_lock);
            EXPECT_TRUE(quantum_lock.is_held_by_current_thread());
            EXPECT_TRUE(other_lock.is_held_by_current_thread());
            std::atomic<bool> acquired{};
            std::thread contender([&] {
                acquired = other_lock.try_lock();
                if (acquired)
                {
                    other_lock.unlock();
                }
            });
            contender.join();
            EXPECT_FALSE(acquired);
        }
        std::atomic<bool> acquired{};
        std::thread contender([&] {
            acquired = other_lock.try_lock();
            if (acquired)
            {
                other_lock.unlock();
            }
        });
        contender.join();
        EXPECT_TRUE(acquired);
        EXPECT_TRUE(quantum_lock.is_held_by_current_thread());
    }

    TEST(KernelExecutionScope, ProfilingCountsPhysicalOwnershipAndNotBorrowedCallbacks)
    {
        kernel_lock lock;
        const auto before = lock.profile();
        {
            const kernel_lock::guest_execution_scope scope(lock, true);
            const auto quantum = lock.profile();
            {
                const std::scoped_lock callback(lock);
            }
            ASSERT_TRUE(lock.try_lock());
            lock.unlock();
            const auto after_callbacks = lock.profile();
            EXPECT_EQ(after_callbacks.acquisitions, quantum.acquisitions);
            EXPECT_EQ(after_callbacks.contended, quantum.contended);
            EXPECT_EQ(after_callbacks.wait_nanos, quantum.wait_nanos);
            EXPECT_EQ(after_callbacks.held_nanos, quantum.held_nanos);
        }
        const auto after_quantum = lock.profile();
        const auto expected_acquisitions = before.acquisitions + (kernel_lock::profiling_enabled() ? 1U : 0U);
        EXPECT_EQ(after_quantum.acquisitions, expected_acquisitions);
        EXPECT_EQ(after_quantum.contended, before.contended);
        EXPECT_EQ(after_quantum.wait_nanos, before.wait_nanos);
        EXPECT_GE(after_quantum.held_nanos, before.held_nanos);
        {
            const std::scoped_lock independent(lock);
            EXPECT_TRUE(lock.is_held_by_current_thread());
        }
        EXPECT_FALSE(lock.is_held_by_current_thread());
        EXPECT_EQ(lock.profile().acquisitions, expected_acquisitions + (kernel_lock::profiling_enabled() ? 1U : 0U));
    }

    TEST(KernelExecutionScope, AttributionReportsActiveOwnerWithoutTakingLock)
    {
        if (!kernel_lock::attribution_enabled())
        {
            GTEST_SKIP() << "Run with SOGEN_LOCK_ATTRIBUTION=1";
        }

        kernel_lock lock;
        std::atomic<bool> ready{};
        std::atomic<bool> release{};
        std::thread holder([&] {
            const kernel_lock::attribution_scope site("test_holder", 7);
            const std::scoped_lock physical_lock(lock);
            ready.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }
        if (ready.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            const auto owner = lock.current_owner();
            EXPECT_TRUE(owner);
            if (owner)
            {
                EXPECT_STREQ(owner.site, "test_holder");
                EXPECT_EQ(owner.detail, 7U);
                EXPECT_GE(owner.held_nanos, 1'000'000U);
            }
            EXPECT_FALSE(lock.try_lock());
        }
        release.store(true, std::memory_order_release);
        holder.join();
        EXPECT_TRUE(ready);
        EXPECT_FALSE(lock.current_owner());
    }

    TEST(KernelExecutionScope, BorrowedCallbackKeepsPhysicalOwnerAttribution)
    {
        if (!kernel_lock::attribution_enabled())
        {
            GTEST_SKIP() << "Run with SOGEN_LOCK_ATTRIBUTION=1";
        }

        kernel_lock lock;
        {
            const kernel_lock::attribution_scope outer("test_quantum", 3);
            const kernel_lock::guest_execution_scope quantum(lock, true);
            const auto physical_owner = lock.current_owner();
            EXPECT_TRUE(physical_owner);
            {
                const kernel_lock::attribution_scope inner("test_callback", 4);
                const std::scoped_lock callback(lock);
                const auto borrowed_owner = lock.current_owner();
                EXPECT_EQ(borrowed_owner.generation, physical_owner.generation);
                EXPECT_EQ(borrowed_owner.site, physical_owner.site);
                EXPECT_EQ(borrowed_owner.detail, physical_owner.detail);
            }
        }
        EXPECT_FALSE(lock.current_owner());

        // The nested callback site must not leak into a later physical acquisition.
        kernel_lock second_lock;
        {
            const kernel_lock::attribution_scope next("test_next", 9);
            const std::scoped_lock next_owner(second_lock);
            const auto owner = second_lock.current_owner();
            EXPECT_TRUE(owner);
            if (owner)
            {
                EXPECT_STREQ(owner.site, "test_next");
                EXPECT_EQ(owner.detail, 9U);
            }
        }
    }
}
