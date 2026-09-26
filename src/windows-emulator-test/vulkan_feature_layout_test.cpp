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

    TEST(VulkanFeatureLayoutTest, DescriptorBufferPropertiesUseFixedWidthWireFields)
    {
        gpu_bridge::descriptor_buffer_properties_wire wire{};
        for (size_t i = 0; i < wire.size(); ++i)
        {
            wire[i] = i + 1;
        }

        VkPhysicalDeviceDescriptorBufferPropertiesEXT properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;
        properties.pNext = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));
        gpu_bridge::decode_descriptor_buffer_properties(wire, properties);

        EXPECT_EQ(properties.sType, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT);
        EXPECT_EQ(properties.pNext, reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234)));
        EXPECT_EQ(properties.combinedImageSamplerDescriptorSingleArray, 1u);
        EXPECT_EQ(properties.descriptorBufferOffsetAlignment, 4u);
        EXPECT_EQ(properties.bufferCaptureReplayDescriptorDataSize, 10u);
        EXPECT_EQ(properties.accelerationStructureDescriptorSize, 28u);
        EXPECT_EQ(properties.maxSamplerDescriptorBufferRange, 29u);
        EXPECT_EQ(properties.descriptorBufferAddressSpaceSize, 33u);
        EXPECT_EQ(gpu_bridge::encode_descriptor_buffer_properties(properties), wire);
    }

    TEST(VulkanFeatureLayoutTest, DescriptorBufferSizeFieldsNarrowWithoutWrappingOnWow64)
    {
        constexpr uint64_t too_large_for_wow64 = static_cast<uint64_t>(UINT32_MAX) + 1;
        EXPECT_EQ(gpu_bridge::narrow_descriptor_buffer_property_size<uint32_t>(too_large_for_wow64), UINT32_MAX);
        EXPECT_EQ(gpu_bridge::narrow_descriptor_buffer_property_size<uint64_t>(too_large_for_wow64), too_large_for_wow64);

        gpu_bridge::descriptor_buffer_properties_wire wire{};
        wire[9] = UINT64_MAX;
        wire[14] = too_large_for_wow64;
        wire[32] = UINT64_MAX;
        VkPhysicalDeviceDescriptorBufferPropertiesEXT properties{};
        gpu_bridge::decode_descriptor_buffer_properties(wire, properties);
        EXPECT_EQ(properties.bufferCaptureReplayDescriptorDataSize, SIZE_MAX);
        EXPECT_EQ(properties.samplerDescriptorSize, gpu_bridge::narrow_descriptor_buffer_property_size<size_t>(too_large_for_wow64));
        EXPECT_EQ(properties.descriptorBufferAddressSpaceSize, UINT64_MAX);
        EXPECT_EQ(gpu_bridge::property_struct_size(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT), 0u);
    }

    TEST(VulkanFeatureLayoutTest, NonFeatureAndUnknownTypesAreNotCopiedAsFeatureBytes)
    {
        EXPECT_EQ(gpu_bridge::feature_body_size(VK_STRUCTURE_TYPE_APPLICATION_INFO), 0u);
        EXPECT_EQ(gpu_bridge::feature_struct_size(static_cast<VkStructureType>(0x7ffffffe)), 0u);
    }
}
