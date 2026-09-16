#pragma once

#include "gpu_bridge_protocol.hpp"
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace sogen::gpu_bridge::queue_submit_wire
{
    inline constexpr uint32_t version = 1;
    inline constexpr size_t max_bytes = max_queue_submit_full_bytes;
    inline constexpr uint32_t max_batches = 4096;
    inline constexpr uint32_t timeline_chain = 1;
    inline constexpr uint32_t device_group_chain = 2;
    inline constexpr uint32_t protected_chain = 4;

    class error : public std::runtime_error
    {
      public:
        VkResult result;

        explicit error(const char* message, const VkResult code = VK_ERROR_VALIDATION_FAILED_EXT)
            : std::runtime_error(message),
              result(code)
        {
        }
    };

    // All offsets/counts are independent of native pointer width and structure padding.
    using header = queue_submit_full_header;

    struct batch_header
    {
        uint32_t type{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        uint32_t chains{};
        uint32_t wait_count{};
        uint32_t command_count{};
        uint32_t signal_count{};
        uint32_t wait_value_count{};
        uint32_t signal_value_count{};
        uint32_t wait_device_count{};
        uint32_t command_mask_count{};
        uint32_t signal_device_count{};
        uint32_t protected_submit{};
        uint32_t reserved{};
    };

    struct alignas(8) wait_entry
    {
        uint64_t semaphore{};
        uint32_t stages{};
        uint32_t reserved{};
    };

    static_assert(std::endian::native == std::endian::little);
    static_assert(sizeof(header) == 32 && alignof(header) == 8);
    static_assert(offsetof(header, queue) == 8 && offsetof(header, fence) == 16 && offsetof(header, batch_count) == 24);
    static_assert(sizeof(batch_header) == 48 && offsetof(batch_header, protected_submit) == 40);
    static_assert(sizeof(wait_entry) == 16 && offsetof(wait_entry, stages) == 8);
    static_assert(sizeof(VkPipelineStageFlags) == 4 && sizeof(uint64_t) == 8);
#if UINTPTR_MAX == UINT64_MAX
    static_assert(sizeof(VkSubmitInfo) == 72 && offsetof(VkSubmitInfo, pWaitSemaphores) == 24);
    static_assert(sizeof(VkTimelineSemaphoreSubmitInfo) == 48 && sizeof(VkDeviceGroupSubmitInfo) == 64);
#else
    static_assert(sizeof(VkSubmitInfo) == 36 && offsetof(VkSubmitInfo, pWaitSemaphores) == 12);
    static_assert(sizeof(VkTimelineSemaphoreSubmitInfo) == 24 && sizeof(VkDeviceGroupSubmitInfo) == 32);
#endif

    template <typename T>
    uint64_t id(const T handle)
    {
        if constexpr (std::is_pointer_v<T>)
        {
            return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
        }
        else
        {
            return static_cast<uint64_t>(handle);
        }
    }

    struct batch
    {
        batch_header info;
        std::vector<wait_entry> waits;
        std::vector<uint64_t> commands;
        std::vector<uint64_t> signals;
        std::vector<uint64_t> wait_values;
        std::vector<uint64_t> signal_values;
        std::vector<uint32_t> wait_devices;
        std::vector<uint32_t> command_masks;
        std::vector<uint32_t> signal_devices;
    };

    struct submission
    {
        header info;
        std::vector<batch> batches;
    };

    inline void validate(const batch_header& info)
    {
        if (info.type != VK_STRUCTURE_TYPE_SUBMIT_INFO || info.reserved || (info.chains & ~7u) || info.protected_submit > 1 ||
            (!(info.chains & timeline_chain) && (info.wait_value_count || info.signal_value_count)) ||
            (!(info.chains & device_group_chain) && (info.wait_device_count || info.command_mask_count || info.signal_device_count)) ||
            (!(info.chains & protected_chain) && info.protected_submit))
        {
            throw error("Invalid legacy submit batch header");
        }
        if ((info.chains & device_group_chain) &&
            (info.wait_device_count != info.wait_count || info.command_mask_count != info.command_count ||
             info.signal_device_count != info.signal_count))
        {
            throw error("Device-group submit counts must match the batch");
        }
    }

    class writer
    {
      public:
        std::vector<std::byte> bytes;

        template <typename T>
        void append(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (sizeof(T) > max_bytes - this->bytes.size())
            {
                throw error("Legacy submit packet exceeds byte limit");
            }
            const auto* first = reinterpret_cast<const std::byte*>(&value);
            this->bytes.insert(this->bytes.end(), first, first + sizeof(T));
        }

        template <typename T, typename Visit>
        void array(const T* values, const uint32_t count, const size_t wire_stride, Visit visit)
        {
            if (count > (max_bytes - this->bytes.size()) / wire_stride || (count && !values))
            {
                throw error("Invalid or oversized legacy submit array");
            }
            for (uint32_t i = 0; i < count; ++i)
            {
                visit(values[i]);
            }
        }

        template <typename T>
        void array(const T* values, const uint32_t count)
        {
            this->array(values, count, sizeof(T), [&](const T& value) { this->append(value); });
        }
    };

    inline std::vector<std::byte> encode(const VkQueue queue, const uint32_t count, const VkSubmitInfo* submits, const VkFence fence)
    {
        if (count > max_batches || (count && !submits))
        {
            throw error("Invalid legacy submit batch array");
        }
        writer output;
        header info{version, 0, id(queue), id(fence), count, 0};
        output.append(info);
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto& source = submits[index];
            batch_header item{};
            item.type = source.sType;
            item.wait_count = source.waitSemaphoreCount;
            item.command_count = source.commandBufferCount;
            item.signal_count = source.signalSemaphoreCount;
            const VkTimelineSemaphoreSubmitInfo* timeline{};
            const VkDeviceGroupSubmitInfo* group{};
            const VkProtectedSubmitInfo* protection{};
            for (const auto* next = static_cast<const VkBaseInStructure*>(source.pNext); next; next = next->pNext)
            {
                uint32_t flag{};
                switch (next->sType)
                {
                case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:
                    flag = timeline_chain;
                    timeline = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(next);
                    item.wait_value_count = timeline->waitSemaphoreValueCount;
                    item.signal_value_count = timeline->signalSemaphoreValueCount;
                    break;
                case VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO:
                    flag = device_group_chain;
                    group = reinterpret_cast<const VkDeviceGroupSubmitInfo*>(next);
                    item.wait_device_count = group->waitSemaphoreCount;
                    item.command_mask_count = group->commandBufferCount;
                    item.signal_device_count = group->signalSemaphoreCount;
                    break;
                case VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO:
                    flag = protected_chain;
                    protection = reinterpret_cast<const VkProtectedSubmitInfo*>(next);
                    item.protected_submit = protection->protectedSubmit;
                    break;
                default:
                    throw error("Unsupported legacy submit pNext structure", VK_ERROR_EXTENSION_NOT_PRESENT);
                }
                // Only three distinct supported types exist; this also bounds cycles without dereferencing indefinitely.
                if (item.chains & flag)
                {
                    throw error("Duplicate or cyclic legacy submit pNext structure");
                }
                item.chains |= flag;
            }
            validate(item);
            if (source.waitSemaphoreCount && !source.pWaitDstStageMask)
            {
                throw error("Missing legacy submit wait stages");
            }
            output.append(item);
            uint32_t wait_index{};
            output.array(source.pWaitSemaphores, source.waitSemaphoreCount, sizeof(wait_entry), [&](const VkSemaphore semaphore) {
                output.append(wait_entry{.semaphore = id(semaphore), .stages = source.pWaitDstStageMask[wait_index++], .reserved = 0});
            });
            output.array(source.pCommandBuffers, source.commandBufferCount, sizeof(uint64_t),
                         [&](const VkCommandBuffer command) { output.append(id(command)); });
            output.array(source.pSignalSemaphores, source.signalSemaphoreCount, sizeof(uint64_t),
                         [&](const VkSemaphore semaphore) { output.append(id(semaphore)); });
            if (timeline)
            {
                output.array(timeline->pWaitSemaphoreValues, timeline->waitSemaphoreValueCount);
                output.array(timeline->pSignalSemaphoreValues, timeline->signalSemaphoreValueCount);
            }
            if (group)
            {
                output.array(group->pWaitSemaphoreDeviceIndices, group->waitSemaphoreCount);
                output.array(group->pCommandBufferDeviceMasks, group->commandBufferCount);
                output.array(group->pSignalSemaphoreDeviceIndices, group->signalSemaphoreCount);
            }
        }
        info.bytes = static_cast<uint32_t>(output.bytes.size());
        std::memcpy(output.bytes.data(), &info, sizeof(info));
        return std::move(output.bytes);
    }

    class reader
    {
      public:
        explicit reader(const std::span<const std::byte> data)
            : data_(data)
        {
            if (data.size() > max_bytes)
            {
                throw error("Legacy submit packet exceeds byte limit");
            }
        }

        template <typename T>
        T scalar()
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (sizeof(T) > this->data_.size() - this->offset_)
            {
                throw error("Truncated legacy submit packet");
            }
            T result{};
            std::memcpy(&result, this->data_.data() + this->offset_, sizeof(result));
            this->offset_ += sizeof(T);
            return result;
        }

        template <typename T>
        std::vector<T> array(const uint32_t count)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (count > (this->data_.size() - this->offset_) / sizeof(T))
            {
                throw error("Truncated legacy submit array");
            }
            std::vector<T> result(count);
            if (count)
            {
                std::memcpy(result.data(), this->data_.data() + this->offset_, sizeof(T) * count);
            }
            this->offset_ += sizeof(T) * count;
            return result;
        }

        void finish() const
        {
            if (this->offset_ != this->data_.size())
            {
                throw error("Trailing legacy submit packet bytes");
            }
        }

      private:
        std::span<const std::byte> data_;
        size_t offset_{};
    };

    inline submission decode(const std::span<const std::byte> bytes)
    {
        reader input(bytes);
        submission result;
        result.info = input.scalar<header>();
        if (result.info.protocol != version || result.info.bytes != bytes.size() || result.info.reserved ||
            result.info.batch_count > max_batches || result.info.batch_count > (bytes.size() - sizeof(header)) / sizeof(batch_header))
        {
            throw error("Invalid legacy submit packet header");
        }
        result.batches.reserve(result.info.batch_count);
        for (uint32_t index = 0; index < result.info.batch_count; ++index)
        {
            batch item;
            item.info = input.scalar<batch_header>();
            validate(item.info);
            item.waits = input.array<wait_entry>(item.info.wait_count);
            for (const auto& wait : item.waits)
            {
                if (wait.reserved)
                {
                    throw error("Invalid legacy submit wait record");
                }
            }
            item.commands = input.array<uint64_t>(item.info.command_count);
            item.signals = input.array<uint64_t>(item.info.signal_count);
            item.wait_values = input.array<uint64_t>(item.info.wait_value_count);
            item.signal_values = input.array<uint64_t>(item.info.signal_value_count);
            item.wait_devices = input.array<uint32_t>(item.info.wait_device_count);
            item.command_masks = input.array<uint32_t>(item.info.command_mask_count);
            item.signal_devices = input.array<uint32_t>(item.info.signal_device_count);
            result.batches.push_back(std::move(item));
        }
        input.finish();
        return result;
    }
}
