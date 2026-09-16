#pragma once

#include <vk_queue_submit.hpp>

namespace sogen::legacy_queue_submit
{
    namespace wire = gpu_bridge::queue_submit_wire;

    struct semaphore
    {
        VkSemaphore handle{};
        bool timeline{};
    };

    struct native_batch
    {
        std::vector<VkSemaphore> waits;
        std::vector<VkPipelineStageFlags> stages;
        std::vector<VkCommandBuffer> commands;
        std::vector<VkSemaphore> signals;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        VkDeviceGroupSubmitInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO};
        VkProtectedSubmitInfo protection{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};
    };

    // Resolve the whole call before submitting any batch. A bad later object must not enqueue an earlier batch.
    template <typename SemaphoreLookup, typename CommandLookup, typename FenceLookup>
    VkResult execute(const wire::submission& source, const VkQueue queue, const PFN_vkQueueSubmit submit,
                     SemaphoreLookup&& semaphore_lookup, CommandLookup&& command_lookup, FenceLookup&& fence_lookup)
    {
        if (!queue || !submit)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        VkFence fence{};
        if (source.info.fence)
        {
            fence = fence_lookup(source.info.fence);
            if (!fence)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        std::vector<native_batch> storage(source.batches.size());
        std::vector<VkSubmitInfo> batches(source.batches.size());
        for (size_t index = 0; index < source.batches.size(); ++index)
        {
            const auto& from = source.batches[index];
            auto& owned = storage[index];
            auto& native = batches[index];
            native.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            bool timeline_wait{};
            bool timeline_signal{};
            for (const auto& wait : from.waits)
            {
                const auto resolved = semaphore_lookup(wait.semaphore);
                if (!resolved.handle)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                timeline_wait |= resolved.timeline;
                owned.waits.push_back(resolved.handle);
                owned.stages.push_back(wait.stages);
            }
            for (const auto signal : from.signals)
            {
                const auto resolved = semaphore_lookup(signal);
                if (!resolved.handle)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                timeline_signal |= resolved.timeline;
                owned.signals.push_back(resolved.handle);
            }
            for (const auto command : from.commands)
            {
                const auto resolved = command_lookup(command);
                if (!resolved)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                owned.commands.push_back(resolved);
            }
            if ((timeline_wait || timeline_signal) && !(from.info.chains & wire::timeline_chain))
            {
                return VK_ERROR_VALIDATION_FAILED_EXT;
            }
            if ((timeline_wait && from.wait_values.size() != from.waits.size()) ||
                (timeline_signal && from.signal_values.size() != from.signals.size()))
            {
                return VK_ERROR_VALIDATION_FAILED_EXT;
            }
            native.waitSemaphoreCount = static_cast<uint32_t>(owned.waits.size());
            native.pWaitSemaphores = owned.waits.empty() ? nullptr : owned.waits.data();
            native.pWaitDstStageMask = owned.stages.empty() ? nullptr : owned.stages.data();
            native.commandBufferCount = static_cast<uint32_t>(owned.commands.size());
            native.pCommandBuffers = owned.commands.empty() ? nullptr : owned.commands.data();
            native.signalSemaphoreCount = static_cast<uint32_t>(owned.signals.size());
            native.pSignalSemaphores = owned.signals.empty() ? nullptr : owned.signals.data();
            if (from.info.chains & wire::timeline_chain)
            {
                owned.timeline.waitSemaphoreValueCount = static_cast<uint32_t>(from.wait_values.size());
                owned.timeline.pWaitSemaphoreValues = from.wait_values.empty() ? nullptr : from.wait_values.data();
                owned.timeline.signalSemaphoreValueCount = static_cast<uint32_t>(from.signal_values.size());
                owned.timeline.pSignalSemaphoreValues = from.signal_values.empty() ? nullptr : from.signal_values.data();
                owned.timeline.pNext = native.pNext;
                native.pNext = &owned.timeline;
            }
            if (from.info.chains & wire::device_group_chain)
            {
                owned.group.waitSemaphoreCount = static_cast<uint32_t>(from.wait_devices.size());
                owned.group.pWaitSemaphoreDeviceIndices = from.wait_devices.empty() ? nullptr : from.wait_devices.data();
                owned.group.commandBufferCount = static_cast<uint32_t>(from.command_masks.size());
                owned.group.pCommandBufferDeviceMasks = from.command_masks.empty() ? nullptr : from.command_masks.data();
                owned.group.signalSemaphoreCount = static_cast<uint32_t>(from.signal_devices.size());
                owned.group.pSignalSemaphoreDeviceIndices = from.signal_devices.empty() ? nullptr : from.signal_devices.data();
                owned.group.pNext = native.pNext;
                native.pNext = &owned.group;
            }
            if (from.info.chains & wire::protected_chain)
            {
                owned.protection.protectedSubmit = from.info.protected_submit;
                owned.protection.pNext = native.pNext;
                native.pNext = &owned.protection;
            }
        }
        // submitCount == 0 is distinct from one empty synchronization batch, even without a fence.
        return submit(queue, static_cast<uint32_t>(batches.size()), batches.empty() ? nullptr : batches.data(), fence);
    }

    template <typename Map>
    const typename Map::mapped_type* find_owned(const Map& objects, const uint64_t id, const uint64_t device)
    {
        const auto found = objects.find(id);
        if (found == objects.end() || found->second.device_id != device)
        {
            return nullptr;
        }
        // Native presentation may retain a destroyed synchronization object's backing until its
        // last completion witness. That lifetime extension does not make the guest handle usable.
        if constexpr (requires { found->second.native_destroy_requested; })
        {
            if (found->second.native_destroy_requested)
            {
                return nullptr;
            }
        }
        return &found->second;
    }

    template <typename Queue, typename SemaphoreMap, typename CommandMap, typename FenceMap>
    VkResult execute_owned(const wire::submission& source, const Queue& queue, const PFN_vkQueueSubmit submit,
                           const SemaphoreMap& semaphores, const CommandMap& commands, const FenceMap& fences)
    {
        return execute(
            source, queue.handle, submit,
            [&](const uint64_t id) {
                const auto* found = find_owned(semaphores, id, queue.device_id);
                return found ? semaphore{found->handle, found->type == VK_SEMAPHORE_TYPE_TIMELINE} : semaphore{};
            },
            [&](const uint64_t id) {
                const auto* found = find_owned(commands, id, queue.device_id);
                return found ? found->handle : VkCommandBuffer{};
            },
            [&](const uint64_t id) {
                const auto* found = find_owned(fences, id, queue.device_id);
                return found ? found->handle : VkFence{};
            });
    }
}
