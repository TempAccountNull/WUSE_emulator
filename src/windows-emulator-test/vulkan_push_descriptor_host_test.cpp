#include <gtest/gtest.h>
#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace sogen::test
{
    TEST(VulkanPushDescriptorHostTest, AmdRecordsLegacyAndMaintenance6SamplerWrites)
    {
        vulkan_host host;
        if (!host.available()) GTEST_SKIP() << "Host Vulkan unavailable";
        uint64_t instance = 0;
        ASSERT_EQ(host.create_instance(instance), VK_SUCCESS);
        struct owned_resources
        {
            vulkan_host& host;
            uint64_t instance{};
            uint64_t device{};
            uint64_t sampler{};
            uint64_t set_layout{};
            uint64_t pipeline_layout{};
            uint64_t command_pool{};
            uint64_t command_buffer{};
            ~owned_resources()
            {
                if (command_buffer) host.free_command_buffer(device, command_pool, command_buffer);
                if (command_pool) host.destroy_command_pool(device, command_pool);
                if (pipeline_layout) host.destroy_pipeline_layout(device, pipeline_layout);
                if (set_layout) host.destroy_descriptor_set_layout(device, set_layout);
                if (sampler) host.destroy_sampler(device, sampler);
                if (device) host.destroy_device(device);
                if (instance) host.destroy_instance(instance);
            }
        } owned{host, instance};

        std::array<uint64_t, 32> physical{};
        uint32_t count = 0;
        ASSERT_EQ(host.enumerate_physical_devices(instance, physical, count), VK_SUCCESS);
        uint64_t amd = 0;
        for (uint32_t i = 0; i < std::min(count, static_cast<uint32_t>(physical.size())); ++i)
        {
            VkPhysicalDeviceProperties properties{};
            ASSERT_EQ(host.get_physical_device_properties(physical[i], &properties, sizeof(properties), false), VK_SUCCESS);
            if (properties.vendorID == 0x1002) { amd = physical[i]; break; }
        }
        if (!amd) GTEST_SKIP() << "AMD device unavailable";
        uint32_t extension_count = 0;
        ASSERT_EQ(host.enumerate_device_extension_properties(amd, nullptr, 0, extension_count), VK_SUCCESS);
        std::vector<VkExtensionProperties> extensions(extension_count);
        ASSERT_EQ(host.enumerate_device_extension_properties(amd, extensions.data(),
                                                              extensions.size() * sizeof(extensions[0]), extension_count), VK_SUCCESS);
        const auto supports = [&](const char* name) {
            return std::any_of(extensions.begin(), extensions.begin() + std::min<size_t>(extension_count, extensions.size()),
                               [&](const auto& ext) { return std::strcmp(ext.extensionName, name) == 0; });
        };
        if (!supports(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) || !supports(VK_KHR_MAINTENANCE_6_EXTENSION_NAME))
            GTEST_SKIP() << "Push descriptor or maintenance6 hidden by bridge";

        std::array<gpu_bridge::queue_family_properties, 64> families{};
        uint32_t family_count = 0;
        ASSERT_EQ(host.get_queue_family_properties(amd, false, families.data(), sizeof(families), family_count), VK_SUCCESS);
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < std::min(family_count, static_cast<uint32_t>(families.size())); ++i)
            if (families[i].queue_count && (families[i].queue_flags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
        ASSERT_NE(family, UINT32_MAX);

        const gpu_bridge::feature_chain_record record{.s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR,
                                                      .body_size = sizeof(VkBool32)};
        std::vector<std::byte> feature_blob;
        ASSERT_EQ(host.get_physical_device_features2(amd, &record, sizeof(record), 1, feature_blob), VK_SUCCESS);
        ASSERT_EQ(feature_blob.size(), sizeof(record) + sizeof(VkBool32));
        VkBool32 maintenance6 = VK_FALSE;
        std::memcpy(&maintenance6, feature_blob.data() + sizeof(record), sizeof(maintenance6));
        ASSERT_EQ(maintenance6, VK_TRUE);
        constexpr char extensions_blob[] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME "\0" VK_KHR_MAINTENANCE_6_EXTENSION_NAME;
        const gpu_bridge::device_queue_create_entry queue{.queue_family_index = family, .queue_count = 1};
        ASSERT_EQ(host.create_device(amd, &queue, 1, extensions_blob, sizeof(extensions_blob), 2,
                                     feature_blob.data(), feature_blob.size(), 1, owned.device), VK_SUCCESS);
        ASSERT_EQ(host.create_sampler(owned.device, VK_FILTER_NEAREST, VK_FILTER_NEAREST,
                                      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      VK_FALSE, VK_COMPARE_OP_ALWAYS, VK_FALSE, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                                      0.0f, 1.0f, 0.0f, 1.0f, owned.sampler), VK_SUCCESS);
        const vulkan_host::descriptor_binding binding{.binding = 0, .descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLER,
                                                       .descriptor_count = 1, .stage_flags = VK_SHADER_STAGE_FRAGMENT_BIT};
        ASSERT_EQ(host.create_descriptor_set_layout(owned.device, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                                    std::span{&binding, 1}, owned.set_layout), VK_SUCCESS);
        ASSERT_EQ(host.create_pipeline_layout(owned.device, 0, 0, std::span{&owned.set_layout, 1},
                                              owned.pipeline_layout), VK_SUCCESS);
        ASSERT_EQ(host.create_command_pool(owned.device, family, 0, owned.command_pool), VK_SUCCESS);
        ASSERT_EQ(host.allocate_command_buffer(owned.device, owned.command_pool, 0, owned.command_buffer), VK_SUCCESS);
        ASSERT_EQ(host.begin_command_buffer(owned.command_buffer, 0, false, 0, {}, VK_FORMAT_UNDEFINED,
                                            VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 0), VK_SUCCESS);
        const vulkan_host::descriptor_write write{.dst_binding = 0, .descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLER,
                                                    .sampler = owned.sampler};
        EXPECT_EQ(host.cmd_push_descriptor_set(owned.command_buffer, owned.pipeline_layout, 0, 0,
                                               VK_PIPELINE_BIND_POINT_GRAPHICS, false, std::span{&write, 1}), VK_SUCCESS);
        EXPECT_EQ(host.cmd_push_descriptor_set(owned.command_buffer, owned.pipeline_layout, 0,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                               true, std::span{&write, 1}), VK_SUCCESS);
        auto foreign = write;
        foreign.sampler = UINT64_MAX;
        EXPECT_EQ(host.cmd_push_descriptor_set(owned.command_buffer, owned.pipeline_layout, 0,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                               true, std::span{&foreign, 1}), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(host.end_command_buffer(owned.command_buffer), VK_SUCCESS);
    }
}
