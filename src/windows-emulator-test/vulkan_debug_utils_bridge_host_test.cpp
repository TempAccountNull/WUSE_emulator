#include <gtest/gtest.h>

#include <devices/vulkan_host.hpp>
#include <vk_debug_utils_messenger_wire.hpp>
#include <vk_debug_utils_wire.hpp>

#include <vulkan/vulkan_core.h>

#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using sogen::vulkan_host;
    namespace messenger_wire = sogen::gpu_bridge::debug_utils_messenger_wire;
    namespace callback_wire = sogen::gpu_bridge::debug_utils_wire;

    VKAPI_ATTR VkBool32 VKAPI_CALL never_direct_callback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                          VkDebugUtilsMessageTypeFlagsEXT,
                                                          const VkDebugUtilsMessengerCallbackDataEXT*, void*)
    {
        ADD_FAILURE() << "The host must relay to the guest, not call a guest function pointer";
        return VK_FALSE;
    }

    TEST(VulkanDebugUtilsBridgeHostTest, AmdNativeSubmitRelaysDeepCopiedCallbackToGuestThread)
    {
        vulkan_host host;
        if (!host.available() || !host.debug_utils_available())
            GTEST_SKIP() << "Native Vulkan debug-utils instance extension unavailable";

        std::thread::id origin = std::this_thread::get_id();
        std::vector<vulkan_host::debug_utils_delivery> deliveries;
        host.set_debug_utils_sink([&](vulkan_host::debug_utils_delivery delivery) {
            EXPECT_EQ(std::this_thread::get_id(), origin);
            deliveries.push_back(std::move(delivery));
            return true;
        });
        uint64_t instance{};
        ASSERT_EQ(host.create_instance(instance, true), VK_SUCCESS);
        ASSERT_NE(instance, 0u);

        uint32_t count{};
        ASSERT_EQ(host.enumerate_physical_devices(instance, {}, count), VK_SUCCESS);
        std::vector<uint64_t> devices(count);
        ASSERT_EQ(host.enumerate_physical_devices(instance, devices, count), VK_SUCCESS);
        bool amd = false;
        for (uint64_t device : devices)
        {
            VkPhysicalDeviceProperties properties{};
            if (host.get_physical_device_properties(device, &properties, sizeof(properties), false) == VK_SUCCESS &&
                properties.vendorID == 0x1002)
                amd = true;
        }
        if (!amd)
        {
            host.destroy_instance(instance);
            GTEST_SKIP() << "No AMD Vulkan device";
        }

        VkDebugUtilsMessengerCreateInfoEXT create_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        create_info.pfnUserCallback = never_direct_callback;
        create_info.pUserData = reinterpret_cast<void*>(0x50607080);
        constexpr uint64_t guest_messenger_id = 0x8877665544332211;
        const auto create = messenger_wire::marshal_create(instance, guest_messenger_id, create_info, 8);
        uint64_t messenger{};
        ASSERT_EQ(host.debug_utils_messenger(create, messenger), VK_SUCCESS);
        ASSERT_EQ(messenger, guest_messenger_id);

        VkDebugUtilsMessengerCallbackDataEXT message{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
        message.pMessageIdName = "sogen-native-amd-relay";
        message.messageIdNumber = 734;
        message.pMessage = "relayed-on-guest-thread";
        const auto submit = messenger_wire::marshal_submit(instance, 8, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                                                            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, message);
        const uint32_t previous = vulkan_host::exchange_debug_utils_guest_thread(80);
        uint64_t ignored{};
        const auto result = host.debug_utils_messenger(submit, ignored);
        vulkan_host::exchange_debug_utils_guest_thread(previous);
        EXPECT_EQ(result, VK_SUCCESS);
        ASSERT_EQ(deliveries.size(), 1u);
        EXPECT_EQ(deliveries[0].guest_thread_id, 80u);
        EXPECT_EQ(deliveries[0].instance_id, instance);
        EXPECT_EQ(deliveries[0].callback_address, reinterpret_cast<uintptr_t>(never_direct_callback));
        EXPECT_EQ(deliveries[0].user_data, 0x50607080u);
        const auto decoded = callback_wire::decode(deliveries[0].packet);
        EXPECT_STREQ(decoded->data.pMessageIdName, "sogen-native-amd-relay");
        EXPECT_STREQ(decoded->data.pMessage, "relayed-on-guest-thread");
        EXPECT_EQ(decoded->data.messageIdNumber, 734);

        const auto destroy = messenger_wire::marshal_destroy(instance, messenger, 8);
        EXPECT_EQ(host.debug_utils_messenger(destroy, ignored), VK_SUCCESS);
        EXPECT_EQ(host.debug_utils_messenger(submit, ignored), VK_SUCCESS);
        EXPECT_EQ(deliveries.size(), 1u);
        host.destroy_instance(instance);
    }
}
