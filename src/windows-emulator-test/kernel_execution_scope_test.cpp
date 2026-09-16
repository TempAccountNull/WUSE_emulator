#include <gtest/gtest.h>
#include <kernel_lock.hpp>
#include <atomic>
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
}
