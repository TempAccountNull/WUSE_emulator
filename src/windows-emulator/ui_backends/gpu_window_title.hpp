#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace sogen::ui
{
    inline constexpr size_t gpu_window_title_max_record_bytes = 4096;
    inline constexpr uint64_t gpu_window_title_freshness_ms = 3000;

    struct gpu_window_title_record
    {
        uint32_t host_pid{};
        uint64_t unix_ms{};
        uint64_t guest_hwnd{};
        std::string identity;
        std::string metrics;
    };

    namespace gpu_window_title_detail
    {
        inline constexpr std::string_view header_prefix = "SOGEN_GPU_TITLE_V1 ";

        inline bool is_title_text(const std::string_view text)
        {
            if (text.empty())
            {
                return false;
            }
            size_t offset{};
            while (offset < text.size())
            {
                const auto first = static_cast<uint8_t>(text[offset]);
                ++offset;
                if (first < 0x80)
                {
                    if (first < 0x20 || first == 0x7F)
                    {
                        return false;
                    }
                    continue;
                }
                uint32_t codepoint{};
                uint32_t minimum{};
                size_t continuation_bytes{};
                if (first >= 0xC2 && first <= 0xDF)
                {
                    codepoint = first & 0x1FU;
                    minimum = 0x80;
                    continuation_bytes = 1;
                }
                else if (first >= 0xE0 && first <= 0xEF)
                {
                    codepoint = first & 0x0FU;
                    minimum = 0x800;
                    continuation_bytes = 2;
                }
                else if (first >= 0xF0 && first <= 0xF4)
                {
                    codepoint = first & 0x07U;
                    minimum = 0x10000;
                    continuation_bytes = 3;
                }
                else
                {
                    return false;
                }
                if (continuation_bytes > text.size() - offset)
                {
                    return false;
                }
                for (size_t index = 0; index < continuation_bytes; ++index)
                {
                    const auto next = static_cast<uint8_t>(text[offset]);
                    ++offset;
                    if ((next & 0xC0U) != 0x80U)
                    {
                        return false;
                    }
                    codepoint = (codepoint << 6U) | (next & 0x3FU);
                }
                if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
                    (codepoint >= 0x80 && codepoint <= 0x9F) || codepoint == 0x2028 || codepoint == 0x2029)
                {
                    return false;
                }
            }
            return true;
        }

        inline bool has_value(const std::string_view text)
        {
            return !text.empty() && text.find_first_not_of(' ') != std::string_view::npos;
        }

        inline bool has_identity_fields(const std::string_view text)
        {
            constexpr std::string_view type_prefix = "Type:";
            constexpr std::string_view adapter_separator = " | Adapter:";
            if (!text.starts_with(type_prefix))
            {
                return false;
            }
            const auto separator = text.find(adapter_separator, type_prefix.size());
            return separator != std::string_view::npos && has_value(text.substr(type_prefix.size(), separator - type_prefix.size())) &&
                   has_value(text.substr(separator + adapter_separator.size()));
        }

        inline bool has_metric_fields(const std::string_view text)
        {
            constexpr std::string_view utilization_prefix = "Util:";
            constexpr std::string_view vram_separator = " | VRAM:";
            constexpr std::string_view memory_separator = " | Mem:";
            if (!text.starts_with(utilization_prefix))
            {
                return false;
            }
            const auto vram = text.find(vram_separator, utilization_prefix.size());
            if (vram == std::string_view::npos)
            {
                return false;
            }
            const auto memory = text.find(memory_separator, vram + vram_separator.size());
            return memory != std::string_view::npos &&
                   has_value(text.substr(utilization_prefix.size(), vram - utilization_prefix.size())) &&
                   has_value(text.substr(vram + vram_separator.size(), memory - vram - vram_separator.size())) &&
                   has_value(text.substr(memory + memory_separator.size()));
        }

        inline bool parse_decimal(const std::string_view text, uint64_t& value)
        {
            if (text.empty() || text.front() < '0' || text.front() > '9')
            {
                return false;
            }
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
            return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
        }

        inline bool is_valid_record(const gpu_window_title_record& record)
        {
            if (record.host_pid == 0 || record.guest_hwnd == 0 || record.identity.size() > gpu_window_title_max_record_bytes ||
                record.metrics.size() > gpu_window_title_max_record_bytes)
            {
                return false;
            }
            const auto encoded_size = header_prefix.size() + std::to_string(record.host_pid).size() + 1 +
                                      std::to_string(record.unix_ms).size() + 1 + std::to_string(record.guest_hwnd).size() + 1 +
                                      record.identity.size() + 1 + record.metrics.size() + 1;
            return encoded_size <= gpu_window_title_max_record_bytes && is_title_text(record.identity) && is_title_text(record.metrics) &&
                   has_identity_fields(record.identity) && has_metric_fields(record.metrics);
        }
    }

    inline std::optional<gpu_window_title_record> decode_gpu_window_title_record(const std::string_view bytes)
    {
        if (bytes.size() > gpu_window_title_max_record_bytes || !bytes.starts_with(gpu_window_title_detail::header_prefix))
        {
            return std::nullopt;
        }
        const auto header_end = bytes.find('\n');
        if (header_end == std::string_view::npos)
        {
            return std::nullopt;
        }
        const auto identity_end = bytes.find('\n', header_end + 1);
        if (identity_end == std::string_view::npos)
        {
            return std::nullopt;
        }
        const auto metrics_end = bytes.find('\n', identity_end + 1);
        if (metrics_end == std::string_view::npos || metrics_end != bytes.size() - 1)
        {
            return std::nullopt;
        }
        const auto header =
            bytes.substr(gpu_window_title_detail::header_prefix.size(), header_end - gpu_window_title_detail::header_prefix.size());
        const auto pid_end = header.find(' ');
        if (pid_end == std::string_view::npos)
        {
            return std::nullopt;
        }
        const auto timestamp_end = header.find(' ', pid_end + 1);
        if (timestamp_end == std::string_view::npos)
        {
            return std::nullopt;
        }
        uint64_t host_pid{};
        gpu_window_title_record result;
        if (!gpu_window_title_detail::parse_decimal(header.substr(0, pid_end), host_pid) ||
            host_pid > std::numeric_limits<uint32_t>::max() ||
            !gpu_window_title_detail::parse_decimal(header.substr(pid_end + 1, timestamp_end - pid_end - 1), result.unix_ms) ||
            !gpu_window_title_detail::parse_decimal(header.substr(timestamp_end + 1), result.guest_hwnd))
        {
            return std::nullopt;
        }
        result.host_pid = static_cast<uint32_t>(host_pid);
        result.identity = bytes.substr(header_end + 1, identity_end - header_end - 1);
        result.metrics = bytes.substr(identity_end + 1, metrics_end - identity_end - 1);
        if (!gpu_window_title_detail::is_valid_record(result))
        {
            return std::nullopt;
        }
        return result;
    }

    inline std::optional<std::string> compose_gpu_window_title(const std::string_view base_title, const gpu_window_title_record& record,
                                                               const uint32_t current_pid, const uint64_t guest_hwnd, const uint64_t now_ms)
    {
        if (!gpu_window_title_detail::is_valid_record(record) || record.host_pid != current_pid || record.guest_hwnd != guest_hwnd)
        {
            return std::nullopt;
        }
        // Future samples are stale too; compare before subtracting to avoid unsigned timestamp wraparound.
        const auto fresh = now_ms >= record.unix_ms && now_ms - record.unix_ms <= gpu_window_title_freshness_ms;
        std::string title{base_title};
        if (!title.empty())
        {
            title += " | ";
        }
        title += record.identity;
        title += " | ";
        title += fresh ? record.metrics : "Telemetry: stale";
        return title;
    }
}
