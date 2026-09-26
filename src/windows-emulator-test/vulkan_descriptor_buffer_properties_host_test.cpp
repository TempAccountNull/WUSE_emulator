#include <gtest/gtest.h>
#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include <vk_feature_chain.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace sogen::test
{
    TEST(VulkanDescriptorBufferPropertiesHostTest, QueriesNativePropertiesThroughFixedWidthWire)
    {
        vulkan_host host;
        if (!host.available())
        {
            GTEST_SKIP() << "Host Vulkan driver unavailable";
        }

        uint64_t instance = 0;
        ASSERT_EQ(host.create_instance(instance), VK_SUCCESS);

        struct instance_cleanup
        {
            vulkan_host& host;
            uint64_t instance;

            ~instance_cleanup()
            {
                host.destroy_instance(instance);
            }
        } cleanup{host, instance};

        std::array<uint64_t, 32> devices{};
        uint32_t count = 0;
        ASSERT_EQ(host.enumerate_physical_devices(instance, devices, count), VK_SUCCESS);
        uint64_t amd_device = 0;
        for (uint32_t i = 0; i < std::min(count, static_cast<uint32_t>(devices.size())); ++i)
        {
            VkPhysicalDeviceProperties properties{};
            ASSERT_EQ(host.get_physical_device_properties(devices[i], &properties, sizeof(properties), false), VK_SUCCESS);
            if (properties.vendorID == 0x1002)
            {
                amd_device = devices[i];
                break;
            }
        }
        if (!amd_device)
        {
            GTEST_SKIP() << "AMD Vulkan device unavailable";
        }

        gpu_bridge::feature_chain_record request{.s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT,
                                                 .body_size = static_cast<uint32_t>(sizeof(gpu_bridge::descriptor_buffer_properties_wire))};
        std::vector<std::byte> response;
        ASSERT_EQ(host.get_physical_device_properties2(amd_device, &request, sizeof(request), 1, response), VK_SUCCESS);
        ASSERT_EQ(response.size(), sizeof(request) + sizeof(gpu_bridge::descriptor_buffer_properties_wire));
        gpu_bridge::feature_chain_record record{};
        std::memcpy(&record, response.data(), sizeof(record));
        EXPECT_EQ(record.s_type, request.s_type);
        EXPECT_EQ(record.body_size, request.body_size);

        gpu_bridge::descriptor_buffer_properties_wire wire{};
        std::memcpy(wire.data(), response.data() + sizeof(record), sizeof(wire));
        VkPhysicalDeviceDescriptorBufferPropertiesEXT properties{};
        gpu_bridge::decode_descriptor_buffer_properties(wire, properties);
        EXPECT_GT(properties.descriptorBufferOffsetAlignment, 0u);
        EXPECT_EQ(properties.descriptorBufferOffsetAlignment & (properties.descriptorBufferOffsetAlignment - 1), 0u);
        EXPECT_GT(properties.samplerDescriptorSize, 0u);
        EXPECT_GT(properties.maxSamplerDescriptorBufferRange, 0u);
        EXPECT_EQ(gpu_bridge::encode_descriptor_buffer_properties(properties), wire);

        --request.body_size;
        response.clear();
        ASSERT_EQ(host.get_physical_device_properties2(amd_device, &request, sizeof(request), 1, response), VK_SUCCESS);
        ASSERT_EQ(response.size(), sizeof(record));
        std::memcpy(&record, response.data(), sizeof(record));
        EXPECT_EQ(record.body_size, 0u);
    }
}
