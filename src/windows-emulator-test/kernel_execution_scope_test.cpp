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
}
