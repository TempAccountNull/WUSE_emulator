#include <gtest/gtest.h>

#include <vk_debug_utils_callback_relay.hpp>
#include <vk_debug_utils_wire.hpp>

#include <cstring>
#include <string>

namespace
{
    namespace relay = sogen::gpu_bridge::debug_utils_callback_relay;
    namespace wire = sogen::gpu_bridge::debug_utils_wire;

    relay::delivery make_delivery(uint64_t instance, const char* message)
    {
        VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
        data.pMessage = message;
        data.pMessageIdName = "relay-id";
        return {.instance_id = instance,
                .callback_address = 0x10203040,
                .user_data = 0x50607080,
                .guest_pointer_bytes = 4,
                .packet = wire::encode(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, data)};
    }

    TEST(VulkanDebugUtilsCallbackRelayTest, IsolatesGuestThreadsAndDeepCopiesNativeCallbackData)
    {
        relay::queue pending;
        char first[] = "original message";
        ASSERT_TRUE(pending.push(80, make_delivery(7, first)));
        std::strcpy(first, "changed message ");
        ASSERT_TRUE(pending.push(84, make_delivery(7, "other thread")));
        ASSERT_TRUE(pending.push(80, make_delivery(9, "second message")));
        EXPECT_EQ(pending.packet_count(), 3u);

        relay::delivery entry;
        ASSERT_TRUE(pending.pop(80, entry));
        EXPECT_EQ(entry.callback_address, 0x10203040u);
        EXPECT_EQ(entry.user_data, 0x50607080u);
        EXPECT_EQ(entry.guest_pointer_bytes, 4u);
        EXPECT_STREQ(wire::decode(entry.packet)->data.pMessage, "original message");
        ASSERT_TRUE(pending.pop(80, entry));
        EXPECT_STREQ(wire::decode(entry.packet)->data.pMessage, "second message");
        EXPECT_FALSE(pending.pop(80, entry));
        ASSERT_TRUE(pending.pop(84, entry));
        EXPECT_STREQ(wire::decode(entry.packet)->data.pMessage, "other thread");
        EXPECT_EQ(pending.packet_count(), 0u);
    }

    TEST(VulkanDebugUtilsCallbackRelayTest, HasBoundedCapacityAndDiscardsDestroyedInstance)
    {
        relay::queue pending;
        EXPECT_FALSE(pending.push(0, make_delivery(7, "invalid thread")));
        for (size_t i = 0; i < relay::queue::max_queued_packets; ++i)
        {
            ASSERT_TRUE(pending.push(80, make_delivery(i % 2 ? 7 : 9, "message")));
        }
        EXPECT_FALSE(pending.push(80, make_delivery(7, "overflow")));
        pending.discard_instance(7);
        EXPECT_EQ(pending.packet_count(), relay::queue::max_queued_packets / 2);
        ASSERT_TRUE(pending.push(80, make_delivery(11, "new message")));
        EXPECT_EQ(pending.packet_count(), relay::queue::max_queued_packets / 2 + 1);
    }
}
