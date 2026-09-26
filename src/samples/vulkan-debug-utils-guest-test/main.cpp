#include <windows.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace
{
    struct callback_state
    {
        DWORD thread_id{};
        unsigned matching_messages{};
        bool correct_arguments{true};
    };

    VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data)
    {
        auto* state = static_cast<callback_state*>(user_data);
        if (!state || !data)
        {
            return VK_FALSE;
        }
        if (!data->pMessageIdName || std::strcmp(data->pMessageIdName, "sogen-public-debug-utils-probe") != 0)
        {
            return VK_FALSE;
        }
        ++state->matching_messages;
        state->correct_arguments &= GetCurrentThreadId() == state->thread_id && severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT &&
                                    type == VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT && data->messageIdNumber == 0x527 &&
                                    data->pMessage && std::strcmp(data->pMessage, "guest-callback-through-public-shim") == 0;
        std::printf("[debug-utils-guest] callback thread=%lu expected=%lu severity=0x%x type=0x%x id=%d text=%s\n", GetCurrentThreadId(),
                    state->thread_id, severity, type, data->messageIdNumber, data->pMessage ? data->pMessage : "<null>");
        return VK_FALSE;
    }

    template <typename Fn>
    Fn load(HMODULE module, const char* name)
    {
        return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    }
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "vulkan-shim.dll";
    HMODULE module = LoadLibraryA(path);
    if (!module)
    {
        std::printf("[debug-utils-guest] LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    const auto get = load<PFN_vkGetInstanceProcAddr>(module, "vkGetInstanceProcAddr");
    const auto create_instance = load<PFN_vkCreateInstance>(module, "vkCreateInstance");
    const auto destroy_instance = load<PFN_vkDestroyInstance>(module, "vkDestroyInstance");
    const auto create_messenger = load<PFN_vkCreateDebugUtilsMessengerEXT>(module, "vkCreateDebugUtilsMessengerEXT");
    const auto destroy_messenger = load<PFN_vkDestroyDebugUtilsMessengerEXT>(module, "vkDestroyDebugUtilsMessengerEXT");
    const auto submit = load<PFN_vkSubmitDebugUtilsMessageEXT>(module, "vkSubmitDebugUtilsMessageEXT");
    if (!get || !create_instance || !destroy_instance || !create_messenger || !destroy_messenger || !submit)
    {
        std::printf("[debug-utils-guest] public Vulkan exports missing\n");
        return 2;
    }
    const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "sogen-debug-utils-public-guest-test";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = 1;
    instance_info.ppEnabledExtensionNames = &extension;
    VkInstance instance{};
    const VkResult created = create_instance(&instance_info, nullptr, &instance);
    std::printf("[debug-utils-guest] vkCreateInstance(VK_EXT_debug_utils)=%d instance=%p\n", created, static_cast<void*>(instance));
    if (created != VK_SUCCESS || !instance)
    {
        return 3;
    }
    if (get(instance, "vkCreateDebugUtilsMessengerEXT") != reinterpret_cast<PFN_vkVoidFunction>(create_messenger) ||
        get(instance, "vkSubmitDebugUtilsMessageEXT") != reinterpret_cast<PFN_vkVoidFunction>(submit))
    {
        std::printf("[debug-utils-guest] instance procedure lookup mismatch\n");
        destroy_instance(instance, nullptr);
        return 4;
    }
    callback_state state{.thread_id = GetCurrentThreadId()};
    VkDebugUtilsMessengerCreateInfoEXT messenger_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    messenger_info.pfnUserCallback = on_message;
    messenger_info.pUserData = &state;
    VkDebugUtilsMessengerEXT messenger{};
    const VkResult registered = create_messenger(instance, &messenger_info, nullptr, &messenger);
    std::printf("[debug-utils-guest] vkCreateDebugUtilsMessengerEXT=%d messenger=0x%llx\n", registered,
                static_cast<unsigned long long>(std::bit_cast<uint64_t>(messenger)));
    if (registered != VK_SUCCESS || !messenger)
    {
        destroy_instance(instance, nullptr);
        return 5;
    }
    VkDebugUtilsMessengerCallbackDataEXT message{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    message.pMessageIdName = "sogen-public-debug-utils-probe";
    message.messageIdNumber = 0x527;
    message.pMessage = "guest-callback-through-public-shim";
    submit(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &message);
    const bool delivered = state.matching_messages == 1 && state.correct_arguments;
    std::printf("[debug-utils-guest] callback-delivered=%u arguments-and-thread=%u\n", state.matching_messages,
                state.correct_arguments ? 1u : 0u);
    destroy_messenger(instance, messenger, nullptr);
    submit(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &message);
    const bool stopped = state.matching_messages == 1;
    std::printf("[debug-utils-guest] callback-after-destroy=%s\n", stopped ? "none" : "UNEXPECTED");
    destroy_instance(instance, nullptr);
    if (!delivered || !stopped)
    {
        return 6;
    }
    std::printf("[debug-utils-guest] PASS\n");
    return 0;
}
