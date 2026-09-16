#include <gtest/gtest.h>
#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include <vk_synchronization.hpp>
#include <array>
#include <bit>

namespace sogen::test
{
    namespace sync = gpu_bridge::synchronization_wire;
    namespace codec = gpu_bridge::render_pass_wire;

    class VulkanSynchronizationHostTest : public testing::Test
    {
      protected:
        vulkan_host host;
        uint64_t instance{};
        std::array<uint64_t, 2> devices{};
        uint64_t command_buffer{};
        uint64_t queue{};

        void SetUp() override
        {
            if (!host.available())
            {
                GTEST_SKIP() << "Host Vulkan driver unavailable";
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
            for (auto& device : devices)
            {
                ASSERT_EQ(host.create_device(physical[0], &entry, 1, nullptr, 0, 0, nullptr, 0, 0, device), VK_SUCCESS);
            }
            uint64_t pool = 0;
            ASSERT_EQ(host.create_command_pool(devices[0], family, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, pool), VK_SUCCESS);
            ASSERT_EQ(host.allocate_command_buffer(devices[0], pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, command_buffer), VK_SUCCESS);
            ASSERT_EQ(host.get_device_queue(devices[0], family, 0, queue), VK_SUCCESS);
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

        int32_t dispatch(const sync::command& command)
        {
            const auto bytes = sync::encode(command);
            return host.cmd_synchronization(command_buffer, bytes);
        }

        void create_buffer(uint64_t device, uint64_t& buffer)
        {
            ASSERT_EQ(host.create_buffer(device, 256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, buffer),
                      VK_SUCCESS);
            uint64_t size = 0;
            uint64_t alignment = 0;
            uint64_t memory = 0;
            uint32_t types = 0;
            ASSERT_EQ(host.get_buffer_memory_requirements(device, buffer, size, alignment, types), VK_SUCCESS);
            ASSERT_NE(types, 0u);
            ASSERT_EQ(host.allocate_memory(device, size, static_cast<uint32_t>(std::countr_zero(types)), 0, 0, memory), VK_SUCCESS);
            ASSERT_EQ(host.bind_buffer_memory(device, buffer, memory, 0), VK_SUCCESS);
        }

        void create_image(uint64_t device, uint64_t& image)
        {
            ASSERT_EQ(host.create_image(device, VK_FORMAT_R8G8B8A8_UNORM, 8, 8,
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_TILING_OPTIMAL, 1,
                                        VK_IMAGE_TYPE_2D, 1, 1, 1, 0, image),
                      VK_SUCCESS);
            uint64_t size = 0;
            uint64_t alignment = 0;
            uint64_t memory = 0;
            uint32_t types = 0;
            ASSERT_EQ(host.get_image_memory_requirements(device, image, size, alignment, types), VK_SUCCESS);
            ASSERT_NE(types, 0u);
            ASSERT_EQ(host.allocate_memory(device, size, static_cast<uint32_t>(std::countr_zero(types)), 0, 0, memory), VK_SUCCESS);
            ASSERT_EQ(host.bind_image_memory(device, image, memory, 0), VK_SUCCESS);
        }

        void reject_event(uint64_t id)
        {
            // Non-dispatchable handles are pointers on x64 and uint64_t on x86.
            const auto event = codec::handle_value<VkEvent>(id); // NOLINT(readability-qualified-auto)
            const VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            for (const auto op : {sync::operation::set_event, sync::operation::reset_event, sync::operation::wait_events,
                                  sync::operation::set_event2, sync::operation::reset_event2, sync::operation::wait_events2})
            {
                SCOPED_TRACE(static_cast<uint32_t>(op));
                sync::command command{};
                command.op = op;
                command.event = event;
                command.stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                command.stage2 = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                command.legacy.source_stages = command.legacy.destination_stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                command.event_count = 1;
                command.events = &event;
                command.dependencies = &dependency;
                EXPECT_EQ(dispatch(command), VK_ERROR_INITIALIZATION_FAILED);
            }
        }

        void reject_buffer(uint64_t id)
        {
            uint64_t owned = 0;
            ASSERT_NO_FATAL_FAILURE(create_buffer(devices[0], owned));
            std::array<VkBufferMemoryBarrier, 2> barriers{};
            std::array<VkBufferMemoryBarrier2, 2> barriers2{};
            for (uint32_t i = 0; i < 2; ++i)
            {
                barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                barriers[i].buffer = codec::handle_value<VkBuffer>(i ? id : owned);
                barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers[i].size = VK_WHOLE_SIZE;
                barriers2[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                barriers2[i].buffer = barriers[i].buffer;
                barriers2[i].srcQueueFamilyIndex = barriers2[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers2[i].size = VK_WHOLE_SIZE;
            }
            sync::command command{};
            command.op = sync::operation::barrier;
            command.legacy.source_stages = command.legacy.destination_stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            command.legacy.buffer_count = 2;
            command.legacy.buffers = barriers.data();
            EXPECT_EQ(dispatch(command), VK_ERROR_INITIALIZATION_FAILED);
            command.op = sync::operation::barrier2;
            command.dependency.bufferMemoryBarrierCount = 2;
            command.dependency.pBufferMemoryBarriers = barriers2.data();
            EXPECT_EQ(dispatch(command), VK_ERROR_INITIALIZATION_FAILED);
            // A rejected packet leaves guest IDs untouched and the native recording usable.
            EXPECT_EQ(codec::handle_id(barriers[0].buffer), owned);
            EXPECT_EQ(codec::handle_id(barriers2[0].buffer), owned);
            EXPECT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
        }

        void reject_image(uint64_t id)
        {
            uint64_t owned = 0;
            ASSERT_NO_FATAL_FAILURE(create_image(devices[0], owned));
            std::array<VkImageMemoryBarrier, 2> barriers{};
            std::array<VkImageMemoryBarrier2, 2> barriers2{};
            for (uint32_t i = 0; i < 2; ++i)
            {
                barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barriers[i].image = codec::handle_value<VkImage>(i ? id : owned);
                barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barriers[i].subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
                barriers2[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barriers2[i].image = barriers[i].image;
                barriers2[i].srcQueueFamilyIndex = barriers2[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers2[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barriers2[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barriers2[i].subresourceRange = barriers[i].subresourceRange;
            }
            sync::command command{};
            command.op = sync::operation::barrier;
            command.legacy.source_stages = command.legacy.destination_stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            command.legacy.image_count = 2;
            command.legacy.images = barriers.data();
            EXPECT_EQ(dispatch(command), VK_ERROR_INITIALIZATION_FAILED);
            command.op = sync::operation::barrier2;
            command.dependency.imageMemoryBarrierCount = 2;
            command.dependency.pImageMemoryBarriers = barriers2.data();
            EXPECT_EQ(dispatch(command), VK_ERROR_INITIALIZATION_FAILED);
            EXPECT_EQ(codec::handle_id(barriers[0].image), owned);
            EXPECT_EQ(barriers[0].oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
            EXPECT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
        }
    };

    TEST_F(VulkanSynchronizationHostTest, WrongDeviceEventRejectedByEveryEventCommandAndStatusQuery)
    {
        uint64_t event = 0;
        ASSERT_EQ(host.create_event(devices[1], 0, event), VK_SUCCESS);
        EXPECT_EQ(host.get_event_status(devices[0], event), VK_ERROR_INITIALIZATION_FAILED);
        reject_event(event);
        EXPECT_EQ(host.get_event_status(devices[1], event), VK_EVENT_RESET);
        EXPECT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
    }

    TEST_F(VulkanSynchronizationHostTest, DestroyedEventRejectedByEveryEventCommandAndStatusQuery)
    {
        uint64_t event = 0;
        ASSERT_EQ(host.create_event(devices[0], 0, event), VK_SUCCESS);
        host.destroy_event(devices[0], event);
        EXPECT_EQ(host.get_event_status(devices[0], event), VK_ERROR_INITIALIZATION_FAILED);
        reject_event(event);
        EXPECT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
    }

    TEST_F(VulkanSynchronizationHostTest, WrongDeviceBufferRejectedAfterOwnedArrayEntry)
    {
        uint64_t buffer = 0;
        ASSERT_NO_FATAL_FAILURE(create_buffer(devices[1], buffer));
        reject_buffer(buffer);
    }

    TEST_F(VulkanSynchronizationHostTest, DestroyedBufferRejectedAfterOwnedArrayEntry)
    {
        uint64_t buffer = 0;
        ASSERT_NO_FATAL_FAILURE(create_buffer(devices[0], buffer));
        host.destroy_buffer(devices[0], buffer);
        reject_buffer(buffer);
    }

    TEST_F(VulkanSynchronizationHostTest, WrongDeviceImageRejectedAfterOwnedArrayEntry)
    {
        uint64_t image = 0;
        ASSERT_NO_FATAL_FAILURE(create_image(devices[1], image));
        reject_image(image);
    }

    TEST_F(VulkanSynchronizationHostTest, DestroyedImageRejectedAfterOwnedArrayEntry)
    {
        uint64_t image = 0;
        ASSERT_NO_FATAL_FAILURE(create_image(devices[0], image));
        host.destroy_image(devices[0], image);
        reject_image(image);
    }

    TEST_F(VulkanSynchronizationHostTest, ZeroArrayBarrierRecordsAndSubmitsAnExecutionDependency)
    {
        sync::command command{};
        command.op = sync::operation::barrier;
        command.legacy.source_stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        command.legacy.destination_stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        ASSERT_EQ(dispatch(command), VK_SUCCESS);
        ASSERT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
        ASSERT_EQ(host.queue_submit(queue, command_buffer, 0), VK_SUCCESS);
        EXPECT_EQ(host.queue_wait_idle(queue), VK_SUCCESS);
    }
}
