#include <gtest/gtest.h>
#include <vk_feature_chain.hpp>
#include <array>
#include <cstring>

namespace sogen::test
{
    TEST(VulkanFeatureLayoutTest, BaseFeaturesExcludeX64TailPadding)
    {
        EXPECT_EQ(gpu_bridge::feature_body_size(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2), sizeof(VkPhysicalDeviceFeatures));
        VkPhysicalDeviceFeatures features{};
        features.inheritedQueries = VK_TRUE;
        std::array<unsigned char, sizeof(VkPhysicalDeviceFeatures) + 8> packet{};
        packet.fill(0xa5);
        std::memcpy(packet.data(), &features, gpu_bridge::feature_body_size(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2));
        for (size_t i = sizeof(VkPhysicalDeviceFeatures); i < packet.size(); ++i)
        {
            EXPECT_EQ(packet[i], 0xa5);
        }
        VkBool32 last{};
        std::memcpy(&last, packet.data() + offsetof(VkPhysicalDeviceFeatures, inheritedQueries), sizeof(last));
        EXPECT_EQ(last, VK_TRUE);
    }

    TEST(VulkanFeatureLayoutTest, SingleBooleanFeaturesHaveFourWireBytesOnBothAbis)
    {
        for (const auto type : {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
                                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,
                                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT})
        {
            EXPECT_EQ(gpu_bridge::feature_body_size(type), 4u);
            EXPECT_GE(gpu_bridge::feature_struct_size(type), gpu_bridge::feature_chain_header_size + 4);
        }
    }

    TEST(VulkanFeatureLayoutTest, Core14AndPromotedAliasesHaveExactFieldSpans)
    {
        EXPECT_EQ(gpu_bridge::feature_body_size(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES),
                  offsetof(VkPhysicalDeviceVulkan14Features, pushDescriptor) + sizeof(VkBool32) -
                      offsetof(VkPhysicalDeviceVulkan14Features, globalPriorityQuery));
        EXPECT_EQ(gpu_bridge::feature_body_size(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES),
                  gpu_bridge::feature_body_size(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR));
        EXPECT_EQ(gpu_bridge::feature_body_size(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT),
                  offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3ShadingRateImageEnable) +
                      sizeof(VkBool32) -
                      offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3TessellationDomainOrigin));
    }

    TEST(VulkanFeatureLayoutTest, NonFeatureAndUnknownTypesAreNotCopiedAsFeatureBytes)
    {
        EXPECT_EQ(gpu_bridge::feature_body_size(VK_STRUCTURE_TYPE_APPLICATION_INFO), 0u);
        EXPECT_EQ(gpu_bridge::feature_struct_size(static_cast<VkStructureType>(0x7ffffffe)), 0u);
    }
}
