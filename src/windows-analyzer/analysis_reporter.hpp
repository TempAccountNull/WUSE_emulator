#pragma once

#include "analysis_event.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{

    class logger;

    struct console_reporter_settings
    {
        bool silent{};
        bool buffer_stdout{};
        bool interesting_only{};
        bool prepend_call_count{};
        // Console only: fold consecutive identical events of one guest thread into bounded repeat
        // summaries. Every event still reaches the structured reporters unchanged.
        bool coalesce_repeats{};
        // Print a line only when its data differs from one already printed by any thread: events
        // that differ only in counters (ic, callCount, call_id) are suppressed and counted.
        bool dedupe{};
        // Lower-case module file names whose observations are dropped (executing in, or called from,
        // the module). Failure packets and run start/end are always printed.
        std::vector<std::string> hidden_modules{};
        uint64_t repeat_summary_every{1000};
        std::chrono::milliseconds repeat_summary_interval{1000};
    };

    class analysis_reporter
    {
      public:
        virtual ~analysis_reporter() = default;
        virtual void report(const analysis_event& event) = 0;

        virtual void flush()
        {
        }
    };

    // Identity of an event's data: its serialized record without the counters (ic, callCount,
    // call_id). Equal hashes mean the same data observed again. Run start/end, guest stdout,
    // progress heartbeats and failure packets carry no data identity and are never deduplicated.
    bool event_is_deduplicable(const analysis_event& event);
    // True when the observation executes in, or was reached from, one of `hidden_lowercase`
    // (module file names compared case-insensitively). Never true for failure packets or run events.
    bool event_from_hidden_module(const analysis_event& event, const std::vector<std::string>& hidden_lowercase);
    uint64_t event_content_hash(const analysis_event& event);
    uint64_t record_content_hash(std::string_view record);

    std::unique_ptr<analysis_reporter> create_console_reporter(logger& log, console_reporter_settings settings);

} // namespace sogen
