#include <windows.h>
#include <cstdio>
#include <cstring>
#include <array>
#include <vector>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

bool test_rendering_granularity(PFN_vkGetInstanceProcAddr get, VkInstance instance, VkPhysicalDevice physical, uint32_t family)
{
    const auto enumerate =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(get(instance, "vkEnumerateDeviceExtensionProperties"));
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(get(instance, "vkCreateDevice"));
    const auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(get(instance, "vkDestroyDevice"));
    const auto query = reinterpret_cast<PFN_vkGetRenderingAreaGranularityKHR>(get(instance, "vkGetRenderingAreaGranularityKHR"));
    const auto core = reinterpret_cast<PFN_vkGetRenderingAreaGranularity>(get(instance, "vkGetRenderingAreaGranularity"));
    const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(instance, "vkGetPhysicalDeviceProperties"));
    const auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(get(nullptr, "vkEnumerateInstanceVersion"));
    if (!enumerate || !create || !destroy || !get_properties)
    {
        return false;
    }
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (enumerate_version && enumerate_version(&loader_version) != VK_SUCCESS)
    {
        std::printf("[shim-test] rendering-area granularity -> FAIL (instance version query)\n");
        return false;
    }
    // Main creates this instance with min(loader version, Vulkan 1.3). Extension-name support alone
    // does not satisfy maintenance5's dynamic-rendering dependency on earlier effective API versions.
    const uint32_t requested_version = loader_version < VK_API_VERSION_1_3 ? loader_version : VK_API_VERSION_1_3;
    VkPhysicalDeviceProperties properties{};
    get_properties(physical, &properties);
    if (requested_version < VK_API_VERSION_1_3 || properties.apiVersion < VK_API_VERSION_1_3)
    {
        std::printf("[shim-test] rendering-area granularity -> SKIP (requested/device Vulkan 1.3 unavailable)\n");
        return true;
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
    bool available = false;
    for (const auto& extension : extensions)
    {
        available |= std::strcmp(extension.extensionName, VK_KHR_MAINTENANCE_5_EXTENSION_NAME) == 0;
    }
    if (!available)
    {
        std::printf("[shim-test] rendering-area granularity -> SKIP (maintenance5 unavailable)\n");
        return true;
    }
    if (!query)
    {
        std::printf("[shim-test] rendering-area granularity -> FAIL (advertised maintenance5 has no KHR entry point)\n");
        return false;
    }
    std::printf("[shim-test] core rendering-area entry point resolved=%s (not called by this KHR fixture)\n", core ? "yes" : "no");
    const char* extension = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
    const float priority = 1;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue;
    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = &extension;
    VkDevice device{};
    if (create(physical, &create_info, nullptr, &device) != VK_SUCCESS)
    {
        return false;
    }
    VkRenderingAreaInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_AREA_INFO;
    VkExtent2D empty{};
    query(device, &info, &empty);
    const std::array<VkFormat, 2> formats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    info.colorAttachmentCount = static_cast<uint32_t>(formats.size());
    info.pColorAttachmentFormats = formats.data();
    info.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkExtent2D colors{};
    query(device, &info, &colors);
    // The KHR command is valid for this extension-enabled Vulkan 1.3 device.
    // Core 1.4 export resolution above does not claim a negotiated 1.4 instance.
    const bool ok = empty.width && empty.height && colors.width && colors.height;
    destroy(device, nullptr);
    std::printf("[shim-test] rendering-area granularity KHR empty=%ux%u MRT/depth=%ux%u -> %s\n", empty.width, empty.height, colors.width,
                colors.height, ok ? "PASS" : "FAIL");
    return ok;
}
