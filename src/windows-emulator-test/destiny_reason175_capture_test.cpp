#include <destiny_startup_capture.hpp>
#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace sogen::test
{
    namespace
    {
        using state = destiny_reason175_capture_state;

        state requested(const uint32_t reason = 175)
        {
            state capture{};
            capture.arm(100);
            capture.observe_request(7, reason, 0x1000, 0xABC, 200, 1);
            return capture;
        }

        state with_setter()
        {
            auto capture = requested();
            capture.observe_setter(7, 28, 175, 0x5000, 0x1000, 0xABC, true);
            return capture;
        }
    }

    TEST(DestinyReason175Capture, AllFourInstructionGuardsMatchOnlyTheirExactBytes)
    {
        for (size_t site = 0; site < state::site_count; ++site)
        {
            EXPECT_TRUE(state::guard_matches(site, state::bytes[site].data(), state::lengths[site]));
        }
        EXPECT_EQ(state::rvas[0], 0xD3EB48u);
        EXPECT_EQ(state::rvas[1], 0xE2DEB0u);
        EXPECT_EQ(state::site_count, 4u);
    }

    TEST(DestinyReason175Capture, EveryChangedInstructionByteRejectsInstallation)
    {
        for (size_t site = 0; site < state::site_count; ++site)
        {
            for (size_t byte = 0; byte < state::lengths[site]; ++byte)
            {
                auto changed = state::bytes[site];
                changed[byte] ^= 1;
                EXPECT_FALSE(state::guard_matches(site, changed.data(), state::lengths[site]));
            }
        }
    }

    TEST(DestinyReason175Capture, ShortNullAndInvalidSiteGuardReadsRejectInstallation)
    {
        for (size_t site = 0; site < state::site_count; ++site)
        {
            EXPECT_FALSE(state::guard_matches(site, state::bytes[site].data(), state::lengths[site] - 1));
        }
        EXPECT_FALSE(state::guard_matches(0, nullptr, 2));
        EXPECT_FALSE(state::guard_matches(4, state::bytes[0].data(), 2));
    }

    TEST(DestinyReason175Capture, UnarmedObserverDoesNotInventCoverage)
    {
        state capture{};
        EXPECT_TRUE(capture.expiry(1000000).empty());
        EXPECT_FALSE(capture.first_seen);
        EXPECT_FALSE(capture.producer_verified);
    }

    TEST(DestinyReason175Capture, RepeatedArmCannotExtendCoverage)
    {
        state capture{};
        capture.arm(0);
        capture.arm(59000);
        EXPECT_EQ(capture.armed_at, 0u);
        EXPECT_EQ(capture.expiry(60000), "total_budget");
    }

    TEST(DestinyReason175Capture, TotalAdmissionDeadlineIsExactlySixtySeconds)
    {
        state capture{};
        capture.arm(100);
        EXPECT_TRUE(capture.expiry(60099).empty());
        EXPECT_EQ(capture.expiry(60100), "total_budget");
    }

    TEST(DestinyReason175Capture, FirstAnyReasonStartsOneTwoSecondClosure)
    {
        auto capture = requested(176);
        EXPECT_TRUE(capture.expiry(2199).empty());
        EXPECT_EQ(capture.expiry(2200), "first_request_closure_budget");
    }

    TEST(DestinyReason175Capture, ClosureCannotExtendTheOriginalDeadline)
    {
        state capture{};
        capture.arm(100);
        capture.observe_request(7, 175, 0x1000, 0xABC, 60000, 1);
        EXPECT_EQ(capture.expiry(60100), "total_budget");
    }

    TEST(DestinyReason175Capture, ARepeatedRequestNeitherReplacesFirstNorExtendsClosure)
    {
        auto capture = requested();
        EXPECT_FALSE(capture.observe_request(7, 175, 0x1000, 0xABC, 2000, 2));
        EXPECT_EQ(capture.first_at, 200u);
        EXPECT_EQ(capture.first_sequence, 1u);
        EXPECT_TRUE(capture.ambiguous);
        EXPECT_FALSE(capture.expiry(2200).empty());
    }

    TEST(DestinyReason175Capture, EarlierNon175ReasonCannotBeReplacedByLater175)
    {
        auto capture = requested(176);
        EXPECT_FALSE(capture.observe_request(7, 175, 0x2000, 0xDEF, 300, 2));
        EXPECT_EQ(capture.reason, 176u);
        EXPECT_EQ(capture.request_caller, 0xABCu);
        EXPECT_TRUE(capture.ambiguous);
    }

    TEST(DestinyReason175Capture, UnspecifiedNativeReasonRemainsExact)
    {
        auto capture = requested(0xFFFFFFFF);
        EXPECT_EQ(capture.reason, 0xFFFFFFFFu);
        EXPECT_EQ(capture.first_sequence, 1u);
    }

    TEST(DestinyReason175Capture, SetterRequiresExactThreadFrameArgumentsAndManager)
    {
        auto capture = requested();
        EXPECT_TRUE(capture.observe_setter(7, 28, 175, 0x5000, 0x1000, 0xABC, true));
        EXPECT_EQ(capture.manager, 0x5000u);
        EXPECT_TRUE(capture.setter_seen);
        EXPECT_FALSE(capture.ambiguous);
    }

    TEST(DestinyReason175Capture, SameValuesOnAnotherThreadDoNotCorrelate)
    {
        auto capture = requested();
        EXPECT_FALSE(capture.observe_setter(8, 28, 175, 0x5000, 0x1000, 0xABC, true));
        EXPECT_FALSE(capture.setter_seen);
        EXPECT_TRUE(capture.ambiguous);
    }

    TEST(DestinyReason175Capture, ChangedSetterReasonCannotCorrelate)
    {
        auto capture = requested();
        EXPECT_FALSE(capture.observe_setter(7, 28, 176, 0x5000, 0x1000, 0xABC, true));
        EXPECT_EQ(capture.reason, 175u);
        EXPECT_TRUE(capture.ambiguous);
    }

    TEST(DestinyReason175Capture, NullManagerCannotBeConfirmed)
    {
        auto capture = requested();
        EXPECT_FALSE(capture.observe_setter(7, 28, 175, 0, 0x1000, 0xABC, true));
        EXPECT_FALSE(capture.setter_seen);
    }

    TEST(DestinyReason175Capture, DifferentWrapperStackCannotCorrelateDespiteSameCaller)
    {
        auto capture = requested();
        EXPECT_FALSE(capture.observe_setter(7, 28, 175, 0x5000, 0x1008, 0xABC, true));
        EXPECT_TRUE(capture.ambiguous);
    }

    TEST(DestinyReason175Capture, DifferentWrapperReturnCannotCorrelateDespiteSameStack)
    {
        auto capture = requested();
        EXPECT_FALSE(capture.observe_setter(7, 28, 175, 0x5000, 0x1000, 0xABD, true));
        EXPECT_TRUE(capture.ambiguous);
    }

    TEST(DestinyReason175Capture, MissingReturnReadCannotBeReconstructedFromZeros)
    {
        auto capture = requested();
        EXPECT_FALSE(capture.observe_setter(7, 28, 175, 0x5000, 0x1000, 0xABC, false));
        EXPECT_FALSE(capture.setter_seen);
        EXPECT_FALSE(state::read_complete(0, 1));
        EXPECT_TRUE(state::read_complete(1, 1));
    }

    TEST(DestinyReason175Capture, ASecondSetterMakesTheChainAmbiguous)
    {
        auto capture = with_setter();
        EXPECT_FALSE(capture.observe_setter(7, 28, 175, 0x6000, 0x1000, 0xABC, true));
        EXPECT_EQ(capture.manager, 0x5000u);
        EXPECT_TRUE(capture.ambiguous);
    }

    TEST(DestinyReason175Capture, NonCleanupSetterDoesNotInventAFirstCleanup)
    {
        state capture{};
        EXPECT_FALSE(capture.observe_setter(7, 23, 175, 0x5000, 0x1000, 0xABC, true));
        EXPECT_FALSE(capture.first_seen);
        EXPECT_FALSE(capture.ambiguous);
    }

    TEST(DestinyReason175Capture, CommitRequiresTheCapturedManagerAndCompleteNativeTuple)
    {
        auto capture = with_setter();
        EXPECT_TRUE(capture.observe_commit(0x5000, 28, 175, 28, 175, true));
        EXPECT_TRUE(capture.commit_seen);
        EXPECT_FALSE(capture.ambiguous);
    }

    TEST(DestinyReason175Capture, AnotherManagerCannotCompleteTheChain)
    {
        auto capture = with_setter();
        EXPECT_FALSE(capture.observe_commit(0x6000, 28, 175, 28, 175, true));
        EXPECT_FALSE(capture.commit_seen);
        EXPECT_TRUE(capture.ambiguous);
    }

    TEST(DestinyReason175Capture, OverwrittenCurrentOrGoalReasonMakesCommitAmbiguous)
    {
        for (const auto reasons : {std::array<uint32_t, 2>{176, 175}, std::array<uint32_t, 2>{175, 176}})
        {
            auto capture = with_setter();
            EXPECT_FALSE(capture.observe_commit(0x5000, 28, reasons[0], 28, reasons[1], true));
            EXPECT_TRUE(capture.ambiguous);
        }
    }

    TEST(DestinyReason175Capture, UnverifiedCommitFrameCannotCompleteTheChain)
    {
        auto capture = with_setter();
        EXPECT_FALSE(capture.observe_commit(0x5000, 28, 175, 28, 175, false));
        EXPECT_FALSE(capture.commit_seen);
    }

    TEST(DestinyReason175Capture, CommitWithoutASetterIsExplicitlyAmbiguous)
    {
        auto capture = requested();
        EXPECT_FALSE(capture.observe_commit(0x5000, 28, 175, 28, 175, true));
        EXPECT_TRUE(capture.ambiguous);
    }

    TEST(DestinyReason175Capture, PartialReadMasksDoNotMakeMissingFieldsAvailable)
    {
        EXPECT_FALSE(state::read_complete(0x2F, 0x3F));
        EXPECT_TRUE(state::read_complete(0x3F, 0x3F));
        EXPECT_FALSE(state::read_complete(0x05, 0x07));
        EXPECT_TRUE(state::read_complete(0x07, 0x07));
    }

    TEST(DestinyReason175Capture, DataAdmissionPreservesCoverageAndStopsAtSixtyFourRecords)
    {
        const auto data_limit = state::max_records - state::coverage_reserve;
        EXPECT_TRUE(state::record_allowed(data_limit - 1, 0, false));
        EXPECT_FALSE(state::record_allowed(data_limit, 0, false));
        EXPECT_TRUE(state::record_allowed(data_limit, 0, true));
        EXPECT_TRUE(state::record_allowed(63, 0, true));
        EXPECT_FALSE(state::record_allowed(64, 0, true));
        EXPECT_FALSE(state::record_allowed(0, 64, true));
    }

    TEST(DestinyReason175Capture, ByteAdmissionPreservesCoverageAndRejectsOverflow)
    {
        const auto data_limit = state::max_bytes - state::coverage_reserve * state::output_line_bytes;
        EXPECT_TRUE(state::output_allowed(0, data_limit - 10, 10, false));
        EXPECT_FALSE(state::output_allowed(0, data_limit - 9, 10, false));
        EXPECT_TRUE(state::output_allowed(63, state::max_bytes - 10, 10, true));
        EXPECT_FALSE(state::output_allowed(64, 0, 1, true));
        EXPECT_FALSE(state::output_allowed(0, std::numeric_limits<size_t>::max(), 1, true));
        EXPECT_FALSE(state::output_allowed(0, 0, std::numeric_limits<size_t>::max(), true));
    }

    TEST(DestinyReason175Capture, CallbackRemovalExcludesOnlyItsOwnExecutingVcpu)
    {
        std::array<bool, 8> running{};
        running[3] = true;
        EXPECT_TRUE(state::quiescent(running.data(), running.size(), 3));
        running[7] = true;
        EXPECT_FALSE(state::quiescent(running.data(), running.size(), 3));
    }

    TEST(DestinyReason175Capture, HostRemovalWaitsForEveryVcpuAndDoesNotForcePeers)
    {
        std::array<bool, 8> running{};
        const auto host = std::numeric_limits<size_t>::max();
        EXPECT_TRUE(state::quiescent(running.data(), running.size(), host));
        running[0] = true;
        EXPECT_FALSE(state::quiescent(running.data(), running.size(), host));
        EXPECT_TRUE(running[0]);
        EXPECT_FALSE(state::quiescent(nullptr, 8, host));
    }

    TEST(DestinyReason175Capture, AllFifteenFrameRoleSpansMatchTheirExactBytes)
    {
        size_t total{};
        for (size_t guard = 0; guard < state::dependencies.size(); ++guard)
        {
            const auto& dependency = state::dependencies[guard];
            EXPECT_TRUE(state::dependency_matches(guard, dependency.bytes.data(), dependency.length));
            total += dependency.length;
        }
        EXPECT_EQ(total, 424u);
        EXPECT_EQ(state::dependencies.size(), 15u);
    }

    TEST(DestinyReason175Capture, EveryChangedFrameRoleByteRejectsInterpretation)
    {
        for (size_t guard = 0; guard < state::dependencies.size(); ++guard)
        {
            const auto& dependency = state::dependencies[guard];
            for (size_t byte = 0; byte < dependency.length; ++byte)
            {
                auto changed = dependency.bytes;
                changed[byte] ^= 1;
                EXPECT_FALSE(state::dependency_matches(guard, changed.data(), dependency.length));
            }
        }
    }

    TEST(DestinyReason175Capture, FailedOrPartialDependencyReadCannotAuthorizeRoots)
    {
        for (size_t guard = 0; guard < state::dependencies.size(); ++guard)
        {
            const auto& dependency = state::dependencies[guard];
            EXPECT_FALSE(state::dependency_matches(guard, dependency.bytes.data(), dependency.length - 1));
            EXPECT_FALSE(state::dependency_matches(guard, nullptr, dependency.length));
        }
        EXPECT_FALSE(state::dependency_matches(15, state::dependencies[0].bytes.data(), 23));
    }

    TEST(DestinyReason175Capture, HitAndCallbackCapsRetireAtTheirExactBoundaries)
    {
        EXPECT_FALSE(state::callback_exhausted(31, 49999999));
        EXPECT_TRUE(state::callback_exhausted(32, 0));
        EXPECT_TRUE(state::callback_exhausted(0, 50000000));
        EXPECT_TRUE(state::callback_exhausted(std::numeric_limits<uint64_t>::max(), 0));
    }

    TEST(DestinyReason175Capture, MissingThreadAndRequestRootsCannotConfirmSetter)
    {
        state capture{};
        capture.observe_request(0, 175, 0, 0, 200, 1);
        EXPECT_FALSE(capture.observe_setter(0, 28, 175, 0x5000, 0, 0, true));
        EXPECT_FALSE(capture.setter_seen);
        EXPECT_TRUE(capture.ambiguous);
    }

    TEST(DestinyReason175Capture, OnlyCompleteProducerSetterCommitCanQualifyFinalCoverage)
    {
        auto capture = with_setter();
        capture.producer_verified = true;
        EXPECT_FALSE(capture.complete(0, 0));
        ASSERT_TRUE(capture.observe_commit(0x5000, 28, 175, 28, 175, true));
        EXPECT_TRUE(capture.complete(0, 0));
        capture.producer_verified = false;
        EXPECT_FALSE(capture.complete(0, 0));
    }

    TEST(DestinyReason175Capture, AnyLostRecordOrFailedReadInvalidatesFinalCompleteness)
    {
        auto capture = with_setter();
        capture.producer_verified = true;
        ASSERT_TRUE(capture.observe_commit(0x5000, 28, 175, 28, 175, true));
        EXPECT_FALSE(capture.complete(1, 0));
        EXPECT_FALSE(capture.complete(0, 1));
        capture.ambiguous = true;
        EXPECT_FALSE(capture.complete(0, 0));
    }
}
