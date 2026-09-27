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
    TEST(VulkanMaintenance6HostTest, AmdFeaturesPropertiesAndNullIndexBinding)
    {
        vulkan_host host;
        if (!host.available()) GTEST_SKIP() << "Host Vulkan driver unavailable";

        uint64_t instance = 0;
        ASSERT_EQ(host.create_instance(instance), VK_SUCCESS);
        struct cleanup
        {
            vulkan_host& host;
            uint64_t instance;
            uint64_t device{};
            uint64_t pool{};
            uint64_t command_buffer{};
            ~cleanup()
            {
                if (device)
                {
                    if (command_buffer) host.free_command_buffer(device, pool, command_buffer);
                    if (pool) host.destroy_command_pool(device, pool);
                    host.destroy_device(device);
                }
                host.destroy_instance(instance);
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
        if (!amd) GTEST_SKIP() << "AMD Vulkan device unavailable";

        uint32_t extension_count = 0;
        ASSERT_EQ(host.enumerate_device_extension_properties(amd, nullptr, 0, extension_count), VK_SUCCESS);
        std::vector<VkExtensionProperties> extensions(extension_count);
        ASSERT_EQ(host.enumerate_device_extension_properties(amd, extensions.data(), extensions.size() * sizeof(extensions[0]),
                                                              extension_count), VK_SUCCESS);
        ASSERT_LE(extension_count, extensions.size());
        extensions.resize(extension_count);
        const auto has_extension = [&](const char* name) {
            return std::any_of(extensions.begin(), extensions.end(), [&](const auto& e) {
                return std::strcmp(e.extensionName, name) == 0;
            });
        };
        ASSERT_TRUE(has_extension(VK_KHR_MAINTENANCE_6_EXTENSION_NAME));

        constexpr gpu_bridge::feature_chain_record maintenance_record{
            .s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR, .body_size = sizeof(VkBool32)};
        std::vector<std::byte> feature_blob;
        ASSERT_EQ(host.get_physical_device_features2(amd, &maintenance_record, sizeof(maintenance_record), 1, feature_blob), VK_SUCCESS);
        ASSERT_EQ(feature_blob.size(), sizeof(maintenance_record) + sizeof(VkBool32));
        VkBool32 maintenance6 = VK_FALSE;
        std::memcpy(&maintenance6, feature_blob.data() + sizeof(maintenance_record), sizeof(maintenance6));
        ASSERT_EQ(maintenance6, VK_TRUE);

        constexpr gpu_bridge::feature_chain_record property_record{
            .s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_PROPERTIES_KHR,
            .body_size = sizeof(VkPhysicalDeviceMaintenance6PropertiesKHR) - 2 * sizeof(void*)};
        std::vector<std::byte> property_blob;
        ASSERT_EQ(host.get_physical_device_properties2(amd, &property_record, sizeof(property_record), 1, property_blob), VK_SUCCESS);
        ASSERT_EQ(property_blob.size(), sizeof(property_record) + property_record.body_size);
        VkPhysicalDeviceMaintenance6PropertiesKHR properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_PROPERTIES_KHR;
        std::memcpy(reinterpret_cast<std::byte*>(&properties) + 2 * sizeof(void*),
                    property_blob.data() + sizeof(property_record), property_record.body_size);
        EXPECT_GT(properties.maxCombinedImageSamplerDescriptorCount, 0u);
        EXPECT_TRUE(properties.blockTexelViewCompatibleMultipleLayers == VK_FALSE ||
                    properties.blockTexelViewCompatibleMultipleLayers == VK_TRUE);
        EXPECT_TRUE(properties.fragmentShadingRateClampCombinerInputs == VK_FALSE ||
                    properties.fragmentShadingRateClampCombinerInputs == VK_TRUE);

        std::array<gpu_bridge::queue_family_properties, 64> families{};
        uint32_t family_count = 0;
        ASSERT_EQ(host.get_queue_family_properties(amd, false, families.data(), sizeof(families), family_count), VK_SUCCESS);
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < std::min(family_count, static_cast<uint32_t>(families.size())); ++i)
        {
            if (families[i].queue_count && (families[i].queue_flags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
        }
        ASSERT_NE(family, UINT32_MAX);
        const gpu_bridge::device_queue_create_entry queue{.queue_family_index = family, .queue_count = 1};
        constexpr char maintenance_extension[] = VK_KHR_MAINTENANCE_6_EXTENSION_NAME;
        ASSERT_EQ(host.create_device(amd, &queue, 1, maintenance_extension, sizeof(maintenance_extension), 1,
                                     feature_blob.data(), feature_blob.size(), 1, owned.device), VK_SUCCESS);
        ASSERT_EQ(host.create_command_pool(owned.device, family, 0, owned.pool), VK_SUCCESS);
        ASSERT_EQ(host.allocate_command_buffer(owned.device, owned.pool, 0, owned.command_buffer), VK_SUCCESS);
        ASSERT_EQ(host.begin_command_buffer(owned.command_buffer, 0, false, 0, {}, VK_FORMAT_UNDEFINED,
                                            VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 0), VK_SUCCESS);
        // The real host must reject null indices unless nullDescriptor was explicitly enabled.
        EXPECT_EQ(host.cmd_bind_index_buffer(owned.command_buffer, 0, 0, VK_INDEX_TYPE_UINT16), VK_ERROR_VALIDATION_FAILED_EXT);
        EXPECT_EQ(host.end_command_buffer(owned.command_buffer), VK_SUCCESS);

        if (!has_extension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME))
        {
            GTEST_SKIP() << "AMD driver has no robustness2 for positive null-index case";
        }
        constexpr gpu_bridge::feature_chain_record robustness_record{
            .s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
            .body_size = 3 * sizeof(VkBool32)};
        std::vector<std::byte> robustness_blob;
        ASSERT_EQ(host.get_physical_device_features2(amd, &robustness_record, sizeof(robustness_record), 1, robustness_blob), VK_SUCCESS);
        ASSERT_EQ(robustness_blob.size(), sizeof(robustness_record) + robustness_record.body_size);
        VkBool32 null_descriptor = VK_FALSE;
        std::memcpy(&null_descriptor, robustness_blob.data() + sizeof(robustness_record) + 2 * sizeof(VkBool32), sizeof(null_descriptor));
        if (null_descriptor != VK_TRUE)
        {
            GTEST_SKIP() << "AMD driver has no nullDescriptor feature";
        }
        host.free_command_buffer(owned.device, owned.pool, owned.command_buffer);
        owned.command_buffer = 0;
        host.destroy_command_pool(owned.device, owned.pool);
        owned.pool = 0;
        host.destroy_device(owned.device);
        owned.device = 0;
        VkBool32 disabled = VK_FALSE;
        std::memcpy(robustness_blob.data() + sizeof(robustness_record), &disabled, sizeof(disabled));
        std::memcpy(robustness_blob.data() + sizeof(robustness_record) + sizeof(VkBool32), &disabled, sizeof(disabled));
        feature_blob.insert(feature_blob.end(), robustness_blob.begin(), robustness_blob.end());
        constexpr char enabled_extensions[] = VK_KHR_MAINTENANCE_6_EXTENSION_NAME "\0" VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
        ASSERT_EQ(host.create_device(amd, &queue, 1, enabled_extensions, sizeof(enabled_extensions), 2,
                                     feature_blob.data(), feature_blob.size(), 2, owned.device), VK_SUCCESS);
        ASSERT_EQ(host.create_command_pool(owned.device, family, 0, owned.pool), VK_SUCCESS);
        ASSERT_EQ(host.allocate_command_buffer(owned.device, owned.pool, 0, owned.command_buffer), VK_SUCCESS);
        ASSERT_EQ(host.begin_command_buffer(owned.command_buffer, 0, false, 0, {}, VK_FORMAT_UNDEFINED,
                                            VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 0), VK_SUCCESS);
        EXPECT_EQ(host.cmd_bind_index_buffer(owned.command_buffer, 0, 0, VK_INDEX_TYPE_UINT16), VK_SUCCESS);
        EXPECT_EQ(host.end_command_buffer(owned.command_buffer), VK_SUCCESS);
    }
}
