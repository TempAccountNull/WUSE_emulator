#include "../windows-emulator/thread_handle_journal.hpp"

#include <gtest/gtest.h>

namespace sogen::test
{
    TEST(ThreadHandleJournal, DisabledJournalKeepsNoHistoryOrFailureSamples)
    {
        thread_handle_journal journal(false);
        const auto thread = make_handle(41, handle_types::thread, false);
        journal.record({.action = thread_handle_journal::operation::create, .value = thread, .target_tid = 12});

        EXPECT_TRUE(journal.recent(thread).empty());
        EXPECT_TRUE(journal.recent_by_id(41).empty());
        EXPECT_FALSE(journal.claim_failure_sample());
    }

    TEST(ThreadHandleJournal, FindsStaleRawThreadSlotHistoryInSequenceOrder)
    {
        thread_handle_journal journal(true);
        const auto thread = make_handle(41, handle_types::thread, false);
        const auto raw = make_handle(41, handle_types::reserved, false);
        const auto other = make_handle(42, handle_types::thread, false);
        journal.record({.action = thread_handle_journal::operation::create, .value = thread, .target_tid = 12});
        journal.record({.action = thread_handle_journal::operation::create, .value = other, .target_tid = 16});
        journal.record({.action = thread_handle_journal::operation::close, .value = thread, .target_tid = 12, .removed = true});

        EXPECT_TRUE(journal.recent(raw).empty());
        const auto history = journal.recent_by_id(static_cast<uint32_t>(raw.value.id));
        ASSERT_EQ(history.size(), 2U);
        EXPECT_EQ(history[0].sequence, 1U);
        EXPECT_EQ(history[0].action, thread_handle_journal::operation::create);
        EXPECT_EQ(history[1].sequence, 3U);
        EXPECT_EQ(history[1].action, thread_handle_journal::operation::close);
        EXPECT_TRUE(history[1].removed);
    }

    TEST(ThreadHandleJournal, RingAndFailureBudgetResetForRestoredTimeline)
    {
        thread_handle_journal journal(true);
        const auto thread = make_handle(41, handle_types::thread, false);
        for (uint32_t i = 0; i < 520; ++i)
        {
            journal.record({.action = thread_handle_journal::operation::open, .value = thread, .target_tid = 12, .detail = i});
        }

        const auto history = journal.recent(thread);
        ASSERT_EQ(history.size(), 32U);
        EXPECT_EQ(history.front().sequence, 489U);
        EXPECT_EQ(history.back().sequence, 520U);
        for (unsigned i = 0; i < 16; ++i)
        {
            EXPECT_TRUE(journal.claim_failure_sample());
        }
        EXPECT_FALSE(journal.claim_failure_sample());

        journal.clear();
        EXPECT_TRUE(journal.recent(thread).empty());
        EXPECT_TRUE(journal.claim_failure_sample());
        journal.record({.action = thread_handle_journal::operation::create, .value = thread, .target_tid = 12});
        ASSERT_EQ(journal.recent(thread).size(), 1U);
        EXPECT_EQ(journal.recent(thread)[0].sequence, 1U);
    }
}
