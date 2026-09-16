#include <gtest/gtest.h>
#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include <vk_render_pass.hpp>
#include <algorithm>
#include <array>
#include <bit>

namespace sogen::test
{
    namespace readback_wire = gpu_bridge::render_pass_wire;

    class VulkanImageReadbackHostTest : public testing::Test
    {
      protected:
        vulkan_host host;
        uint64_t instance{};
        std::array<uint64_t, 2> devices{};
        std::array<uint64_t, 2> images{};
        std::array<uint64_t, 2> buffers{};
        uint64_t command_buffer{};

        void SetUp() override
        {
            if (!host.available())
            {
                GTEST_SKIP() << "Host Vulkan unavailable";
            }
            const auto result = host.create_instance(instance);
            if (result != VK_SUCCESS)
            {
                GTEST_SKIP() << "Host Vulkan instance unavailable: " << result;
            }
            std::array<uint64_t, 32> physical{};
            uint32_t count = 0;
            ASSERT_EQ(host.enumerate_physical_devices(instance, physical, count), VK_SUCCESS);
            if (!count)
            {
                GTEST_SKIP() << "Host Vulkan has no physical devices";
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
                GTEST_SKIP() << "Host Vulkan has no graphics queue";
            }
            const gpu_bridge::device_queue_create_entry entry{.queue_family_index = family, .queue_count = 1};
            for (uint32_t i = 0; i < devices.size(); ++i)
            {
                ASSERT_EQ(host.create_device(physical[0], &entry, 1, nullptr, 0, 0, nullptr, 0, 0, devices[i]), VK_SUCCESS);
                ASSERT_EQ(host.create_buffer(devices[i], 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, buffers[i]), VK_SUCCESS);
                uint64_t size = 0;
                uint64_t alignment = 0;
                uint64_t memory = 0;
                uint32_t types = 0;
                ASSERT_EQ(host.get_buffer_memory_requirements(devices[i], buffers[i], size, alignment, types), VK_SUCCESS);
                ASSERT_NE(types, 0u);
                ASSERT_EQ(host.allocate_memory(devices[i], size, static_cast<uint32_t>(std::countr_zero(types)), 0, 0, memory), VK_SUCCESS);
                ASSERT_EQ(host.bind_buffer_memory(devices[i], buffers[i], memory, 0), VK_SUCCESS);
                ASSERT_EQ(host.create_image(devices[i], VK_FORMAT_R8G8B8A8_UNORM, 8, 8, VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                            VK_IMAGE_TILING_OPTIMAL, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TYPE_2D, 1, 1, 2, 0, images[i]),
                          VK_SUCCESS);
                ASSERT_EQ(host.get_image_memory_requirements(devices[i], images[i], size, alignment, types), VK_SUCCESS);
                ASSERT_NE(types, 0u);
                ASSERT_EQ(host.allocate_memory(devices[i], size, static_cast<uint32_t>(std::countr_zero(types)), 0, 0, memory), VK_SUCCESS);
                ASSERT_EQ(host.bind_image_memory(devices[i], images[i], memory, 0), VK_SUCCESS);
            }
            uint64_t pool = 0;
            ASSERT_EQ(host.create_command_pool(devices[0], family, 0, pool), VK_SUCCESS);
            ASSERT_EQ(host.allocate_command_buffer(devices[0], pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, command_buffer), VK_SUCCESS);
            ASSERT_EQ(host.begin_command_buffer(command_buffer, 0, false, 0, {}, 0, 0, 1, 0), VK_SUCCESS);
        }

        void TearDown() override
        {
            for (const auto device : devices)
            {
                if (device)
                {
                    host.destroy_device(device);
                }
            }
            if (instance)
            {
                host.destroy_instance(instance);
            }
        }

        static std::vector<std::byte> packet(uint64_t image, uint64_t buffer, const void* chain = nullptr)
        {
            const std::array<VkBufferImageCopy2, 2> regions{{
                {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                 .pNext = nullptr,
                 .bufferOffset = 64,
                 .bufferRowLength = 8,
                 .bufferImageHeight = 8,
                 .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                 .imageOffset = {1, 2, 0},
                 .imageExtent = {3, 4, 1}},
                {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                 .pNext = chain,
                 .bufferOffset = 1024,
                 .bufferRowLength = 9,
                 .bufferImageHeight = 7,
                 .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1},
                 .imageOffset = {2, 1, 0},
                 .imageExtent = {4, 3, 1}},
            }};
            const VkCopyImageToBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
                                                nullptr,
                                                readback_wire::handle_value<VkImage>(image),
                                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                readback_wire::handle_value<VkBuffer>(buffer),
                                                2,
                                                regions.data()};
            return readback_wire::encode(info);
        }

        void reject_handles(uint64_t image, uint64_t buffer)
        {
            const auto bytes = packet(image, buffer);
            for (const bool copy2 : {false, true})
            {
                EXPECT_EQ(host.cmd_copy_image_to_buffer_full(command_buffer, bytes, copy2), VK_ERROR_INITIALIZATION_FAILED);
            }
        }
    };

    TEST_F(VulkanImageReadbackHostTest, RejectsEachForeignOwnerBeforeNativeDispatch)
    {
        reject_handles(images[1], buffers[0]);
        reject_handles(images[0], buffers[1]);
        EXPECT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
    }

    TEST_F(VulkanImageReadbackHostTest, RejectsDestroyedImageBeforeNativeDispatch)
    {
        host.destroy_image(devices[0], images[0]);
        reject_handles(images[0], buffers[0]);
        EXPECT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
    }

    TEST_F(VulkanImageReadbackHostTest, RejectsDestroyedBufferBeforeNativeDispatch)
    {
        host.destroy_buffer(devices[0], buffers[0]);
        reject_handles(images[0], buffers[0]);
        EXPECT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
    }

    TEST_F(VulkanImageReadbackHostTest, LegacyRejectsLastRegionExtensionInsteadOfDroppingIt)
    {
        const VkCopyCommandTransformInfoQCOM transform{VK_STRUCTURE_TYPE_COPY_COMMAND_TRANSFORM_INFO_QCOM, nullptr,
                                                       VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR};
        const auto bytes = packet(images[0], buffers[0], &transform);
        EXPECT_EQ(host.cmd_copy_image_to_buffer_full(command_buffer, bytes, false), VK_ERROR_FEATURE_NOT_PRESENT);
        EXPECT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
    }

    TEST_F(VulkanImageReadbackHostTest, RejectsMalformedPacketAndInvalidCommandBuffer)
    {
        auto bytes = packet(images[0], buffers[0]);
        EXPECT_EQ(host.cmd_copy_image_to_buffer_full(0, bytes, false), VK_ERROR_INITIALIZATION_FAILED);
        bytes.pop_back();
        for (const bool copy2 : {false, true})
        {
            EXPECT_EQ(host.cmd_copy_image_to_buffer_full(command_buffer, bytes, copy2), VK_ERROR_UNKNOWN);
        }
        EXPECT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
    }
}
