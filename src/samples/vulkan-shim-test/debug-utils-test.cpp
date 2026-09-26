#include <windows.h>

#include <array>
#include <cstdio>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

bool test_debug_utils_entrypoints(PFN_vkGetInstanceProcAddr get_instance_proc, HMODULE module)
{
    const auto get_device_proc =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(reinterpret_cast<void*>(GetProcAddress(module, "vkGetDeviceProcAddr")));
    const auto shim_create_instance =
        reinterpret_cast<PFN_vkVoidFunction>(reinterpret_cast<void*>(GetProcAddress(module, "vkCreateInstance")));
    if (!get_device_proc || !shim_create_instance)
    {
        std::printf("[shim-test] debug-utils resolver exports -> FAIL\n");
        return false;
    }

    const auto begin = reinterpret_cast<PFN_vkQueueBeginDebugUtilsLabelEXT>(
        reinterpret_cast<void*>(GetProcAddress(module, "vkQueueBeginDebugUtilsLabelEXT")));
    const auto end =
        reinterpret_cast<PFN_vkQueueEndDebugUtilsLabelEXT>(reinterpret_cast<void*>(GetProcAddress(module, "vkQueueEndDebugUtilsLabelEXT")));
    const auto insert = reinterpret_cast<PFN_vkQueueInsertDebugUtilsLabelEXT>(
        reinterpret_cast<void*>(GetProcAddress(module, "vkQueueInsertDebugUtilsLabelEXT")));
    const auto set_name =
        reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(reinterpret_cast<void*>(GetProcAddress(module, "vkSetDebugUtilsObjectNameEXT")));
    const auto set_tag =
        reinterpret_cast<PFN_vkSetDebugUtilsObjectTagEXT>(reinterpret_cast<void*>(GetProcAddress(module, "vkSetDebugUtilsObjectTagEXT")));
    if (!begin || !end || !insert || !set_name || !set_tag)
    {
        std::printf("[shim-test] debug-utils direct exports -> FAIL\n");
        return false;
    }

    const bool bridge_mode = get_instance_proc(VK_NULL_HANDLE, "vkCreateInstance") == shim_create_instance;
    if (bridge_mode)
    {
        struct entry
        {
            const char* name;
            PFN_vkVoidFunction function;
        };

        const std::array entries{
            entry{"vkQueueBeginDebugUtilsLabelEXT", reinterpret_cast<PFN_vkVoidFunction>(begin)},
            entry{"vkQueueEndDebugUtilsLabelEXT", reinterpret_cast<PFN_vkVoidFunction>(end)},
            entry{"vkQueueInsertDebugUtilsLabelEXT", reinterpret_cast<PFN_vkVoidFunction>(insert)},
            entry{"vkSetDebugUtilsObjectNameEXT", reinterpret_cast<PFN_vkVoidFunction>(set_name)},
            entry{"vkSetDebugUtilsObjectTagEXT", reinterpret_cast<PFN_vkVoidFunction>(set_tag)},
        };
        for (const auto& item : entries)
        {
            if (get_instance_proc(VK_NULL_HANDLE, item.name) != item.function ||
                get_device_proc(VK_NULL_HANDLE, item.name) != item.function)
            {
                std::printf("[shim-test] debug-utils resolver %s -> FAIL\n", item.name);
                return false;
            }
        }
    }

    if (!bridge_mode)
    {
        std::printf("[shim-test] debug-utils direct exports (passthrough) -> PASS\n");
        return true;
    }

    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = "shim-test";
    begin(VK_NULL_HANDLE, &label);
    insert(VK_NULL_HANDLE, &label);
    end(VK_NULL_HANDLE);

    VkDebugUtilsObjectNameInfoEXT name{};
    name.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    name.pObjectName = "shim-test";

    const std::array<unsigned char, 1> tag_data{0x5a};
    VkDebugUtilsObjectTagInfoEXT tag{};
    tag.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT;
    tag.tagSize = tag_data.size();
    tag.pTag = tag_data.data();

    const bool ok = set_name(VK_NULL_HANDLE, &name) == VK_SUCCESS && set_tag(VK_NULL_HANDLE, &tag) == VK_SUCCESS;
    std::printf("[shim-test] debug-utils exports, resolver, prototypes, no-ops -> %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
