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
    TEST(VulkanAmdBufferMarkerHostTest, WritesBothMarkersToAmdHostMemory)
    {
        vulkan_host host;
        if (!host.available())
        {
            GTEST_SKIP() << "Host Vulkan unavailable";
        }

        struct cleanup
        {
            vulkan_host& host;
            uint64_t instance{};
            uint64_t device{};
            uint64_t pool{};
            uint64_t buffer{};
            uint64_t memory{};

            ~cleanup()
            {
                if (device)
                {
                    host.device_wait_idle(device);
                    if (pool)
                    {
                        host.destroy_command_pool(device, pool);
                    }
                    if (buffer)
                    {
                        host.destroy_buffer(device, buffer);
                    }
                    if (memory)
                    {
                        host.free_memory(device, memory);
                    }
                    host.destroy_device(device);
                }
                if (instance)
                {
                    host.destroy_instance(instance);
                }
            }
        } owned{host};

        ASSERT_EQ(host.create_instance(owned.instance), VK_SUCCESS);
        std::array<uint64_t, 32> physical_devices{};
        uint32_t physical_count = 0;
        ASSERT_EQ(host.enumerate_physical_devices(owned.instance, physical_devices, physical_count), VK_SUCCESS);
        uint64_t physical_device = 0;
        for (uint32_t i = 0; i < std::min(physical_count, static_cast<uint32_t>(physical_devices.size())); ++i)
        {
            VkPhysicalDeviceProperties properties{};
            ASSERT_EQ(host.get_physical_device_properties(physical_devices[i], &properties, sizeof(properties), false), VK_SUCCESS);
            if (properties.vendorID == 0x1002)
            {
                physical_device = physical_devices[i];
                break;
            }
        }
        if (!physical_device)
        {
            GTEST_SKIP() << "AMD Vulkan device unavailable";
        }

        uint32_t extension_count = 0;
        ASSERT_EQ(host.enumerate_device_extension_properties(physical_device, nullptr, 0, extension_count), VK_SUCCESS);
        std::vector<VkExtensionProperties> extensions(extension_count);
        ASSERT_EQ(host.enumerate_device_extension_properties(physical_device, extensions.data(), extensions.size() * sizeof(extensions[0]),
                                                             extension_count),
                  VK_SUCCESS);
        ASSERT_LE(extension_count, extensions.size());
        extensions.resize(extension_count);
        const bool marker_supported = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_AMD_BUFFER_MARKER_EXTENSION_NAME) == 0;
        });
        if (!marker_supported)
        {
            GTEST_SKIP() << "AMD driver does not expose VK_AMD_buffer_marker";
        }
        constexpr gpu_bridge::feature_chain_record sync2_record{.s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
                                                                .body_size = sizeof(VkBool32)};
        std::vector<std::byte> features;
        ASSERT_EQ(host.get_physical_device_features2(physical_device, &sync2_record, sizeof(sync2_record), 1, features), VK_SUCCESS);
        ASSERT_GE(features.size(), sizeof(sync2_record) + sizeof(VkBool32));
        VkBool32 sync2_supported = VK_FALSE;
        std::memcpy(&sync2_supported, features.data() + sizeof(sync2_record), sizeof(sync2_supported));

        std::array<gpu_bridge::queue_family_properties, 64> families{};
        uint32_t family_count = 0;
        ASSERT_EQ(host.get_queue_family_properties(physical_device, false, families.data(), sizeof(families), family_count), VK_SUCCESS);
        uint32_t graphics_family = UINT32_MAX;
        for (uint32_t i = 0; i < std::min(family_count, static_cast<uint32_t>(families.size())); ++i)
        {
            if (families[i].queue_count && (families[i].queue_flags & VK_QUEUE_GRAPHICS_BIT))
            {
                graphics_family = i;
                break;
            }
        }
        ASSERT_NE(graphics_family, UINT32_MAX);
        const gpu_bridge::device_queue_create_entry queue{.queue_family_index = graphics_family, .queue_count = 1};
        std::array<std::byte, sizeof(sync2_record) + sizeof(VkBool32)> enabled_features{};
        const bool use_sync2 = sync2_supported == VK_TRUE;
        if (use_sync2)
        {
            std::memcpy(enabled_features.data(), &sync2_record, sizeof(sync2_record));
            std::memcpy(enabled_features.data() + sizeof(sync2_record), &sync2_supported, sizeof(sync2_supported));
        }
        constexpr char extension_name[] = VK_AMD_BUFFER_MARKER_EXTENSION_NAME;
        ASSERT_EQ(host.create_device(physical_device, &queue, 1, extension_name, sizeof(extension_name), 1,
                                     use_sync2 ? enabled_features.data() : nullptr, use_sync2 ? enabled_features.size() : 0,
                                     use_sync2 ? 1 : 0, owned.device),
                  VK_SUCCESS);

        const bool marker2_available = host.supports_buffer_marker2(owned.device);
        ASSERT_EQ(host.create_command_pool(owned.device, graphics_family, 0, owned.pool), VK_SUCCESS);
        uint64_t command_buffer = 0;
        ASSERT_EQ(host.allocate_command_buffer(owned.device, owned.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, command_buffer), VK_SUCCESS);
        ASSERT_EQ(host.create_buffer(owned.device, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, owned.buffer), VK_SUCCESS);
        uint64_t memory_size = 0;
        uint64_t alignment = 0;
        uint32_t memory_bits = 0;
        ASSERT_EQ(host.get_buffer_memory_requirements(owned.device, owned.buffer, memory_size, alignment, memory_bits), VK_SUCCESS);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        ASSERT_EQ(host.get_physical_device_memory_properties(physical_device, &memory_properties, sizeof(memory_properties)), VK_SUCCESS);
        uint32_t memory_type = UINT32_MAX;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        {
            if ((memory_bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags &
                                              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                                                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            {
                memory_type = i;
                break;
            }
        }
        if (memory_type == UINT32_MAX)
        {
            GTEST_SKIP() << "No host-visible coherent memory for marker readback";
        }
        ASSERT_EQ(host.allocate_memory(owned.device, memory_size, memory_type, 0, 0, owned.memory), VK_SUCCESS);
        ASSERT_EQ(host.bind_buffer_memory(owned.device, owned.buffer, owned.memory, 0), VK_SUCCESS);
        const std::array<uint32_t, 4> initial{0, 0, 0, 0};
        ASSERT_EQ(host.upload_memory(owned.device, owned.memory, 0, sizeof(initial), initial.data(), sizeof(initial)), VK_SUCCESS);

        ASSERT_EQ(host.begin_command_buffer(command_buffer, 0, false, 0, {}, 0, 0, 1, 0), VK_SUCCESS);
        EXPECT_EQ(host.cmd_write_buffer_marker(command_buffer, owned.buffer, 2, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, false),
                  VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(host.cmd_write_buffer_marker(command_buffer, owned.buffer, 16, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, false),
                  VK_ERROR_INITIALIZATION_FAILED);
        ASSERT_EQ(host.cmd_write_buffer_marker(command_buffer, owned.buffer, 4, VK_PIPELINE_STAGE_TRANSFER_BIT, 0xA1B2C3D4, false),
                  VK_SUCCESS);
        if (marker2_available)
        {
            ASSERT_EQ(host.cmd_write_buffer_marker(command_buffer, owned.buffer, 12, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0x12345678, true),
                      VK_SUCCESS);
        }
        else
        {
            EXPECT_EQ(host.cmd_write_buffer_marker(command_buffer, owned.buffer, 12, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0x12345678, true),
                      VK_ERROR_FEATURE_NOT_PRESENT);
        }
        ASSERT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
        uint64_t host_queue = 0;
        ASSERT_EQ(host.get_device_queue(owned.device, graphics_family, 0, host_queue), VK_SUCCESS);
        ASSERT_EQ(host.queue_submit(host_queue, command_buffer, 0), VK_SUCCESS);
        ASSERT_EQ(host.queue_wait_idle(host_queue), VK_SUCCESS);
        std::array<uint32_t, 4> observed{};
        ASSERT_EQ(host.download_memory(owned.device, owned.memory, 0, sizeof(observed), observed.data(), sizeof(observed)), VK_SUCCESS);
        EXPECT_EQ(observed[0], 0u);
        EXPECT_EQ(observed[1], 0xA1B2C3D4u);
        EXPECT_EQ(observed[2], 0u);
        EXPECT_EQ(observed[3], marker2_available ? 0x12345678u : 0u);
    }
}
