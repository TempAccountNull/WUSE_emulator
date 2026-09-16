#include <windows.h>
#include <array>
#include <cstdio>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

bool test_synchronization(PFN_vkGetInstanceProcAddr get, VkInstance instance, VkDevice device, VkQueue queue, uint32_t family, bool sync2)
{
#define LOAD(name)                                                        \
    const auto name = reinterpret_cast<PFN_##name>(get(instance, #name)); \
    if (!name)                                                            \
    return false
    LOAD(vkCreateEvent);
    LOAD(vkDestroyEvent);
    LOAD(vkGetEventStatus);
    LOAD(vkSetEvent);
    LOAD(vkResetEvent);
    LOAD(vkCmdSetEvent);
    LOAD(vkCmdResetEvent);
    LOAD(vkCmdWaitEvents);
    LOAD(vkCmdSetEvent2);
    LOAD(vkCmdSetEvent2KHR);
    LOAD(vkCmdResetEvent2);
    LOAD(vkCmdResetEvent2KHR);
    LOAD(vkCmdWaitEvents2);
    LOAD(vkCmdWaitEvents2KHR);
    LOAD(vkCmdPipelineBarrier2);
    LOAD(vkCmdPipelineBarrier2KHR);
    LOAD(vkCreateCommandPool);
    LOAD(vkDestroyCommandPool);
    LOAD(vkAllocateCommandBuffers);
    LOAD(vkBeginCommandBuffer);
    LOAD(vkEndCommandBuffer);
    LOAD(vkCreateFence);
    LOAD(vkDestroyFence);
    LOAD(vkResetFences);
    LOAD(vkQueueSubmit);
    LOAD(vkWaitForFences);
    LOAD(vkDeviceWaitIdle);
#undef LOAD
    std::array<VkEvent, 2> events{};
    VkCommandPool pool{};
    VkCommandBuffer buffer{};
    VkFence fence{};
    bool submitted = false;
    const auto execute = [&]() -> bool {
        const VkEventCreateInfo event_info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        for (auto& event : events)
        {
            if (vkCreateEvent(device, &event_info, nullptr, &event) != VK_SUCCESS || vkGetEventStatus(device, event) != VK_EVENT_RESET)
            {
                return false;
            }
            if (vkSetEvent(device, event) != VK_SUCCESS || vkGetEventStatus(device, event) != VK_EVENT_SET)
            {
                return false;
            }
            if (vkResetEvent(device, event) != VK_SUCCESS || vkGetEventStatus(device, event) != VK_EVENT_RESET)
            {
                return false;
            }
        }
        const VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
        if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
        {
            return false;
        }
        const VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        if (vkAllocateCommandBuffers(device, &allocate, &buffer) != VK_SUCCESS)
        {
            return false;
        }
        const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS)
        {
            return false;
        }
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        const auto submit = [&]() {
            if (vkEndCommandBuffer(buffer) != VK_SUCCESS || vkResetFences(device, 1, &fence) != VK_SUCCESS)
            {
                return false;
            }
            VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &buffer;
            if (vkQueueSubmit(queue, 1, &submit_info, fence) != VK_SUCCESS)
            {
                return false;
            }
            submitted = true;
            return vkWaitForFences(device, 1, &fence, VK_TRUE, 30000000000ULL) == VK_SUCCESS;
        };
        if (vkBeginCommandBuffer(buffer, &begin) != VK_SUCCESS)
        {
            return false;
        }
        for (const VkEvent event : events)
        {
            vkCmdSetEvent(buffer, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        const VkMemoryBarrier legacy{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT};
        vkCmdWaitEvents(buffer, 2, events.data(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 1, &legacy, 0,
                        nullptr, 0, nullptr);
        if (!submit())
        {
            return false;
        }
        for (const VkEvent event : events)
        {
            if (vkGetEventStatus(device, event) != VK_EVENT_SET)
            {
                return false;
            }
        }
        if (vkBeginCommandBuffer(buffer, &begin) != VK_SUCCESS)
        {
            return false;
        }
        for (const VkEvent event : events)
        {
            vkCmdResetEvent(buffer, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        if (!submit())
        {
            return false;
        }
        for (const VkEvent event : events)
        {
            if (vkGetEventStatus(device, event) != VK_EVENT_RESET)
            {
                return false;
            }
        }
        if (sync2)
        {
            VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            memory.srcStageMask = memory.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            memory.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            memory.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &memory;
            const std::array<VkDependencyInfo, 2> dependencies{dependency, dependency};
            if (vkBeginCommandBuffer(buffer, &begin) != VK_SUCCESS)
            {
                return false;
            }
            vkCmdPipelineBarrier2(buffer, &dependency);
            vkCmdSetEvent2(buffer, events[0], &dependency);
            vkCmdSetEvent2KHR(buffer, events[1], &dependency);
            vkCmdWaitEvents2(buffer, 2, events.data(), dependencies.data());
            vkCmdWaitEvents2KHR(buffer, 2, events.data(), dependencies.data());
            vkCmdPipelineBarrier2KHR(buffer, &dependency);
            if (!submit())
            {
                return false;
            }
            for (const VkEvent event : events)
            {
                if (vkGetEventStatus(device, event) != VK_EVENT_SET)
                {
                    return false;
                }
            }
            if (vkBeginCommandBuffer(buffer, &begin) != VK_SUCCESS)
            {
                return false;
            }
            vkCmdResetEvent2(buffer, events[0], VK_PIPELINE_STAGE_2_COPY_BIT);
            vkCmdResetEvent2KHR(buffer, events[1], VK_PIPELINE_STAGE_2_COPY_BIT);
            if (!submit())
            {
                return false;
            }
            for (const VkEvent event : events)
            {
                if (vkGetEventStatus(device, event) != VK_EVENT_RESET)
                {
                    return false;
                }
            }
        }
        else
        {
            std::printf("[shim-test] synchronization2 events -> SKIP (feature not enabled)\n");
        }
        return true;
    };
    const bool ok = execute();
    if (submitted)
    {
        vkDeviceWaitIdle(device);
    }
    if (fence)
    {
        vkDestroyFence(device, fence, nullptr);
    }
    if (pool)
    {
        vkDestroyCommandPool(device, pool, nullptr);
    }
    for (const VkEvent event : events)
    {
        if (event)
        {
            vkDestroyEvent(device, event, nullptr);
        }
    }
    std::printf("[shim-test] real event state, legacy wait/set/reset and synchronization2 core/KHR -> %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
