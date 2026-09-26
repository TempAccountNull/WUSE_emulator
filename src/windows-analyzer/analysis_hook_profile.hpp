#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace sogen
{

    enum class analysis_profile_channel
    {
        object_callback,
        environment_callback,
        object_report,
        environment_report,
    };

    struct sampled_analysis_duration
    {
        std::atomic_uint64_t samples{};
        std::atomic_uint64_t sampled_nanos{};
        std::atomic_uint64_t max_nanos{};

        void record(const uint64_t nanos) noexcept
        {
            this->samples.fetch_add(1, std::memory_order_relaxed);
            this->sampled_nanos.fetch_add(nanos, std::memory_order_relaxed);
            auto current = this->max_nanos.load(std::memory_order_relaxed);
            while (current < nanos && !this->max_nanos.compare_exchange_weak(current, nanos, std::memory_order_relaxed))
            {
            }
        }
    };

    struct analysis_hook_profile
    {
        static constexpr uint64_t sample_period = 1024;

        bool enabled{};
        sampled_analysis_duration object_callback{};
        sampled_analysis_duration object_lock_wait{};
        sampled_analysis_duration environment_callback{};
        sampled_analysis_duration object_report{};
        sampled_analysis_duration environment_report{};
    };

    template <analysis_profile_channel Channel>
    class sampled_analysis_timer
    {
      public:
        sampled_analysis_timer(const analysis_hook_profile* profile, sampled_analysis_duration* duration) noexcept
            : duration_(duration)
        {
            if (!profile || !profile->enabled || !duration)
            {
                return;
            }
            static thread_local uint64_t ordinal{};
            if ((++ordinal & (analysis_hook_profile::sample_period - 1)) == 0)
            {
                this->start_ = std::chrono::steady_clock::now();
                this->sampled_ = true;
            }
        }

        ~sampled_analysis_timer()
        {
            if (this->sampled_)
            {
                const auto elapsed = std::chrono::steady_clock::now() - this->start_;
                const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
                this->duration_->record(static_cast<uint64_t>(nanos));
            }
        }

        sampled_analysis_timer(const sampled_analysis_timer&) = delete;
        sampled_analysis_timer& operator=(const sampled_analysis_timer&) = delete;

        bool sampled() const noexcept
        {
            return this->sampled_;
        }

      private:
        sampled_analysis_duration* duration_{};
        std::chrono::steady_clock::time_point start_{};
        bool sampled_{};
    };

} // namespace sogen
