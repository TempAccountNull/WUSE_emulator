#include <gtest/gtest.h>
#include <vk_synchronization.hpp>
#include <array>
#include <cstring>

namespace sogen::test
{
    namespace sync = gpu_bridge::synchronization_wire;
    namespace codec = gpu_bridge::render_pass_wire;

    TEST(VulkanSynchronizationWireTest, LegacyPreservesAllArraysRangesAndQueueFamilies)
    {
        VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        VkBufferMemoryBarrier buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        buffer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        buffer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        buffer.srcQueueFamilyIndex = 7;
        buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        buffer.buffer = codec::handle_value<VkBuffer>(0x123456789ULL);
        buffer.offset = 0x100000002ULL;
        buffer.size = VK_WHOLE_SIZE;
        VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image.dstQueueFamilyIndex = 3;
        image.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        image.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        image.image = codec::handle_value<VkImage>(0xabcdef012ULL);
        image.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT, .baseMipLevel = 2, .levelCount = 3, .baseArrayLayer = 4, .layerCount = 5};
        VkExternalMemoryAcquireUnmodifiedEXT acquire{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_ACQUIRE_UNMODIFIED_EXT, nullptr, VK_TRUE};
        buffer.pNext = &acquire;
        sync::command command{};
        command.op = sync::operation::barrier;
        command.legacy = {.source_stages = VK_PIPELINE_STAGE_TRANSFER_BIT,
                          .destination_stages = VK_PIPELINE_STAGE_HOST_BIT,
                          .flags = VK_DEPENDENCY_BY_REGION_BIT,
                          .memory_count = 1,
                          .memory = &memory,
                          .buffer_count = 1,
                          .buffers = &buffer,
                          .image_count = 1,
                          .images = &image};
        const auto bytes = sync::encode(command);
        codec::reader reader(bytes);
        sync::command decoded{};
        sync::decode(reader, decoded);
        EXPECT_EQ(decoded.legacy.memory[0].dstAccessMask, VK_ACCESS_HOST_READ_BIT);
        EXPECT_EQ(decoded.legacy.flags, VK_DEPENDENCY_BY_REGION_BIT);
        EXPECT_EQ(decoded.legacy.buffers[0].offset, 0x100000002ULL);
        EXPECT_EQ(decoded.legacy.buffers[0].size, VK_WHOLE_SIZE);
        EXPECT_EQ(decoded.legacy.buffers[0].dstQueueFamilyIndex, VK_QUEUE_FAMILY_EXTERNAL);
        EXPECT_EQ(codec::handle_id(decoded.legacy.buffers[0].buffer), 0x123456789ULL);
        EXPECT_EQ(decoded.legacy.images[0].oldLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        EXPECT_EQ(decoded.legacy.images[0].subresourceRange.baseArrayLayer, 4u);
        EXPECT_EQ(decoded.legacy.images[0].subresourceRange.layerCount, 5u);
        EXPECT_EQ(static_cast<const VkExternalMemoryAcquireUnmodifiedEXT*>(decoded.legacy.buffers[0].pNext)->acquireUnmodifiedMemory,
                  VK_TRUE);
        EXPECT_EQ(sync::encode(decoded), bytes);
    }

    TEST(VulkanSynchronizationWireTest, Synchronization2PreservesNative64BitMasksAndSampleLocations)
    {
        VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        memory.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        memory.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        VkBufferMemoryBarrier2 buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        buffer.srcStageMask = memory.srcStageMask;
        buffer.dstStageMask = memory.dstStageMask;
        buffer.srcAccessMask = memory.srcAccessMask;
        buffer.dstAccessMask = memory.dstAccessMask;
        buffer.buffer = codec::handle_value<VkBuffer>(12);
        buffer.size = VK_WHOLE_SIZE;
        VkMemoryBarrierAccessFlags3KHR access3{VK_STRUCTURE_TYPE_MEMORY_BARRIER_ACCESS_FLAGS_3_KHR};
        access3.srcAccessMask3 = 1ULL << 42;
        access3.dstAccessMask3 = 1ULL << 43;
        buffer.pNext = &access3;
        VkSampleLocationEXT point{0.25f, 0.75f};
        VkSampleLocationsInfoEXT samples{VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT, nullptr, VK_SAMPLE_COUNT_1_BIT, {1, 1}, 1, &point};
        VkImageMemoryBarrier2 image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, &samples};
        image.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        image.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        image.image = codec::handle_value<VkImage>(34);
        image.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        sync::command command{};
        command.op = sync::operation::barrier2;
        command.dependency.memoryBarrierCount = 1;
        command.dependency.pMemoryBarriers = &memory;
        command.dependency.bufferMemoryBarrierCount = 1;
        command.dependency.pBufferMemoryBarriers = &buffer;
        command.dependency.imageMemoryBarrierCount = 1;
        command.dependency.pImageMemoryBarriers = &image;
        const auto bytes = sync::encode(command);
        codec::reader reader(bytes);
        sync::command decoded{};
        sync::decode(reader, decoded);
        EXPECT_EQ(decoded.dependency.pMemoryBarriers[0].srcStageMask, VK_PIPELINE_STAGE_2_COPY_BIT);
        EXPECT_EQ(decoded.dependency.pBufferMemoryBarriers[0].dstAccessMask, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        EXPECT_EQ(static_cast<const VkMemoryBarrierAccessFlags3KHR*>(decoded.dependency.pBufferMemoryBarriers[0].pNext)->srcAccessMask3,
                  1ULL << 42);
        EXPECT_EQ(static_cast<const VkSampleLocationsInfoEXT*>(decoded.dependency.pImageMemoryBarriers[0].pNext)->pSampleLocations[0].y,
                  0.75f);
        EXPECT_EQ(sync::encode(decoded), bytes);
    }

    TEST(VulkanSynchronizationWireTest, WaitEvents2KeepsOneDependencyPerEvent)
    {
        const std::array<VkEvent, 2> events{codec::handle_value<VkEvent>(0x12345678abcdefULL),
                                            codec::handle_value<VkEvent>(0xfedcba98765432ULL)};
        std::array<VkMemoryBarrier2, 2> memory{};
        std::array<VkDependencyInfo, 2> dependencies{};
        for (uint32_t i = 0; i < dependencies.size(); ++i)
        {
            memory[i].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            memory[i].srcStageMask = 1ULL << (32 + i);
            dependencies[i].sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependencies[i].memoryBarrierCount = 1;
            dependencies[i].pMemoryBarriers = &memory[i];
        }
        sync::command command{};
        command.op = sync::operation::wait_events2;
        command.event_count = 2;
        command.events = events.data();
        command.dependencies = dependencies.data();
        const auto bytes = sync::encode(command);
        codec::reader reader(bytes);
        sync::command decoded{};
        sync::decode(reader, decoded);
        EXPECT_EQ(codec::handle_id(decoded.events[1]), 0xfedcba98765432ULL);
        EXPECT_EQ(decoded.dependencies[0].pMemoryBarriers[0].srcStageMask, 1ULL << 32);
        EXPECT_EQ(decoded.dependencies[1].pMemoryBarriers[0].srcStageMask, 1ULL << 33);
        command.dependencies = nullptr;
        EXPECT_THROW(sync::encode(command), codec::error);
    }

    TEST(VulkanSynchronizationWireTest, ResetEvent2HasStableTwentyByteWire)
    {
        sync::command command{};
        command.op = sync::operation::reset_event2;
        command.event = codec::handle_value<VkEvent>(0x0123456789abcdefULL);
        command.stage2 = VK_PIPELINE_STAGE_2_COPY_BIT;
        const auto bytes = sync::encode(command);
        ASSERT_EQ(bytes.size(), 20u);
        uint64_t handle = 0;
        uint64_t stage = 0;
        std::memcpy(&handle, bytes.data() + 4, 8);
        std::memcpy(&stage, bytes.data() + 12, 8);
        EXPECT_EQ(handle, 0x0123456789abcdefULL);
        EXPECT_EQ(stage, VK_PIPELINE_STAGE_2_COPY_BIT);
    }

    TEST(VulkanSynchronizationWireTest, RejectsTruncationsUnknownOperationsAndUnknownChains)
    {
        sync::command command{};
        command.op = sync::operation::barrier2;
        const auto bytes = sync::encode(command);
        for (size_t length = 0; length < bytes.size(); ++length)
        {
            codec::reader reader({bytes.data(), length});
            sync::command decoded{};
            EXPECT_THROW(sync::decode(reader, decoded), codec::error) << length;
        }
        command.op = static_cast<sync::operation>(0xffff);
        EXPECT_THROW(sync::encode(command), codec::error);
        command.op = sync::operation::barrier2;
        VkBaseInStructure unsupported{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr};
        command.dependency.pNext = &unsupported;
        EXPECT_THROW(sync::encode(command), codec::error);
        command = {};
        command.op = sync::operation::wait_events;
        EXPECT_THROW(sync::encode(command), codec::error);
    }
}
