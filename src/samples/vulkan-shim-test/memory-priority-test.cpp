#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

bool test_memory_priority(PFN_vkGetInstanceProcAddr get, VkInstance instance, VkPhysicalDevice physical, uint32_t family,
                          uint32_t instance_version)
{
#define LOAD(name)                                                                \
    const auto name = reinterpret_cast<PFN_##name>(get(instance, #name));         \
    if (!name)                                                                    \
    {                                                                             \
        std::printf("[shim-test] memory priority -> FAIL (missing %s)\n", #name); \
        return false;                                                             \
    }
    LOAD(vkEnumerateDeviceExtensionProperties);
    LOAD(vkGetPhysicalDeviceProperties);
    VkPhysicalDeviceProperties physical_properties{};
    vkGetPhysicalDeviceProperties(physical, &physical_properties);
    if (instance_version < VK_API_VERSION_1_1 || physical_properties.apiVersion < VK_API_VERSION_1_1)
    {
        std::printf("[shim-test] memory priority -> SKIP (fixture requires instance/device API 1.1)\n");
        return true;
    }
    LOAD(vkGetPhysicalDeviceFeatures2);
    LOAD(vkCreateDevice);
    LOAD(vkDestroyDevice);
    LOAD(vkCreateBuffer);
    LOAD(vkDestroyBuffer);
    LOAD(vkGetBufferMemoryRequirements);
    LOAD(vkBindBufferMemory);
    LOAD(vkAllocateMemory);
    LOAD(vkFreeMemory);
#undef LOAD
    uint32_t count{};
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS)
    {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()) != VK_SUCCESS)
    {
        return false;
    }
    bool have_priority = false;
    bool have_pageable = false;
    for (const auto& extension : extensions)
    {
        have_priority |= std::strcmp(extension.extensionName, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME) == 0;
        have_pageable |= std::strcmp(extension.extensionName, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME) == 0;
    }
    if (!have_priority || !have_pageable)
    {
        std::printf("[shim-test] memory priority -> SKIP (memory-priority/pageable extensions unavailable)\n");
        return true;
    }
    const auto set_priority = reinterpret_cast<PFN_vkSetDeviceMemoryPriorityEXT>(get(instance, "vkSetDeviceMemoryPriorityEXT"));
    if (!set_priority)
    {
        std::printf("[shim-test] memory priority -> FAIL (advertised pageable extension has no setter)\n");
        return false;
    }
    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT available_pageable{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT};
    VkPhysicalDeviceMemoryPriorityFeaturesEXT available_priority{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT,
                                                                 &available_pageable};
    VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &available_priority};
    vkGetPhysicalDeviceFeatures2(physical, &available);
    bool all_ok = true;
    uint32_t passed = 0;
    uint32_t skipped = 0;
    for (const bool enable_features : {false, true})
    {
        if (enable_features && (!available_priority.memoryPriority || !available_pageable.pageableDeviceLocalMemory))
        {
            std::printf("[shim-test] memory priority enabled-feature case -> SKIP (feature unavailable)\n");
            ++skipped;
            continue;
        }
        const std::array<const char*, 2> names{VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME};
        const float queue_priority = 1.0f;
        const VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &queue_priority};
        VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT pageable{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT, nullptr, enable_features ? VK_TRUE : VK_FALSE};
        const VkPhysicalDeviceMemoryPriorityFeaturesEXT priority_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT,
                                                                          &pageable, enable_features ? VK_TRUE : VK_FALSE};
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.pNext = &priority_features;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<uint32_t>(names.size());
        device_info.ppEnabledExtensionNames = names.data();
        VkDevice device{};
        if (vkCreateDevice(physical, &device_info, nullptr, &device) != VK_SUCCESS)
        {
            std::printf("[shim-test] memory priority -> FAIL (create device, features=%u)\n", enable_features ? 1u : 0u);
            all_ok = false;
            continue;
        }
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        const auto execute = [&]() {
            VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer_info.size = 4096;
            buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS)
            {
                return false;
            }
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, buffer, &requirements);
            if (!requirements.memoryTypeBits)
            {
                return false;
            }
            uint32_t type = 0;
            while (!(requirements.memoryTypeBits & (1u << type)))
            {
                ++type;
            }
            const VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, VK_NULL_HANDLE,
                                                          buffer};
            VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT, &dedicated, 0.75f};
            VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &priority, VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT, 1};
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags, requirements.size, type};
            if (!enable_features)
            {
                // This is an explicit bridge-validation result for the generic enabled-feature contract.
                VkDeviceMemory rejected{};
                const auto rejected_result = vkAllocateMemory(device, &allocation, nullptr, &rejected);
                if (rejected_result != VK_ERROR_FEATURE_NOT_PRESENT || rejected != VK_NULL_HANDLE)
                {
                    if (rejected)
                    {
                        vkFreeMemory(device, rejected, nullptr);
                    }
                    return false;
                }
                flags.pNext = &dedicated;
            }
            auto result = vkAllocateMemory(device, &allocation, nullptr, &memory);
            if (result != VK_SUCCESS || !memory || vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS)
            {
                return false;
            }
            // The setter is extension functionality even when automatic paging was not enabled.
            // Vulkan exposes no query for the selected priority or guarantee that the driver migrates memory.
            set_priority(device, memory, 0.0f);
            set_priority(device, memory, 0.5f);
            set_priority(device, memory, 1.0f);
            if (enable_features)
            {
                VkDeviceMemory rejected{};
                priority.priority = std::numeric_limits<float>::quiet_NaN();
                result = vkAllocateMemory(device, &allocation, nullptr, &rejected);
                if (result != VK_ERROR_VALIDATION_FAILED_EXT || rejected != VK_NULL_HANDLE)
                {
                    if (rejected)
                    {
                        vkFreeMemory(device, rejected, nullptr);
                    }
                    return false;
                }
            }
            const VkExportMemoryAllocateInfo unsupported{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr,
                                                         VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
            allocation.pNext = &unsupported;
            VkDeviceMemory rejected{};
            result = vkAllocateMemory(device, &allocation, nullptr, &rejected);
            if (rejected)
            {
                vkFreeMemory(device, rejected, nullptr);
            }
            return result == VK_ERROR_FEATURE_NOT_PRESENT && rejected == VK_NULL_HANDLE;
        };
        const bool ok = execute();
        if (buffer)
        {
            vkDestroyBuffer(device, buffer, nullptr);
        }
        if (memory)
        {
            vkFreeMemory(device, memory, nullptr);
        }
        vkDestroyDevice(device, nullptr);
        std::printf("[shim-test] priority allocation/flags/dedicated chain and void setter calls, features=%u -> %s\n",
                    enable_features ? 1u : 0u, ok ? "PASS" : "FAIL");
        passed += ok ? 1u : 0u;
        all_ok &= ok;
    }
    std::printf("[shim-test] memory priority: %u PASS, %u SKIP; void-call failures must also be checked in shim diagnostics\n", passed,
                skipped);
    return all_ok;
}
