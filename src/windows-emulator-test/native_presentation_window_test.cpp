#include <platform/native_presentation_window.hpp>
#include <platform/ui_completion_queue.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <latch>
#include <mutex>
#include <thread>
#include <vector>

namespace sogen::test
{
    namespace
    {
        struct resource_observations
        {
            std::thread::id owner{std::this_thread::get_id()};
            size_t destroyed{};
            size_t suspended{};
            size_t resumed{};
            size_t retired{};
            bool wrong_thread{};
            bool suspend_succeeds{true};
            bool resume_succeeds{true};
            uintptr_t replacement_window{};
        };

        class observed_window final : public native_presentation_window_resource
        {
          public:
            explicit observed_window(resource_observations& observations, const uintptr_t native_window = 0x3456)
                : observations_(observations),
                  target_{.window = native_window, .instance = 0x6789}
            {
            }

            ~observed_window() override
            {
                this->observe_thread();
                ++this->observations_.destroyed;
            }

            win32_presentation_target target() const noexcept override
            {
                return this->target_;
            }

            bool suspend_legacy_presentation() noexcept override
            {
                this->observe_thread();
                ++this->observations_.suspended;
                return this->observations_.suspend_succeeds;
            }

            bool resume_legacy_presentation() noexcept override
            {
                this->observe_thread();
                ++this->observations_.resumed;
                if (this->observations_.replacement_window != 0)
                {
                    this->target_.window = this->observations_.replacement_window;
                }
                return this->observations_.resume_succeeds;
            }

            void retire() noexcept override
            {
                this->observe_thread();
                ++this->observations_.retired;
            }

          private:
            void observe_thread() noexcept
            {
                this->observations_.wrong_thread |= this->observations_.owner != std::this_thread::get_id();
            }

            resource_observations& observations_;
            win32_presentation_target target_;
        };

        class NativePresentationWindowTest : public testing::Test
        {
          protected:
            resource_observations first;
            resource_observations second;
            native_presentation_window_registry registry;
            ui_completion_queue queue;

            void TearDown() override
            {
                queue.close();
                registry.close();
                registry.collect();
                EXPECT_TRUE(registry.drained());
                EXPECT_FALSE(first.wrong_thread);
                EXPECT_FALSE(second.wrong_thread);
            }

            uint64_t publish(const uint64_t guest = 8)
            {
                return registry.publish(guest, true, std::make_unique<observed_window>(first));
            }
        };
    }

    TEST(UiCompletionQueue, NeverRunsInlineEvenOnOwnerThread)
    {
        ui_completion_queue queue;
        std::mutex kernel_lock;
        std::unique_lock held(kernel_lock);
        bool called{};
        auto ticket = queue.post([&](const ui_cancellation_token&) {
            called = true;
            const bool acquired = kernel_lock.try_lock();
            if (acquired)
            {
                kernel_lock.unlock();
            }
            return acquired;
        });
        EXPECT_FALSE(called);
        EXPECT_FALSE(ticket.ready());
        held.unlock();
        EXPECT_EQ(queue.pump(), 1u);
        auto result = ticket.try_take();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, ui_completion_status::completed);
        EXPECT_EQ(result->value, true);
        EXPECT_FALSE(ticket.try_take());
    }

    TEST(UiCompletionQueue, FifoBudgetAndClose)
    {
        ui_completion_queue queue;
        std::vector<int> sequence;
        auto first = queue.post([&](const ui_cancellation_token&) {
            sequence.push_back(1);
            return 1;
        });
        auto second = queue.post([&](const ui_cancellation_token&) {
            sequence.push_back(2);
            return 2;
        });
        EXPECT_EQ(queue.pump(1), 1u);
        EXPECT_EQ(sequence, std::vector<int>({1}));
        EXPECT_TRUE(first.ready());
        EXPECT_FALSE(second.ready());
        queue.close();
        auto result = second.try_take();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, ui_completion_status::queue_closed);
        auto late = queue.post([](const ui_cancellation_token&) { return 3; });
        ASSERT_TRUE(late.ready());
        EXPECT_EQ(late.try_take()->status, ui_completion_status::queue_closed);
        EXPECT_EQ(sequence, std::vector<int>({1}));
    }

    TEST(UiCompletionQueue, PendingCancellationDoesNotInvokeOperation)
    {
        ui_completion_queue queue;
        bool called{};
        auto ticket = queue.post([&](const ui_cancellation_token&) {
            called = true;
            return 1;
        });
        ticket.cancel();
        ASSERT_TRUE(ticket.ready());
        EXPECT_EQ(ticket.try_take()->status, ui_completion_status::cancelled);
        queue.pump();
        EXPECT_FALSE(called);
    }

    TEST(UiCompletionQueue, DetachedAndNestedCommandsShareFifoWithoutHoldingQueueLock)
    {
        ui_completion_queue queue;
        std::vector<int> sequence;
        auto first = queue.post([&](const ui_cancellation_token&) {
            sequence.push_back(1);
            auto nested = queue.post([&](const ui_cancellation_token&) {
                sequence.push_back(3);
                return 3;
            });
            nested.detach();
            return 1;
        });
        first.detach();
        auto second = queue.post([&](const ui_cancellation_token&) {
            sequence.push_back(2);
            return 2;
        });
        queue.pump();
        EXPECT_EQ(sequence, std::vector<int>({1, 2}));
        queue.pump();
        EXPECT_EQ(sequence, std::vector<int>({1, 2, 3}));
        EXPECT_TRUE(second.ready());
    }

    TEST(UiCompletionQueue, RunningCancellationWaitsForActualCompletion)
    {
        ui_completion_queue queue;
        std::latch entered{1};
        std::latch cancelled{1};
        bool premature_completion{};
        auto ticket = queue.post([&](const ui_cancellation_token& token) {
            entered.count_down();
            cancelled.wait();
            EXPECT_TRUE(token.requested());
            return 42;
        });
        std::thread guest([&] {
            entered.wait();
            ticket.cancel();
            premature_completion = ticket.ready();
            cancelled.count_down();
        });
        queue.pump();
        guest.join();
        EXPECT_FALSE(premature_completion);
        auto result = ticket.try_take();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, ui_completion_status::cancelled);
        EXPECT_FALSE(result->value);
    }

    TEST(UiCompletionQueue, ExceptionAndWrongThreadAreReported)
    {
        ui_completion_queue queue;
        auto ticket = queue.post([](const ui_cancellation_token&) -> int { throw std::runtime_error("native failure"); });
        queue.pump();
        auto result = ticket.try_take();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, ui_completion_status::failed);
        EXPECT_NE(result->error, nullptr);
        EXPECT_THROW(std::rethrow_exception(result->error), std::runtime_error);
        bool rejected{};
        std::thread worker([&] {
            try
            {
                queue.pump();
            }
            catch (const std::logic_error&)
            {
                rejected = true;
            }
        });
        worker.join();
        EXPECT_TRUE(rejected);
    }

    TEST_F(NativePresentationWindowTest, RetiredWindowOutlivesSurfaceAndSwapchainLeases)
    {
        const auto generation = publish();
        auto acquired = registry.acquire(8, generation);
        ASSERT_EQ(acquired.status, native_window_acquire_status::success);
        auto surface = std::move(acquired.lease);
        auto swapchain = surface;
        registry.retire(8);
        EXPECT_FALSE(surface.live());
        EXPECT_EQ(surface.target().window, 0x3456u);
        EXPECT_EQ(registry.collect().destroyed, 0u);
        surface = {};
        EXPECT_EQ(registry.collect().destroyed, 0u);
        std::thread guest([lease = std::move(swapchain)]() mutable { lease = {}; });
        guest.join();
        EXPECT_EQ(first.destroyed, 0u);
        EXPECT_EQ(registry.collect().destroyed, 1u);
        EXPECT_EQ(first.destroyed, 1u);
        EXPECT_EQ(first.resumed, 0u);
        EXPECT_TRUE(registry.drained());
    }

    TEST_F(NativePresentationWindowTest, ResetAndHandleReuseNeverRebindAnOldLease)
    {
        const auto old_generation = publish();
        auto old = registry.acquire(8).lease;
        registry.reset();
        const auto new_generation = registry.publish(8, true, std::make_unique<observed_window>(second, 0xabcdef));
        EXPECT_GT(new_generation, old_generation);
        EXPECT_FALSE(old.live());
        EXPECT_EQ(old.target().window, 0x3456u);
        EXPECT_EQ(registry.acquire(8, old_generation).status, native_window_acquire_status::stale_generation);
        auto current = registry.acquire(8, new_generation);
        EXPECT_EQ(current.status, native_window_acquire_status::success);
        EXPECT_EQ(current.lease.target().window, 0xabcdefu);
        old = {};
        EXPECT_EQ(registry.collect().destroyed, 1u);
        EXPECT_EQ(second.destroyed, 0u);
    }

    TEST_F(NativePresentationWindowTest, LegacyPresentationResumesOnlyAfterLastLease)
    {
        publish();
        auto first_lease = registry.acquire(8).lease;
        auto second_lease = registry.acquire(8).lease;
        EXPECT_EQ(first.suspended, 1u);
        first_lease = {};
        EXPECT_EQ(registry.collect().restored, 0u);
        second_lease = {};
        EXPECT_EQ(registry.collect().restored, 1u);
        EXPECT_EQ(first.resumed, 1u);
        auto reacquired = registry.acquire(8).lease;
        EXPECT_EQ(first.suspended, 2u);
        reacquired = {};
        first.resume_succeeds = false;
        EXPECT_EQ(registry.collect().restore_failed, 1u);
    }

    TEST_F(NativePresentationWindowTest, ChildAndMissingNativeTargetsAreHonestFailures)
    {
        registry.publish(8, false);
        registry.publish(12, true);
        EXPECT_EQ(registry.acquire(8).status, native_window_acquire_status::child_window_unsupported);
        EXPECT_EQ(registry.acquire(12).status, native_window_acquire_status::native_target_unavailable);
        EXPECT_EQ(registry.acquire(99).status, native_window_acquire_status::unknown_window);
        publish(16);
        first.suspend_succeeds = false;
        auto failed = registry.acquire(16);
        EXPECT_EQ(failed.status, native_window_acquire_status::legacy_handoff_failed);
        EXPECT_FALSE(failed.lease);
        EXPECT_EQ(registry.collect().restored, 0u);
        registry.close();
        EXPECT_EQ(registry.acquire(16).status, native_window_acquire_status::registry_closed);
    }

    TEST_F(NativePresentationWindowTest, QueuedCreatePrecedesAcquireAndCompletedTicketPinsWindow)
    {
        auto create = queue.post([&](const ui_cancellation_token&) { return publish(); });
        ui_completion_ticket<native_window_acquisition> acquire;
        std::thread guest([&] { acquire = queue.post([&](const ui_cancellation_token&) { return registry.acquire(8); }); });
        guest.join();
        EXPECT_FALSE(acquire.ready());
        queue.pump();
        EXPECT_TRUE(create.ready());
        EXPECT_TRUE(acquire.ready());
        registry.retire(8);
        EXPECT_EQ(registry.collect().destroyed, 0u);
        std::thread abandoning_guest([ticket = std::move(acquire)]() mutable { ticket = {}; });
        abandoning_guest.join();
        EXPECT_EQ(first.destroyed, 0u);
        EXPECT_EQ(registry.collect().destroyed, 1u);
    }

    TEST_F(NativePresentationWindowTest, ClosingWithLiveLeaseDoesNotClaimDrainOrDestroy)
    {
        publish();
        auto lease = registry.acquire(8).lease;
        registry.close();
        EXPECT_FALSE(registry.drained());
        EXPECT_EQ(registry.collect().destroyed, 0u);
        EXPECT_EQ(first.destroyed, 0u);
        lease = {};
        EXPECT_EQ(registry.collect().destroyed, 1u);
        EXPECT_TRUE(registry.drained());
    }

    TEST_F(NativePresentationWindowTest, CancellationDuringAcquisitionReleasesPinOnUiThread)
    {
        publish();
        std::latch acquired{1};
        std::latch cancelled{1};
        auto ticket = queue.post([&](const ui_cancellation_token&) {
            auto value = registry.acquire(8);
            acquired.count_down();
            cancelled.wait();
            return value;
        });
        std::thread guest([&] {
            acquired.wait();
            ticket.cancel();
            cancelled.count_down();
        });
        queue.pump();
        guest.join();
        auto result = ticket.try_take();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, ui_completion_status::cancelled);
        EXPECT_FALSE(result->value);
        EXPECT_EQ(first.suspended, 1u);
        EXPECT_EQ(registry.collect().restored, 1u);
        EXPECT_EQ(first.destroyed, 0u);
    }

    TEST_F(NativePresentationWindowTest, GuestIdentityPreservesHighBitsAndRegistryIsUiAffine)
    {
        constexpr uint64_t guest_window = 0x1234567800000008ull;
        publish(guest_window);
        EXPECT_EQ(registry.acquire(8).status, native_window_acquire_status::unknown_window);
        auto value = registry.acquire(guest_window);
        EXPECT_EQ(value.lease.guest_window(), guest_window);
        EXPECT_EQ(value.lease.target().window, 0x3456u);
        bool rejected{};
        std::thread guest([&] {
            try
            {
                registry.acquire(guest_window);
            }
            catch (const std::logic_error&)
            {
                rejected = true;
            }
        });
        guest.join();
        EXPECT_TRUE(rejected);
    }

    TEST_F(NativePresentationWindowTest, FailedRendererRestoreRetriesAndRefreshesRecreatedTarget)
    {
        const auto original = publish();
        auto acquired = registry.acquire(8, original);
        ASSERT_EQ(acquired.status, native_window_acquire_status::success);
        acquired.lease = {};
        first.resume_succeeds = false;
        first.replacement_window = 0xABC0;
        EXPECT_EQ(registry.collect().restore_failed, 1u);
        EXPECT_EQ(registry.acquire(8, original).status, native_window_acquire_status::stale_generation);
        first.resume_succeeds = true;
        EXPECT_EQ(registry.collect().restored, 1u);
        EXPECT_EQ(first.resumed, 2u);
        auto current = registry.acquire(8);
        ASSERT_EQ(current.status, native_window_acquire_status::success);
        EXPECT_NE(current.lease.generation(), original);
        EXPECT_EQ(current.lease.target().window, 0xABC0u);
    }
}
