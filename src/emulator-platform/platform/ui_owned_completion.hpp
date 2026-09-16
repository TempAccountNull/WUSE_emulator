#pragma once

#include "ui_completion_queue.hpp"

namespace sogen
{
    // A result may be abandoned before, during, or after UI execution. Its disposer is posted
    // back to the owner in all three cases; publishing a terminal ticket alone is not adoption.
    template <typename T>
    class ui_owned_completion
    {
      public:
        using disposer = std::function<void(T)>;

        ui_owned_completion(std::shared_ptr<ui_completion_queue> queue, disposer dispose)
            : queue_(std::move(queue)),
              dispose_(std::move(dispose))
        {
        }

        ui_owned_completion(const ui_owned_completion&) = delete;
        ui_owned_completion& operator=(const ui_owned_completion&) = delete;

        ~ui_owned_completion() noexcept
        {
            if (this->adopted_)
            {
                return;
            }
            try
            {
                auto cleanup = this->queue_->post(
                    [value = std::move(this->value), dispose = std::move(this->dispose_)](const ui_cancellation_token&) mutable {
                        dispose(std::move(value));
                        return 0;
                    });
                if (cleanup.ready())
                {
                    // Closing an owner before its results drain is a lifecycle error. Do not
                    // silently leak a live native object or destroy it on a receiving thread.
                    const auto result = cleanup.try_take();
                    if (result && result->status == ui_completion_status::queue_closed)
                    {
                        std::terminate();
                    }
                }
                cleanup.detach();
            }
            catch (...)
            {
                // Failure to allocate a mandatory owner-thread disposal command cannot safely
                // be recovered by freeing a Win32/Vulkan object from the current thread.
                std::terminate();
            }
        }

        void adopt() noexcept
        {
            this->adopted_ = true;
        }

        T value{};

      private:
        std::shared_ptr<ui_completion_queue> queue_;
        disposer dispose_;
        bool adopted_{};
    };
}
