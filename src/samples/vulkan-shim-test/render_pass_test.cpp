#include <windows.h>
#include <array>
#include <cstdio>
#include <cstdint>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

bool test_render_pass2(PFN_vkGetInstanceProcAddr get_proc, VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                       VkQueue queue, uint32_t family)
{
#define LOAD(name)                                                             \
    const auto name = reinterpret_cast<PFN_##name>(get_proc(instance, #name)); \
    if (!name)                                                                 \
    return false
    LOAD(vkCreateRenderPass2);
    LOAD(vkCreateRenderPass2KHR);
    LOAD(vkCmdBeginRenderPass2);
    LOAD(vkCmdBeginRenderPass2KHR);
    LOAD(vkCmdNextSubpass2);
    LOAD(vkCmdNextSubpass2KHR);
    LOAD(vkCmdEndRenderPass2);
    LOAD(vkCmdEndRenderPass2KHR);
    LOAD(vkGetRenderAreaGranularity);
    LOAD(vkDestroyRenderPass);
    LOAD(vkCreateFramebuffer);
    LOAD(vkDestroyFramebuffer);
    LOAD(vkCreateImage);
    LOAD(vkDestroyImage);
    LOAD(vkGetImageMemoryRequirements);
    LOAD(vkBindImageMemory);
    LOAD(vkCreateImageView);
    LOAD(vkDestroyImageView);
    LOAD(vkCreateBuffer);
    LOAD(vkDestroyBuffer);
    LOAD(vkGetBufferMemoryRequirements);
    LOAD(vkBindBufferMemory);
    LOAD(vkAllocateMemory);
    LOAD(vkFreeMemory);
    LOAD(vkMapMemory);
    LOAD(vkUnmapMemory);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkCreateCommandPool);
    LOAD(vkDestroyCommandPool);
    LOAD(vkAllocateCommandBuffers);
    LOAD(vkBeginCommandBuffer);
    LOAD(vkEndCommandBuffer);
    LOAD(vkCmdCopyImageToBuffer);
    LOAD(vkCmdPipelineBarrier);
    LOAD(vkCreateFence);
    LOAD(vkDestroyFence);
    LOAD(vkQueueSubmit);
    LOAD(vkWaitForFences);
    LOAD(vkDeviceWaitIdle);
#undef LOAD
    constexpr uint32_t width = 16;
    constexpr uint32_t height = 16;
    constexpr VkDeviceSize readback_size = width * height * sizeof(uint32_t);
    std::array<VkImage, 2> images{};
    std::array<VkImageView, 2> views{};
    std::array<VkDeviceMemory, 2> image_memory{};
    std::array<VkBuffer, 2> buffers{};
    std::array<VkDeviceMemory, 2> buffer_memory{};
    VkRenderPass render_pass{};
    VkRenderPass alias_pass{};
    VkFramebuffer framebuffer{};
    VkCommandPool pool{};
    VkFence fence{};
    bool submitted = false;
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    const auto allocate = [&](const VkMemoryRequirements& requirements, VkMemoryPropertyFlags flags, VkDeviceMemory& memory) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        {
            if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
            {
                const VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, i};
                return vkAllocateMemory(device, &info, nullptr, &memory) == VK_SUCCESS;
            }
        }
        return false;
    };
    const auto execute = [&]() -> bool {
        for (uint32_t i = 0; i < images.size(); ++i)
        {
            VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image.imageType = VK_IMAGE_TYPE_2D;
            image.format = VK_FORMAT_R8G8B8A8_UINT;
            image.extent = {.width = width, .height = height, .depth = 1};
            image.mipLevels = 1;
            image.arrayLayers = 2;
            image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL;
            image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (vkCreateImage(device, &image, nullptr, &images[i]) != VK_SUCCESS)
            {
                return false;
            }
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, images[i], &requirements);
            if (!allocate(requirements, 0, image_memory[i]) || vkBindImageMemory(device, images[i], image_memory[i], 0) != VK_SUCCESS)
            {
                return false;
            }
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = images[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            view.format = image.format;
            view.subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 2};
            if (vkCreateImageView(device, &view, nullptr, &views[i]) != VK_SUCCESS)
            {
                return false;
            }
            VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer.size = readback_size;
            buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (vkCreateBuffer(device, &buffer, nullptr, &buffers[i]) != VK_SUCCESS)
            {
                return false;
            }
            vkGetBufferMemoryRequirements(device, buffers[i], &requirements);
            if (!allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer_memory[i]) ||
                vkBindBufferMemory(device, buffers[i], buffer_memory[i], 0) != VK_SUCCESS)
            {
                return false;
            }
        }
        std::array<VkAttachmentDescription2, 2> attachments{};
        std::array<VkAttachmentReference2, 2> references{};
        for (uint32_t i = 0; i < attachments.size(); ++i)
        {
            auto& a = attachments[i];
            a.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            a.format = VK_FORMAT_R8G8B8A8_UINT;
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            references[i] = {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
                             .pNext = nullptr,
                             .attachment = i,
                             .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
        }
        std::array<VkSubpassDescription2, 2> subpasses{};
        for (auto& subpass : subpasses)
        {
            subpass.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 2;
            subpass.pColorAttachments = references.data();
        }
        std::array<VkSubpassDependency2, 3> dependencies{};
        for (auto& dependency : dependencies)
        {
            dependency.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        }
        dependencies[0].srcSubpass = 0;
        dependencies[0].dstSubpass = 1;
        dependencies[0].srcStageMask = dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 1;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        dependencies[2].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[2].dstSubpass = 0;
        dependencies[2].srcStageMask = dependencies[2].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[2].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo2 pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        pass.attachmentCount = 2;
        pass.pAttachments = attachments.data();
        pass.subpassCount = 2;
        pass.pSubpasses = subpasses.data();
        pass.dependencyCount = 3;
        pass.pDependencies = dependencies.data();
        if (vkCreateRenderPass2(device, &pass, nullptr, &render_pass) != VK_SUCCESS ||
            vkCreateRenderPass2KHR(device, &pass, nullptr, &alias_pass) != VK_SUCCESS)
        {
            return false;
        }
        VkExtent2D granularity{};
        vkGetRenderAreaGranularity(device, render_pass, &granularity);
        if (!granularity.width || !granularity.height)
        {
            return false;
        }
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = render_pass;
        fb.attachmentCount = 2;
        fb.pAttachments = views.data();
        fb.width = width;
        fb.height = height;
        fb.layers = 2;
        if (vkCreateFramebuffer(device, &fb, nullptr, &framebuffer) != VK_SUCCESS)
        {
            return false;
        }
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = family;
        if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
        {
            return false;
        }
        VkCommandBuffer command{};
        const VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        if (vkAllocateCommandBuffers(device, &command_info, &command) != VK_SUCCESS)
        {
            return false;
        }
        VkCommandBufferBeginInfo command_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        if (vkBeginCommandBuffer(command, &command_begin) != VK_SUCCESS)
        {
            return false;
        }
        std::array<VkClearValue, 2> clears{};
        clears[0].color.uint32[0] = 0x12;
        clears[0].color.uint32[1] = 0x34;
        clears[0].color.uint32[2] = 0x56;
        clears[0].color.uint32[3] = 0x78;
        clears[1].color.uint32[0] = 0x87;
        clears[1].color.uint32[1] = 0x65;
        clears[1].color.uint32[2] = 0x43;
        clears[1].color.uint32[3] = 0x21;
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = render_pass;
        begin.framebuffer = framebuffer;
        begin.renderArea = {.offset = {2, 3}, .extent = {8, 6}};
        begin.clearValueCount = 2;
        begin.pClearValues = clears.data();
        const VkSubpassBeginInfo subpass_begin{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, nullptr, VK_SUBPASS_CONTENTS_INLINE};
        const VkSubpassEndInfo subpass_end{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
        vkCmdBeginRenderPass2(command, &begin, &subpass_begin);
        vkCmdNextSubpass2KHR(command, &subpass_begin, &subpass_end);
        vkCmdEndRenderPass2(command, &subpass_end);
        // A compatible render pass created through the alias exercises the other three aliases.
        begin.renderPass = alias_pass;
        vkCmdBeginRenderPass2KHR(command, &begin, &subpass_begin);
        vkCmdNextSubpass2(command, &subpass_begin, &subpass_end);
        vkCmdEndRenderPass2KHR(command, &subpass_end);
        VkBufferImageCopy region{};
        region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        region.imageExtent = {.width = width, .height = height, .depth = 1};
        for (uint32_t i = 0; i < images.size(); ++i)
        {
            vkCmdCopyImageToBuffer(command, images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffers[i], 1, &region);
        }
        // Fence waiting orders execution, while this memory dependency makes transfer writes visible to the host.
        const VkMemoryBarrier host_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_read, 0, nullptr, 0, nullptr);
        if (vkEndCommandBuffer(command) != VK_SUCCESS)
        {
            return false;
        }
        const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS)
        {
            return false;
        }
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS)
        {
            return false;
        }
        submitted = true;
        if (vkWaitForFences(device, 1, &fence, VK_TRUE, 30000000000ULL) != VK_SUCCESS)
        {
            return false;
        }
        const std::array<uint32_t, 2> expected{0x78563412, 0x21436587};
        for (uint32_t i = 0; i < buffers.size(); ++i)
        {
            void* mapped = nullptr;
            if (vkMapMemory(device, buffer_memory[i], 0, readback_size, 0, &mapped) != VK_SUCCESS || !mapped)
            {
                return false;
            }
            bool matches = true;
            for (uint32_t y = 3; y < 9; ++y)
            {
                for (uint32_t x = 2; x < 10; ++x)
                {
                    matches &= static_cast<const uint32_t*>(mapped)[y * width + x] == expected[i];
                }
            }
            vkUnmapMemory(device, buffer_memory[i]);
            if (!matches)
            {
                return false;
            }
        }
        // The failed stream never reaches the host; void entry points must surface an unsupported
        // extension chain at EndCommandBuffer instead of dropping it and reporting success.
        if (vkBeginCommandBuffer(command, &command_begin) != VK_SUCCESS)
        {
            return false;
        }
        vkCmdBeginRenderPass2(command, &begin, &subpass_begin);
        VkBaseInStructure unsupported{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr};
        VkSubpassEndInfo invalid_end{VK_STRUCTURE_TYPE_SUBPASS_END_INFO, &unsupported};
        vkCmdEndRenderPass2(command, &invalid_end);
        return vkEndCommandBuffer(command) == VK_ERROR_FEATURE_NOT_PRESENT;
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
    if (framebuffer)
    {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    if (render_pass)
    {
        vkDestroyRenderPass(device, render_pass, nullptr);
    }
    if (alias_pass)
    {
        vkDestroyRenderPass(device, alias_pass, nullptr);
    }
    for (uint32_t i = 0; i < images.size(); ++i)
    {
        if (views[i])
        {
            vkDestroyImageView(device, views[i], nullptr);
        }
        if (images[i])
        {
            vkDestroyImage(device, images[i], nullptr);
        }
        if (image_memory[i])
        {
            vkFreeMemory(device, image_memory[i], nullptr);
        }
        if (buffers[i])
        {
            vkDestroyBuffer(device, buffers[i], nullptr);
        }
        if (buffer_memory[i])
        {
            vkFreeMemory(device, buffer_memory[i], nullptr);
        }
    }
    std::printf("[shim-test] render-pass2 core/KHR MRT, two layers, offset integer clear/readback and error propagation -> %s\n",
                ok ? "PASS" : "FAIL");
    return ok;
}
