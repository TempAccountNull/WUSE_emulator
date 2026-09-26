#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>
#include <vk_debug_utils_callback_pump.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace sogen::test
{
    namespace wire = gpu_bridge::debug_utils_wire;

    namespace
    {
        struct native_loader
        {
            HMODULE module{};
            VkInstance instance{};
            VkDebugUtilsMessengerEXT messenger{};
            PFN_vkDestroyInstance destroy_instance{};
            PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger{};

            ~native_loader()
            {
                if (messenger && destroy_messenger)
                {
                    destroy_messenger(instance, messenger, nullptr);
                }
                if (instance && destroy_instance)
                {
                    destroy_instance(instance, nullptr);
                }
                if (module)
                {
                    FreeLibrary(module);
                }
            }
        };

        VkDebugUtilsMessengerCallbackDataEXT message_data(const char* id, const char* message)
        {
            VkDebugUtilsMessengerCallbackDataEXT data{};
            data.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
            data.pMessageIdName = id;
            data.pMessage = message;
            return data;
        }
    }

    TEST(VulkanDebugUtilsCallbackPumpTest, NativeVulkanCallbackRunsOnOriginatingThreadAndKeepsUserData)
    {
        std::array<wchar_t, MAX_PATH> directory{};
        const UINT directory_size = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        ASSERT_GT(directory_size, 0u);
        ASSERT_LT(directory_size, directory.size());
        const std::wstring loader_path = std::wstring(directory.data(), directory_size) + L"\\vulkan-1.dll";
        native_loader native{};
        native.module = LoadLibraryW(loader_path.c_str());
        if (!native.module)
        {
            GTEST_SKIP() << "Native Vulkan loader unavailable";
        }
        const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(native.module, "vkGetInstanceProcAddr"));
        ASSERT_NE(get_proc, nullptr);
        const auto enumerate_extensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            get_proc(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
        const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get_proc(VK_NULL_HANDLE, "vkCreateInstance"));
        ASSERT_NE(enumerate_extensions, nullptr);
        ASSERT_NE(create_instance, nullptr);
        uint32_t extension_count = 0;
        ASSERT_EQ(enumerate_extensions(nullptr, &extension_count, nullptr), VK_SUCCESS);
        std::vector<VkExtensionProperties> extensions(extension_count);
        ASSERT_EQ(enumerate_extensions(nullptr, &extension_count, extensions.data()), VK_SUCCESS);
        if (std::none_of(extensions.begin(), extensions.end(), [](const auto& extension) {
                return std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
            }))
        {
            GTEST_SKIP() << "Native VK_EXT_debug_utils unavailable";
        }

        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.apiVersion = VK_API_VERSION_1_0;
        const char* extension_name = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &application;
        instance_info.enabledExtensionCount = 1;
        instance_info.ppEnabledExtensionNames = &extension_name;
        ASSERT_EQ(create_instance(&instance_info, nullptr, &native.instance), VK_SUCCESS);
        native.destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get_proc(native.instance, "vkDestroyInstance"));
        native.destroy_messenger =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(get_proc(native.instance, "vkDestroyDebugUtilsMessengerEXT"));
        const auto create_messenger =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(get_proc(native.instance, "vkCreateDebugUtilsMessengerEXT"));
        const auto submit_message =
            reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(get_proc(native.instance, "vkSubmitDebugUtilsMessageEXT"));
        ASSERT_NE(native.destroy_instance, nullptr);
        ASSERT_NE(native.destroy_messenger, nullptr);
        ASSERT_NE(create_messenger, nullptr);
        ASSERT_NE(submit_message, nullptr);

        struct marker
        {
            uint64_t value;
        } user_data{0x1122334455667788ull};

        const auto origin_id = std::this_thread::get_id();
        std::thread::id callback_thread{};
        std::thread::id worker_thread{};
        void* received_user_data = nullptr;
        std::string received_id;
        std::string received_message;
        std::string received_label;
        std::string received_object;
        uint32_t callback_count = 0;
        wire::callback_pump pump(
            [&](VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                const VkDebugUtilsMessengerCallbackDataEXT& data, void* context) {
                callback_thread = std::this_thread::get_id();
                received_user_data = context;
                ++callback_count;
                if (severity != VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT || types != VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT ||
                    !data.pMessageIdName || !data.pMessage || data.queueLabelCount != 1 || data.objectCount != 1)
                {
                    throw std::runtime_error("unexpected native debug-utils callback fields");
                }
                received_id = data.pMessageIdName;
                received_message = data.pMessage;
                received_label = data.pQueueLabels[0].pLabelName;
                received_object = data.pObjects[0].pObjectName;
                return VK_FALSE;
            },
            &user_data);

        VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
        messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        messenger_info.pfnUserCallback = wire::callback_pump::native_callback;
        messenger_info.pUserData = &pump;
        ASSERT_EQ(create_messenger(native.instance, &messenger_info, nullptr, &native.messenger), VK_SUCCESS);

        VkDebugUtilsLabelEXT label{};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = "worker-label";
        VkDebugUtilsObjectNameInfoEXT object{};
        object.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        object.objectType = VK_OBJECT_TYPE_INSTANCE;
        object.objectHandle = reinterpret_cast<uint64_t>(native.instance);
        object.pObjectName = "worker-instance";
        auto data = message_data("pump-fixture", "native operation resumed");
        data.queueLabelCount = 1;
        data.pQueueLabels = &label;
        data.objectCount = 1;
        data.pObjects = &object;

        bool resumed = false;
        ASSERT_NO_THROW(pump.run([&] {
            worker_thread = std::this_thread::get_id();
            submit_message(native.instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
                           &data);
            resumed = true;
        }));
        EXPECT_TRUE(resumed);
        EXPECT_NE(worker_thread, origin_id);
        EXPECT_EQ(callback_thread, origin_id);
        EXPECT_EQ(received_user_data, &user_data);
        EXPECT_EQ(user_data.value, 0x1122334455667788ull);
        EXPECT_EQ(callback_count, 1u);
        EXPECT_EQ(received_id, "pump-fixture");
        EXPECT_EQ(received_message, "native operation resumed");
        EXPECT_EQ(received_label, "worker-label");
        EXPECT_EQ(received_object, "worker-instance");
        EXPECT_EQ(pump.failure_count(), 0u) << pump.first_failure();
    }

    TEST(VulkanDebugUtilsCallbackPumpTest, ReturnsCallbackBoolToWaitingWorker)
    {
        const auto origin_id = std::this_thread::get_id();
        std::thread::id invoked_on{};
        int token = 7;
        wire::callback_pump pump(
            [&](VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT& data,
                void* context) {
                invoked_on = std::this_thread::get_id();
                EXPECT_EQ(context, &token);
                EXPECT_STREQ(data.pMessage, "callback result");
                return VK_TRUE;
            },
            &token);
        VkBool32 native_result = VK_FALSE;
        const auto data = message_data("result", "callback result");
        pump.run([&] {
            native_result =
                pump.relay(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, data);
        });
        EXPECT_EQ(native_result, VK_TRUE);
        EXPECT_EQ(invoked_on, origin_id);
        EXPECT_EQ(pump.failure_count(), 0u);
    }

    TEST(VulkanDebugUtilsCallbackPumpTest, OversizedMessageFailsClosedWithoutCallingOrigin)
    {
        uint32_t calls = 0;
        wire::callback_pump pump(
            [&](VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT&,
                void*) {
                ++calls;
                return VK_TRUE;
            },
            nullptr);
        const std::string oversized(wire::max_message_bytes + 1, 'x');
        const auto data = message_data("oversized", oversized.c_str());
        VkBool32 native_result = VK_TRUE;
        pump.run([&] {
            native_result = pump.relay(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, data);
        });
        EXPECT_EQ(native_result, VK_FALSE);
        EXPECT_EQ(calls, 0u);
        EXPECT_EQ(pump.failure_count(), 1u);
        EXPECT_FALSE(pump.first_failure().empty());
    }

    TEST(VulkanDebugUtilsCallbackPumpTest, TimedOutCallbackResumesWorkerWithFalse)
    {
        wire::callback_pump pump(
            [&](VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT&,
                void*) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                return VK_TRUE;
            },
            nullptr, std::chrono::milliseconds(1));
        const auto data = message_data("timeout", "slow callback");
        VkBool32 native_result = VK_TRUE;
        pump.run([&] {
            native_result = pump.relay(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, data);
        });
        EXPECT_EQ(native_result, VK_FALSE);
        EXPECT_EQ(pump.failure_count(), 1u);
        EXPECT_EQ(pump.first_failure(), "debug-utils callback response timed out");
    }
}
