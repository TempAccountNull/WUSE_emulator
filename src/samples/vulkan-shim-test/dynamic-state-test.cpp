#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

bool test_rendering_granularity(PFN_vkGetInstanceProcAddr get, VkInstance instance, VkPhysicalDevice physical, uint32_t family);

bool test_dynamic_commands(PFN_vkGetInstanceProcAddr get, VkInstance instance, VkPhysicalDevice physical, uint32_t family)
{
    if (!test_rendering_granularity(get, instance, physical, family))
    {
        return false;
    }

    const auto enumerate =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(get(instance, "vkEnumerateDeviceExtensionProperties"));
    const auto get_features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(get(instance, "vkGetPhysicalDeviceFeatures2"));
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(get(instance, "vkCreateDevice"));
    const auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(get(instance, "vkDestroyDevice"));
    const auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(get(instance, "vkGetDeviceQueue"));
    const auto create_pool = reinterpret_cast<PFN_vkCreateCommandPool>(get(instance, "vkCreateCommandPool"));
    const auto destroy_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(get(instance, "vkDestroyCommandPool"));
    const auto allocate = reinterpret_cast<PFN_vkAllocateCommandBuffers>(get(instance, "vkAllocateCommandBuffers"));
    const auto begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(get(instance, "vkBeginCommandBuffer"));
    const auto end = reinterpret_cast<PFN_vkEndCommandBuffer>(get(instance, "vkEndCommandBuffer"));
    const auto submit = reinterpret_cast<PFN_vkQueueSubmit>(get(instance, "vkQueueSubmit"));
    const auto idle = reinterpret_cast<PFN_vkQueueWaitIdle>(get(instance, "vkQueueWaitIdle"));
    if (!enumerate || !get_features || !create || !destroy || !get_queue || !create_pool || !destroy_pool || !allocate || !begin || !end ||
        !submit || !idle)
    {
        return false;
    }
    uint32_t count{};
    if (enumerate(physical, nullptr, &count, nullptr) != VK_SUCCESS)
    {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (enumerate(physical, nullptr, &count, extensions.data()) != VK_SUCCESS)
    {
        return false;
    }
    bool have_dynamic{};
    bool have_locations{};
    for (const auto& extension : extensions)
    {
        have_dynamic |= std::strcmp(extension.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) == 0;
        have_locations |= std::strcmp(extension.extensionName, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME) == 0;
    }
    if (!have_dynamic)
    {
        std::printf("[shim-test] extended dynamic recording -> SKIP (driver extension unavailable)\n");
        return true;
    }
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &available};
    get_features(physical, &features);
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    enabled.extendedDynamicState3AlphaToCoverageEnable = available.extendedDynamicState3AlphaToCoverageEnable;
    enabled.extendedDynamicState3AlphaToOneEnable = available.extendedDynamicState3AlphaToOneEnable;
    enabled.extendedDynamicState3DepthClampEnable = available.extendedDynamicState3DepthClampEnable;
    enabled.extendedDynamicState3LogicOpEnable = available.extendedDynamicState3LogicOpEnable;
    enabled.extendedDynamicState3PolygonMode = available.extendedDynamicState3PolygonMode;
    enabled.extendedDynamicState3RasterizationSamples = available.extendedDynamicState3RasterizationSamples;
    enabled.extendedDynamicState3SampleMask = available.extendedDynamicState3SampleMask;
    enabled.extendedDynamicState3ColorBlendEnable = available.extendedDynamicState3ColorBlendEnable;
    enabled.extendedDynamicState3ColorBlendEquation = available.extendedDynamicState3ColorBlendEquation;
    enabled.extendedDynamicState3ColorWriteMask = available.extendedDynamicState3ColorWriteMask;
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &priority};
    const std::array<const char*, 2> extension_names = {VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
                                                        VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = have_locations ? 2 : 1;
    device_info.ppEnabledExtensionNames = extension_names.data();
    VkDevice device{};
    const auto create_result = create(physical, &device_info, nullptr, &device);
    if (create_result != VK_SUCCESS)
    {
        std::printf("[shim-test] extended dynamic device -> FAIL %d\n", create_result);
        return false;
    }
    bool ok = true;
    uint32_t recorded = 0;
    VkCommandPool pool{};
    const VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, family};
    ok = create_pool(device, &pool_info, nullptr, &pool) == VK_SUCCESS;
    VkCommandBuffer buffer{};
    const VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                                 VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (ok)
    {
        ok = allocate(device, &allocation, &buffer) == VK_SUCCESS;
    }
    const VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (ok)
    {
        ok = begin(buffer, &begin_info) == VK_SUCCESS;
    }
    if (ok)
    {
        if (enabled.extendedDynamicState3AlphaToCoverageEnable)
        {
            const auto fn = reinterpret_cast<PFN_vkCmdSetAlphaToCoverageEnableEXT>(get(instance, "vkCmdSetAlphaToCoverageEnableEXT"));
            if (fn)
            {
                fn(buffer, VK_FALSE);
                ++recorded;
            }
            else
            {
                ok = false;
            }
        }
        if (enabled.extendedDynamicState3AlphaToOneEnable)
        {
            const auto fn = reinterpret_cast<PFN_vkCmdSetAlphaToOneEnableEXT>(get(instance, "vkCmdSetAlphaToOneEnableEXT"));
            if (fn)
            {
                fn(buffer, VK_FALSE);
                ++recorded;
            }
            else
            {
                ok = false;
            }
        }
        if (enabled.extendedDynamicState3DepthClampEnable)
        {
            const auto fn = reinterpret_cast<PFN_vkCmdSetDepthClampEnableEXT>(get(instance, "vkCmdSetDepthClampEnableEXT"));
            if (fn)
            {
                fn(buffer, VK_FALSE);
                ++recorded;
            }
            else
            {
                ok = false;
            }
        }
        if (enabled.extendedDynamicState3LogicOpEnable)
        {
            const auto fn = reinterpret_cast<PFN_vkCmdSetLogicOpEnableEXT>(get(instance, "vkCmdSetLogicOpEnableEXT"));
            if (fn)
            {
                fn(buffer, VK_FALSE);
                ++recorded;
            }
            else
            {
                ok = false;
            }
        }
        if (enabled.extendedDynamicState3PolygonMode)
        {
            const auto fn = reinterpret_cast<PFN_vkCmdSetPolygonModeEXT>(get(instance, "vkCmdSetPolygonModeEXT"));
            if (fn)
            {
                fn(buffer, VK_POLYGON_MODE_FILL);
                ++recorded;
            }
            else
            {
                ok = false;
            }
        }
        if (enabled.extendedDynamicState3RasterizationSamples)
        {
            const auto fn = reinterpret_cast<PFN_vkCmdSetRasterizationSamplesEXT>(get(instance, "vkCmdSetRasterizationSamplesEXT"));
            if (fn)
            {
                fn(buffer, VK_SAMPLE_COUNT_1_BIT);
                ++recorded;
            }
            else
            {
                ok = false;
            }
        }
        if (enabled.extendedDynamicState3SampleMask)
        {
            const auto fn = reinterpret_cast<PFN_vkCmdSetSampleMaskEXT>(get(instance, "vkCmdSetSampleMaskEXT"));
            const VkSampleMask value = 1;
            if (fn)
            {
                fn(buffer, VK_SAMPLE_COUNT_1_BIT, &value);
                ++recorded;
            }
            else
            {
                ok = false;
            }
        }
        if (enabled.extendedDynamicState3ColorBlendEnable)
        {
            const auto fn = reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(get(instance, "vkCmdSetColorBlendEnableEXT"));
            const VkBool32 value = VK_FALSE;
            if (fn)
            {
                fn(buffer, 0, 1, &value);
                ++recorded;
            }
            else
            {
                ok = false;
            }
        }
        if (enabled.extendedDynamicState3ColorBlendEquation)
        {
            const auto fn = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(get(instance, "vkCmdSetColorBlendEquationEXT"));
            const VkColorBlendEquationEXT value{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
                                                VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
            if (fn)
            {
                fn(buffer, 0, 1, &value);
                ++recorded;
            }
            else
            {
                ok = false;
            }
        }
        if (enabled.extendedDynamicState3ColorWriteMask)
        {
            const auto fn = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(get(instance, "vkCmdSetColorWriteMaskEXT"));
            const VkColorComponentFlags value = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
            if (fn)
            {
                fn(buffer, 0, 1, &value);
                ++recorded;
            }
            else
            {
                ok = false;
            }
        }
        if (have_locations)
        {
            const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceMultisamplePropertiesEXT>(
                get(instance, "vkGetPhysicalDeviceMultisamplePropertiesEXT"));
            const auto set = reinterpret_cast<PFN_vkCmdSetSampleLocationsEXT>(get(instance, "vkCmdSetSampleLocationsEXT"));
            if (!query || !set)
            {
                ok = false;
            }
            else
            {
                VkMultisamplePropertiesEXT properties{VK_STRUCTURE_TYPE_MULTISAMPLE_PROPERTIES_EXT};
                properties.maxSampleLocationGridSize = {.width = UINT32_MAX, .height = UINT32_MAX};
                query(physical, VK_SAMPLE_COUNT_1_BIT, &properties);
                if (properties.maxSampleLocationGridSize.width == UINT32_MAX || properties.maxSampleLocationGridSize.height == UINT32_MAX)
                {
                    ok = false;
                }
                else if (properties.maxSampleLocationGridSize.width && properties.maxSampleLocationGridSize.height)
                {
                    const VkSampleLocationEXT point{0.5f, 0.5f};
                    const VkSampleLocationsInfoEXT info{
                        VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT, nullptr, VK_SAMPLE_COUNT_1_BIT, {1, 1}, 1, &point};
                    set(buffer, &info);
                    ++recorded;
                }
                std::printf("[shim-test] sample-count1 grid=%ux%u\n", properties.maxSampleLocationGridSize.width,
                            properties.maxSampleLocationGridSize.height);
            }
        }
        const VkResult end_result = end(buffer);
        ok &= end_result == VK_SUCCESS;
        if (ok)
        {
            VkQueue queue{};
            get_queue(device, family, 0, &queue);
            VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submission.commandBufferCount = 1;
            submission.pCommandBuffers = &buffer;
            ok = submit(queue, 1, &submission, VK_NULL_HANDLE) == VK_SUCCESS && idle(queue) == VK_SUCCESS;
        }
    }
    if (pool)
    {
        destroy_pool(device, pool, nullptr);
    }
    destroy(device, nullptr);
    std::printf("[shim-test] extended dynamic recording+submission (%u driver-supported commands) -> %s\n", recorded, ok ? "PASS" : "FAIL");
    return ok;
}
