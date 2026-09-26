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
                                             "VK_AMD_buffer_marker",
                                             "VK_EXT_conditional_rendering",
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
}
