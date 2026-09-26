#include <gtest/gtest.h>

#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <vk_debug_utils_messenger_wire.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace wire = sogen::gpu_bridge::debug_utils_messenger_wire;

    struct callback_capture
    {
        void* expected_user_data{};
        std::thread::id originating_thread;
        std::thread::id callback_thread;
        uint32_t count{};
        bool user_data_matches{};
        std::string name;
        std::string message;
    };

    VKAPI_ATTR VkBool32 VKAPI_CALL native_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data)
    {
        auto* capture = static_cast<callback_capture*>(user_data);
        capture->user_data_matches = capture->expected_user_data == user_data;
        capture->callback_thread = std::this_thread::get_id();
        ++capture->count;
        if (severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT && types == VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT && data &&
            data->pMessageIdName && data->pMessage)
        {
            capture->name = data->pMessageIdName;
            capture->message = data->pMessage;
        }
        return VK_FALSE;
    }

    TEST(VulkanDebugUtilsMessengerWireTest, CreateAndDestroyPreserveLifecycleAndGuestPointerWidths)
    {
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = native_callback;
        info.pUserData = reinterpret_cast<void*>(static_cast<uintptr_t>(0x12345678));
        const auto encoded = wire::marshal_create(0x1122, 0x8877665544332211, info, 8);
        ASSERT_EQ(encoded.size(), wire::header_size);
        auto decoded = wire::decode(encoded);
        EXPECT_EQ(decoded.op, wire::operation::create);
        EXPECT_EQ(decoded.instance_id, 0x1122u);
        EXPECT_EQ(decoded.messenger_id, 0x8877665544332211u);
        EXPECT_EQ(decoded.callback_address, reinterpret_cast<uintptr_t>(native_callback));
        EXPECT_EQ(decoded.user_data, 0x12345678u);
        EXPECT_EQ(decoded.severity, info.messageSeverity);
        EXPECT_EQ(decoded.types, info.messageType);
        EXPECT_EQ(decoded.guest_pointer_bytes, 8u);

        const auto chained = wire::decode(wire::marshal_create(0, 0, info, 8));
        EXPECT_EQ(chained.instance_id, 0u);
        EXPECT_EQ(chained.messenger_id, 0u);
        EXPECT_EQ(chained.flags, wire::flag_instance_chain);
        EXPECT_THROW((void)wire::marshal_create(0, 99, info, 8), std::invalid_argument);
        info.flags = 1;
        EXPECT_THROW((void)wire::marshal_create(0x1122, 44, info, 8), std::invalid_argument);
        info.flags = 0;

        const auto destroyed = wire::decode(wire::marshal_destroy(0x1122, 0x8877665544332211, 4));
        EXPECT_EQ(destroyed.op, wire::operation::destroy);
        EXPECT_EQ(destroyed.messenger_id, decoded.messenger_id);
        EXPECT_EQ(destroyed.guest_pointer_bytes, 4u);

        if (reinterpret_cast<uintptr_t>(native_callback) > UINT32_MAX)
        {
            EXPECT_THROW((void)wire::marshal_create(0x1122, 44, info, 4), std::invalid_argument);
        }
        decoded.callback_address = 0x100000000;
        decoded.guest_pointer_bytes = 4;
        EXPECT_THROW((void)wire::encode(decoded), std::invalid_argument);

        decoded.callback_address = 0x87654321;
        decoded.user_data = 0x10203040;
        const auto wow64 = wire::decode(wire::encode(decoded));
        EXPECT_EQ(wow64.guest_pointer_bytes, 4u);
        EXPECT_EQ(wow64.callback_address, 0x87654321u);
        EXPECT_EQ(wow64.user_data, 0x10203040u);
        EXPECT_EQ(wow64.messenger_id, 0x8877665544332211u);
    }

    TEST(VulkanDebugUtilsMessengerWireTest, SubmitDeepCopiesMessageLabelsAndObjects)
    {
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = "queue-label";
        label.color[0] = 0.75f;
        VkDebugUtilsObjectNameInfoEXT object{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        object.objectType = VK_OBJECT_TYPE_BUFFER;
        object.objectHandle = 0x8877665544332211;
        object.pObjectName = "buffer-name";
        VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
        data.pMessageIdName = "wire-id";
        data.messageIdNumber = 72;
        data.pMessage = "wire-message";
        data.queueLabelCount = 1;
        data.pQueueLabels = &label;
        data.objectCount = 1;
        data.pObjects = &object;
        auto encoded = wire::marshal_submit(0x1234, 4, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                                            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, data);
        label.pLabelName = "changed";
        object.pObjectName = "changed";
        data.pMessage = "changed";
        const auto decoded = wire::decode(encoded);
        ASSERT_NE(decoded.message, nullptr);
        EXPECT_EQ(decoded.op, wire::operation::submit);
        EXPECT_EQ(decoded.instance_id, 0x1234u);
        EXPECT_EQ(decoded.message->severity, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT);
        EXPECT_EQ(decoded.message->types, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT);
        EXPECT_STREQ(decoded.message->data.pMessageIdName, "wire-id");
        EXPECT_STREQ(decoded.message->data.pMessage, "wire-message");
        EXPECT_EQ(decoded.message->data.messageIdNumber, 72);
        EXPECT_STREQ(decoded.message->data.pQueueLabels[0].pLabelName, "queue-label");
        EXPECT_EQ(decoded.message->data.pQueueLabels[0].color[0], 0.75f);
        EXPECT_EQ(decoded.message->data.pObjects[0].objectHandle, 0x8877665544332211u);
        EXPECT_STREQ(decoded.message->data.pObjects[0].pObjectName, "buffer-name");

        encoded.pop_back();
        EXPECT_THROW((void)wire::decode(encoded), std::invalid_argument);
        encoded = wire::marshal_submit(0x1234, 4, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
                                       data);
        encoded[12] = std::byte{3};
        EXPECT_THROW((void)wire::decode(encoded), std::invalid_argument);
    }

    TEST(VulkanDebugUtilsMessengerWireTest, NativeAmdMessengerCreateSubmitDestroyUsesOriginatingThread)
    {
        std::array<wchar_t, MAX_PATH> path{};
        const auto length = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
        ASSERT_GT(length, 0u);
        ASSERT_LT(length + 14, path.size());
        wcscat_s(path.data(), path.size(), L"\\vulkan-1.dll");
        HMODULE loader = LoadLibraryW(path.data());
        ASSERT_NE(loader, nullptr);

        struct loader_guard
        {
            HMODULE handle;

            ~loader_guard()
            {
                FreeLibrary(handle);
            }
        } unload{loader};

        const auto get =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(loader, "vkGetInstanceProcAddr")));
        ASSERT_NE(get, nullptr);
        const auto extensions =
            reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(get(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
        const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
        ASSERT_NE(extensions, nullptr);
        ASSERT_NE(create_instance, nullptr);
        uint32_t extension_count = 0;
        ASSERT_EQ(extensions(nullptr, &extension_count, nullptr), VK_SUCCESS);
        std::vector<VkExtensionProperties> available(extension_count);
        ASSERT_EQ(extensions(nullptr, &extension_count, available.data()), VK_SUCCESS);
        if (std::none_of(available.begin(), available.end(),
                         [](const auto& entry) { return std::strcmp(entry.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0; }))
        {
            GTEST_SKIP() << "Native VK_EXT_debug_utils unavailable";
        }
        const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.enabledExtensionCount = 1;
        instance_info.ppEnabledExtensionNames = &extension;
        VkInstance vk_instance{};
        ASSERT_EQ(create_instance(&instance_info, nullptr, &vk_instance), VK_SUCCESS);
        const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get(vk_instance, "vkDestroyInstance"));
        ASSERT_NE(destroy_instance, nullptr);

        struct instance_guard
        {
            VkInstance handle;
            PFN_vkDestroyInstance destroy;

            ~instance_guard()
            {
                destroy(handle, nullptr);
            }
        } destroy{vk_instance, destroy_instance};

        const auto enumerate_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(vk_instance, "vkEnumeratePhysicalDevices"));
        const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(vk_instance, "vkGetPhysicalDeviceProperties"));
        ASSERT_NE(enumerate_devices, nullptr);
        ASSERT_NE(properties, nullptr);
        uint32_t device_count = 0;
        ASSERT_EQ(enumerate_devices(vk_instance, &device_count, nullptr), VK_SUCCESS);
        std::vector<VkPhysicalDevice> devices(device_count);
        ASSERT_EQ(enumerate_devices(vk_instance, &device_count, devices.data()), VK_SUCCESS);
        const bool amd = std::any_of(devices.begin(), devices.end(), [&](auto device) {
            VkPhysicalDeviceProperties value{};
            properties(device, &value);
            return value.vendorID == 0x1002;
        });
        if (!amd)
        {
            GTEST_SKIP() << "No AMD Vulkan physical device";
        }

        const auto create_messenger =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(get(vk_instance, "vkCreateDebugUtilsMessengerEXT"));
        const auto destroy_messenger =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(get(vk_instance, "vkDestroyDebugUtilsMessengerEXT"));
        const auto submit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(get(vk_instance, "vkSubmitDebugUtilsMessageEXT"));
        ASSERT_NE(create_messenger, nullptr);
        ASSERT_NE(destroy_messenger, nullptr);
        ASSERT_NE(submit, nullptr);

        callback_capture capture{};
        capture.expected_user_data = &capture;
        capture.originating_thread = std::this_thread::get_id();
        VkDebugUtilsMessengerCreateInfoEXT guest_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        guest_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        guest_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        guest_info.pfnUserCallback = native_callback;
        guest_info.pUserData = &capture;
        const auto create_wire = wire::decode(wire::marshal_create(reinterpret_cast<uintptr_t>(vk_instance), 17, guest_info, 8));
        VkDebugUtilsMessengerCreateInfoEXT native_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        native_info.messageSeverity = create_wire.severity;
        native_info.messageType = create_wire.types;
        native_info.pfnUserCallback = native_callback;
        native_info.pUserData = reinterpret_cast<void*>(static_cast<uintptr_t>(create_wire.user_data));
        VkDebugUtilsMessengerEXT messenger{};
        ASSERT_EQ(create_messenger(vk_instance, &native_info, nullptr, &messenger), VK_SUCCESS);

        struct messenger_guard
        {
            VkInstance instance;
            VkDebugUtilsMessengerEXT handle;
            PFN_vkDestroyDebugUtilsMessengerEXT destroy;

            ~messenger_guard()
            {
                if (handle)
                {
                    destroy(instance, handle, nullptr);
                }
            }
        } destroy_native{vk_instance, messenger, destroy_messenger};

        VkDebugUtilsMessengerCallbackDataEXT guest_message{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
        guest_message.pMessageIdName = "wire-lifecycle";
        guest_message.pMessage = "AMD debug-utils submit";
        const auto submit_wire =
            wire::decode(wire::marshal_submit(reinterpret_cast<uintptr_t>(vk_instance), 8, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                                              VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, guest_message));
        ASSERT_NE(submit_wire.message, nullptr);
        submit(vk_instance, submit_wire.message->severity, submit_wire.message->types, &submit_wire.message->data);
        EXPECT_EQ(capture.count, 1u);
        EXPECT_TRUE(capture.user_data_matches);
        EXPECT_EQ(capture.callback_thread, capture.originating_thread);
        EXPECT_EQ(capture.name, "wire-lifecycle");
        EXPECT_EQ(capture.message, "AMD debug-utils submit");

        const auto destroy_wire = wire::decode(wire::marshal_destroy(reinterpret_cast<uintptr_t>(vk_instance), 17, 8));
        EXPECT_EQ(destroy_wire.messenger_id, create_wire.messenger_id);
        destroy_messenger(vk_instance, messenger, nullptr);
        destroy_native.handle = VK_NULL_HANDLE;
        submit(vk_instance, submit_wire.message->severity, submit_wire.message->types, &submit_wire.message->data);
        EXPECT_EQ(capture.count, 1u);
    }
} // namespace
