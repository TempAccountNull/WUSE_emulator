#pragma once

#include <gpu_bridge_protocol.hpp>
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <cstring>

namespace sogen::gpu_bridge::instance_create_policy
{
    struct validation
    {
        VkResult result{VK_SUCCESS};
        uint32_t extension_bits{};
        const VkDebugUtilsMessengerCreateInfoEXT* callback{};
        VkStructureType rejected_pnext{VK_STRUCTURE_TYPE_MAX_ENUM};
    };

    inline validation validate(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
                               VkInstance* instance, bool native_wsi, bool debug_utils_available)
    {
        validation out{};
        if (!info || !instance || allocator || info->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO)
        {
            out.result = VK_ERROR_INITIALIZATION_FAILED;
            return out;
        }
        if (info->enabledLayerCount)
        {
            out.result = VK_ERROR_LAYER_NOT_PRESENT; // No guest instance layer is advertised.
            return out;
        }
        if (info->flags || info->enabledExtensionCount > 64 ||
            (info->enabledExtensionCount && !info->ppEnabledExtensionNames))
        {
            out.result = VK_ERROR_EXTENSION_NOT_PRESENT;
            return out;
        }
        for (uint32_t index = 0; index < info->enabledExtensionCount; ++index)
        {
            const char* name = info->ppEnabledExtensionNames[index];
            if (!name)
            {
                out.result = VK_ERROR_EXTENSION_NOT_PRESENT;
                return out;
            }
            if (std::strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME) == 0)
                out.extension_bits |= instance_ext_surface;
            else if (std::strcmp(name, "VK_KHR_win32_surface") == 0)
                out.extension_bits |= instance_ext_win32_surface;
            else if (std::strcmp(name, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0 && native_wsi)
                out.extension_bits |= instance_ext_surface_capabilities2;
            else if (std::strcmp(name, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0 && debug_utils_available)
                out.extension_bits |= instance_ext_debug_utils;
            else
            {
                out.result = VK_ERROR_EXTENSION_NOT_PRESENT;
                return out;
            }
        }
        if ((out.extension_bits & (instance_ext_win32_surface | instance_ext_surface_capabilities2)) &&
            !(out.extension_bits & instance_ext_surface))
        {
            out.result = VK_ERROR_EXTENSION_NOT_PRESENT;
            return out;
        }
        if (const VkApplicationInfo* app = info->pApplicationInfo)
        {
            if (app->sType != VK_STRUCTURE_TYPE_APPLICATION_INFO || app->pNext)
            {
                out.result = VK_ERROR_INITIALIZATION_FAILED;
                return out;
            }
            const uint32_t requested = app->apiVersion ? app->apiVersion : VK_API_VERSION_1_0;
            if (VK_API_VERSION_VARIANT(requested) != 0 || VK_API_VERSION_MAJOR(requested) != 1 ||
                requested > VK_API_VERSION_1_3)
            {
                out.result = VK_ERROR_INCOMPATIBLE_DRIVER;
                return out;
            }
        }
        uint32_t chain_length = 0;
        for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next; next = next->pNext)
        {
            if (++chain_length > 16)
            {
                out.result = VK_ERROR_INITIALIZATION_FAILED;
                return out;
            }
            if (next->sType != VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT || out.callback)
            {
                out.result = VK_ERROR_EXTENSION_NOT_PRESENT;
                out.rejected_pnext = next->sType;
                return out;
            }
            if (!(out.extension_bits & instance_ext_debug_utils))
            {
                out.result = VK_ERROR_EXTENSION_NOT_PRESENT;
                return out;
            }
            out.callback = reinterpret_cast<const VkDebugUtilsMessengerCreateInfoEXT*>(next);
        }
        return out;
    }
}
