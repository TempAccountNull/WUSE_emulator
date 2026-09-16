#include <gtest/gtest.h>
#include <vk_render_pass.hpp>
#include <array>
#include <cstring>

namespace sogen::test
{
    namespace copy_wire = gpu_bridge::render_pass_wire;

    TEST(VulkanImageReadbackWireTest, CompleteRegionArrayAndHandlesRoundTrip)
    {
        const std::array<VkBufferImageCopy2, 2> regions{{
            {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
             .pNext = nullptr,
             .bufferOffset = 0x123456789abcdef0ULL,
             .bufferRowLength = 37,
             .bufferImageHeight = 41,
             .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 3, 5, 7},
             .imageOffset = {-11, 13, -17},
             .imageExtent = {19, 23, 29}},
            {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
             .pNext = nullptr,
             .bufferOffset = 0xfedcba9876543210ULL,
             .bufferRowLength = 0,
             .bufferImageHeight = 0,
             .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 9, 1},
             .imageOffset = {31, -37, 43},
             .imageExtent = {47, 53, 59}},
        }};
        VkCopyImageToBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
        info.srcImage = copy_wire::handle_value<VkImage>(0x1234567887654321ULL);
        info.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        info.dstBuffer = copy_wire::handle_value<VkBuffer>(0xabcdef0198765432ULL);
        info.regionCount = static_cast<uint32_t>(regions.size());
        info.pRegions = regions.data();
        const auto bytes = copy_wire::encode(info);
        copy_wire::reader reader(bytes);
        VkCopyImageToBufferInfo2 decoded{};
        copy_wire::decode(reader, decoded);
        EXPECT_EQ(copy_wire::handle_id(decoded.srcImage), 0x1234567887654321ULL);
        EXPECT_EQ(copy_wire::handle_id(decoded.dstBuffer), 0xabcdef0198765432ULL);
        EXPECT_EQ(decoded.srcImageLayout, VK_IMAGE_LAYOUT_GENERAL);
        ASSERT_EQ(decoded.regionCount, 2u);
        EXPECT_NE(decoded.pRegions, info.pRegions);
        for (uint32_t i = 0; i < decoded.regionCount; ++i)
        {
            const auto& a = decoded.pRegions[i];
            const auto& b = regions[i];
            EXPECT_EQ(a.bufferOffset, b.bufferOffset);
            EXPECT_EQ(a.bufferRowLength, b.bufferRowLength);
            EXPECT_EQ(a.bufferImageHeight, b.bufferImageHeight);
            EXPECT_EQ(a.imageSubresource.aspectMask, b.imageSubresource.aspectMask);
            EXPECT_EQ(a.imageSubresource.mipLevel, b.imageSubresource.mipLevel);
            EXPECT_EQ(a.imageSubresource.baseArrayLayer, b.imageSubresource.baseArrayLayer);
            EXPECT_EQ(a.imageSubresource.layerCount, b.imageSubresource.layerCount);
            EXPECT_EQ(a.imageOffset.x, b.imageOffset.x);
            EXPECT_EQ(a.imageOffset.y, b.imageOffset.y);
            EXPECT_EQ(a.imageOffset.z, b.imageOffset.z);
            EXPECT_EQ(a.imageExtent.width, b.imageExtent.width);
            EXPECT_EQ(a.imageExtent.height, b.imageExtent.height);
            EXPECT_EQ(a.imageExtent.depth, b.imageExtent.depth);
        }
        EXPECT_EQ(copy_wire::encode(decoded), bytes);
    }

    TEST(VulkanImageReadbackWireTest, Copy2RegionTransformOwnsItsDecodedStorage)
    {
        VkCopyCommandTransformInfoQCOM transform{VK_STRUCTURE_TYPE_COPY_COMMAND_TRANSFORM_INFO_QCOM, nullptr,
                                                 VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR};
        const VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, &transform, 64,       16, 12,
                                        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 2},  {3, 5, 0},  {7, 9, 1}};
        VkCopyImageToBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
        info.regionCount = 1;
        info.pRegions = &region;
        const auto bytes = copy_wire::encode(info);
        copy_wire::reader reader(bytes);
        VkCopyImageToBufferInfo2 decoded{};
        copy_wire::decode(reader, decoded);
        const auto* node = static_cast<const VkCopyCommandTransformInfoQCOM*>(decoded.pRegions[0].pNext);
        ASSERT_NE(node, nullptr);
        EXPECT_NE(node, &transform);
        EXPECT_EQ(node->sType, VK_STRUCTURE_TYPE_COPY_COMMAND_TRANSFORM_INFO_QCOM);
        EXPECT_EQ(node->transform, VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
        EXPECT_EQ(node->pNext, nullptr);
    }

    TEST(VulkanImageReadbackWireTest, FixedWireHasNoNativePointersOrPadding)
    {
        const VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, nullptr,     0x0102030405060708ULL, 11, 13,
                                        {VK_IMAGE_ASPECT_COLOR_BIT, 2, 3, 4},  {-5, 7, -9}, {17, 19, 23}};
        const VkCopyImageToBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
                                            nullptr,
                                            copy_wire::handle_value<VkImage>(0x1234567887654321ULL),
                                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                            copy_wire::handle_value<VkBuffer>(0xabcdef0198765432ULL),
                                            1,
                                            &region};
        const std::array<uint32_t, 25> expected{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
                                                0,
                                                0x87654321,
                                                0x12345678,
                                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                0x98765432,
                                                0xabcdef01,
                                                1,
                                                1,
                                                VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                                                0,
                                                0x05060708,
                                                0x01020304,
                                                11,
                                                13,
                                                VK_IMAGE_ASPECT_COLOR_BIT,
                                                2,
                                                3,
                                                4,
                                                static_cast<uint32_t>(-5),
                                                7,
                                                static_cast<uint32_t>(-9),
                                                17,
                                                19,
                                                23};
        const auto bytes = copy_wire::encode(info);
        ASSERT_EQ(bytes.size(), sizeof(expected));
        EXPECT_EQ(std::memcmp(bytes.data(), expected.data(), bytes.size()), 0);
    }

    TEST(VulkanImageReadbackWireTest, RejectsRootRegionUnknownDuplicateAndCyclicChains)
    {
        VkBaseInStructure unknown{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr};
        VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
        VkCopyImageToBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, &unknown};
        info.regionCount = 1;
        info.pRegions = &region;
        EXPECT_THROW(copy_wire::encode(info), copy_wire::error);
        info.pNext = nullptr;
        region.pNext = &unknown;
        try
        {
            copy_wire::encode(info);
            FAIL() << "unknown region chain accepted";
        }
        catch (const copy_wire::error& error)
        {
            EXPECT_EQ(error.result, VK_ERROR_FEATURE_NOT_PRESENT);
        }
        VkCopyCommandTransformInfoQCOM transform{VK_STRUCTURE_TYPE_COPY_COMMAND_TRANSFORM_INFO_QCOM};
        VkCopyCommandTransformInfoQCOM duplicate{VK_STRUCTURE_TYPE_COPY_COMMAND_TRANSFORM_INFO_QCOM};
        region.pNext = &transform;
        transform.pNext = &duplicate;
        EXPECT_THROW(copy_wire::encode(info), copy_wire::error);
        transform.pNext = &transform;
        EXPECT_THROW(copy_wire::encode(info), copy_wire::error);
        transform.pNext = nullptr;
        region.pNext = nullptr;
        info.pNext = &transform;
        EXPECT_THROW(copy_wire::encode(info), copy_wire::error);
    }

    TEST(VulkanImageReadbackWireTest, RejectsTruncationTrailingDataAndInvalidRegionCounts)
    {
        VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
        VkCopyImageToBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
        info.regionCount = 1;
        info.pRegions = &region;
        auto bytes = copy_wire::encode(info);
        for (size_t size = 0; size < bytes.size(); ++size)
        {
            copy_wire::reader reader(std::span<const std::byte>(bytes.data(), size));
            VkCopyImageToBufferInfo2 decoded{};
            EXPECT_THROW(copy_wire::decode(reader, decoded), copy_wire::error) << size;
        }
        bytes.push_back(std::byte{});
        copy_wire::reader reader(bytes);
        VkCopyImageToBufferInfo2 decoded{};
        EXPECT_THROW(copy_wire::decode(reader, decoded), copy_wire::error);
        info.regionCount = 0;
        EXPECT_THROW(copy_wire::encode(info), copy_wire::error);
        info.regionCount = copy_wire::max_elements + 1;
        EXPECT_THROW(copy_wire::encode(info), copy_wire::error);
        info.regionCount = 1;
        info.pRegions = nullptr;
        EXPECT_THROW(copy_wire::encode(info), copy_wire::error);
    }
}
