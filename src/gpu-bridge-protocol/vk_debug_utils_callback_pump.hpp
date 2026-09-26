#pragma once

#include <vk_debug_utils_wire.hpp>

#include <vulkan/vulkan_core.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace sogen::gpu_bridge::debug_utils_wire
{
    class callback_pump
    {
      public:
        using callback = std::function<VkBool32(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                                const VkDebugUtilsMessengerCallbackDataEXT&, void*)>;

        static constexpr size_t max_pending_callbacks = 64;

        explicit callback_pump(callback handler, void* user_data, std::chrono::milliseconds response_timeout = std::chrono::seconds(30))
            : handler_(std::move(handler)),
              user_data_(user_data),
              response_timeout_(response_timeout)
        {
            if (!handler_ || response_timeout_ < std::chrono::milliseconds::zero())
            {
                throw std::invalid_argument("invalid debug-utils callback pump configuration");
            }
        }

        callback_pump(const callback_pump&) = delete;
        callback_pump& operator=(const callback_pump&) = delete;
        callback_pump(callback_pump&&) = delete;
        callback_pump& operator=(callback_pump&&) = delete;

        static VKAPI_ATTR VkBool32 VKAPI_CALL native_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                              VkDebugUtilsMessageTypeFlagsEXT types,
                                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) noexcept
        {
            if (!data || !user_data)
            {
                return VK_FALSE;
            }
            try
            {
                return static_cast<callback_pump*>(user_data)->relay(severity, types, *data);
            }
            catch (...)
            {
                return VK_FALSE;
            }
        }

        VkBool32 relay(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                       const VkDebugUtilsMessengerCallbackDataEXT& data) noexcept
        {
            std::vector<std::byte> packet;
            try
            {
                packet = encode(severity, types, data);
            }
            catch (const std::exception& error)
            {
                record_failure(error.what());
                return VK_FALSE;
            }
            catch (...)
            {
                record_failure("failed to serialize native debug-utils callback");
                return VK_FALSE;
            }

            try
            {
                auto request = std::make_shared<pending_callback>(std::move(packet));
                std::unique_lock lock(mutex_);
                if (!accepting_ || callbacks_.size() >= max_pending_callbacks)
                {
                    record_failure_locked("debug-utils callback pump is unavailable or full");
                    return VK_FALSE;
                }
                callbacks_.push(request);
                ++active_callbacks_;
                wake_origin_.notify_one();
                if (!request->answered.wait_for(lock, response_timeout_, [&] { return request->complete; }))
                {
                    request->complete = true;
                    request->result = VK_FALSE;
                    record_failure_locked("debug-utils callback response timed out");
                }
                --active_callbacks_;
                wake_origin_.notify_one();
                return request->result;
            }
            catch (...)
            {
                record_failure("failed to queue native debug-utils callback");
                return VK_FALSE;
            }
        }

        void run(std::function<void()> native_operation)
        {
            if (!native_operation)
            {
                throw std::invalid_argument("native Vulkan operation is missing");
            }
            {
                std::lock_guard lock(mutex_);
                if (started_)
                {
                    throw std::logic_error("debug-utils callback pump can run only once");
                }
                started_ = true;
                accepting_ = true;
            }

            std::thread worker([this, operation = std::move(native_operation)] {
                try
                {
                    operation();
                }
                catch (...)
                {
                    operation_error_ = std::current_exception();
                }
                {
                    std::lock_guard lock(mutex_);
                    worker_done_ = true;
                }
                wake_origin_.notify_one();
            });

            for (;;)
            {
                std::unique_lock lock(mutex_);
                wake_origin_.wait(lock, [&] { return !callbacks_.empty() || (worker_done_ && active_callbacks_ == 0); });
                if (callbacks_.empty() && worker_done_ && active_callbacks_ == 0)
                {
                    accepting_ = false;
                    break;
                }
                if (callbacks_.empty())
                {
                    continue;
                }
                auto request = std::move(callbacks_.front());
                callbacks_.pop();
                if (request->complete)
                {
                    continue;
                }
                lock.unlock();

                VkBool32 result = VK_FALSE;
                try
                {
                    auto decoded = decode(request->packet);
                    result = handler_(decoded->severity, decoded->types, decoded->data, user_data_);
                }
                catch (const std::exception& error)
                {
                    record_failure(error.what());
                }
                catch (...)
                {
                    record_failure("guest debug-utils callback failed");
                }

                lock.lock();
                if (!request->complete)
                {
                    request->result = result;
                    request->complete = true;
                    request->answered.notify_one();
                }
            }

            worker.join();
            if (operation_error_)
            {
                std::rethrow_exception(operation_error_);
            }
        }

        size_t failure_count() const
        {
            std::lock_guard lock(mutex_);
            return failures_;
        }

        std::string first_failure() const
        {
            std::lock_guard lock(mutex_);
            return first_failure_;
        }

      private:
        struct pending_callback
        {
            explicit pending_callback(std::vector<std::byte> encoded)
                : packet(std::move(encoded))
            {
            }

            std::vector<std::byte> packet;
            std::condition_variable answered;
            bool complete{};
            VkBool32 result{VK_FALSE};
        };

        void record_failure_locked(const char* message) noexcept
        {
            ++failures_;
            if (first_failure_.empty())
            {
                try
                {
                    first_failure_ = message;
                }
                catch (...)
                {
                }
            }
        }

        void record_failure(const char* message) noexcept
        {
            try
            {
                std::lock_guard lock(mutex_);
                record_failure_locked(message);
            }
            catch (...)
            {
            }
        }

        callback handler_;
        void* user_data_{};
        std::chrono::milliseconds response_timeout_;
        mutable std::mutex mutex_;
        std::condition_variable wake_origin_;
        std::queue<std::shared_ptr<pending_callback>> callbacks_;
        size_t active_callbacks_{};
        size_t failures_{};
        std::string first_failure_;
        std::exception_ptr operation_error_;
        bool started_{};
        bool accepting_{};
        bool worker_done_{};
    };
}
