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

    // None of these commands has a bridge implementation yet. Returning a function pointer
    // would let applications mistake silent no-ops and a fabricated messenger for support.
    constexpr std::array names{
        "vkCmdBeginDebugUtilsLabelEXT", "vkCmdEndDebugUtilsLabelEXT", "vkCmdInsertDebugUtilsLabelEXT",
        "vkQueueBeginDebugUtilsLabelEXT", "vkQueueEndDebugUtilsLabelEXT", "vkQueueInsertDebugUtilsLabelEXT",
        "vkSetDebugUtilsObjectNameEXT", "vkSetDebugUtilsObjectTagEXT", "vkCreateDebugUtilsMessengerEXT",
        "vkDestroyDebugUtilsMessengerEXT", "vkSubmitDebugUtilsMessageEXT",
    };
    for (const char* name : names)
    {
        if (GetProcAddress(module, name))
        {
            std::printf("[shim-test] unsupported debug-utils export %s exposed -> FAIL\n", name);
            return false;
        }
    }

    // The real system loader owns proc-address support when the shim runs outside Sogen.
    if (get_instance_proc(VK_NULL_HANDLE, "vkCreateInstance") != shim_create_instance)
    {
        std::printf("[shim-test] unsupported direct exports hidden; host passthrough -> PASS\n");
        return true;
    }

    for (const char* name : names)
    {
        if (get_instance_proc(VK_NULL_HANDLE, name) || get_device_proc(VK_NULL_HANDLE, name))
        {
            std::printf("[shim-test] unsupported debug-utils entry %s exposed -> FAIL\n", name);
            return false;
        }
    }

    std::printf("[shim-test] unsupported debug-utils entries hidden -> PASS\n");
    return true;
}
