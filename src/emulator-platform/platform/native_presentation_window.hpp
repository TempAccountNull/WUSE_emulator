#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace sogen
{
    struct win32_presentation_target
    {
        uintptr_t window{};
        uintptr_t instance{};
    };

    class native_presentation_window_resource
    {
      public:
        virtual ~native_presentation_window_resource() = default;
        virtual win32_presentation_target target() const noexcept = 0;
        virtual bool suspend_legacy_presentation() noexcept = 0;
        virtual bool resume_legacy_presentation() noexcept = 0;
        virtual void retire() noexcept = 0;
    };

    namespace detail
    {
        struct native_presentation_window_record
        {
            uint64_t guest_window{};
            uint64_t generation{};
            win32_presentation_target target{};
            std::atomic<bool> live{true};
            std::atomic<size_t> pins{};
            bool top_level{};
            bool legacy_suspended{};
            std::unique_ptr<native_presentation_window_resource> resource{};
        };
    }

    class native_presentation_window_lease
    {
      public:
        native_presentation_window_lease() = default;

        native_presentation_window_lease(const native_presentation_window_lease& other) noexcept
            : record_(other.record_)
        {
            this->pin();
        }

        native_presentation_window_lease& operator=(const native_presentation_window_lease& other) noexcept
        {
            if (this != &other)
            {
                native_presentation_window_lease copy{other};
                this->swap(copy);
            }
            return *this;
        }

        native_presentation_window_lease(native_presentation_window_lease&&) noexcept = default;

        native_presentation_window_lease& operator=(native_presentation_window_lease&& other) noexcept
        {
            if (this != &other)
            {
                this->release();
                this->record_ = std::move(other.record_);
            }
            return *this;
        }

        ~native_presentation_window_lease()
        {
            this->release();
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(this->record_);
        }

        [[nodiscard]] bool live() const noexcept
        {
            return this->record_ && this->record_->live.load(std::memory_order_acquire);
        }

        [[nodiscard]] uint64_t guest_window() const noexcept
        {
            return this->record_ ? this->record_->guest_window : 0;
        }

        [[nodiscard]] uint64_t generation() const noexcept
        {
            return this->record_ ? this->record_->generation : 0;
        }

        [[nodiscard]] win32_presentation_target target() const noexcept
        {
            return this->record_ ? this->record_->target : win32_presentation_target{};
        }

        void swap(native_presentation_window_lease& other) noexcept
        {
            this->record_.swap(other.record_);
        }

      private:
        friend class native_presentation_window_registry;

        explicit native_presentation_window_lease(std::shared_ptr<detail::native_presentation_window_record> record) noexcept
            : record_(std::move(record))
        {
            this->pin();
        }

        void pin() noexcept
        {
            if (this->record_)
            {
                this->record_->pins.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void release() noexcept
        {
            if (this->record_)
            {
                // No SDL calls or UI commands here: a final lease may be released under kernel_lock.
                // The registry owns the native resource until a later UI-thread collect().
                this->record_->pins.fetch_sub(1, std::memory_order_release);
                this->record_.reset();
            }
        }

        std::shared_ptr<detail::native_presentation_window_record> record_{};
    };

    enum class native_window_acquire_status
    {
        success,
        registry_closed,
        unknown_window,
        stale_generation,
        child_window_unsupported,
        native_target_unavailable,
        legacy_handoff_failed,
    };

    struct native_window_acquisition
    {
        native_window_acquire_status status{};
        native_presentation_window_lease lease{};
    };

    struct native_window_collection
    {
        size_t destroyed{};
        size_t restored{};
        size_t restore_failed{};
    };

    class native_presentation_window_registry
    {
      public:
        native_presentation_window_registry() = default;
        native_presentation_window_registry(const native_presentation_window_registry&) = delete;
        native_presentation_window_registry& operator=(const native_presentation_window_registry&) = delete;

        ~native_presentation_window_registry()
        {
            // Destroying the UI backend before its Vulkan leases would leave live WSI objects with
            // dangling HWNDs. Shutdown must first release GPU resources and collect on the UI thread.
            if (!this->records_.empty())
            {
                std::terminate();
            }
        }

        uint64_t publish(const uint64_t guest_window, const bool top_level,
                         std::unique_ptr<native_presentation_window_resource> resource = {})
        {
            this->require_owner();
            if (this->closed_ || guest_window == 0 || this->next_generation_ == std::numeric_limits<uint64_t>::max())
            {
                throw std::logic_error("Cannot publish native presentation window");
            }
            if (!top_level && resource)
            {
                throw std::invalid_argument("A child window cannot own a native top-level presentation target");
            }

            auto record = std::make_shared<detail::native_presentation_window_record>();
            record->guest_window = guest_window;
            record->generation = this->next_generation_;
            record->top_level = top_level;
            record->target = resource ? resource->target() : win32_presentation_target{};
            record->resource = std::move(resource);

            this->records_.emplace(record->generation, record);
            try
            {
                const auto [it, inserted] = this->current_.try_emplace(guest_window, record);
                if (!inserted)
                {
                    native_presentation_window_registry::retire_record(*it->second);
                    it->second = record;
                }
            }
            catch (...)
            {
                this->records_.erase(record->generation);
                throw;
            }
            ++this->next_generation_;
            return record->generation;
        }

        native_window_acquisition acquire(const uint64_t guest_window, const uint64_t expected_generation = 0)
        {
            this->require_owner();
            if (this->closed_)
            {
                return {.status = native_window_acquire_status::registry_closed, .lease = {}};
            }
            const auto it = this->current_.find(guest_window);
            if (it == this->current_.end())
            {
                return {.status = native_window_acquire_status::unknown_window, .lease = {}};
            }
            auto& record = *it->second;
            if (record.pins.load(std::memory_order_acquire) == 0 && record.resource)
            {
                this->refresh_target(record);
            }
            if (expected_generation != 0 && expected_generation != record.generation)
            {
                return {.status = native_window_acquire_status::stale_generation, .lease = {}};
            }
            if (!record.top_level)
            {
                return {.status = native_window_acquire_status::child_window_unsupported, .lease = {}};
            }
            if (!record.resource || record.target.window == 0 || record.target.instance == 0)
            {
                return {.status = native_window_acquire_status::native_target_unavailable, .lease = {}};
            }
            if (!record.legacy_suspended)
            {
                if (!record.resource->suspend_legacy_presentation())
                {
                    return {.status = native_window_acquire_status::legacy_handoff_failed, .lease = {}};
                }
                record.legacy_suspended = true;
                this->refresh_target(record);
                if (expected_generation != 0 && expected_generation != record.generation)
                {
                    return {.status = native_window_acquire_status::stale_generation, .lease = {}};
                }
                if (record.target.window == 0 || record.target.instance == 0)
                {
                    return {.status = native_window_acquire_status::native_target_unavailable, .lease = {}};
                }
            }
            return {.status = native_window_acquire_status::success, .lease = native_presentation_window_lease{it->second}};
        }

        void retire(const uint64_t guest_window)
        {
            this->require_owner();
            const auto it = this->current_.find(guest_window);
            if (it != this->current_.end())
            {
                native_presentation_window_registry::retire_record(*it->second);
                this->current_.erase(it);
            }
        }

        void reset()
        {
            this->require_owner();
            for (auto& [guest, record] : this->current_)
            {
                (void)guest;
                native_presentation_window_registry::retire_record(*record);
            }
            this->current_.clear();
        }

        void close()
        {
            this->require_owner();
            this->closed_ = true;
            this->reset();
        }

        native_window_collection collect()
        {
            this->require_owner();
            native_window_collection result{};
            for (auto it = this->records_.begin(); it != this->records_.end();)
            {
                auto& record = *it->second;
                if (record.pins.load(std::memory_order_acquire) != 0)
                {
                    ++it;
                    continue;
                }
                if (!record.live.load(std::memory_order_relaxed))
                {
                    record.resource.reset();
                    it = this->records_.erase(it);
                    ++result.destroyed;
                    continue;
                }
                if (record.legacy_suspended)
                {
                    if (record.resource->resume_legacy_presentation())
                    {
                        record.legacy_suspended = false;
                        ++result.restored;
                    }
                    else
                    {
                        ++result.restore_failed;
                    }
                    // SDL renderer fallback may replace the native HWND even when creation fails.
                    // No lease exists here; refresh the target and invalidate old generation tokens.
                    this->refresh_target(record);
                }
                ++it;
            }
            return result;
        }

        [[nodiscard]] bool drained() const
        {
            this->require_owner();
            return this->records_.empty();
        }

      private:
        void refresh_target(detail::native_presentation_window_record& record)
        {
            const auto target = record.resource->target();
            if (record.target.window == target.window && record.target.instance == target.instance)
            {
                return;
            }
            if (this->next_generation_ == std::numeric_limits<uint64_t>::max())
            {
                throw std::overflow_error("Native presentation generation exhausted");
            }
            record.target = target;
            record.generation = this->next_generation_++;
        }

        void require_owner() const
        {
            if (this->owner_ != std::this_thread::get_id())
            {
                throw std::logic_error("Native presentation window registry accessed off UI thread");
            }
        }

        static void retire_record(detail::native_presentation_window_record& record) noexcept
        {
            record.live.store(false, std::memory_order_release);
            if (record.resource)
            {
                record.resource->retire();
            }
        }

        const std::thread::id owner_{std::this_thread::get_id()};
        std::unordered_map<uint64_t, std::shared_ptr<detail::native_presentation_window_record>> current_{};
        // The key is the immutable publication ID. A record's acquisition generation advances
        // independently if SDL replaces its HWND while there are no outstanding leases.
        std::unordered_map<uint64_t, std::shared_ptr<detail::native_presentation_window_record>> records_{};
        uint64_t next_generation_{1};
        bool closed_{};
    };
}
