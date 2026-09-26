#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>
#include <vk_debug_utils_wire.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace sogen::test
{
    namespace wire = gpu_bridge::debug_utils_wire;

    namespace
    {
        void put_u32(std::vector<std::byte>& bytes, size_t offset, uint32_t value)
        {
            for (unsigned shift = 0; shift < 32; shift += 8)
            {
                bytes[offset + shift / 8] = static_cast<std::byte>((value >> shift) & 0xff);
            }
        }

        VkDebugUtilsMessengerCallbackDataEXT basic_message(const char* id, const char* message)
        {
            VkDebugUtilsMessengerCallbackDataEXT data{};
            data.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
            data.pMessageIdName = id;
            data.messageIdNumber = -39;
            data.pMessage = message;
            return data;
        }

        struct native_callback_capture
        {
            uint32_t calls{};
            std::string error;
            std::unique_ptr<wire::callback_storage> decoded;
        };

        VKAPI_ATTR VkBool32 VKAPI_CALL capture_native_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                               VkDebugUtilsMessageTypeFlagsEXT types,
                                                               const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data)
        {
            auto& capture = *static_cast<native_callback_capture*>(user_data);
            if (!data || !data->pMessageIdName || std::strcmp(data->pMessageIdName, "wire-fixture") != 0)
            {
                return VK_FALSE;
            }
            ++capture.calls;
            try
            {
                capture.decoded = wire::decode(wire::encode(severity, types, *data));
            }
            catch (const std::exception& error)
            {
                capture.error = error.what();
            }
            catch (...)
            {
                capture.error = "unknown callback wire error";
            }
            return VK_FALSE;
        }
    }

    TEST(VulkanDebugUtilsWireTest, DeepCopiesMessagesLabelsObjectsAndTheirNames)
    {
        std::array<char, 16> id_name{"VUID-Example"};
        std::array<char, 32> message{"validation text"};
        std::array<char, 16> queue_name{"queue scope"};
        std::array<char, 16> command_name{"draw scope"};
        std::array<char, 16> object_name{"image one"};
        VkDebugUtilsLabelEXT queue_label{};
        queue_label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        queue_label.pLabelName = queue_name.data();
        queue_label.color[0] = 0.25f;
        queue_label.color[1] = 0.5f;
        queue_label.color[2] = 0.75f;
        queue_label.color[3] = 1.0f;
        VkDebugUtilsLabelEXT command_label{};
        command_label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        command_label.pLabelName = command_name.data();
        command_label.color[3] = 1.0f;
        VkDebugUtilsObjectNameInfoEXT object{};
        object.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        object.objectType = VK_OBJECT_TYPE_IMAGE;
        object.objectHandle = 0x1122334455667788ull;
        object.pObjectName = object_name.data();
        auto data = basic_message(id_name.data(), message.data());
        data.queueLabelCount = 1;
        data.pQueueLabels = &queue_label;
        data.cmdBufLabelCount = 1;
        data.pCmdBufLabels = &command_label;
        data.objectCount = 1;
        data.pObjects = &object;

        auto bytes = wire::encode(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, data);
        id_name[0] = 'X';
        message[0] = 'X';
        queue_name[0] = 'X';
        command_name[0] = 'X';
        object_name[0] = 'X';

        const auto decoded = wire::decode(bytes);
        EXPECT_EQ(decoded->severity, VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
        EXPECT_EQ(decoded->types, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
        EXPECT_EQ(decoded->data.messageIdNumber, -39);
        EXPECT_STREQ(decoded->data.pMessageIdName, "VUID-Example");
        EXPECT_STREQ(decoded->data.pMessage, "validation text");
        ASSERT_EQ(decoded->data.queueLabelCount, 1u);
        ASSERT_EQ(decoded->data.cmdBufLabelCount, 1u);
        ASSERT_EQ(decoded->data.objectCount, 1u);
        EXPECT_STREQ(decoded->data.pQueueLabels[0].pLabelName, "queue scope");
        EXPECT_EQ(decoded->data.pQueueLabels[0].color[1], 0.5f);
        EXPECT_STREQ(decoded->data.pCmdBufLabels[0].pLabelName, "draw scope");
        EXPECT_EQ(decoded->data.pObjects[0].objectType, VK_OBJECT_TYPE_IMAGE);
        EXPECT_EQ(decoded->data.pObjects[0].objectHandle, 0x1122334455667788ull);
        EXPECT_STREQ(decoded->data.pObjects[0].pObjectName, "image one");
        bytes.clear();
        EXPECT_STREQ(decoded->data.pMessage, "validation text");
        EXPECT_STREQ(decoded->data.pObjects[0].pObjectName, "image one");
    }

    TEST(VulkanDebugUtilsWireTest, PreservesOptionalDeviceAddressBindingPayloadAndNullText)
    {
        VkDeviceAddressBindingCallbackDataEXT binding{};
        binding.sType = VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT;
        binding.flags = VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT;
        binding.baseAddress = 0x8877665544332211ull;
        binding.size = 0x12345000;
        binding.bindingType = VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT;
        auto data = basic_message(nullptr, nullptr);
        data.pNext = &binding;
        const auto bytes =
            wire::encode(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT, data);
        const auto decoded = wire::decode(bytes);
        EXPECT_EQ(decoded->data.pMessageIdName, nullptr);
        EXPECT_EQ(decoded->data.pMessage, nullptr);
        ASSERT_NE(decoded->data.pNext, nullptr);
        const auto* restored = static_cast<const VkDeviceAddressBindingCallbackDataEXT*>(decoded->data.pNext);
        EXPECT_EQ(restored->sType, VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT);
        EXPECT_EQ(restored->pNext, nullptr);
        EXPECT_EQ(restored->flags, binding.flags);
        EXPECT_EQ(restored->baseAddress, binding.baseAddress);
        EXPECT_EQ(restored->size, binding.size);
        EXPECT_EQ(restored->bindingType, binding.bindingType);
    }

    TEST(VulkanDebugUtilsWireTest, UsesFixedLittleEndianFieldsAndExplicitLengths)
    {
        auto data = basic_message("ID", "hello");
        const auto bytes = wire::encode(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, data);
        ASSERT_EQ(bytes.size(), wire::header_size + 2 + 5);
        EXPECT_EQ(bytes[0], std::byte{0x56});
        EXPECT_EQ(bytes[1], std::byte{0x42});
        EXPECT_EQ(bytes[2], std::byte{0x44});
        EXPECT_EQ(bytes[3], std::byte{0x55});
        EXPECT_EQ(bytes[4], std::byte{1});
        EXPECT_EQ(bytes[28], std::byte{2});
        EXPECT_EQ(bytes[32], std::byte{5});
        EXPECT_EQ(bytes[wire::header_size], std::byte{'I'});
        EXPECT_EQ(bytes[wire::header_size + 2], std::byte{'h'});
        EXPECT_STREQ(wire::decode(bytes)->data.pMessage, "hello");
    }

    TEST(VulkanDebugUtilsWireTest, RejectsUnsupportedChainsInvalidArraysAndOversizedText)
    {
        auto data = basic_message("ID", "hello");
        VkBaseInStructure unsupported{};
        unsupported.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        data.pNext = &unsupported;
        EXPECT_THROW(wire::encode(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, data),
                     std::invalid_argument);
        data.pNext = nullptr;
        data.queueLabelCount = 1;
        EXPECT_THROW(wire::encode(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, data),
                     std::invalid_argument);
        data.queueLabelCount = 0;
        std::string oversized(wire::max_message_bytes + 1, 'a');
        data.pMessage = oversized.c_str();
        EXPECT_THROW(wire::encode(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, data),
                     std::length_error);
    }

    TEST(VulkanDebugUtilsWireTest, RejectsTruncationCorruptCountsAndUnsupportedPayloadType)
    {
        auto data = basic_message("ID", "message");
        auto bytes = wire::encode(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, data);
        auto truncated = bytes;
        truncated.pop_back();
        EXPECT_THROW(wire::decode(truncated), std::invalid_argument);
        auto count = bytes;
        put_u32(count, 36, wire::max_labels + 1);
        EXPECT_THROW(wire::decode(count), std::invalid_argument);
        auto unsupported = bytes;
        put_u32(unsupported, 48, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        EXPECT_THROW(wire::decode(unsupported), std::invalid_argument);
        auto oversized = std::vector<std::byte>(wire::max_packet_bytes + 1);
        EXPECT_THROW(wire::decode(oversized), std::length_error);
        bytes.push_back(std::byte{0});
        EXPECT_THROW(wire::decode(bytes), std::invalid_argument);
    }

    TEST(VulkanDebugUtilsWireTest, NativeVulkanSubmitDeliversRoundTrippableCallback)
    {
        std::array<wchar_t, MAX_PATH> system_directory{};
        const UINT directory_length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
        ASSERT_GT(directory_length, 0u);
        ASSERT_LT(directory_length, system_directory.size());
        const std::wstring loader_path = std::wstring(system_directory.data(), directory_length) + L"\\vulkan-1.dll";
        HMODULE loader = LoadLibraryW(loader_path.c_str());
        if (!loader)
        {
            GTEST_SKIP() << "Native Vulkan loader unavailable";
        }

        struct loader_cleanup
        {
            HMODULE handle;

            ~loader_cleanup()
            {
                FreeLibrary(handle);
            }
        } unload{loader};

        const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
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
        const bool debug_utils_available = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
        });
        if (!debug_utils_available)
        {
            GTEST_SKIP() << "Native VK_EXT_debug_utils unavailable";
        }

        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.apiVersion = VK_API_VERSION_1_0;
        const char* extension_name = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &application;
        create_info.enabledExtensionCount = 1;
        create_info.ppEnabledExtensionNames = &extension_name;
        VkInstance instance = VK_NULL_HANDLE;
        if (create_instance(&create_info, nullptr, &instance) != VK_SUCCESS)
        {
            GTEST_SKIP() << "Native Vulkan instance with VK_EXT_debug_utils unavailable";
        }
        const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"));
        ASSERT_NE(destroy_instance, nullptr);

        struct instance_cleanup
        {
            VkInstance instance;
            PFN_vkDestroyInstance destroy;

            ~instance_cleanup()
            {
                destroy(instance, nullptr);
            }
        } destroy{instance, destroy_instance};

        const auto create_messenger =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(get_proc(instance, "vkCreateDebugUtilsMessengerEXT"));
        const auto destroy_messenger =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(get_proc(instance, "vkDestroyDebugUtilsMessengerEXT"));
        const auto submit_message = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(get_proc(instance, "vkSubmitDebugUtilsMessageEXT"));
        ASSERT_NE(create_messenger, nullptr);
        ASSERT_NE(destroy_messenger, nullptr);
        ASSERT_NE(submit_message, nullptr);

        native_callback_capture capture{};
        VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
        messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        messenger_info.pfnUserCallback = capture_native_callback;
        messenger_info.pUserData = &capture;
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
        ASSERT_EQ(create_messenger(instance, &messenger_info, nullptr, &messenger), VK_SUCCESS);

        struct messenger_cleanup
        {
            VkInstance instance;
            VkDebugUtilsMessengerEXT messenger;
            PFN_vkDestroyDebugUtilsMessengerEXT destroy;

            ~messenger_cleanup()
            {
                destroy(instance, messenger, nullptr);
            }
        } destroy_debug_messenger{instance, messenger, destroy_messenger};

        VkDebugUtilsLabelEXT label{};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = "native-label";
        label.color[0] = 0.75f;
        VkDebugUtilsObjectNameInfoEXT object{};
        object.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        object.objectType = VK_OBJECT_TYPE_INSTANCE;
        object.objectHandle = reinterpret_cast<uint64_t>(instance);
        object.pObjectName = "native-instance";
        auto data = basic_message("wire-fixture", "native Vulkan callback");
        data.queueLabelCount = 1;
        data.pQueueLabels = &label;
        data.objectCount = 1;
        data.pObjects = &object;
        submit_message(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &data);

        EXPECT_EQ(capture.calls, 1u);
        EXPECT_TRUE(capture.error.empty()) << capture.error;
        ASSERT_NE(capture.decoded, nullptr);
        EXPECT_STREQ(capture.decoded->data.pMessageIdName, "wire-fixture");
        EXPECT_STREQ(capture.decoded->data.pMessage, "native Vulkan callback");
        ASSERT_EQ(capture.decoded->data.queueLabelCount, 1u);
        ASSERT_EQ(capture.decoded->data.objectCount, 1u);
        EXPECT_STREQ(capture.decoded->data.pQueueLabels[0].pLabelName, "native-label");
        EXPECT_EQ(capture.decoded->data.pObjects[0].objectHandle, object.objectHandle);
        EXPECT_STREQ(capture.decoded->data.pObjects[0].pObjectName, "native-instance");
    }
}
