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
        bool dedupe{};
        std::vector<std::string> hidden_modules{};
        std::vector<std::string> hidden_event_types{};
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

    bool event_is_deduplicable(const analysis_event& event);
    bool event_from_hidden_module(const analysis_event& event, const std::vector<std::string>& hidden);
    bool event_of_hidden_type(const analysis_event& event, const std::vector<std::string>& hidden_types);
    std::string_view event_type_name(const analysis_event& event);
    uint64_t event_content_hash(const analysis_event& event);
    uint64_t record_content_hash(std::string_view record);

    std::unique_ptr<analysis_reporter> create_console_reporter(logger& log, console_reporter_settings settings);

} // namespace sogen
