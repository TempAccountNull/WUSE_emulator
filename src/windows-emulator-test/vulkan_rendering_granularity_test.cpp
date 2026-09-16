#include <gtest/gtest.h>
#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include <vk_render_pass.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace sogen::test
{
    namespace area_wire = gpu_bridge::render_pass_wire;

    TEST(VulkanRenderingGranularityWireTest, PreservesAllFieldsWithoutNativePointersOrPadding)
    {
        const std::array<VkFormat, 3> formats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, VK_FORMAT_R16G16B16A16_SFLOAT};
        VkRenderingAreaInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_AREA_INFO;
        info.viewMask = 0x80000003;
        info.colorAttachmentCount = static_cast<uint32_t>(formats.size());
        info.pColorAttachmentFormats = formats.data();
        info.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        info.stencilAttachmentFormat = VK_FORMAT_S8_UINT;
        const auto packet = area_wire::encode(info);
        const std::array<uint32_t, 10> expected{VK_STRUCTURE_TYPE_RENDERING_AREA_INFO,
                                                0,
                                                0x80000003,
                                                3,
                                                1,
                                                VK_FORMAT_R8G8B8A8_UNORM,
                                                VK_FORMAT_UNDEFINED,
                                                VK_FORMAT_R16G16B16A16_SFLOAT,
                                                VK_FORMAT_D32_SFLOAT,
                                                VK_FORMAT_S8_UINT};
        ASSERT_EQ(packet.size(), sizeof(expected));
        EXPECT_EQ(std::memcmp(packet.data(), expected.data(), sizeof(expected)), 0);
        area_wire::reader reader(packet);
        VkRenderingAreaInfo decoded{};
        area_wire::decode(reader, decoded);
        EXPECT_EQ(decoded.viewMask, info.viewMask);
        EXPECT_EQ(decoded.colorAttachmentCount, 3u);
        ASSERT_NE(decoded.pColorAttachmentFormats, nullptr);
        EXPECT_EQ(decoded.pColorAttachmentFormats[2], formats[2]);
        EXPECT_EQ(decoded.depthAttachmentFormat, info.depthAttachmentFormat);
        EXPECT_EQ(decoded.stencilAttachmentFormat, info.stencilAttachmentFormat);
        EXPECT_EQ(area_wire::encode(decoded), packet);
    }

    TEST(VulkanRenderingGranularityWireTest, RejectsMalformedArraysChainsAndTruncatedPackets)
    {
        VkRenderingAreaInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_AREA_INFO;
        info.pColorAttachmentFormats = reinterpret_cast<const VkFormat*>(uintptr_t{1});
        const auto packet = area_wire::encode(info);
        for (size_t size = 0; size < packet.size(); ++size)
        {
            area_wire::reader reader(std::span<const std::byte>(packet.data(), size));
            VkRenderingAreaInfo decoded{};
            EXPECT_THROW(area_wire::decode(reader, decoded), area_wire::error);
        }
        auto trailing = packet;
        trailing.push_back(std::byte{});
        area_wire::reader reader(trailing);
        VkRenderingAreaInfo decoded{};
        EXPECT_THROW(area_wire::decode(reader, decoded), area_wire::error);
        info.pColorAttachmentFormats = nullptr;
        info.colorAttachmentCount = 1;
        EXPECT_THROW(area_wire::encode(info), area_wire::error);
        info.colorAttachmentCount = area_wire::max_elements + 1;
        EXPECT_THROW(area_wire::encode(info), area_wire::error);
        info.colorAttachmentCount = 0;
        const VkBaseInStructure unsupported{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr};
        info.pNext = &unsupported;
        EXPECT_THROW(area_wire::encode(info), area_wire::error);
        info.pNext = nullptr;
        info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        EXPECT_THROW(area_wire::encode(info), area_wire::error);
    }

    TEST(VulkanRenderingGranularityInstanceTest, ReportsNegotiatedVersionOnlyForLiveInstances)
    {
        vulkan_host host;
        if (!host.available())
        {
            GTEST_SKIP() << "Vulkan driver unavailable";
        }
        uint32_t version = UINT32_MAX;
        EXPECT_EQ(host.get_instance_api_version(0, version), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(version, 0u);
        uint64_t instance{};
        ASSERT_EQ(host.create_instance(instance), VK_SUCCESS);
        EXPECT_EQ(host.get_instance_api_version(instance, version), VK_SUCCESS);
        EXPECT_GE(version, VK_API_VERSION_1_0);
        EXPECT_LE(version, VK_API_VERSION_1_3);
        const uint32_t negotiated = version;
        EXPECT_EQ(host.get_instance_api_version(instance, version), VK_SUCCESS);
        EXPECT_EQ(version, negotiated);
        host.destroy_instance(instance);
        version = UINT32_MAX;
        EXPECT_EQ(host.get_instance_api_version(instance, version), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(version, 0u);
    }

    class VulkanRenderingGranularityHostTest : public testing::Test
    {
      protected:
        vulkan_host host;
        uint64_t instance{};
        uint64_t device{};
        uint64_t unextended_device{};

        void SetUp() override
        {
            if (!host.available())
            {
                GTEST_SKIP() << "Vulkan driver unavailable";
            }
            ASSERT_EQ(host.create_instance(instance), VK_SUCCESS);
            uint32_t instance_version{};
            ASSERT_EQ(host.get_instance_api_version(instance, instance_version), VK_SUCCESS);
            if (instance_version < VK_API_VERSION_1_3)
            {
                GTEST_SKIP() << "Native instance API below Vulkan 1.3; this fixture does not enable the pre-1.3 dependency chain";
            }
            std::array<uint64_t, 32> physical{};
            uint32_t count{};
            ASSERT_EQ(host.enumerate_physical_devices(instance, physical, count), VK_SUCCESS);
            if (!count)
            {
                GTEST_SKIP() << "No physical devices";
            }
            VkPhysicalDeviceProperties properties{};
            ASSERT_EQ(host.get_physical_device_properties(physical[0], &properties, sizeof(properties), false), VK_SUCCESS);
            if (properties.apiVersion < VK_API_VERSION_1_3)
            {
                GTEST_SKIP() << "Physical-device API below Vulkan 1.3; maintenance5 also needs dynamic-rendering dependencies";
            }
            ASSERT_EQ(host.enumerate_device_extension_properties(physical[0], nullptr, 0, count), VK_SUCCESS);
            std::vector<VkExtensionProperties> extensions(count);
            ASSERT_EQ(host.enumerate_device_extension_properties(physical[0], extensions.data(),
                                                                 extensions.size() * sizeof(VkExtensionProperties), count),
                      VK_SUCCESS);
            if (std::ranges::none_of(extensions, [](const auto& extension) {
                    return std::strcmp(extension.extensionName, VK_KHR_MAINTENANCE_5_EXTENSION_NAME) == 0;
                }))
            {
                GTEST_SKIP() << "VK_KHR_maintenance5 unavailable";
            }
            std::array<gpu_bridge::queue_family_properties, 64> families{};
            ASSERT_EQ(host.get_queue_family_properties(physical[0], false, families.data(), sizeof(families), count), VK_SUCCESS);
            uint32_t family = UINT32_MAX;
            for (uint32_t i = 0; i < std::min(count, static_cast<uint32_t>(families.size())); ++i)
            {
                if (families[i].queue_count && (families[i].queue_flags & VK_QUEUE_GRAPHICS_BIT))
                {
                    family = i;
                    break;
                }
            }
            if (family == UINT32_MAX)
            {
                GTEST_SKIP() << "No graphics queue";
            }
            const gpu_bridge::device_queue_create_entry queue{.queue_family_index = family, .queue_count = 1};
            constexpr auto extension = std::to_array(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
            ASSERT_EQ(host.create_device(physical[0], &queue, 1, extension.data(), extension.size(), 1, nullptr, 0, 0, device), VK_SUCCESS);
            ASSERT_EQ(host.create_device(physical[0], &queue, 1, nullptr, 0, 0, nullptr, 0, 0, unextended_device), VK_SUCCESS);
        }

        void TearDown() override
        {
            if (device)
            {
                host.destroy_device(device);
            }
            if (unextended_device)
            {
                host.destroy_device(unextended_device);
            }
            if (instance)
            {
                host.destroy_instance(instance);
            }
        }
    };

    TEST_F(VulkanRenderingGranularityHostTest, QueriesActualDriverForAttachmentlessAndMultipleFormats)
    {
        VkRenderingAreaInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_AREA_INFO;
        uint32_t width{};
        uint32_t height{};
        EXPECT_EQ(host.get_rendering_area_granularity(device, area_wire::encode(info), width, height), VK_SUCCESS);
        EXPECT_GT(width, 0u);
        EXPECT_GT(height, 0u);
        const std::array<VkFormat, 2> formats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
        info.colorAttachmentCount = static_cast<uint32_t>(formats.size());
        info.pColorAttachmentFormats = formats.data();
        info.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        EXPECT_EQ(host.get_rendering_area_granularity(device, area_wire::encode(info), width, height), VK_SUCCESS);
        EXPECT_GT(width, 0u);
        EXPECT_GT(height, 0u);
    }

    TEST_F(VulkanRenderingGranularityHostTest, RejectsUnavailableCapabilityDeadDeviceAndMalformedInput)
    {
        VkRenderingAreaInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_AREA_INFO;
        const auto packet = area_wire::encode(info);
        uint32_t width = UINT32_MAX;
        uint32_t height = UINT32_MAX;
        EXPECT_EQ(host.get_rendering_area_granularity(unextended_device, packet, width, height), VK_ERROR_FEATURE_NOT_PRESENT);
        EXPECT_EQ(width, 0u);
        EXPECT_EQ(height, 0u);
        EXPECT_EQ(host.get_rendering_area_granularity(device, {}, width, height), VK_ERROR_UNKNOWN);
        EXPECT_EQ(width, 0u);
        EXPECT_EQ(height, 0u);
        EXPECT_EQ(host.get_rendering_area_granularity(0, packet, width, height), VK_ERROR_INITIALIZATION_FAILED);
        const auto dead = device;
        host.destroy_device(device);
        device = 0;
        EXPECT_EQ(host.get_rendering_area_granularity(dead, packet, width, height), VK_ERROR_INITIALIZATION_FAILED);
    }
}
