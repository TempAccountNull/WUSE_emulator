#pragma once

#include "analysis_event.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>

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

    std::unique_ptr<analysis_reporter> create_console_reporter(logger& log, console_reporter_settings settings);

} // namespace sogen
