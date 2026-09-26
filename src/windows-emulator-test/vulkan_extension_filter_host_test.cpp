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
    TEST(VulkanExtensionFilterHostTest, HidesUnbridgedExtensionFamiliesInBothPresentationModes)
    {
        for (const bool native_wsi : {false, true})
        {
            SCOPED_TRACE(native_wsi ? "native WSI" : "readback");
            vulkan_host host;
            if (!host.available())
            {
                GTEST_SKIP() << "Host Vulkan driver unavailable";
            }
            if (native_wsi)
            {
                host.enable_native_wsi();
            }

            uint64_t instance = 0;
            ASSERT_EQ(host.create_instance(instance), VK_SUCCESS);
            ASSERT_NE(instance, 0u);

            std::array<uint64_t, 32> physical_devices{};
            uint32_t physical_count = 0;
            ASSERT_EQ(host.enumerate_physical_devices(instance, physical_devices, physical_count), VK_SUCCESS);
            if (physical_count == 0)
            {
                host.destroy_instance(instance);
                GTEST_SKIP() << "Host Vulkan has no physical devices";
            }

            std::array<gpu_bridge::queue_family_properties, 64> queue_families{};
            uint32_t queue_family_count = 0;
            ASSERT_EQ(host.get_queue_family_properties(physical_devices[0], false, queue_families.data(), sizeof(queue_families),
                                                       queue_family_count),
                      VK_SUCCESS);
            uint32_t graphics_family = UINT32_MAX;
            for (uint32_t i = 0; i < std::min(queue_family_count, static_cast<uint32_t>(queue_families.size())); ++i)
            {
                if (queue_families[i].queue_count && (queue_families[i].queue_flags & VK_QUEUE_GRAPHICS_BIT))
                {
                    graphics_family = i;
                    break;
                }
            }
            if (graphics_family == UINT32_MAX)
            {
                host.destroy_instance(instance);
                GTEST_SKIP() << "Host Vulkan has no graphics queue";
            }
            const gpu_bridge::device_queue_create_entry queue{.queue_family_index = graphics_family, .queue_count = 1};

            uint32_t extension_count = 0;
            ASSERT_EQ(host.enumerate_device_extension_properties(physical_devices[0], nullptr, 0, extension_count), VK_SUCCESS);
            std::vector<VkExtensionProperties> extensions(extension_count);
            ASSERT_EQ(host.enumerate_device_extension_properties(physical_devices[0], extensions.data(),
                                                                 extensions.size() * sizeof(extensions[0]), extension_count),
                      VK_SUCCESS);
            ASSERT_LE(extension_count, extensions.size());
            extensions.resize(extension_count);

            const auto contains = [&](const char* name) {
                return std::any_of(extensions.begin(), extensions.end(),
                                   [&](const VkExtensionProperties& extension) { return std::strcmp(extension.extensionName, name) == 0; });
            };
            constexpr std::array unsupported{"VK_KHR_external_memory_win32",
                                             "VK_KHR_external_semaphore_win32",
                                             "VK_KHR_external_fence_win32",
                                             "VK_KHR_win32_keyed_mutex",
                                             "VK_EXT_full_screen_exclusive",
                                             "VK_NV_low_latency2",
                                             "VK_EXT_hdr_metadata",
                                             "VK_KHR_maintenance6",
                                             "VK_EXT_depth_bias_control",
                                             "VK_EXT_descriptor_buffer",
                                             "VK_EXT_descriptor_heap",
                                             "VK_EXT_multi_draw",
                                             "VK_EXT_present_timing",
                                             "VK_KHR_device_fault",
                                             "VK_KHR_present_wait",
                                             "VK_KHR_present_wait2",
                                             "VK_NVX_binary_import",
                                             "VK_NVX_image_view_handle",
                                             "VK_NV_device_diagnostic_checkpoints"};
            for (const char* name : unsupported)
            {
                EXPECT_FALSE(contains(name)) << name;
            }

            for (const char* name : unsupported)
            {
                uint64_t device = 0;
                EXPECT_EQ(host.create_device(physical_devices[0], &queue, 1, name, std::strlen(name) + 1, 1, nullptr, 0, 0, device),
                          VK_ERROR_EXTENSION_NOT_PRESENT);
                EXPECT_EQ(device, 0u);
            }

            host.destroy_instance(instance);
        }
    }

    TEST(VulkanExtensionFilterHostTest, ConditionalRenderingRecordsOnAmdDevice)
    {
        vulkan_host host;
        if (!host.available())
        {
            GTEST_SKIP() << "Host Vulkan driver unavailable";
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
        for (uint32_t index = 0; index < std::min(physical_count, static_cast<uint32_t>(physical_devices.size())); ++index)
        {
            VkPhysicalDeviceProperties properties{};
            ASSERT_EQ(host.get_physical_device_properties(physical_devices[index], &properties, sizeof(properties), false), VK_SUCCESS);
            if (properties.vendorID == 0x1002)
            {
                physical_device = physical_devices[index];
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
        const bool supported = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME) == 0;
        });
        if (!supported)
        {
            GTEST_SKIP() << "AMD Vulkan driver does not expose VK_EXT_conditional_rendering";
        }

        constexpr gpu_bridge::feature_chain_record feature_record{
            .s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT, .body_size = 2 * sizeof(VkBool32)};
        std::vector<std::byte> feature_query;
        ASSERT_EQ(host.get_physical_device_features2(physical_device, &feature_record, sizeof(feature_record), 1, feature_query),
                  VK_SUCCESS);
        ASSERT_GE(feature_query.size(), sizeof(feature_record) + 2 * sizeof(VkBool32));
        VkBool32 conditional_supported = VK_FALSE;
        VkBool32 inherited_supported = VK_FALSE;
        std::memcpy(&conditional_supported, feature_query.data() + sizeof(feature_record), sizeof(conditional_supported));
        std::memcpy(&inherited_supported, feature_query.data() + sizeof(feature_record) + sizeof(VkBool32), sizeof(inherited_supported));
        if (conditional_supported != VK_TRUE)
        {
            GTEST_SKIP() << "AMD Vulkan driver does not enable conditional rendering";
        }

        std::array<gpu_bridge::queue_family_properties, 64> families{};
        uint32_t family_count = 0;
        ASSERT_EQ(host.get_queue_family_properties(physical_device, false, families.data(), sizeof(families), family_count), VK_SUCCESS);
        uint32_t graphics_family = UINT32_MAX;
        for (uint32_t index = 0; index < std::min(family_count, static_cast<uint32_t>(families.size())); ++index)
        {
            if (families[index].queue_count && (families[index].queue_flags & VK_QUEUE_GRAPHICS_BIT))
            {
                graphics_family = index;
                break;
            }
        }
        ASSERT_NE(graphics_family, UINT32_MAX);
        const gpu_bridge::device_queue_create_entry queue{.queue_family_index = graphics_family, .queue_count = 1};
        std::array<std::byte, sizeof(feature_record) + 2 * sizeof(VkBool32)> enabled_features{};
        const std::array<VkBool32, 2> enabled{VK_TRUE, inherited_supported};
        std::memcpy(enabled_features.data(), &feature_record, sizeof(feature_record));
        std::memcpy(enabled_features.data() + sizeof(feature_record), enabled.data(), sizeof(enabled));
        constexpr char extension_name[] = VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME;
        ASSERT_EQ(host.create_device(physical_device, &queue, 1, extension_name, sizeof(extension_name), 1, enabled_features.data(),
                                     enabled_features.size(), 1, owned.device),
                  VK_SUCCESS);
        ASSERT_EQ(host.create_command_pool(owned.device, graphics_family, 0, owned.pool), VK_SUCCESS);
        uint64_t command_buffer = 0;
        ASSERT_EQ(host.allocate_command_buffer(owned.device, owned.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, command_buffer), VK_SUCCESS);
        ASSERT_EQ(host.create_buffer(owned.device, 8, VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT, owned.buffer), VK_SUCCESS);
        uint64_t memory_size = 0;
        uint64_t alignment = 0;
        uint32_t memory_bits = 0;
        ASSERT_EQ(host.get_buffer_memory_requirements(owned.device, owned.buffer, memory_size, alignment, memory_bits), VK_SUCCESS);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        ASSERT_EQ(host.get_physical_device_memory_properties(physical_device, &memory_properties, sizeof(memory_properties)), VK_SUCCESS);
        uint32_t memory_type = UINT32_MAX;
        for (uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index)
        {
            if ((memory_bits & (1u << index)) && (memory_properties.memoryTypes[index].propertyFlags &
                                                  (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                                                     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            {
                memory_type = index;
                break;
            }
        }
        if (memory_type == UINT32_MAX)
        {
            GTEST_SKIP() << "No host-coherent memory for AMD conditional buffer";
        }
        ASSERT_EQ(host.allocate_memory(owned.device, memory_size, memory_type, 0, 0, owned.memory), VK_SUCCESS);
        ASSERT_EQ(host.bind_buffer_memory(owned.device, owned.buffer, owned.memory, 0), VK_SUCCESS);
        const std::array<uint32_t, 2> predicates{0, 1};
        ASSERT_EQ(host.upload_memory(owned.device, owned.memory, 0, sizeof(predicates), predicates.data(), sizeof(predicates)), VK_SUCCESS);

        uint64_t secondary = 0;
        if (inherited_supported == VK_TRUE)
        {
            ASSERT_EQ(host.allocate_command_buffer(owned.device, owned.pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, secondary), VK_SUCCESS);
            ASSERT_EQ(host.begin_command_buffer(secondary, 0, true, 0, {}, 0, 0, 1, 0, false, true), VK_SUCCESS);
            ASSERT_EQ(host.end_command_buffer(secondary), VK_SUCCESS);
        }

        ASSERT_EQ(host.begin_command_buffer(command_buffer, 0, false, 0, {}, 0, 0, 1, 0), VK_SUCCESS);
        EXPECT_EQ(host.begin_command_buffer(command_buffer, 0, false, 0, {}, 0, 0, 1, 0, false, true), VK_ERROR_FEATURE_NOT_PRESENT);
        EXPECT_EQ(host.cmd_begin_conditional_rendering(command_buffer, 0, 0, 0), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(host.cmd_begin_conditional_rendering(command_buffer, owned.buffer, 2, 0), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(host.cmd_begin_conditional_rendering(command_buffer, owned.buffer, 8, 0), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(host.cmd_begin_conditional_rendering(command_buffer, owned.buffer, 0, 2), VK_ERROR_INITIALIZATION_FAILED);
        ASSERT_EQ(host.cmd_begin_conditional_rendering(command_buffer, owned.buffer, 0, 0), VK_SUCCESS);
        if (secondary)
        {
            const std::array<uint64_t, 1> secondaries{secondary};
            ASSERT_EQ(host.cmd_execute_commands(command_buffer, secondaries), VK_SUCCESS);
        }
        ASSERT_EQ(host.cmd_end_conditional_rendering(command_buffer), VK_SUCCESS);
        ASSERT_EQ(host.cmd_begin_conditional_rendering(command_buffer, owned.buffer, 4, VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT),
                  VK_SUCCESS);
        ASSERT_EQ(host.cmd_end_conditional_rendering(command_buffer), VK_SUCCESS);
        ASSERT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
        uint64_t host_queue = 0;
        ASSERT_EQ(host.get_device_queue(owned.device, graphics_family, 0, host_queue), VK_SUCCESS);
        ASSERT_EQ(host.queue_submit(host_queue, command_buffer, 0), VK_SUCCESS);
        EXPECT_EQ(host.queue_wait_idle(host_queue), VK_SUCCESS);
    }
}
