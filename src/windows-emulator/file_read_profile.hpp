#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace sogen
{
    class file_read_profile
    {
      public:
        struct sample
        {
            uint64_t requested_bytes{};
            uint64_t read_bytes{};
            uint64_t allocation_nanos{};
            uint64_t seek_nanos{};
            uint64_t host_read_nanos{};
            uint64_t guest_write_nanos{};
            uint64_t total_nanos{};
        };

        struct bucket
        {
            uint64_t calls{};
            uint64_t requested_bytes{};
            uint64_t read_bytes{};
            uint64_t allocation_nanos{};
            uint64_t seek_nanos{};
            uint64_t host_read_nanos{};
            uint64_t guest_write_nanos{};
            uint64_t total_nanos{};
        };

        struct snapshot
        {
            bucket package{};
            bucket other{};
        };

        explicit file_read_profile(const bool enabled = env_enabled())
            : enabled_(enabled)
        {
        }

        [[nodiscard]] bool enabled() const noexcept
        {
            return enabled_;
        }

        void record(const bool package, const sample& value) noexcept
        {
            if (!enabled_)
            {
                return;
            }

            auto& target = buckets_[package ? 0 : 1];
            target.calls.fetch_add(1, std::memory_order_relaxed);
            target.requested_bytes.fetch_add(value.requested_bytes, std::memory_order_relaxed);
            target.read_bytes.fetch_add(value.read_bytes, std::memory_order_relaxed);
            target.allocation_nanos.fetch_add(value.allocation_nanos, std::memory_order_relaxed);
            target.seek_nanos.fetch_add(value.seek_nanos, std::memory_order_relaxed);
            target.host_read_nanos.fetch_add(value.host_read_nanos, std::memory_order_relaxed);
            target.guest_write_nanos.fetch_add(value.guest_write_nanos, std::memory_order_relaxed);
            target.total_nanos.fetch_add(value.total_nanos, std::memory_order_relaxed);
        }

        [[nodiscard]] snapshot read() const noexcept
        {
            return {read_bucket(buckets_[0]), read_bucket(buckets_[1])};
        }

      private:
        struct atomic_bucket
        {
            std::atomic<uint64_t> calls{};
            std::atomic<uint64_t> requested_bytes{};
            std::atomic<uint64_t> read_bytes{};
            std::atomic<uint64_t> allocation_nanos{};
            std::atomic<uint64_t> seek_nanos{};
            std::atomic<uint64_t> host_read_nanos{};
            std::atomic<uint64_t> guest_write_nanos{};
            std::atomic<uint64_t> total_nanos{};
        };

        static bucket read_bucket(const atomic_bucket& source) noexcept
        {
            return {
                source.calls.load(std::memory_order_relaxed),
                source.requested_bytes.load(std::memory_order_relaxed),
                source.read_bytes.load(std::memory_order_relaxed),
                source.allocation_nanos.load(std::memory_order_relaxed),
                source.seek_nanos.load(std::memory_order_relaxed),
                source.host_read_nanos.load(std::memory_order_relaxed),
                source.guest_write_nanos.load(std::memory_order_relaxed),
                source.total_nanos.load(std::memory_order_relaxed),
            };
        }

        static bool env_enabled() noexcept
        {
            const char* value = std::getenv("SOGEN_PROFILE_FILE_READ");
            return value && *value && *value != '0';
        }

        const bool enabled_;
        std::array<atomic_bucket, 2> buckets_{};
    };
}
