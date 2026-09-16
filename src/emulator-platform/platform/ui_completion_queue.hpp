#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace sogen
{
    enum class ui_completion_status
    {
        queued,
        running,
        completed,
        cancelled,
        queue_closed,
        failed,
    };

    inline bool ui_completion_is_terminal(const ui_completion_status status) noexcept
    {
        return status != ui_completion_status::queued && status != ui_completion_status::running;
    }

    class ui_cancellation_token
    {
      public:
        explicit ui_cancellation_token(const std::atomic<bool>& requested) noexcept
            : requested_(&requested)
        {
        }

        [[nodiscard]] bool requested() const noexcept
        {
            return this->requested_->load(std::memory_order_acquire);
        }

      private:
        const std::atomic<bool>* requested_;
    };

    template <typename T>
    struct ui_completion_result
    {
        ui_completion_status status{};
        std::optional<T> value{};
        std::exception_ptr error{};
    };

    namespace detail
    {
        template <typename T>
        struct ui_completion_state
        {
            std::mutex mutex{};
            std::atomic<ui_completion_status> status{ui_completion_status::queued};
            std::atomic<bool> cancel_requested{};
            std::optional<T> value{};
            std::exception_ptr error{};
            bool consumed{};

            void cancel() noexcept
            {
                const std::lock_guard lock(this->mutex);
                const auto current = this->status.load(std::memory_order_relaxed);
                if (ui_completion_is_terminal(current))
                {
                    return;
                }
                this->cancel_requested.store(true, std::memory_order_release);
                if (current == ui_completion_status::queued)
                {
                    this->status.store(ui_completion_status::cancelled, std::memory_order_release);
                }
            }
        };
    }

    template <typename T>
    class ui_completion_ticket
    {
      public:
        ui_completion_ticket() = default;
        ui_completion_ticket(const ui_completion_ticket&) = delete;
        ui_completion_ticket& operator=(const ui_completion_ticket&) = delete;
        ui_completion_ticket(ui_completion_ticket&&) noexcept = default;

        ui_completion_ticket& operator=(ui_completion_ticket&& other) noexcept
        {
            if (this != &other)
            {
                this->cancel();
                this->state_ = std::move(other.state_);
            }
            return *this;
        }

        ~ui_completion_ticket()
        {
            this->cancel();
        }

        [[nodiscard]] bool ready() const noexcept
        {
            return this->state_ && ui_completion_is_terminal(this->state_->status.load(std::memory_order_acquire));
        }

        void cancel() noexcept
        {
            if (this->state_)
            {
                this->state_->cancel();
            }
        }

        void detach() noexcept
        {
            this->state_.reset();
        }

        std::optional<ui_completion_result<T>> try_take()
        {
            if (!this->ready())
            {
                return std::nullopt;
            }
            const std::lock_guard lock(this->state_->mutex);
            if (this->state_->consumed)
            {
                return std::nullopt;
            }
            this->state_->consumed = true;
            return ui_completion_result<T>{this->state_->status.load(std::memory_order_relaxed), std::move(this->state_->value),
                                           this->state_->error};
        }

      private:
        friend class ui_completion_queue;

        explicit ui_completion_ticket(std::shared_ptr<detail::ui_completion_state<T>> state)
            : state_(std::move(state))
        {
        }

        std::shared_ptr<detail::ui_completion_state<T>> state_{};
    };

    class ui_completion_queue
    {
      public:
        ui_completion_queue() = default;
        ui_completion_queue(const ui_completion_queue&) = delete;
        ui_completion_queue& operator=(const ui_completion_queue&) = delete;

        ~ui_completion_queue()
        {
            this->close();
        }

        // Results and captures must permit destruction on the posting/receiving thread; native window
        // leases meet that contract, raw SDL/Vulkan resource owners do not.
        // Never execute inline: even the UI thread can enter a syscall while holding kernel_lock.
        // Vulkan WSI can send synchronous window messages (VK_KHR_win32_surface, wsi.adoc).
        template <typename F>
        auto post(F&& operation)
        {
            using function_type = std::decay_t<F>;
            using value_type = std::invoke_result_t<function_type&, const ui_cancellation_token&>;
            static_assert(!std::is_void_v<value_type> && std::is_nothrow_move_constructible_v<value_type>);

            auto state = std::make_shared<detail::ui_completion_state<value_type>>();
            auto task = std::make_unique<operation_task<value_type, function_type>>(state, std::forward<F>(operation));
            {
                const std::lock_guard lock(this->mutex_);
                if (this->closed_)
                {
                    task->close();
                }
                else
                {
                    this->tasks_.emplace_back(std::move(task));
                }
            }
            return ui_completion_ticket<value_type>{std::move(state)};
        }

        size_t pump(const size_t budget = 64)
        {
            std::vector<std::unique_ptr<task_base>> pending;
            {
                const std::lock_guard lock(this->mutex_);
                this->bind_or_require_owner();
                if (this->pumping_)
                {
                    throw std::logic_error("Recursive UI completion pump");
                }
                pending.reserve(std::min(budget, this->tasks_.size()));
                while (pending.size() < budget && !this->tasks_.empty())
                {
                    pending.emplace_back(std::move(this->tasks_.front()));
                    this->tasks_.pop_front();
                }
                this->pumping_ = true;
            }

            for (auto& task : pending)
            {
                task->run();
            }
            {
                const std::lock_guard lock(this->mutex_);
                this->pumping_ = false;
            }
            return pending.size();
        }

        void close() noexcept
        {
            std::deque<std::unique_ptr<task_base>> pending;
            {
                const std::lock_guard lock(this->mutex_);
                if (this->owner_ != std::thread::id{} && this->owner_ != std::this_thread::get_id())
                {
                    std::terminate();
                }
                if (this->pumping_)
                {
                    std::terminate();
                }
                this->closed_ = true;
                pending.swap(this->tasks_);
            }
            for (auto& task : pending)
            {
                task->close();
            }
        }

        [[nodiscard]] bool owner_matches_current()
        {
            const std::lock_guard lock(this->mutex_);
            return this->owner_ == std::this_thread::get_id();
        }

        [[nodiscard]] bool pending()
        {
            const std::lock_guard lock(this->mutex_);
            return !this->tasks_.empty();
        }

      private:
        struct task_base
        {
            virtual ~task_base() = default;
            virtual void run() noexcept = 0;
            virtual void close() noexcept = 0;
        };

        template <typename T, typename F>
        struct operation_task final : task_base
        {
            std::shared_ptr<detail::ui_completion_state<T>> state;
            F operation;

            template <typename Fn>
            operation_task(std::shared_ptr<detail::ui_completion_state<T>> state_value, Fn&& function)
                : state(std::move(state_value)),
                  operation(std::forward<Fn>(function))
            {
            }

            void run() noexcept override
            {
                {
                    const std::lock_guard lock(this->state->mutex);
                    if (this->state->status.load(std::memory_order_relaxed) != ui_completion_status::queued)
                    {
                        return;
                    }
                    this->state->status.store(ui_completion_status::running, std::memory_order_release);
                }
                try
                {
                    auto value = std::invoke(this->operation, ui_cancellation_token{this->state->cancel_requested});
                    const std::lock_guard lock(this->state->mutex);
                    if (this->state->cancel_requested.load(std::memory_order_acquire))
                    {
                        this->state->status.store(ui_completion_status::cancelled, std::memory_order_release);
                    }
                    else
                    {
                        this->state->value.emplace(std::move(value));
                        this->state->status.store(ui_completion_status::completed, std::memory_order_release);
                    }
                }
                catch (...)
                {
                    const std::lock_guard lock(this->state->mutex);
                    this->state->error = std::current_exception();
                    this->state->status.store(ui_completion_status::failed, std::memory_order_release);
                }
            }

            void close() noexcept override
            {
                const std::lock_guard lock(this->state->mutex);
                if (this->state->status.load(std::memory_order_relaxed) == ui_completion_status::queued)
                {
                    this->state->cancel_requested.store(true, std::memory_order_release);
                    this->state->status.store(ui_completion_status::queue_closed, std::memory_order_release);
                }
            }
        };

        void bind_or_require_owner()
        {
            const auto current = std::this_thread::get_id();
            if (this->owner_ == std::thread::id{})
            {
                this->owner_ = current;
            }
            else if (this->owner_ != current)
            {
                throw std::logic_error("UI completion queue pumped on a different thread");
            }
        }

        std::mutex mutex_{};
        std::deque<std::unique_ptr<task_base>> tasks_{};
        std::thread::id owner_{};
        bool closed_{};
        bool pumping_{};
    };
}
