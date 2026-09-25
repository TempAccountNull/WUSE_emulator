#pragma once

#include "handles.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace sogen
{
    class thread_handle_journal
    {
      public:
        enum class operation : uint8_t
        {
            create,
            open,
            duplicate,
            close,
            terminate,
            reap,
        };

        struct entry
        {
            uint64_t sequence{};
            operation action{};
            handle value{};
            uint32_t target_tid{};
            uint32_t caller_tid{};
            uint64_t caller_rip{};
            uint64_t detail{};
            uint32_t refs_before{};
            uint32_t refs_after{};
            bool removed{};
        };

        thread_handle_journal()
        {
            const auto* explicit_probe = std::getenv("SOGEN_THREAD_HANDLE_HISTORY");
            const auto* throw_probe = std::getenv("SOGEN_GUEST_CXX_THROW_PROBE");
            enabled_ = (explicit_probe && *explicit_probe == '1') || (throw_probe && *throw_probe == '1');
        }

        explicit thread_handle_journal(const bool enabled)
            : enabled_(enabled)
        {
        }

        bool enabled() const
        {
            return enabled_;
        }

        void record(entry event)
        {
            if (!enabled_)
            {
                return;
            }

            const std::scoped_lock lock(mutex_);
            event.sequence = ++sequence_;
            entries_[next_++ % entries_.size()] = event;
        }

        // Restoring a snapshot replaces the thread table. Events from the abandoned
        // execution timeline cannot explain handles in the restored table.
        void clear()
        {
            const std::scoped_lock lock(mutex_);
            entries_ = {};
            sequence_ = 0;
            next_ = 0;
            failure_samples_.store(0, std::memory_order_relaxed);
        }

        bool claim_failure_sample()
        {
            if (!enabled_)
            {
                return false;
            }
            auto count = failure_samples_.load(std::memory_order_relaxed);
            while (count < max_failure_samples)
            {
                if (failure_samples_.compare_exchange_weak(count, count + 1, std::memory_order_relaxed))
                {
                    return true;
                }
            }
            return false;
        }

        std::vector<entry> recent(const handle value, const size_t limit = 32) const
        {
            return this->recent_matching([&](const entry& event) { return event.value == value; }, limit);
        }

        // Once a raw/untyped handle goes stale, the wait resolver cannot recover its
        // thread type from the live store. Its numeric slot can still locate history.
        std::vector<entry> recent_by_id(const uint32_t id, const size_t limit = 32) const
        {
            if (id == 0)
            {
                return {};
            }
            return this->recent_matching([&](const entry& event) { return event.value.value.id == id; }, limit);
        }

        static const char* name(const operation action)
        {
            switch (action)
            {
            case operation::create:
                return "create";
            case operation::open:
                return "open";
            case operation::duplicate:
                return "duplicate";
            case operation::close:
                return "close";
            case operation::terminate:
                return "terminate";
            case operation::reap:
                return "reap";
            }

            return "unknown";
        }

      private:
        template <typename Predicate>
        std::vector<entry> recent_matching(Predicate&& matches, const size_t limit) const
        {
            std::vector<entry> result{};
            if (!enabled_)
            {
                return result;
            }

            const std::scoped_lock lock(mutex_);
            for (const auto& event : entries_)
            {
                if (event.sequence && matches(event))
                {
                    result.push_back(event);
                }
            }

            std::sort(result.begin(), result.end(), [](const entry& left, const entry& right) { return left.sequence < right.sequence; });
            if (result.size() > limit)
            {
                result.erase(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(result.size() - limit));
            }
            return result;
        }

        static constexpr uint32_t max_failure_samples = 16;
        bool enabled_{};
        mutable std::mutex mutex_{};
        std::array<entry, 512> entries_{};
        uint64_t sequence_{};
        size_t next_{};
        std::atomic<uint32_t> failure_samples_{};
    };
}
