#include <native_marker_capture_state.hpp>

#include <array>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    using namespace sogen::detail;

    constexpr native_marker_identity identity{1234, 55, 7, 0x140000000, 0x9000000};
    constexpr uint64_t expiry = 100000000;

    void require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    native_marker_capture_state armed(const native_marker_limits limits = {})
    {
        native_marker_capture_state state;
        require(state.arm("1", identity, 0, expiry, limits), "valid arm rejected");
        return state;
    }

    void exact_opt_in()
    {
        constexpr std::array rejected = {std::string_view{},     std::string_view{"0"},  std::string_view{"true"},  std::string_view{"01"},
                                         std::string_view{" 1"}, std::string_view{"1 "}, std::string_view{"1\0", 2}};
        for (const auto flag : rejected)
        {
            native_marker_capture_state state;
            require(!state.arm(flag, identity, 0, expiry), "nonexact opt-in accepted");
            require(state.arm_error() == native_marker_arm_error::not_opted_in, "wrong opt-in rejection");
            require(state.phase() == native_marker_phase::unarmed, "rejected opt-in armed state");
            require(!state.observe(identity, native_marker_capture_state::investment_marker, 1), "unarmed capture admitted");
            require(state.counts().observations == 0, "unarmed observer consumed budget");
            require(state.arm("1", identity, 0, expiry), "valid opt-in after rejection failed");
        }
    }

    void invalid_identity_and_span()
    {
        auto cases = std::array{identity, identity, identity, identity, identity, identity};
        cases[0].pid = 0;
        cases[1].birth = 0;
        cases[2].generation = 0;
        cases[3].module_base = 0;
        cases[4].module_size = 0;
        cases[5].module_base = UINT64_MAX;
        cases[5].module_size = 1;
        for (const auto value : cases)
        {
            native_marker_capture_state state;
            require(!state.arm("1", value, 0, expiry), "invalid identity accepted");
            require(state.arm_error() == native_marker_arm_error::invalid_identity, "wrong identity rejection");
            require(state.phase() == native_marker_phase::unarmed, "invalid identity changed phase");
        }
        native_marker_capture_state state;
        auto last_span = identity;
        last_span.module_base = UINT64_MAX - 1;
        last_span.module_size = 1;
        require(state.arm("1", last_span, 0, expiry), "last nonoverflowing exclusive-end span rejected");
    }

    void invalid_expiry()
    {
        constexpr std::array<std::pair<uint64_t, uint64_t>, 5> cases{
            {{0, 0}, {4, 4}, {4, 3}, {0, native_marker_limits::maximum_lease_ns + 1}, {UINT64_MAX - 1, 5}}};
        for (const auto [now, end] : cases)
        {
            native_marker_capture_state state;
            require(!state.arm("1", identity, now, end), "invalid expiry accepted");
            require(state.arm_error() == native_marker_arm_error::invalid_expiry, "wrong expiry rejection");
        }
        native_marker_capture_state state;
        require(state.arm("1", identity, 0, native_marker_limits::maximum_lease_ns), "exact lease maximum rejected");
    }

    void invalid_limits()
    {
        std::array<native_marker_limits, 9> cases{};
        cases[0].observations = 0;
        cases[1].observations = native_marker_limits::maximum_observations + 1;
        cases[2].capture_attempts = 0;
        cases[3].capture_attempts = 3;
        cases[4].event_collection_ns = 0;
        cases[5].event_collection_ns = native_marker_limits::maximum_event_collection_ns + 1;
        cases[6].total_collection_ns = 0;
        cases[7].total_collection_ns = native_marker_limits::maximum_total_collection_ns + 1;
        cases[8].total_collection_ns = cases[8].event_collection_ns - 1;
        for (const auto limits : cases)
        {
            native_marker_capture_state state;
            require(!state.arm("1", identity, 0, expiry, limits), "invalid limits accepted");
            require(state.arm_error() == native_marker_arm_error::invalid_limits, "wrong limits rejection");
        }
    }

    void normal_first_entry_then_cleanup()
    {
        auto state = armed();
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 100);
        require(first.event == native_marker_event::investment_entry && first.sequence == 1 && first.sample_steady_ns == 100,
                "wrong first reservation");
        require(state.phase() == native_marker_phase::capturing_investment, "first not reserved before capture");
        const auto first_done = state.finish(identity, first.sequence, true, 200);
        require(first_done.accepted && first_done.complete, "first completion rejected");
        require(state.phase() == native_marker_phase::awaiting_cleanup, "first completion lost cleanup phase");
        const auto second = state.observe(identity, native_marker_capture_state::cleanup_marker, 300);
        require(second.event == native_marker_event::cleanup_entry && second.sequence == 2, "wrong second reservation");
        const auto second_done = state.finish(identity, second.sequence, true, 500);
        require(second_done.accepted && second_done.complete, "second completion rejected");
        require(state.phase() == native_marker_phase::retired && state.stop_reason() == native_marker_stop::completed,
                "successful workflow did not retire");
        require(state.counts().capture_attempts == 2 && state.counts().finished_attempts == 2 && state.counts().complete_captures == 2 &&
                    state.counts().collection_ns == 300,
                "workflow coverage incorrect");
    }

    void cleanup_before_entry()
    {
        auto state = armed();
        require(!state.observe(identity, native_marker_capture_state::cleanup_marker, 10), "early cleanup captured");
        require(!state.observe(identity, "client ev=retail text=world_controller:state_manager: Entering state 'cleanup'", 20),
                "wrapped early cleanup captured");
        require(state.counts().cleanup_before_entry == 2 && state.counts().capture_attempts == 0, "early cleanup altered capture count");
        require(state.phase() == native_marker_phase::awaiting_investment, "early cleanup changed phase");
        require(static_cast<bool>(state.observe(identity, native_marker_capture_state::investment_marker, 30)),
                "early cleanup prevented first entry");
    }

    void exact_markers_and_prefix_boundaries()
    {
        constexpr std::array rejected = {
            "ordinary log",
            "world_controller:state_manager: Entering state 'bootflow:cleanup'",
            "world_controller:state_manager: Entering state 'cleanup_extra'",
            "world_controller:state_manager: Entering state 'bootflow:investment_signin_extra'",
            "world_controller:state_manager: Entering state 'bootflow:investment_signin",
            "prefixworld_controller:state_manager: Entering state 'cleanup'",
            "nottext=world_controller:state_manager: Entering state 'cleanup'",
            "world_controller:state_manager: Entering state 'cleanup'garbage",
            "world_controller:state_manager: Entering state 'cleanup' for other text",
            "WORLD_controller:state_manager: Entering state 'cleanup'",
        };
        for (const std::string_view message : rejected)
        {
            require(native_marker_capture_state::classify(message) == native_marker_event::none, "inexact marker classified");
        }
        require(native_marker_capture_state::classify(native_marker_capture_state::investment_marker) ==
                    native_marker_event::investment_entry,
                "standalone investment marker rejected");
        require(native_marker_capture_state::classify(native_marker_capture_state::cleanup_marker) == native_marker_event::cleanup_entry,
                "cleanup without bootflow prefix rejected");
        require(native_marker_capture_state::classify(std::string(native_marker_capture_state::cleanup_marker) + "\r\n") ==
                    native_marker_event::cleanup_entry,
                "CRLF marker rejected");
    }

    void actual_retail_envelope()
    {
        constexpr std::string_view entry = "client level=info t=2738110 ev=retail site=4 text=world_controller:state_manager: "
                                           "Entering state 'bootflow:investment_signin' for reason 'unavailable'.\r\n";
        constexpr std::string_view cleanup = "client level=info t=2769625 ev=retail site=4 text=world_controller:state_manager: "
                                             "Entering state 'cleanup' for reason 'unavailable'.\r\n";
        require(native_marker_capture_state::classify(entry) == native_marker_event::investment_entry, "saved entry form rejected");
        require(native_marker_capture_state::classify(cleanup) == native_marker_event::cleanup_entry, "saved cleanup form rejected");
    }

    void message_size_and_ambiguity()
    {
        const auto marker = native_marker_capture_state::investment_marker;
        std::string bounded(native_marker_capture_state::maximum_message_bytes - marker.size() - 6, 'x');
        bounded += " text=";
        bounded += marker;
        require(native_marker_capture_state::classify(bounded) == native_marker_event::investment_entry, "exact DBWIN size rejected");
        bounded.insert(0, 1, 'x');
        require(native_marker_capture_state::classify(bounded) == native_marker_event::none, "oversize message accepted");
        const auto both = std::string(native_marker_capture_state::investment_marker) +
                          " for reason 'one'. text=" + std::string(native_marker_capture_state::cleanup_marker) + " for reason 'two'.";
        require(native_marker_capture_state::classify(both) == native_marker_event::none, "ambiguous multi-marker message accepted");
    }

    void duplicate_and_pending_ownership()
    {
        auto state = armed();
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 10);
        require(!state.observe(identity, native_marker_capture_state::investment_marker, 11), "pending duplicate entry captured");
        require(!state.observe(identity, native_marker_capture_state::cleanup_marker, 12), "cleanup overlapped entry capture");
        require(state.counts().capture_attempts == 1, "overlapping message consumed a packet");
        const auto wrong = state.finish(identity, first.sequence + 1, true, 13);
        require(!wrong.accepted && wrong.reason == native_marker_stop::invalid_completion, "wrong sequence accepted");
        require(state.phase() == native_marker_phase::capturing_investment, "wrong completion destroyed pending event");
        require(state.finish(identity, first.sequence, true, 14).accepted, "valid completion after wrong sequence rejected");
        require(!state.observe(identity, native_marker_capture_state::investment_marker, 15), "finished duplicate entry captured");
        require(state.counts().duplicate_markers == 2, "duplicates not counted");
        const auto second = state.observe(identity, native_marker_capture_state::cleanup_marker, 16);
        require(static_cast<bool>(second), "first subsequent cleanup not captured");
        require(!state.observe(identity, native_marker_capture_state::cleanup_marker, 17), "pending duplicate cleanup captured");
        require(state.finish(identity, second.sequence, true, 18).accepted, "second completion rejected");
        const auto final_counts = state.counts();
        for (uint64_t now = 19; now < 119; ++now)
        {
            require(!state.observe(identity, native_marker_capture_state::investment_marker, now), "retired state recaptured");
        }
        require(state.counts().observations == final_counts.observations && state.counts().capture_attempts == 2,
                "retired state continued consuming observations");
    }

    void partial_samples_are_not_replaced()
    {
        auto state = armed();
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 10);
        const auto first_done = state.finish(identity, first.sequence, false, 11);
        require(first_done.accepted && !first_done.complete, "partial first packet rejected rather than consumed");
        require(!state.observe(identity, native_marker_capture_state::investment_marker, 12), "partial first packet replaced");
        const auto second = state.observe(identity, native_marker_capture_state::cleanup_marker, 13);
        const auto second_done = state.finish(identity, second.sequence, false, 14);
        require(second_done.accepted && !second_done.complete, "partial cleanup packet rejected rather than consumed");
        require(state.counts().partial_captures == 2 && state.counts().complete_captures == 0, "partial counters incorrect");
        require(state.stop_reason() == native_marker_stop::completed, "finite partial workflow remained armed");
    }

    void every_identity_component_invalidates()
    {
        auto changed = std::array{identity, identity, identity, identity, identity};
        ++changed[0].pid;
        ++changed[1].birth;
        ++changed[2].generation;
        ++changed[3].module_base;
        ++changed[4].module_size;
        for (const auto other : changed)
        {
            auto state = armed();
            require(!state.observe(other, native_marker_capture_state::investment_marker, 1), "changed identity captured");
            require(state.stop_reason() == native_marker_stop::identity_changed, "identity replacement not retired");
            require(!state.observe(identity, native_marker_capture_state::investment_marker, 2), "old identity revived retired arm");
        }
    }

    void generation_change_during_collection()
    {
        auto state = armed();
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 10);
        auto replacement = identity;
        ++replacement.generation;
        const auto done = state.finish(replacement, first.sequence, true, 20);
        require(!done.accepted && !done.complete && done.reason == native_marker_stop::identity_changed,
                "changed generation completion accepted");
        require(state.counts().finished_attempts == 1 && state.counts().partial_captures == 1 && state.counts().collection_ns == 10,
                "failed identity completion lost actual attempted cost");
    }

    void identity_change_between_markers()
    {
        auto state = armed();
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 10);
        require(state.finish(identity, first.sequence, true, 20).accepted, "first completion failed");
        auto replacement = identity;
        ++replacement.birth;
        require(!state.observe(replacement, native_marker_capture_state::cleanup_marker, 30), "replacement cleanup accepted");
        require(state.counts().capture_attempts == 1 && state.stop_reason() == native_marker_stop::identity_changed,
                "mixed-lifetime packets possible");
    }

    void expiry_before_and_during_collection()
    {
        auto before = armed();
        require(!before.observe(identity, native_marker_capture_state::investment_marker, expiry), "capture at expiry admitted");
        require(before.stop_reason() == native_marker_stop::expired && before.counts().capture_attempts == 0,
                "expired observer consumed capture slot");
        auto during = armed();
        const auto first = during.observe(identity, native_marker_capture_state::investment_marker, expiry - 1);
        require(static_cast<bool>(first), "event before expiry rejected");
        const auto done = during.finish(identity, first.sequence, true, expiry);
        require(!done.accepted && !done.complete && done.reason == native_marker_stop::expired, "late completion accepted");
        require(during.counts().collection_ns == 1 && during.counts().partial_captures == 1, "late completion evidence lost");
    }

    void expiry_without_a_message()
    {
        auto state = armed();
        require(state.check(identity, expiry - 1), "lease expired early");
        require(!state.check(identity, expiry), "safe-point expiry check did not retire");
        require(state.counts().observations == 0 && state.stop_reason() == native_marker_stop::expired,
                "expiry check pretended to be a message");
    }

    void clock_regression_is_terminal()
    {
        auto state = armed();
        require(state.check(identity, 10), "clock check failed");
        require(!state.observe(identity, native_marker_capture_state::investment_marker, 9), "regressed clock captured");
        require(state.stop_reason() == native_marker_stop::clock_regressed, "clock regression not classified");
        auto during = armed();
        const auto first = during.observe(identity, native_marker_capture_state::investment_marker, 10);
        const auto done = during.finish(identity, first.sequence, true, 9);
        require(!done.accepted && done.reason == native_marker_stop::clock_regressed && during.counts().collection_ns == 0,
                "negative elapsed wrapped or accepted");
    }

    void cancellation_preserves_first_stop()
    {
        native_marker_capture_state unarmed;
        unarmed.cancel();
        require(unarmed.phase() == native_marker_phase::unarmed, "cancelled unarmed object cannot be opted in");
        auto state = armed();
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 10);
        state.cancel();
        require(!state.finish(identity, first.sequence, true, 20).accepted, "cancelled packet accepted");
        require(state.stop_reason() == native_marker_stop::cancelled, "cancel reason lost");
        state.cancel();
        auto changed = identity;
        ++changed.generation;
        require(!state.check(changed, expiry), "retired cancellation reopened");
        require(state.stop_reason() == native_marker_stop::cancelled, "later identity/expiry overwrote first stop");
    }

    void observation_cap_without_marker()
    {
        native_marker_limits limits;
        limits.observations = 2;
        auto state = armed(limits);
        require(!state.observe(identity, "ordinary one", 1), "ordinary message captured");
        require(state.phase() == native_marker_phase::awaiting_investment, "observer retired before cap");
        require(!state.observe(identity, "ordinary two", 2), "ordinary message captured");
        require(state.stop_reason() == native_marker_stop::observation_cap, "exact observation cap did not retire");
        require(!state.observe(identity, native_marker_capture_state::investment_marker, 3), "capture after observation cap admitted");
        require(state.counts().observations == 2 && state.counts().capture_attempts == 0, "observation cap exceeded");
    }

    void last_observation_can_reserve_then_retires()
    {
        native_marker_limits limits;
        limits.observations = 1;
        auto state = armed(limits);
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 10);
        require(static_cast<bool>(first), "last budgeted observation could not reserve");
        require(state.finish(identity, first.sequence, true, 11).accepted, "last observation completion rejected");
        require(state.stop_reason() == native_marker_stop::observation_cap && state.counts().complete_captures == 1,
                "last observation not consumed/retired correctly");
    }

    void one_packet_capture_cap()
    {
        native_marker_limits limits;
        limits.capture_attempts = 1;
        auto state = armed(limits);
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 10);
        require(state.finish(identity, first.sequence, true, 11).accepted, "single packet completion rejected");
        require(state.stop_reason() == native_marker_stop::capture_cap, "one packet cap remained armed");
        require(!state.observe(identity, native_marker_capture_state::cleanup_marker, 12), "capture cap admitted cleanup");
    }

    void collection_time_overrun_preserves_actual_cost()
    {
        auto state = armed();
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 10);
        const auto actual_cost = native_marker_limits::maximum_event_collection_ns + 1;
        const auto done = state.finish(identity, first.sequence, true, 10 + actual_cost);
        require(!done.accepted && !done.complete && done.reason == native_marker_stop::collection_time_cap,
                "single collection overrun accepted");
        require(state.counts().collection_ns == actual_cost && state.counts().partial_captures == 1,
                "reported collection cost was clipped to policy cap");
    }

    void total_collection_cap_and_exact_boundary()
    {
        native_marker_limits limits;
        limits.total_collection_ns = 3000000;
        auto state = armed(limits);
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 1);
        require(state.finish(identity, first.sequence, true, 2000001).accepted, "first exact event cap rejected");
        const auto second = state.observe(identity, native_marker_capture_state::cleanup_marker, 2000002);
        const auto done = state.finish(identity, second.sequence, true, 4000002);
        require(!done.accepted && done.reason == native_marker_stop::collection_time_cap, "combined cost cap ignored");
        require(state.counts().collection_ns == 4000000 && state.counts().complete_captures == 1 && state.counts().partial_captures == 1,
                "total overrun evidence incorrect");
        auto exact = armed();
        const auto exact_first = exact.observe(identity, native_marker_capture_state::investment_marker, 1);
        require(exact.finish(identity, exact_first.sequence, true, 2000001).accepted, "exact first event cap rejected");
        const auto exact_second = exact.observe(identity, native_marker_capture_state::cleanup_marker, 2000002);
        const auto exact_done = exact.finish(identity, exact_second.sequence, true, 4000002);
        require(exact_done.accepted && exact_done.complete && exact.stop_reason() == native_marker_stop::completed,
                "exact complete two-event budget rejected");
    }

    void exhausted_total_budget_retires_without_next_hit()
    {
        native_marker_limits limits;
        limits.total_collection_ns = 2000000;
        auto state = armed(limits);
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 1);
        require(state.finish(identity, first.sequence, true, 2000001).accepted, "exact first total cap rejected");
        require(state.stop_reason() == native_marker_stop::collection_time_cap, "exhausted total budget remained armed");
        require(!state.observe(identity, native_marker_capture_state::cleanup_marker, 2000002), "zero budget cleanup admitted");
    }

    void zero_cost_and_near_uint64_clock()
    {
        auto zero = armed();
        const auto first_zero = zero.observe(identity, native_marker_capture_state::investment_marker, 0);
        require(zero.finish(identity, first_zero.sequence, true, 0).accepted, "equal monotonic stamps rejected");
        const auto second_zero = zero.observe(identity, native_marker_capture_state::cleanup_marker, 0);
        require(zero.finish(identity, second_zero.sequence, true, 0).accepted, "equal cleanup stamps rejected");
        require(zero.counts().collection_ns == 0, "zero elapsed invented work");
        native_marker_capture_state high;
        require(high.arm("1", identity, UINT64_MAX - 400, UINT64_MAX - 1), "high clock arm overflowed");
        const auto first = high.observe(identity, native_marker_capture_state::investment_marker, UINT64_MAX - 300);
        require(high.finish(identity, first.sequence, true, UINT64_MAX - 200).accepted, "high first elapsed overflowed");
        const auto second = high.observe(identity, native_marker_capture_state::cleanup_marker, UINT64_MAX - 100);
        require(high.finish(identity, second.sequence, true, UINT64_MAX - 2).accepted, "high second elapsed overflowed");
        require(high.counts().collection_ns == 198, "high clock elapsed truncated");
    }

    void late_maximum_timestamp_never_wraps()
    {
        auto state = armed();
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 10);
        const auto done = state.finish(identity, first.sequence, true, UINT64_MAX);
        require(!done.accepted && done.reason == native_marker_stop::expired, "extreme late clock accepted");
        require(state.counts().collection_ns == UINT64_MAX - 10, "late raw duration truncated/wrapped");
        require(state.counts().finished_attempts == 1, "late attempted capture lost");
    }

    void cannot_rearm_or_recomplete()
    {
        auto state = armed();
        require(!state.arm("1", identity, 0, expiry), "second arm overwrote identity/window");
        require(state.arm_error() == native_marker_arm_error::already_armed, "second arm rejection missing");
        const auto first = state.observe(identity, native_marker_capture_state::investment_marker, 10);
        require(state.finish(identity, first.sequence, true, 11).accepted, "first completion failed");
        const auto duplicate = state.finish(identity, first.sequence, true, 12);
        require(!duplicate.accepted && duplicate.reason == native_marker_stop::invalid_completion, "duplicate completion changed history");
        require(state.counts().finished_attempts == 1, "duplicate completion counted twice");
        state.cancel();
        require(!state.arm("1", identity, 0, expiry), "retired state rearmed");
        require(state.stop_reason() == native_marker_stop::cancelled, "rearm changed first stop");
    }

    void identity_precedes_clock_failures()
    {
        auto state = armed();
        auto changed = identity;
        ++changed.generation;
        require(!state.check(changed, expiry), "changed generation at expiry accepted");
        require(state.stop_reason() == native_marker_stop::identity_changed, "identity cause overwritten by expiry");
    }
}

TEST(NativeMarkerCaptureState, ExactOptIn)
{
    exact_opt_in();
}

TEST(NativeMarkerCaptureState, IdentityAndExclusiveEndSpan)
{
    invalid_identity_and_span();
}

TEST(NativeMarkerCaptureState, ExpiryValidation)
{
    invalid_expiry();
}

TEST(NativeMarkerCaptureState, LimitValidation)
{
    invalid_limits();
}

TEST(NativeMarkerCaptureState, FirstEntryThenCleanup)
{
    normal_first_entry_then_cleanup();
}

TEST(NativeMarkerCaptureState, CleanupBeforeEntry)
{
    cleanup_before_entry();
}

TEST(NativeMarkerCaptureState, MarkerBoundaries)
{
    exact_markers_and_prefix_boundaries();
}

TEST(NativeMarkerCaptureState, ActualRetailEnvelope)
{
    actual_retail_envelope();
}

TEST(NativeMarkerCaptureState, MessageSizeAndAmbiguity)
{
    message_size_and_ambiguity();
}

TEST(NativeMarkerCaptureState, DuplicatesAndReservationOwnership)
{
    duplicate_and_pending_ownership();
}

TEST(NativeMarkerCaptureState, PartialSamplesAreNotReplaced)
{
    partial_samples_are_not_replaced();
}

TEST(NativeMarkerCaptureState, EveryIdentityComponentInvalidates)
{
    every_identity_component_invalidates();
}

TEST(NativeMarkerCaptureState, GenerationChangesDuringCollection)
{
    generation_change_during_collection();
}

TEST(NativeMarkerCaptureState, IdentityChangesBetweenMarkers)
{
    identity_change_between_markers();
}

TEST(NativeMarkerCaptureState, ExpiryBeforeAndDuringCollection)
{
    expiry_before_and_during_collection();
}

TEST(NativeMarkerCaptureState, ExpiryWithoutMessage)
{
    expiry_without_a_message();
}

TEST(NativeMarkerCaptureState, ClockRegression)
{
    clock_regression_is_terminal();
}

TEST(NativeMarkerCaptureState, CancellationAndFirstStop)
{
    cancellation_preserves_first_stop();
}

TEST(NativeMarkerCaptureState, ObservationCapWithoutMarker)
{
    observation_cap_without_marker();
}

TEST(NativeMarkerCaptureState, LastObservationBoundary)
{
    last_observation_can_reserve_then_retires();
}

TEST(NativeMarkerCaptureState, OnePacketCaptureCap)
{
    one_packet_capture_cap();
}

TEST(NativeMarkerCaptureState, CollectionTimeAndActualCost)
{
    collection_time_overrun_preserves_actual_cost();
}

TEST(NativeMarkerCaptureState, TotalCostAndExactBoundary)
{
    total_collection_cap_and_exact_boundary();
}

TEST(NativeMarkerCaptureState, BudgetExhaustionRetirement)
{
    exhausted_total_budget_retires_without_next_hit();
}

TEST(NativeMarkerCaptureState, ZeroAndHighClock)
{
    zero_cost_and_near_uint64_clock();
}

TEST(NativeMarkerCaptureState, LateMaximumTimestamp)
{
    late_maximum_timestamp_never_wraps();
}

TEST(NativeMarkerCaptureState, NoRearmOrRecompletion)
{
    cannot_rearm_or_recomplete();
}

TEST(NativeMarkerCaptureState, IdentityFailurePrecedence)
{
    identity_precedes_clock_failures();
}
