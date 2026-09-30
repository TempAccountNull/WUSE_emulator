#include <destiny_startup_capture.hpp>

#include <gtest/gtest.h>

namespace sogen::test
{
    TEST(DestinyStartupCaptureBudget, UnarmedCaptureHasNoDeadline)
    {
        const destiny_startup_capture_budget budget{};

        EXPECT_TRUE(budget.expiry(0).empty());
        EXPECT_TRUE(budget.expiry(300000).empty());
        EXPECT_TRUE(budget.expiry(1000000).empty());
    }

    TEST(DestinyStartupCaptureBudget, TotalDeadlineExpiresAtFiveMinutes)
    {
        destiny_startup_capture_budget budget{};
        budget.arm(7000);

        EXPECT_TRUE(budget.armed);
        EXPECT_EQ(budget.armed_at, 7000u);
        EXPECT_TRUE(budget.expiry(306999).empty());
        EXPECT_FALSE(budget.expiry(307000).empty());
        EXPECT_FALSE(budget.expiry(307001).empty());
    }

    TEST(DestinyStartupCaptureBudget, ZeroTimestampIsAValidArmTime)
    {
        destiny_startup_capture_budget budget{};
        budget.arm(0);
        budget.arm(299999);

        EXPECT_TRUE(budget.armed);
        EXPECT_EQ(budget.armed_at, 0u);
        EXPECT_TRUE(budget.expiry(299999).empty());
        EXPECT_FALSE(budget.expiry(300000).empty());
    }

    TEST(DestinyStartupCaptureBudget, RepeatedArmDoesNotExtendTheWindow)
    {
        destiny_startup_capture_budget budget{};
        budget.arm(5000);
        budget.arm(6000);
        budget.arm(304999);

        EXPECT_EQ(budget.armed_at, 5000u);
        EXPECT_TRUE(budget.expiry(304999).empty());
        EXPECT_FALSE(budget.expiry(305000).empty());
    }

    TEST(DestinyStartupCaptureBudget, CleanupRetainsTerminalCoverageForTwoMinutes)
    {
        destiny_startup_capture_budget budget{};
        budget.arm(5000);
        budget.observe_cleanup(25000);

        EXPECT_TRUE(budget.cleanup_seen);
        EXPECT_EQ(budget.cleanup_at, 25000u);
        EXPECT_TRUE(budget.expiry(25000).empty());
        EXPECT_TRUE(budget.expiry(144999).empty());
        EXPECT_FALSE(budget.expiry(145000).empty());
    }

    TEST(DestinyStartupCaptureBudget, RepeatedCleanupDoesNotExtendTerminalCoverage)
    {
        destiny_startup_capture_budget budget{};
        budget.arm(5000);
        budget.observe_cleanup(25000);
        budget.observe_cleanup(100000);
        budget.observe_cleanup(144999);

        EXPECT_EQ(budget.cleanup_at, 25000u);
        EXPECT_TRUE(budget.expiry(144999).empty());
        EXPECT_FALSE(budget.expiry(145000).empty());
    }

    TEST(DestinyStartupCaptureBudget, LateCleanupCannotExtendTheTotalDeadline)
    {
        destiny_startup_capture_budget budget{};
        budget.arm(5000);
        budget.observe_cleanup(255000);

        EXPECT_TRUE(budget.expiry(304999).empty());
        EXPECT_FALSE(budget.expiry(305000).empty());
        EXPECT_FALSE(budget.expiry(374999).empty());
    }

    TEST(DestinyStartupCaptureBudget, RepeatedArmPreservesAnExistingCleanupDeadline)
    {
        destiny_startup_capture_budget budget{};
        budget.arm(5000);
        budget.observe_cleanup(25000);
        budget.arm(100000);

        EXPECT_EQ(budget.armed_at, 5000u);
        EXPECT_TRUE(budget.cleanup_seen);
        EXPECT_EQ(budget.cleanup_at, 25000u);
        EXPECT_TRUE(budget.expiry(144999).empty());
        EXPECT_FALSE(budget.expiry(145000).empty());
    }
}
