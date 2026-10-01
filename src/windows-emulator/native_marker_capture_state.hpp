#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace sogen::detail
{
    enum class native_marker_event : uint8_t
    {
        none,
        investment_entry,
        cleanup_entry,
    };

    enum class native_marker_phase : uint8_t
    {
        unarmed,
        awaiting_investment,
        capturing_investment,
        awaiting_cleanup,
        capturing_cleanup,
        retired,
    };

    enum class native_marker_stop : uint8_t
    {
        none,
        completed,
        identity_changed,
        expired,
        clock_regressed,
        observation_cap,
        capture_cap,
        collection_time_cap,
        cancelled,
        invalid_completion,
    };

    enum class native_marker_arm_error : uint8_t
    {
        none,
        not_opted_in,
        invalid_identity,
        invalid_expiry,
        invalid_limits,
        already_armed,
    };

    struct native_marker_identity
    {
        uint32_t pid{};
        uint64_t birth{};
        uint64_t generation{};
        uint64_t module_base{};
        uint64_t module_size{};

        bool operator==(const native_marker_identity&) const = default;

        bool valid() const
        {
            return pid && birth && generation && module_base && module_size &&
                   module_base <= (std::numeric_limits<uint64_t>::max)() - module_size;
        }
    };

    struct native_marker_limits
    {
        static constexpr uint64_t maximum_observations = 65536;
        static constexpr uint64_t maximum_capture_attempts = 2;
        static constexpr uint64_t maximum_event_collection_ns = 2000000;
        static constexpr uint64_t maximum_total_collection_ns = 4000000;
        static constexpr uint64_t maximum_lease_ns = 300000000000;

        uint64_t observations{4096};
        uint64_t capture_attempts{2};
        uint64_t event_collection_ns{2000000};
        uint64_t total_collection_ns{4000000};

        bool valid() const
        {
            return observations && observations <= maximum_observations && capture_attempts &&
                   capture_attempts <= maximum_capture_attempts && event_collection_ns &&
                   event_collection_ns <= maximum_event_collection_ns && total_collection_ns &&
                   total_collection_ns <= maximum_total_collection_ns && event_collection_ns <= total_collection_ns;
        }
    };

    struct native_marker_reservation
    {
        native_marker_event event{native_marker_event::none};
        uint64_t sequence{};
        uint64_t sample_steady_ns{};

        explicit operator bool() const
        {
            return event != native_marker_event::none;
        }
    };

    struct native_marker_completion
    {
        bool accepted{};
        bool complete{};
        native_marker_stop reason{native_marker_stop::none};
    };

    struct native_marker_counts
    {
        uint64_t observations{};
        uint64_t capture_attempts{};
        uint64_t finished_attempts{};
        uint64_t complete_captures{};
        uint64_t partial_captures{};
        uint64_t duplicate_markers{};
        uint64_t cleanup_before_entry{};
        uint64_t collection_ns{};
    };

    // The owner serializes calls under its existing dispatch lock; this state never acquires a lock or captures guest data.
    class native_marker_capture_state
    {
      public:
        static constexpr std::size_t maximum_message_bytes = 4092;
        static constexpr std::string_view investment_marker = "world_controller:state_manager: Entering state 'bootflow:investment_signin'";
        static constexpr std::string_view cleanup_marker = "world_controller:state_manager: Entering state 'cleanup'";

        bool arm(const std::string_view opt_in, const native_marker_identity identity, const uint64_t now_ns, const uint64_t expiry_ns,
                 const native_marker_limits limits = {})
        {
            if (phase_ != native_marker_phase::unarmed)
            {
                arm_error_ = native_marker_arm_error::already_armed;
                return false;
            }
            if (opt_in != "1")
            {
                arm_error_ = native_marker_arm_error::not_opted_in;
                return false;
            }
            if (!identity.valid())
            {
                arm_error_ = native_marker_arm_error::invalid_identity;
                return false;
            }
            if (expiry_ns <= now_ns || expiry_ns - now_ns > native_marker_limits::maximum_lease_ns)
            {
                arm_error_ = native_marker_arm_error::invalid_expiry;
                return false;
            }
            if (!limits.valid())
            {
                arm_error_ = native_marker_arm_error::invalid_limits;
                return false;
            }
            identity_ = identity;
            limits_ = limits;
            expiry_ns_ = expiry_ns;
            last_steady_ns_ = now_ns;
            phase_ = native_marker_phase::awaiting_investment;
            arm_error_ = native_marker_arm_error::none;
            return true;
        }

        native_marker_reservation observe(const native_marker_identity identity, const std::string_view message, const uint64_t now_ns)
        {
            if (!validate(identity, now_ns))
            {
                return {};
            }
            if (counts_.observations >= limits_.observations)
            {
                retire(native_marker_stop::observation_cap);
                return {};
            }
            ++counts_.observations;
            const auto event = classify(message);
            if (event == native_marker_event::cleanup_entry && phase_ == native_marker_phase::awaiting_investment)
            {
                ++counts_.cleanup_before_entry;
            }
            else if ((event == native_marker_event::investment_entry && phase_ != native_marker_phase::awaiting_investment) ||
                     (event == native_marker_event::cleanup_entry && phase_ == native_marker_phase::capturing_cleanup))
            {
                ++counts_.duplicate_markers;
            }
            else if ((event == native_marker_event::investment_entry && phase_ == native_marker_phase::awaiting_investment) ||
                     (event == native_marker_event::cleanup_entry && phase_ == native_marker_phase::awaiting_cleanup))
            {
                if (counts_.capture_attempts >= limits_.capture_attempts)
                {
                    retire(native_marker_stop::capture_cap);
                    return {};
                }
                ++counts_.capture_attempts;
                pending_ = {event, counts_.capture_attempts, now_ns};
                phase_ = event == native_marker_event::investment_entry ? native_marker_phase::capturing_investment
                                                                        : native_marker_phase::capturing_cleanup;
                return pending_;
            }
            if (counts_.observations == limits_.observations && !pending_)
            {
                retire(native_marker_stop::observation_cap);
            }
            return {};
        }

        native_marker_completion finish(const native_marker_identity identity, const uint64_t sequence, const bool capture_complete,
                                        const uint64_t ended_ns)
        {
            if (!pending_ || sequence != pending_.sequence)
            {
                return {false, false, phase_ == native_marker_phase::retired ? stop_ : native_marker_stop::invalid_completion};
            }
            const auto event = pending_.event;
            const auto started_ns = pending_.sample_steady_ns;
            const auto previous_collection_ns = counts_.collection_ns;
            const auto elapsed_ns = ended_ns >= started_ns ? ended_ns - started_ns : 0;
            pending_ = {};
            ++counts_.finished_attempts;
            counts_.collection_ns = saturated_add(counts_.collection_ns, elapsed_ns);
            if (!validate(identity, ended_ns))
            {
                ++counts_.partial_captures;
                return {false, false, stop_};
            }
            const auto remaining_ns =
                previous_collection_ns < limits_.total_collection_ns ? limits_.total_collection_ns - previous_collection_ns : 0;
            if (elapsed_ns > limits_.event_collection_ns || elapsed_ns > remaining_ns)
            {
                ++counts_.partial_captures;
                retire(native_marker_stop::collection_time_cap);
                return {false, false, stop_};
            }
            if (capture_complete)
            {
                ++counts_.complete_captures;
            }
            else
            {
                ++counts_.partial_captures;
            }
            if (event == native_marker_event::cleanup_entry)
            {
                retire(native_marker_stop::completed);
            }
            else if (counts_.capture_attempts == limits_.capture_attempts)
            {
                retire(native_marker_stop::capture_cap);
            }
            else if (counts_.observations == limits_.observations)
            {
                retire(native_marker_stop::observation_cap);
            }
            else if (counts_.collection_ns == limits_.total_collection_ns)
            {
                retire(native_marker_stop::collection_time_cap);
            }
            else
            {
                phase_ = native_marker_phase::awaiting_cleanup;
            }
            return {true, capture_complete, stop_};
        }

        bool check(const native_marker_identity identity, const uint64_t now_ns)
        {
            return validate(identity, now_ns);
        }

        void cancel()
        {
            if (phase_ != native_marker_phase::unarmed && phase_ != native_marker_phase::retired)
            {
                retire(native_marker_stop::cancelled);
            }
        }

        native_marker_phase phase() const
        {
            return phase_;
        }

        native_marker_stop stop_reason() const
        {
            return stop_;
        }

        native_marker_arm_error arm_error() const
        {
            return arm_error_;
        }

        const native_marker_counts& counts() const
        {
            return counts_;
        }

        const native_marker_identity& identity() const
        {
            return identity_;
        }

        static native_marker_event classify(const std::string_view message)
        {
            if (message.size() > maximum_message_bytes)
            {
                return native_marker_event::none;
            }
            const bool investment = matches(message, investment_marker);
            const bool cleanup = matches(message, cleanup_marker);
            return investment == cleanup ? native_marker_event::none
                   : investment          ? native_marker_event::investment_entry
                                         : native_marker_event::cleanup_entry;
        }

      private:
        static bool matches(const std::string_view message, const std::string_view marker)
        {
            const auto position = message.find(marker);
            if (position == std::string_view::npos || (position && (position < 6 || message.substr(position - 6, 6) != " text=")))
            {
                return false;
            }
            const auto end = position + marker.size();
            return end == message.size() || message.substr(end) == "\r\n" || message.substr(end) == "\n" ||
                   message.substr(end).starts_with(" for reason '");
        }

        static uint64_t saturated_add(const uint64_t first, const uint64_t second)
        {
            return first > (std::numeric_limits<uint64_t>::max)() - second ? (std::numeric_limits<uint64_t>::max)() : first + second;
        }

        bool validate(const native_marker_identity identity, const uint64_t now_ns)
        {
            if (phase_ == native_marker_phase::unarmed || phase_ == native_marker_phase::retired)
            {
                return false;
            }
            if (identity != identity_)
            {
                retire(native_marker_stop::identity_changed);
                return false;
            }
            if (now_ns < last_steady_ns_)
            {
                retire(native_marker_stop::clock_regressed);
                return false;
            }
            if (now_ns >= expiry_ns_)
            {
                retire(native_marker_stop::expired);
                return false;
            }
            last_steady_ns_ = now_ns;
            return true;
        }

        void retire(const native_marker_stop reason)
        {
            pending_ = {};
            phase_ = native_marker_phase::retired;
            stop_ = reason;
        }

        native_marker_phase phase_{native_marker_phase::unarmed};
        native_marker_stop stop_{native_marker_stop::none};
        native_marker_arm_error arm_error_{native_marker_arm_error::none};
        native_marker_identity identity_{};
        native_marker_limits limits_{};
        native_marker_counts counts_{};
        native_marker_reservation pending_{};
        uint64_t expiry_ns_{};
        uint64_t last_steady_ns_{};
    };
}
