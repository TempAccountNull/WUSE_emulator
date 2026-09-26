#pragma once

#include <vk_debug_utils_wire.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sogen::gpu_bridge::debug_utils_callback_relay
{
    // Native callbacks cannot enter guest code while the originating IOCTL owns the syscall
    // lock. Deep-copy the message and drain it on the same guest thread after that IOCTL returns.
    struct delivery
    {
        uint64_t instance_id{};
        uint64_t callback_address{};
        uint64_t user_data{};
        uint32_t guest_pointer_bytes{};
        std::vector<std::byte> packet;
    };

    class queue
    {
      public:
        static constexpr size_t max_queued_packets = 64;
        static constexpr size_t max_queued_bytes = 8 * 1024 * 1024;

        bool push(uint32_t guest_thread_id, delivery value)
        {
            if (!guest_thread_id || !value.callback_address ||
                (value.guest_pointer_bytes != 4 && value.guest_pointer_bytes != 8) ||
                value.packet.empty() || value.packet.size() > debug_utils_wire::max_packet_bytes ||
                (value.guest_pointer_bytes == 4 && (value.callback_address > UINT32_MAX || value.user_data > UINT32_MAX)))
            {
                return false;
            }
            std::scoped_lock lock(mutex_);
            if (packet_count_ >= max_queued_packets || value.packet.size() > max_queued_bytes - byte_count_)
            {
                return false;
            }
            byte_count_ += value.packet.size();
            ++packet_count_;
            pending_[guest_thread_id].push_back(std::move(value));
            return true;
        }

        bool pop(uint32_t guest_thread_id, delivery& output)
        {
            std::scoped_lock lock(mutex_);
            const auto entry = pending_.find(guest_thread_id);
            if (entry == pending_.end() || entry->second.empty())
            {
                return false;
            }
            output = std::move(entry->second.front());
            byte_count_ -= output.packet.size();
            --packet_count_;
            entry->second.pop_front();
            if (entry->second.empty())
            {
                pending_.erase(entry);
            }
            return true;
        }

        void discard_instance(uint64_t instance_id)
        {
            std::scoped_lock lock(mutex_);
            for (auto thread = pending_.begin(); thread != pending_.end();)
            {
                auto& entries = thread->second;
                for (auto entry = entries.begin(); entry != entries.end();)
                {
                    if (entry->instance_id == instance_id)
                    {
                        byte_count_ -= entry->packet.size();
                        --packet_count_;
                        entry = entries.erase(entry);
                    }
                    else
                    {
                        ++entry;
                    }
                }
                thread = entries.empty() ? pending_.erase(thread) : std::next(thread);
            }
        }

        size_t packet_count() const
        {
            std::scoped_lock lock(mutex_);
            return packet_count_;
        }

      private:
        mutable std::mutex mutex_;
        std::unordered_map<uint32_t, std::deque<delivery>> pending_;
        size_t packet_count_{};
        size_t byte_count_{};
    };
} // namespace sogen::gpu_bridge::debug_utils_callback_relay
