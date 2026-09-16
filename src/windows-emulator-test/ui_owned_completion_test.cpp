#include <platform/ui_owned_completion.hpp>
#include <gtest/gtest.h>
#include <latch>

namespace sogen::test
{
    namespace
    {
        using owned_number = ui_owned_completion<unsigned>;
    }

    TEST(UiOwnedCompletion, CancelledBeforeExecutionCreatesNothing)
    {
        auto queue = std::make_shared<ui_completion_queue>();
        size_t created{};
        size_t destroyed{};
        auto ticket = queue->post([&](const ui_cancellation_token&) {
            ++created;
            return std::make_shared<owned_number>(queue, [&](unsigned) { ++destroyed; });
        });
        ticket.cancel();
        queue->pump();
        EXPECT_EQ(created, 0u);
        EXPECT_EQ(destroyed, 0u);
    }

    TEST(UiOwnedCompletion, AbandonedCompletedResultDisposesOnOwner)
    {
        auto queue = std::make_shared<ui_completion_queue>();
        const auto owner = std::this_thread::get_id();
        unsigned destroyed{};
        {
            auto ticket = queue->post([&](const ui_cancellation_token&) {
                auto result = std::make_shared<owned_number>(queue, [&](unsigned value) {
                    EXPECT_EQ(std::this_thread::get_id(), owner);
                    destroyed = value;
                });
                result->value = 42;
                return result;
            });
            queue->pump();
            EXPECT_TRUE(ticket.ready());
        }
        EXPECT_EQ(destroyed, 0u);
        queue->pump();
        EXPECT_EQ(destroyed, 42u);
    }

    TEST(UiOwnedCompletion, CancelDuringRunningTaskDisposesAfterNativeReturn)
    {
        auto queue = std::make_shared<ui_completion_queue>();
        const auto owner = std::this_thread::get_id();
        std::latch running{1};
        std::latch cancelled{1};
        bool returned{};
        bool destroyed{};
        auto ticket = queue->post([&](const ui_cancellation_token&) {
            auto result = std::make_shared<owned_number>(queue, [&](unsigned) {
                EXPECT_EQ(std::this_thread::get_id(), owner);
                EXPECT_TRUE(returned);
                destroyed = true;
            });
            running.count_down();
            cancelled.wait();
            returned = true;
            return result;
        });
        std::thread other([&] {
            running.wait();
            ticket.cancel();
            cancelled.count_down();
        });
        queue->pump();
        other.join();
        EXPECT_FALSE(destroyed);
        EXPECT_EQ(ticket.try_take()->status, ui_completion_status::cancelled);
        queue->pump();
        EXPECT_TRUE(destroyed);
    }

    TEST(UiOwnedCompletion, ReceiverAbandonmentQueuesDisposalAndAdoptionSuppressesIt)
    {
        auto queue = std::make_shared<ui_completion_queue>();
        const auto owner = std::this_thread::get_id();
        unsigned destroyed{};
        auto make = [&] {
            return queue->post([&](const ui_cancellation_token&) {
                return std::make_shared<owned_number>(queue, [&](unsigned) {
                    EXPECT_EQ(std::this_thread::get_id(), owner);
                    ++destroyed;
                });
            });
        };
        auto abandoned = make();
        queue->pump();
        auto completed = abandoned.try_take();
        std::thread receiver([value = std::move(completed)]() mutable { value.reset(); });
        receiver.join();
        EXPECT_EQ(destroyed, 0u);
        queue->pump();
        EXPECT_EQ(destroyed, 1u);
        auto accepted = make();
        queue->pump();
        auto result = accepted.try_take();
        (*result->value)->adopt();
        result.reset();
        queue->pump();
        EXPECT_EQ(destroyed, 1u);
    }
}
