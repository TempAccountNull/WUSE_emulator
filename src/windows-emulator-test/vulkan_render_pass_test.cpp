#include <gtest/gtest.h>
#include <vk_render_pass.hpp>
#include <array>
#include <cstring>

namespace sogen::test
{
    namespace wire = gpu_bridge::render_pass_wire;

    TEST(VulkanRenderPassWireTest, NestedSubpassesAndCoreChainsRoundTrip)
    {
        VkAttachmentDescriptionStencilLayout stencil{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT};
        stencil.stencilInitialLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
        stencil.stencilFinalLayout = VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
        std::array<VkAttachmentDescription2, 3> attachments{};
        for (uint32_t i = 0; i < attachments.size(); ++i)
        {
            attachments[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            attachments[i].flags = i;
            attachments[i].format = VK_FORMAT_R8G8B8A8_UINT;
            attachments[i].samples = VK_SAMPLE_COUNT_4_BIT;
            attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[i].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachments[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        attachments[2].pNext = &stencil;
        VkAttachmentReferenceStencilLayout reference_stencil{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT};
        reference_stencil.stencilLayout = VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
        std::array<VkAttachmentReference2, 2> refs{};
        for (uint32_t i = 0; i < refs.size(); ++i)
        {
            refs[i] = {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
                       .pNext = nullptr,
                       .attachment = i,
                       .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
        }
        refs[1].pNext = &reference_stencil;
        VkSubpassDescriptionDepthStencilResolve resolve{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
        resolve.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        resolve.stencilResolveMode = VK_RESOLVE_MODE_MAX_BIT;
        resolve.pDepthStencilResolveAttachment = &refs[1];
        uint32_t preserve = 2;
        std::array<VkSubpassDescription2, 2> subpasses{};
        for (auto& subpass : subpasses)
        {
            subpass.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.viewMask = 3;
            subpass.colorAttachmentCount = 2;
            subpass.pColorAttachments = refs.data();
            subpass.preserveAttachmentCount = 1;
            subpass.pPreserveAttachments = &preserve;
        }
        subpasses[0].pNext = &resolve;
        subpasses[1].inputAttachmentCount = 2;
        subpasses[1].pInputAttachments = refs.data();
        subpasses[1].pResolveAttachments = refs.data();
        subpasses[1].pDepthStencilAttachment = &refs[1];
        VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        memory.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        memory.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        VkSubpassDependency2 dependency{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, &memory};
        dependency.srcSubpass = 0;
        dependency.dstSubpass = 1;
        dependency.viewOffset = -1;
        uint32_t correlated = 3;
        VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = static_cast<uint32_t>(subpasses.size());
        info.pSubpasses = subpasses.data();
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        info.correlatedViewMaskCount = 1;
        info.pCorrelatedViewMasks = &correlated;
        auto bytes = wire::encode(info);
        wire::reader reader(bytes);
        VkRenderPassCreateInfo2 decoded{};
        wire::decode(reader, decoded);
        EXPECT_EQ(decoded.attachmentCount, 3u);
        EXPECT_NE(decoded.pAttachments, info.pAttachments);
        EXPECT_EQ(decoded.pAttachments[2].flags, 2u);
        EXPECT_EQ(decoded.pAttachments[0].samples, VK_SAMPLE_COUNT_4_BIT);
        EXPECT_EQ(decoded.pSubpasses[0].pResolveAttachments, nullptr);
        ASSERT_NE(decoded.pSubpasses[1].pResolveAttachments, nullptr);
        EXPECT_EQ(decoded.pSubpasses[1].pResolveAttachments[1].attachment, 1u);
        EXPECT_EQ(decoded.pSubpasses[1].pPreserveAttachments[0], 2u);
        const auto* decoded_resolve = static_cast<const VkSubpassDescriptionDepthStencilResolve*>(decoded.pSubpasses[0].pNext);
        ASSERT_NE(decoded_resolve, nullptr);
        EXPECT_EQ(decoded_resolve->stencilResolveMode, VK_RESOLVE_MODE_MAX_BIT);
        EXPECT_EQ(decoded_resolve->pDepthStencilResolveAttachment->attachment, 1u);
        const auto* decoded_memory = static_cast<const VkMemoryBarrier2*>(decoded.pDependencies[0].pNext);
        ASSERT_NE(decoded_memory, nullptr);
        EXPECT_EQ(decoded_memory->srcStageMask, VK_PIPELINE_STAGE_2_COPY_BIT);
        EXPECT_EQ(decoded_memory->dstAccessMask, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        EXPECT_EQ(decoded.pDependencies[0].viewOffset, -1);
        EXPECT_EQ(wire::encode(decoded), bytes);
    }

    TEST(VulkanRenderPassWireTest, BeginPreservesHandlesOffsetIntegerAndStencilBits)
    {
        std::array<VkClearValue, 3> clear{};
        clear[0].color.uint32[0] = 0xf1234567;
        clear[1].color.int32[2] = -234567;
        clear[2].depthStencil = {.depth = 0.625f, .stencil = 0xa5};
        std::array<VkImageView, 2> views{wire::handle_value<VkImageView>(0x123456789abcULL),
                                         wire::handle_value<VkImageView>(0xdef012345678ULL)};
        VkRenderPassAttachmentBeginInfo attachments{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO, nullptr, 2, views.data()};
        VkRect2D rect{{7, 11}, {13, 17}};
        VkDeviceGroupRenderPassBeginInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO, &attachments, 3, 1, &rect};
        VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, &group};
        info.renderPass = wire::handle_value<VkRenderPass>(0xabcdef0123456789ULL);
        info.framebuffer = wire::handle_value<VkFramebuffer>(0x123456789abcdef0ULL);
        info.renderArea = {.offset = {.x = 3, .y = 5}, .extent = {.width = 19, .height = 23}};
        info.clearValueCount = 3;
        info.pClearValues = clear.data();
        VkSubpassBeginInfo begin{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, nullptr, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS};
        const auto bytes = wire::encode(info, begin);
        wire::reader reader(bytes);
        VkRenderPassBeginInfo decoded{};
        VkSubpassBeginInfo decoded_begin{};
        wire::decode(reader, decoded, decoded_begin);
        EXPECT_EQ(wire::handle_id(decoded.renderPass), 0xabcdef0123456789ULL);
        EXPECT_EQ(wire::handle_id(decoded.framebuffer), 0x123456789abcdef0ULL);
        EXPECT_EQ(decoded.renderArea.offset.x, 3);
        EXPECT_EQ(decoded.renderArea.offset.y, 5);
        EXPECT_EQ(std::memcmp(decoded.pClearValues, clear.data(), sizeof(clear)), 0);
        EXPECT_EQ(decoded_begin.contents, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        const auto* decoded_group = static_cast<const VkDeviceGroupRenderPassBeginInfo*>(decoded.pNext);
        EXPECT_EQ(decoded_group->pDeviceRenderAreas[0].offset.y, 11);
        const auto* decoded_attachments = static_cast<const VkRenderPassAttachmentBeginInfo*>(decoded_group->pNext);
        EXPECT_EQ(wire::handle_id(decoded_attachments->pAttachments[1]), 0xdef012345678ULL);
    }

    TEST(VulkanRenderPassWireTest, FramebufferPreservesAllAttachmentsLayersAndImagelessDescriptions)
    {
        std::array<VkImageView, 3> views{wire::handle_value<VkImageView>(11), wire::handle_value<VkImageView>(22),
                                         wire::handle_value<VkImageView>(33)};
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = wire::handle_value<VkRenderPass>(44);
        info.attachmentCount = 3;
        info.pAttachments = views.data();
        info.width = 128;
        info.height = 96;
        info.layers = 4;
        const auto bytes = wire::encode(info);
        wire::reader reader(bytes);
        VkFramebufferCreateInfo decoded{};
        wire::decode(reader, decoded);
        EXPECT_EQ(decoded.layers, 4u);
        EXPECT_EQ(wire::handle_id(decoded.pAttachments[2]), 33u);
        const std::array<VkFormat, 2> formats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
        VkFramebufferAttachmentImageInfo image{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO};
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        image.width = 128;
        image.height = 96;
        image.layerCount = 4;
        image.viewFormatCount = 2;
        image.pViewFormats = formats.data();
        VkFramebufferAttachmentsCreateInfo images{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO, nullptr, 1, &image};
        info.flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
        info.pNext = &images;
        info.attachmentCount = 1;
        // This field is ignored for imageless framebuffers and must not be dereferenced.
        info.pAttachments = reinterpret_cast<const VkImageView*>(uintptr_t{1});
        const auto imageless_bytes = wire::encode(info);
        wire::reader imageless_reader(imageless_bytes);
        wire::decode(imageless_reader, decoded);
        EXPECT_EQ(decoded.pAttachments, nullptr);
        const auto* decoded_images = static_cast<const VkFramebufferAttachmentsCreateInfo*>(decoded.pNext);
        EXPECT_EQ(decoded_images->pAttachmentImageInfos[0].pViewFormats[1], VK_FORMAT_R8G8B8A8_SRGB);
    }

    TEST(VulkanRenderPassWireTest, SampleLocationArraysRoundTrip)
    {
        const std::array<VkSampleLocationEXT, 2> points{{{.x = 0.25f, .y = 0.75f}, {.x = 0.75f, .y = 0.25f}}};
        VkSampleLocationsInfoEXT locations{
            VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT, nullptr, VK_SAMPLE_COUNT_2_BIT, {1, 1}, 2, points.data()};
        VkAttachmentSampleLocationsEXT attachment{2, locations};
        VkSubpassSampleLocationsEXT subpass{1, locations};
        VkRenderPassSampleLocationsBeginInfoEXT samples{
            VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT, nullptr, 1, &attachment, 1, &subpass};
        VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, &samples};
        const auto bytes = wire::encode(info);
        wire::reader reader(bytes);
        VkRenderPassBeginInfo decoded{};
        wire::decode(reader, decoded);
        const auto* chain = static_cast<const VkRenderPassSampleLocationsBeginInfoEXT*>(decoded.pNext);
        EXPECT_EQ(chain->pAttachmentInitialSampleLocations[0].attachmentIndex, 2u);
        EXPECT_EQ(chain->pPostSubpassSampleLocations[0].sampleLocationsInfo.pSampleLocations[1].x, 0.75f);
    }

    TEST(VulkanRenderPassWireTest, RejectsEveryTruncationTrailingBytesAndExcessiveCounts)
    {
        VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        auto bytes = wire::encode(info);
        for (size_t size = 0; size < bytes.size(); ++size)
        {
            wire::reader reader(std::span<const std::byte>(bytes.data(), size));
            VkRenderPassCreateInfo2 decoded{};
            EXPECT_THROW(wire::decode(reader, decoded), wire::error) << size;
        }
        bytes.push_back(std::byte{});
        wire::reader reader(bytes);
        VkRenderPassCreateInfo2 decoded{};
        EXPECT_THROW(wire::decode(reader, decoded), wire::error);
        info.attachmentCount = wire::max_elements + 1;
        EXPECT_THROW(wire::encode(info), wire::error);
        info.attachmentCount = 1;
        EXPECT_THROW(wire::encode(info), wire::error);
    }

    TEST(VulkanRenderPassWireTest, RejectsUnknownWrongParentDuplicateAndCyclicChains)
    {
        VkBaseInStructure unknown{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr};
        VkSubpassEndInfo end{VK_STRUCTURE_TYPE_SUBPASS_END_INFO, &unknown};
        EXPECT_THROW(wire::encode(end), wire::error);
        VkAttachmentReferenceStencilLayout stencil{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT};
        end.pNext = &stencil;
        EXPECT_THROW(wire::encode(end), wire::error);
        VkAttachmentReference2 ref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, &stencil};
        VkAttachmentReferenceStencilLayout duplicate = stencil;
        stencil.pNext = &duplicate;
        EXPECT_THROW(wire::encode(ref), wire::error);
        stencil.pNext = &stencil;
        EXPECT_THROW(wire::encode(ref), wire::error);
    }

    TEST(VulkanRenderPassWireTest, EmptySubpassBeginHasStablePointerFreeWire)
    {
        VkSubpassBeginInfo begin{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, nullptr, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS};
        const auto bytes = wire::encode(begin);
        const std::array<uint32_t, 3> expected{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, 0, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS};
        ASSERT_EQ(bytes.size(), sizeof(expected));
        EXPECT_EQ(std::memcmp(bytes.data(), expected.data(), sizeof(expected)), 0);
    }
}
