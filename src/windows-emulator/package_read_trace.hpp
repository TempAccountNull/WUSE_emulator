#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace sogen
{
    // Opt-in diagnostic only: retain metadata and hashes, never package bytes.
    class package_read_trace
    {
      public:
        static constexpr size_t capacity = 32;
        static constexpr size_t hash_chunk_bytes = 64 * 1024;
        static constexpr size_t max_hashed_bytes = 1024 * 1024;
        static constexpr size_t max_path_bytes = 160;

        struct read_entry
        {
            std::string path{};
            uint64_t handle{};
            uint32_t tid{};
            int64_t offset{};
            uint64_t requested{};
            uint64_t actual{};
            uint64_t hashed_bytes{};
            uint64_t host_hash{};
            uint64_t guest_hash{};
            bool guest_valid{};
        };

        explicit package_read_trace(const bool enabled = env_enabled()) : enabled_(enabled) {}
        [[nodiscard]] bool enabled() const noexcept { return enabled_; }

        static constexpr uint64_t hash_seed = 14695981039346656037ull;

        static uint64_t hash_append(uint64_t value, const std::span<const char> bytes) noexcept
        {
            for (const unsigned char byte : bytes)
                value = (value ^ byte) * 1099511628211ull;
            return value;
        }

        static uint64_t hash(const std::span<const char> bytes) noexcept
        {
            return hash_append(hash_seed, bytes);
        }

        void record(read_entry entry)
        {
            if (!enabled_) return;
            if (entry.path.size() > max_path_bytes)
            {
                size_t boundary = max_path_bytes;
                while (boundary && (static_cast<unsigned char>(entry.path[boundary]) & 0xc0) == 0x80)
                    --boundary;
                entry.path.resize(boundary);
            }
            const std::lock_guard lock(mutex_);
            if (triggered_) return;
            entries_[next_ % capacity] = std::move(entry);
            ++next_;
        }

        // Returns true exactly once and freezes the ring for direct status/MCP reads.
        bool trigger_on_oodle(const std::string_view message)
        {
            if (!enabled_ || message.find("OODLE ERROR") == std::string_view::npos) return false;
            const std::lock_guard lock(mutex_);
            if (triggered_) return false;
            triggered_ = true;
            return true;
        }

        [[nodiscard]] std::string snapshot_json() const
        {
            const std::lock_guard lock(mutex_);
            std::string result = "{\"enabled\":";
            result += enabled_ ? "true" : "false";
            result += ",\"triggered\":";
            result += triggered_ ? "true" : "false";
            result += ",\"capacity\":" + std::to_string(capacity) + ",\"reads\":[";
            if (triggered_)
            {
                const auto count = std::min<uint64_t>(next_, capacity);
                const auto first = next_ - count;
                for (uint64_t i = 0; i < count; ++i)
                {
                    const auto& entry = entries_[(first + i) % capacity];
                    if (i) result += ',';
                    result += "{\"path\":\"" + escape_json(entry.path) + "\",\"handle\":" + std::to_string(entry.handle);
                    result += ",\"tid\":" + std::to_string(entry.tid) + ",\"offset\":" + std::to_string(entry.offset);
                    result += ",\"requested\":" + std::to_string(entry.requested) + ",\"actual\":" + std::to_string(entry.actual);
                    result += ",\"hashed_bytes\":" + std::to_string(entry.hashed_bytes);
                    result += ",\"truncated\":";
                    result += entry.actual > entry.hashed_bytes ? "true" : "false";
                    result += ",\"host_fnv1a64\":\"" + hex(entry.host_hash) + "\"";
                    result += ",\"guest_fnv1a64\":";
                    result += entry.guest_valid ? "\"" + hex(entry.guest_hash) + "\"" : "null";
                    result += '}';
                }
            }
            result += "]}";
            return result;
        }

      private:
        static bool env_enabled() noexcept
        {
            const auto* value = std::getenv("SOGEN_TRACE_PACKAGE_READS");
            return value && *value && *value != '0';
        }

        static std::string hex(const uint64_t value)
        {
            std::ostringstream out;
            out << std::hex << std::setfill('0') << std::setw(16) << value;
            return out.str();
        }

        static std::string escape_json(const std::string_view value)
        {
            std::string escaped;
            for (const unsigned char c : value)
            {
                if (c == '"' || c == '\\') { escaped += '\\'; escaped += static_cast<char>(c); }
                else if (c < 0x20)
                {
                    constexpr char digits[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped += digits[c >> 4];
                    escaped += digits[c & 0xf];
                }
                else escaped += static_cast<char>(c);
            }
            return escaped;
        }

        const bool enabled_{};
        mutable std::mutex mutex_{};
        std::array<read_entry, capacity> entries_{};
        uint64_t next_{};
        bool triggered_{};
    };
}
