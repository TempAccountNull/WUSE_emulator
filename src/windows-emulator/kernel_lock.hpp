#pragma once

#include <platform/compiler.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>

namespace sogen
{

    // See docs/multi-vcpu-design.md, sections 2 and 7. Guest execution normally releases
    // this lock; a single-vCPU instruction-precise quantum retains it for same-thread callbacks.
    class kernel_lock
    {
      public:
        struct profile_stats
        {
            uint64_t acquisitions; // total successful lock() calls
            uint64_t contended;    // acquisitions that had to block because another thread held it
            uint64_t wait_nanos;   // cumulative time spent blocked on contended acquisitions
            uint64_t held_nanos;   // cumulative time the lock was held (BEL "busy" time)
        };

        struct owner_snapshot
        {
            uint64_t generation{};
            const char* site{};
            uint64_t detail{};
            uint64_t host_thread{};
            uint64_t held_nanos{};

            explicit operator bool() const
            {
                return site != nullptr;
            }
        };

        static bool attribution_enabled()
        {
            static const bool enabled = [] {
                const char* value = std::getenv("SOGEN_LOCK_ATTRIBUTION");
                return value && value[0] == '1' && value[1] == '\0';
            }();
            return enabled;
        }

        // Set before attempting the lock. Nested scopes restore the enclosing site, including
        // when guest callbacks borrow an instruction-precise quantum's physical mutex.
        class attribution_scope
        {
          public:
            attribution_scope(const char* site, uint64_t detail = 0)
                : enabled_(attribution_enabled())
            {
                if (enabled_)
                {
                    previous_site_ = requested_site_;
                    previous_detail_ = requested_detail_;
                    requested_site_ = site;
                    requested_detail_ = detail;
                }
            }

            ~attribution_scope()
            {
                if (enabled_)
                {
                    requested_site_ = previous_site_;
                    requested_detail_ = previous_detail_;
                }
            }

            attribution_scope(const attribution_scope&) = delete;
            attribution_scope& operator=(const attribution_scope&) = delete;

          private:
            bool enabled_{};
            const char* previous_site_{};
            uint64_t previous_detail_{};
        };

        // Env-gated so a normal run pays nothing: the instrumented path (a try_lock plus two
        // steady_clock reads per acquisition) only runs when SOGEN_LOCK_PROFILE is set.
        static bool profiling_enabled()
        {
            static const bool enabled = std::getenv("SOGEN_LOCK_PROFILE") != nullptr || attribution_enabled();
            return enabled;
        }

        // Nonblocking, bounded snapshot: safe even while another host thread owns the mutex.
        owner_snapshot current_owner() const
        {
            if (!attribution_enabled())
            {
                return {};
            }
            for (unsigned attempt = 0; attempt != 4; ++attempt)
            {
                const auto first = owner_generation_.load(std::memory_order_seq_cst);
                if (first & 1)
                {
                    continue;
                }
                const auto site = owner_site_.load(std::memory_order_seq_cst);
                const auto detail = owner_detail_.load(std::memory_order_seq_cst);
                const auto thread = owner_thread_.load(std::memory_order_seq_cst);
                const auto since = owner_since_ns_.load(std::memory_order_seq_cst);
                const auto last = owner_generation_.load(std::memory_order_seq_cst);
                if (first == last && !(last & 1))
                {
                    const auto now = clock_nanos();
                    return {last, site, detail, thread, site && now >= since ? now - since : 0};
                }
            }
            return {};
        }

        class guest_execution_scope
        {
          public:
            guest_execution_scope(kernel_lock& lock, const bool enabled)
                : lock_(enabled ? &lock : nullptr)
            {
                if (this->lock_)
                {
                    assert(!guest_owner_);
                    this->lock_->lock();
                    guest_owner_ = this->lock_;
                }
            }

            ~guest_execution_scope()
            {
                if (this->lock_)
                {
                    assert(!guest_callback_active_);
                    guest_owner_ = nullptr;
                    this->lock_->unlock();
                }
            }

            guest_execution_scope(const guest_execution_scope&) = delete;
            guest_execution_scope& operator=(const guest_execution_scope&) = delete;
            guest_execution_scope(guest_execution_scope&&) = delete;
            guest_execution_scope& operator=(guest_execution_scope&&) = delete;

          private:
            kernel_lock* lock_{};
        };

        void lock()
        {
            if (guest_owner_ == this)
            {
                assert(!guest_callback_active_ && "The kernel lock is not recursive");
                guest_callback_active_ = true;
                return;
            }
            this->lock_slow();
        }

        // Acquire only if the lock is free. Lets a host-owned thread touch kernel state without ever stalling
        // behind a long-running emulator operation; the caller is expected to retry later or skip the work.
        bool try_lock()
        {
            if (guest_owner_ == this)
            {
                assert(!guest_callback_active_ && "The kernel lock is not recursive");
                guest_callback_active_ = true;
                return true;
            }
            assert(!this->is_held_by_current_thread() && "The kernel lock is not recursive");

            if (!this->mutex_.try_lock())
            {
                return false;
            }

            if (profiling_enabled())
            {
                this->acquisitions_.fetch_add(1, std::memory_order_relaxed);
                this->held_since_ = std::chrono::steady_clock::now();
            }

            this->publish_owner();

            this->owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
            return true;
        }

        void unlock()
        {
            if (guest_owner_ == this)
            {
                assert(guest_callback_active_);
                guest_callback_active_ = false;
                return;
            }
            this->unlock_slow();
        }

        bool is_held_by_current_thread() const
        {
            return this->owner_.load(std::memory_order_relaxed) == std::this_thread::get_id();
        }

        void assert_held() const
        {
            // Evaluated outside assert() so the member access survives in NDEBUG builds (where assert
            // expands to nothing); the relaxed load is otherwise dead and elided by the optimizer.
            [[maybe_unused]] const bool held = this->is_held_by_current_thread();
            assert(held && "The kernel lock must be held");
        }

        profile_stats profile() const
        {
            return {
                .acquisitions = this->acquisitions_.load(std::memory_order_relaxed),
                .contended = this->contended_.load(std::memory_order_relaxed),
                .wait_nanos = this->wait_nanos_.load(std::memory_order_relaxed),
                .held_nanos = this->held_nanos_.load(std::memory_order_relaxed),
            };
        }

      private:
        using clock = std::chrono::steady_clock;

        // A borrowed callback only changes the thread-local borrowing flag. The real mutex and
        // profiling work stay out of that path; the surrounding quantum still owns the mutex.
        NO_INLINE void lock_slow()
        {
            assert(!this->is_held_by_current_thread() && "The kernel lock is not recursive");

            if (profiling_enabled())
            {
                this->lock_profiled();
            }
            else
            {
                this->mutex_.lock();
            }

            this->publish_owner();
            this->owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
        }

        NO_INLINE void unlock_slow()
        {
            this->clear_owner();
            this->owner_.store({}, std::memory_order_relaxed);

            if (profiling_enabled())
            {
                this->held_nanos_.fetch_add(nanos_since(this->held_since_), std::memory_order_relaxed);
            }

            this->mutex_.unlock();
        }

        static uint64_t nanos_since(const clock::time_point start)
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count());
        }

        static uint64_t clock_nanos()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());
        }

        void publish_owner()
        {
            if (!attribution_enabled())
            {
                return;
            }
            owner_generation_.fetch_add(1, std::memory_order_seq_cst); // odd: updating
            owner_site_.store(requested_site_ ? requested_site_ : "unattributed", std::memory_order_seq_cst);
            owner_detail_.store(requested_detail_, std::memory_order_seq_cst);
            owner_thread_.store(static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())), std::memory_order_seq_cst);
            owner_since_ns_.store(clock_nanos(), std::memory_order_seq_cst);
            owner_generation_.fetch_add(1, std::memory_order_seq_cst); // even: stable
        }

        void clear_owner()
        {
            if (!attribution_enabled())
            {
                return;
            }
            owner_generation_.fetch_add(1, std::memory_order_seq_cst);
            owner_site_.store(nullptr, std::memory_order_seq_cst);
            owner_detail_.store(0, std::memory_order_seq_cst);
            owner_thread_.store(0, std::memory_order_seq_cst);
            owner_since_ns_.store(0, std::memory_order_seq_cst);
            owner_generation_.fetch_add(1, std::memory_order_seq_cst);
        }

        void lock_profiled()
        {
            if (!this->mutex_.try_lock())
            {
                const auto start = clock::now();
                this->mutex_.lock();
                this->wait_nanos_.fetch_add(nanos_since(start), std::memory_order_relaxed);
                this->contended_.fetch_add(1, std::memory_order_relaxed);
            }

            this->acquisitions_.fetch_add(1, std::memory_order_relaxed);
            this->held_since_ = clock::now();
        }

        // Only software backends with cooperative instruction time slicing use this scope.
        // The real mutex stays held for the quantum; callbacks borrow it on the same host thread.
        static inline thread_local kernel_lock* guest_owner_{};
        static inline thread_local bool guest_callback_active_{};
        static inline thread_local const char* requested_site_{};
        static inline thread_local uint64_t requested_detail_{};

        std::mutex mutex_{};
        std::atomic<std::thread::id> owner_{};

        // Guarded by mutex_ (written/read only by the current holder).
        clock::time_point held_since_{};

        std::atomic<uint64_t> acquisitions_{};
        std::atomic<uint64_t> contended_{};
        std::atomic<uint64_t> wait_nanos_{};
        std::atomic<uint64_t> held_nanos_{};
        std::atomic<uint64_t> owner_generation_{};
        std::atomic<const char*> owner_site_{};
        std::atomic<uint64_t> owner_detail_{};
        std::atomic<uint64_t> owner_thread_{};
        std::atomic<uint64_t> owner_since_ns_{};
    };

} // namespace sogen
