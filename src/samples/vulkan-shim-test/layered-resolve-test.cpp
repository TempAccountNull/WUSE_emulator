#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace
{
    enum class readback_result
    {
        passed,
        skipped,
        failed,
    };
}

bool test_layered_resolve_readback(PFN_vkGetInstanceProcAddr get)
{
    constexpr std::array route_names{"legacy", "core2", "KHR2"};
    constexpr std::array result_names{"PASS", "SKIP", "FAIL"};
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
    const auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(get(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (!create_instance)
    {
        std::printf("[shim-test] layered resolve -> FAIL (missing vkCreateInstance)\n");
        return false;
    }
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (enumerate_version && enumerate_version(&loader_version) != VK_SUCCESS)
    {
        std::printf("[shim-test] layered resolve -> FAIL (enumerate instance version)\n");
        return false;
    }
    // Request the core version this fixture uses instead of inheriting the main test's Vulkan 1.0 instance.
    const uint32_t requested_version = (std::min)(loader_version, VK_API_VERSION_1_3);
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Sogen layered readback";
    application.apiVersion = requested_version;
    const VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &application};
    VkInstance instance{};
    if (create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS)
    {
        std::printf("[shim-test] layered resolve -> FAIL (create instance)\n");
        return false;
    }
    const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
    if (!destroy_instance)
    {
        std::printf("[shim-test] layered resolve -> FAIL (missing vkDestroyInstance)\n");
        return false;
    }

    struct instance_scope
    {
        VkInstance handle;
        PFN_vkDestroyInstance destroy;

        ~instance_scope()
        {
            destroy(handle, nullptr);
        }
    } instance_lifetime{.handle = instance, .destroy = destroy_instance};

#define LOAD(name)                                                                \
    const auto name = reinterpret_cast<PFN_##name>(get(instance, #name));         \
    if (!name)                                                                    \
    {                                                                             \
        std::printf("[shim-test] layered resolve -> FAIL (missing %s)\n", #name); \
        return false;                                                             \
    }
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkGetPhysicalDeviceFormatProperties);
    LOAD(vkGetPhysicalDeviceImageFormatProperties);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkEnumerateDeviceExtensionProperties);
    LOAD(vkCreateDevice);
    LOAD(vkDestroyDevice);
    LOAD(vkGetDeviceQueue);
    LOAD(vkDeviceWaitIdle);
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
    LOAD(vkCreateRenderPass);
    LOAD(vkDestroyRenderPass);
    LOAD(vkCreateFramebuffer);
    LOAD(vkDestroyFramebuffer);
    LOAD(vkCreateCommandPool);
    LOAD(vkDestroyCommandPool);
    LOAD(vkAllocateCommandBuffers);
    LOAD(vkBeginCommandBuffer);
    LOAD(vkEndCommandBuffer);
    LOAD(vkCmdBeginRenderPass);
    LOAD(vkCmdClearAttachments);
    LOAD(vkCmdEndRenderPass);
    LOAD(vkCmdCopyImageToBuffer);
    LOAD(vkCmdPipelineBarrier);
    LOAD(vkCreateFence);
    LOAD(vkDestroyFence);
    LOAD(vkQueueSubmit);
    LOAD(vkWaitForFences);
#undef LOAD
    const auto vkCmdCopyImageToBuffer2 = reinterpret_cast<PFN_vkCmdCopyImageToBuffer2>(get(instance, "vkCmdCopyImageToBuffer2"));
    const auto vkCmdCopyImageToBuffer2KHR = reinterpret_cast<PFN_vkCmdCopyImageToBuffer2KHR>(get(instance, "vkCmdCopyImageToBuffer2KHR"));

    uint32_t physical_count = 0;
    if (vkEnumeratePhysicalDevices(instance, &physical_count, nullptr) != VK_SUCCESS || !physical_count)
    {
        std::printf("[shim-test] layered resolve -> FAIL (enumerate physical devices)\n");
        return false;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    if (vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices.data()) != VK_SUCCESS || !physical_count)
    {
        std::printf("[shim-test] layered resolve -> FAIL (read physical devices)\n");
        return false;
    }
    const VkPhysicalDevice physical = physical_devices[0];
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < (std::min)(family_count, static_cast<uint32_t>(families.size())); ++i)
    {
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            family = i;
            break;
        }
    }
    if (family == UINT32_MAX)
    {
        std::printf("[shim-test] direct layered readback -> 0 PASS, 6 SKIP, 0 FAIL (no graphics queue)\n");
        return true;
    }

    constexpr VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr uint32_t width = 16;
    constexpr uint32_t height = 12;
    constexpr uint32_t layers = 2;
    constexpr size_t buffer_size = 2048;
    constexpr unsigned char sentinel = 0xa7;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    vkGetPhysicalDeviceFormatProperties(physical, format, &format_properties);
    uint32_t extension_count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr) != VK_SUCCESS)
    {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, extensions.data()) != VK_SUCCESS)
    {
        return false;
    }
    bool copy2_khr = false;
    for (const auto& extension : extensions)
    {
        copy2_khr |= std::strcmp(extension.extensionName, VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME) == 0;
    }
    const bool copy2_core = requested_version >= VK_API_VERSION_1_3 && properties.apiVersion >= VK_API_VERSION_1_3;
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &priority};
    const char* extension_name = VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = copy2_khr ? 1u : 0u;
    device_info.ppEnabledExtensionNames = copy2_khr ? &extension_name : nullptr;
    VkDevice device{};
    const auto create_result = vkCreateDevice(physical, &device_info, nullptr, &device);
    if (create_result != VK_SUCCESS)
    {
        std::printf("[shim-test] layered resolve -> FAIL (create device %d)\n", create_result);
        return false;
    }
    VkQueue queue{};
    vkGetDeviceQueue(device, family, 0, &queue);
    if (!queue)
    {
        vkDestroyDevice(device, nullptr);
        return false;
    }

    uint32_t passed = 0;
    uint32_t skipped = 0;
    uint32_t failed = 0;
    const auto run_case = [&](bool multisampled, uint32_t route) -> readback_result {
        const auto capability_skip = [](const char* reason) {
            std::printf("[shim-test] layered readback capability -> SKIP (%s)\n", reason);
            return readback_result::skipped;
        };
        if ((route == 1 && !copy2_core) || (route == 2 && !copy2_khr))
        {
            return capability_skip(route == 1 ? "core copy2 requires Vulkan 1.3" : "KHR_copy_commands2 is not exposed");
        }
        if ((route == 1 && !vkCmdCopyImageToBuffer2) || (route == 2 && !vkCmdCopyImageToBuffer2KHR))
        {
            std::printf("[shim-test] layered readback -> FAIL (advertised copy2 route has no entry point)\n");
            return readback_result::failed;
        }
        if (properties.limits.maxFramebufferLayers < layers || properties.limits.maxFramebufferWidth < width ||
            properties.limits.maxFramebufferHeight < height)
        {
            return capability_skip("two-layer framebuffer limits");
        }
        const VkFormatFeatureFlags required_format = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((format_properties.optimalTilingFeatures & required_format) != required_format)
        {
            return capability_skip("R8G8B8A8_UNORM optimal color attachment / transfer source features");
        }
        VkImageFormatProperties output_properties{};
        auto result = vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0,
                                                               &output_properties);
        if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
        {
            return capability_skip("single-sample attachment/readback image usage is reported unsupported");
        }
        if (result != VK_SUCCESS)
        {
            std::printf("[shim-test] image format query failed: %d\n", result);
            return readback_result::failed;
        }
        if (!(output_properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) || output_properties.maxArrayLayers < layers ||
            output_properties.maxExtent.width < width || output_properties.maxExtent.height < height)
        {
            return capability_skip("single-sample two-layer attachment/readback image limits");
        }
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        if (multisampled)
        {
            VkImageFormatProperties multisample_properties{};
            result = vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &multisample_properties);
            if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
            {
                return capability_skip("multisampled color image usage is reported unsupported");
            }
            if (result != VK_SUCCESS)
            {
                return readback_result::failed;
            }
            const auto supported = multisample_properties.sampleCounts & properties.limits.framebufferColorSampleCounts;
            if (multisample_properties.maxArrayLayers < layers || multisample_properties.maxExtent.width < width ||
                multisample_properties.maxExtent.height < height || !(supported & (VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_2_BIT)))
            {
                return capability_skip("two-layer 2x/4x color samples unavailable");
            }
            samples = supported & VK_SAMPLE_COUNT_4_BIT ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_2_BIT;
        }

        std::array<VkImage, 2> images{};
        std::array<VkImageView, 2> views{};
        std::array<VkDeviceMemory, 2> image_memory{};
        VkBuffer buffer{};
        VkDeviceMemory buffer_memory{};
        VkRenderPass render_pass{};
        VkFramebuffer framebuffer{};
        VkCommandPool pool{};
        VkFence fence{};
        bool submitted = false;
        bool missing_coherent_memory = false;
        const auto allocate = [&](const VkMemoryRequirements& requirements, VkMemoryPropertyFlags flags, VkDeviceMemory& memory) {
            for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
            {
                if ((requirements.memoryTypeBits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & flags) == flags)
                {
                    const VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, i};
                    return vkAllocateMemory(device, &info, nullptr, &memory) == VK_SUCCESS;
                }
            }
            missing_coherent_memory = flags != 0;
            return false;
        };
        const auto execute = [&]() -> readback_result {
            const uint32_t image_count = multisampled ? 2u : 1u;
            for (uint32_t i = 0; i < image_count; ++i)
            {
                VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                image.imageType = VK_IMAGE_TYPE_2D;
                image.format = format;
                image.extent = {.width = width, .height = height, .depth = 1};
                image.mipLevels = 1;
                image.arrayLayers = layers;
                image.samples = i == 0 ? samples : VK_SAMPLE_COUNT_1_BIT;
                image.tiling = VK_IMAGE_TILING_OPTIMAL;
                image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                if (!multisampled || i == 1)
                {
                    image.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                }
                if (vkCreateImage(device, &image, nullptr, &images[i]) != VK_SUCCESS)
                {
                    return readback_result::failed;
                }
                VkMemoryRequirements requirements{};
                vkGetImageMemoryRequirements(device, images[i], &requirements);
                if (!allocate(requirements, 0, image_memory[i]) || vkBindImageMemory(device, images[i], image_memory[i], 0) != VK_SUCCESS)
                {
                    return readback_result::failed;
                }
                VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                view.image = images[i];
                view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                view.format = format;
                view.subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = layers};
                if (vkCreateImageView(device, &view, nullptr, &views[i]) != VK_SUCCESS)
                {
                    return readback_result::failed;
                }
            }
            VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer_info.size = buffer_size;
            buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS)
            {
                return readback_result::failed;
            }
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, buffer, &requirements);
            if (!allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer_memory))
            {
                return missing_coherent_memory ? capability_skip("no matching host-visible coherent buffer memory")
                                               : readback_result::failed;
            }
            if (vkBindBufferMemory(device, buffer, buffer_memory, 0) != VK_SUCCESS)
            {
                return readback_result::failed;
            }
            void* mapped = nullptr;
            if (vkMapMemory(device, buffer_memory, 0, buffer_size, 0, &mapped) != VK_SUCCESS || !mapped)
            {
                return readback_result::failed;
            }
            std::memset(mapped, sentinel, buffer_size);
            vkUnmapMemory(device, buffer_memory);

            std::array<VkAttachmentDescription, 2> attachments{};
            for (uint32_t i = 0; i < image_count; ++i)
            {
                auto& attachment = attachments[i];
                attachment.format = format;
                attachment.samples = i == 0 ? samples : VK_SAMPLE_COUNT_1_BIT;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachment.storeOp = i == 0 && multisampled ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
                attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                attachment.finalLayout =
                    i == 0 && multisampled ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            }
            const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            const VkAttachmentReference resolve{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color;
            subpass.pResolveAttachments = multisampled ? &resolve : nullptr;
            const std::array<VkSubpassDependency, 2> dependencies{{
                {.srcSubpass = VK_SUBPASS_EXTERNAL,
                 .dstSubpass = 0,
                 .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 .srcAccessMask = 0,
                 .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 .dependencyFlags = 0},
                {.srcSubpass = 0,
                 .dstSubpass = VK_SUBPASS_EXTERNAL,
                 .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
                 .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                 .dependencyFlags = 0},
            }};
            VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            pass.attachmentCount = image_count;
            pass.pAttachments = attachments.data();
            pass.subpassCount = 1;
            pass.pSubpasses = &subpass;
            pass.dependencyCount = static_cast<uint32_t>(dependencies.size());
            pass.pDependencies = dependencies.data();
            if (vkCreateRenderPass(device, &pass, nullptr, &render_pass) != VK_SUCCESS)
            {
                return readback_result::failed;
            }
            const VkFramebufferCreateInfo fb{
                VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, render_pass, image_count, views.data(), width, height, layers};
            if (vkCreateFramebuffer(device, &fb, nullptr, &framebuffer) != VK_SUCCESS)
            {
                return readback_result::failed;
            }
            const VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, family};
            if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
            {
                return readback_result::failed;
            }
            const VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                                         VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
            VkCommandBuffer command{};
            if (vkAllocateCommandBuffers(device, &allocation, &command) != VK_SUCCESS)
            {
                return readback_result::failed;
            }
            const VkCommandBufferBeginInfo begin_command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            if (vkBeginCommandBuffer(command, &begin_command) != VK_SUCCESS)
            {
                return readback_result::failed;
            }
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass = render_pass;
            begin.framebuffer = framebuffer;
            begin.renderArea = {.offset = {0, 0}, .extent = {width, height}};
            vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
            using pixel = std::array<unsigned char, 4>;
            const std::array<pixel, 2> background = multisampled ? std::array<pixel, 2>{{{0, 255, 255, 255}, {255, 0, 255, 255}}}
                                                                 : std::array<pixel, 2>{{{255, 0, 0, 255}, {0, 255, 0, 255}}};
            const std::array<pixel, 2> foreground = multisampled ? std::array<pixel, 2>{{{255, 255, 255, 255}, {0, 0, 0, 255}}}
                                                                 : std::array<pixel, 2>{{{0, 0, 255, 255}, {255, 255, 0, 255}}};
            for (uint32_t layer = 0; layer < layers; ++layer)
            {
                for (uint32_t patch = 0; patch < 2; ++patch)
                {
                    VkClearAttachment clear{};
                    clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    clear.colorAttachment = 0;
                    const auto& rgba = patch ? foreground[layer] : background[layer];
                    for (uint32_t component = 0; component < rgba.size(); ++component)
                    {
                        clear.clearValue.color.float32[component] = rgba[component] == 0 ? 0.0f : 1.0f;
                    }
                    // Without multiview, clear layers are relative to the framebuffer attachment view's base layer.
                    const VkClearRect rect{patch ? VkRect2D{{2, 1}, {7, 5}} : VkRect2D{{0, 0}, {width, height}}, layer, 1};
                    vkCmdClearAttachments(command, 1, &clear, 1, &rect);
                }
            }
            vkCmdEndRenderPass(command);

            const std::array<VkBufferImageCopy, 2> regions{{
                {.bufferOffset = 64,
                 .bufferRowLength = 13,
                 .bufferImageHeight = 9,
                 .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2},
                 .imageOffset = {2, 1, 0},
                 .imageExtent = {7, 5, 1}},
                {.bufferOffset = 1280,
                 .bufferRowLength = 11,
                 .bufferImageHeight = 7,
                 .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1},
                 .imageOffset = {4, 3, 0},
                 .imageExtent = {5, 4, 1}},
            }};
            const VkImage source = images[multisampled ? 1 : 0];
            if (route == 0)
            {
                vkCmdCopyImageToBuffer(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, static_cast<uint32_t>(regions.size()),
                                       regions.data());
            }
            else
            {
                std::array<VkBufferImageCopy2, 2> regions2{};
                for (uint32_t i = 0; i < regions.size(); ++i)
                {
                    const auto& region = regions[i];
                    regions2[i] = {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                                   .pNext = nullptr,
                                   .bufferOffset = region.bufferOffset,
                                   .bufferRowLength = region.bufferRowLength,
                                   .bufferImageHeight = region.bufferImageHeight,
                                   .imageSubresource = region.imageSubresource,
                                   .imageOffset = region.imageOffset,
                                   .imageExtent = region.imageExtent};
                }
                const VkCopyImageToBufferInfo2 copy{
                    VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, nullptr,        source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
                    static_cast<uint32_t>(regions2.size()),        regions2.data()};
                if (route == 1)
                {
                    vkCmdCopyImageToBuffer2(command, &copy);
                }
                else
                {
                    vkCmdCopyImageToBuffer2KHR(command, &copy);
                }
            }
            // Subpass dependencies make clear/resolve writes available to transfer reads; this barrier covers the readback writes.
            const VkMemoryBarrier host_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                                            VK_ACCESS_HOST_READ_BIT};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_read, 0, nullptr, 0,
                                 nullptr);
            if (vkEndCommandBuffer(command) != VK_SUCCESS)
            {
                return readback_result::failed;
            }
            const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS)
            {
                return readback_result::failed;
            }
            const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &command, 0, nullptr};
            if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS)
            {
                return readback_result::failed;
            }
            submitted = true;
            if (vkWaitForFences(device, 1, &fence, VK_TRUE, 30000000000ULL) != VK_SUCCESS)
            {
                return readback_result::failed;
            }
            std::array<unsigned char, buffer_size> expected{};
            expected.fill(sentinel);
            for (const auto& region : regions)
            {
                for (uint32_t layer = 0; layer < region.imageSubresource.layerCount; ++layer)
                {
                    const uint32_t image_layer = region.imageSubresource.baseArrayLayer + layer;
                    for (uint32_t y = 0; y < region.imageExtent.height; ++y)
                    {
                        for (uint32_t x = 0; x < region.imageExtent.width; ++x)
                        {
                            const auto image_x = static_cast<uint32_t>(region.imageOffset.x) + x;
                            const auto image_y = static_cast<uint32_t>(region.imageOffset.y) + y;
                            const bool patch = image_x >= 2 && image_x < 9 && image_y >= 1 && image_y < 6;
                            const auto& rgba = patch ? foreground[image_layer] : background[image_layer];
                            const auto offset = static_cast<size_t>(region.bufferOffset) +
                                                ((layer * region.bufferImageHeight + y) * region.bufferRowLength + x) * rgba.size();
                            std::memcpy(expected.data() + offset, rgba.data(), rgba.size());
                        }
                    }
                }
            }
            mapped = nullptr;
            if (vkMapMemory(device, buffer_memory, 0, buffer_size, 0, &mapped) != VK_SUCCESS || !mapped)
            {
                return readback_result::failed;
            }
            size_t mismatch = 0;
            const auto* actual = static_cast<const unsigned char*>(mapped);
            while (mismatch < expected.size() && actual[mismatch] == expected[mismatch])
            {
                ++mismatch;
            }
            if (mismatch != expected.size())
            {
                std::printf("[shim-test] readback byte %zu: expected %02x got %02x\n", mismatch, static_cast<unsigned>(expected[mismatch]),
                            static_cast<unsigned>(actual[mismatch]));
            }
            vkUnmapMemory(device, buffer_memory);
            return mismatch == expected.size() ? readback_result::passed : readback_result::failed;
        };
        const auto outcome = execute();
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
        if (buffer)
        {
            vkDestroyBuffer(device, buffer, nullptr);
        }
        if (buffer_memory)
        {
            vkFreeMemory(device, buffer_memory, nullptr);
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
        }
        std::printf("[shim-test] layered %s samples=%u copy=%s -> %s\n", multisampled ? "MSAA resolve" : "single-sample",
                    static_cast<unsigned>(samples), route_names[route], result_names[static_cast<size_t>(outcome)]);
        return outcome;
    };
    for (const bool multisampled : {false, true})
    {
        for (uint32_t route = 0; route < 3; ++route)
        {
            std::printf("[shim-test] starting layered %s copy=%s\n", multisampled ? "MSAA resolve" : "single-sample", route_names[route]);
            const auto result = run_case(multisampled, route);
            passed += result == readback_result::passed ? 1u : 0u;
            skipped += result == readback_result::skipped ? 1u : 0u;
            failed += result == readback_result::failed ? 1u : 0u;
        }
    }
    vkDestroyDevice(device, nullptr);
    std::printf("[shim-test] direct two-layer / padded-region / MSAA readback: %u PASS, %u SKIP, %u FAIL\n", passed, skipped, failed);
    return failed == 0;
}
