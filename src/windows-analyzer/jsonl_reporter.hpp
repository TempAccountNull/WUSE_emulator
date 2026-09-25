#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sogen
{

    class analysis_reporter;
    struct analysis_hook_profile;

    enum class jsonl_report_mode
    {
        // Every event is serialized. Long game runs wrote 20 MB/s this way (176 GB in 3.5 hours).
        full,
        // Diagnostic events are written individually; routine events (library-to-library calls,
        // regular syscalls, non-main object/environment reads, thread switches) are counted and
        // periodically emitted as `event_aggregate` records. Interesting routine events keep their
        // first `retained_per_key` occurrences so the distinct calls a module makes stay visible.
        audit,
    };

    struct jsonl_report_settings
    {
        jsonl_report_mode mode{jsonl_report_mode::full};
        uint32_t retained_per_key{3};
        uint32_t aggregate_top{24};
        uint64_t aggregate_interval_events{100000};
        std::chrono::milliseconds aggregate_interval{5000};
        std::chrono::milliseconds status_interval{1000};
        bool dedupe{};
        std::vector<std::string> hidden_modules{};
        std::vector<std::string> hidden_event_types{};
        // Optional small sidecar (rewritten atomically) with counters and the last guest location.
        std::filesystem::path status_path{};
        const analysis_hook_profile* hook_profile{};
    };

    std::unique_ptr<analysis_reporter> create_jsonl_reporter(const std::filesystem::path& path, jsonl_report_settings settings = {});

} // namespace sogen
