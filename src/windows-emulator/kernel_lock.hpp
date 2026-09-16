#pragma once

#include <platform/compiler.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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

        // Env-gated so a normal run pays nothing: the instrumented path (a try_lock plus two
        // steady_clock reads per acquisition) only runs when SOGEN_LOCK_PROFILE is set.
        static bool profiling_enabled()
        {
            static const bool enabled = std::getenv("SOGEN_LOCK_PROFILE") != nullptr;
            return enabled;
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

            this->owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
        }

        NO_INLINE void unlock_slow()
        {
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

        std::mutex mutex_{};
        std::atomic<std::thread::id> owner_{};

        // Guarded by mutex_ (written/read only by the current holder).
        clock::time_point held_since_{};

        std::atomic<uint64_t> acquisitions_{};
        std::atomic<uint64_t> contended_{};
        std::atomic<uint64_t> wait_nanos_{};
        std::atomic<uint64_t> held_nanos_{};
    };

} // namespace sogen
