#include <gtest/gtest.h>
#include <vk_render_pass.hpp>
#include <array>
#include <cstring>

namespace sogen::test
{
    namespace wire = gpu_bridge::render_pass_wire;

    TEST(VulkanLegacyRenderPassWireTest, PreservesCompleteAttachmentSubpassAndDependencyGraph)
    {
        std::array<VkAttachmentDescription, 5> attachments{};
        for (auto& attachment : attachments)
        {
            attachment = {.flags = VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT,
                          .format = VK_FORMAT_D24_UNORM_S8_UINT,
                          .samples = VK_SAMPLE_COUNT_4_BIT,
                          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                          .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        }
        const std::array<VkAttachmentReference, 2> colors{{{.attachment = 2, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                                           {.attachment = VK_ATTACHMENT_UNUSED, .layout = VK_IMAGE_LAYOUT_GENERAL}}};
        const std::array<VkAttachmentReference, 2> resolves{{{.attachment = 3, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                                             {.attachment = VK_ATTACHMENT_UNUSED, .layout = VK_IMAGE_LAYOUT_GENERAL}}};
        const VkAttachmentReference input{3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkAttachmentReference depth{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        const uint32_t preserve = 4;
        std::array<VkSubpassDescription, 2> subpasses{};
        subpasses[0].flags = VK_SUBPASS_DESCRIPTION_PER_VIEW_ATTRIBUTES_BIT_NVX;
        subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpasses[0].colorAttachmentCount = 2;
        subpasses[0].pColorAttachments = colors.data();
        subpasses[0].pResolveAttachments = resolves.data();
        subpasses[0].pDepthStencilAttachment = &depth;
        subpasses[0].preserveAttachmentCount = 1;
        subpasses[0].pPreserveAttachments = &preserve;
        subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpasses[1].inputAttachmentCount = 1;
        subpasses[1].pInputAttachments = &input;
        const std::array<VkSubpassDependency, 2> dependencies{{
            {.srcSubpass = VK_SUBPASS_EXTERNAL,
             .dstSubpass = 0,
             .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
             .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             .srcAccessMask = 0,
             .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
             .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT},
            {.srcSubpass = 0,
             .dstSubpass = 1,
             .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
             .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
             .dependencyFlags = VK_DEPENDENCY_VIEW_LOCAL_BIT},
        }};
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.flags = VK_RENDER_PASS_CREATE_TRANSFORM_BIT_QCOM;
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = static_cast<uint32_t>(subpasses.size());
        info.pSubpasses = subpasses.data();
        info.dependencyCount = static_cast<uint32_t>(dependencies.size());
        info.pDependencies = dependencies.data();
        const auto bytes = wire::encode(info);
        wire::reader reader(bytes);
        VkRenderPassCreateInfo decoded{};
        wire::decode(reader, decoded);
        EXPECT_EQ(decoded.flags, info.flags);
        ASSERT_EQ(decoded.attachmentCount, attachments.size());
        EXPECT_NE(decoded.pAttachments, info.pAttachments);
        EXPECT_EQ(std::memcmp(decoded.pAttachments, attachments.data(), sizeof(attachments)), 0);
        ASSERT_EQ(decoded.subpassCount, 2u);
        EXPECT_EQ(decoded.pSubpasses[0].flags, subpasses[0].flags);
        EXPECT_EQ(decoded.pSubpasses[0].pColorAttachments[0].attachment, 2u);
        EXPECT_EQ(decoded.pSubpasses[0].pColorAttachments[1].attachment, VK_ATTACHMENT_UNUSED);
        EXPECT_EQ(decoded.pSubpasses[0].pResolveAttachments[0].attachment, 3u);
        EXPECT_EQ(decoded.pSubpasses[0].pDepthStencilAttachment->attachment, 0u);
        EXPECT_EQ(decoded.pSubpasses[0].pPreserveAttachments[0], 4u);
        EXPECT_EQ(decoded.pSubpasses[1].pInputAttachments[0].layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EXPECT_EQ(decoded.pSubpasses[1].pResolveAttachments, nullptr);
        EXPECT_EQ(decoded.pSubpasses[1].pDepthStencilAttachment, nullptr);
        EXPECT_EQ(std::memcmp(decoded.pDependencies, dependencies.data(), sizeof(dependencies)), 0);
        EXPECT_EQ(wire::encode(decoded), bytes);
    }

    TEST(VulkanLegacyRenderPassWireTest, PreservesAllFivePinnedRootExtensionStructures)
    {
        const std::array<uint32_t, 2> masks{3, 12};
        const std::array<int32_t, 2> offsets{-3, 2};
        const std::array<uint32_t, 2> correlations{3, 12};
        const std::array<VkInputAttachmentAspectReference, 2> references{
            {{.subpass = 1, .inputAttachmentIndex = 0, .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT},
             {.subpass = 0, .inputAttachmentIndex = 2, .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT}}};
        VkRenderPassTileShadingCreateInfoQCOM tile{VK_STRUCTURE_TYPE_RENDER_PASS_TILE_SHADING_CREATE_INFO_QCOM};
        tile.flags = VK_TILE_SHADING_RENDER_PASS_ENABLE_BIT_QCOM;
        tile.tileApronSize = {.width = 7, .height = 13};
        VkTileMemorySizeInfoQCOM memory{VK_STRUCTURE_TYPE_TILE_MEMORY_SIZE_INFO_QCOM, &tile, 0x12345678abcdef01ULL};
        VkRenderPassFragmentDensityMapCreateInfoEXT density{VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT,
                                                            &memory,
                                                            {9, VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT}};
        VkRenderPassInputAttachmentAspectCreateInfo aspect{VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO, &density, 2,
                                                           references.data()};
        VkRenderPassMultiviewCreateInfo multiview{
            VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO, &aspect, 2, masks.data(), 2, offsets.data(), 2, correlations.data()};
        VkSubpassDescription subpass{};
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, &multiview};
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        const auto bytes = wire::encode(info);
        wire::reader reader(bytes);
        VkRenderPassCreateInfo decoded{};
        wire::decode(reader, decoded);
        const auto* views = static_cast<const VkRenderPassMultiviewCreateInfo*>(decoded.pNext);
        ASSERT_NE(views, nullptr);
        EXPECT_EQ(views->pViewMasks[1], 12u);
        EXPECT_EQ(views->pViewOffsets[0], -3);
        EXPECT_EQ(views->pCorrelationMasks[1], 12u);
        const auto* aspects = static_cast<const VkRenderPassInputAttachmentAspectCreateInfo*>(views->pNext);
        ASSERT_NE(aspects, nullptr);
        EXPECT_EQ(aspects->pAspectReferences[1].inputAttachmentIndex, 2u);
        EXPECT_EQ(aspects->pAspectReferences[1].aspectMask, VK_IMAGE_ASPECT_STENCIL_BIT);
        const auto* map = static_cast<const VkRenderPassFragmentDensityMapCreateInfoEXT*>(aspects->pNext);
        ASSERT_NE(map, nullptr);
        EXPECT_EQ(map->fragmentDensityMapAttachment.attachment, 9u);
        const auto* size = static_cast<const VkTileMemorySizeInfoQCOM*>(map->pNext);
        ASSERT_NE(size, nullptr);
        EXPECT_EQ(size->size, 0x12345678abcdef01ULL);
        const auto* shading = static_cast<const VkRenderPassTileShadingCreateInfoQCOM*>(size->pNext);
        ASSERT_NE(shading, nullptr);
        EXPECT_EQ(shading->flags, tile.flags);
        EXPECT_EQ(shading->tileApronSize.width, 7u);
        EXPECT_EQ(shading->tileApronSize.height, 13u);
        EXPECT_EQ(shading->pNext, nullptr);
        EXPECT_EQ(wire::encode(decoded), bytes);
    }

    TEST(VulkanLegacyRenderPassWireTest, AttachmentlessPassHasStablePointerFreeWireAndIgnoresZeroCountPointers)
    {
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pInputAttachments = reinterpret_cast<const VkAttachmentReference*>(uintptr_t{1});
        subpass.pColorAttachments = reinterpret_cast<const VkAttachmentReference*>(uintptr_t{1});
        subpass.pResolveAttachments = reinterpret_cast<const VkAttachmentReference*>(uintptr_t{1});
        subpass.pPreserveAttachments = reinterpret_cast<const uint32_t*>(uintptr_t{1});
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.pAttachments = reinterpret_cast<const VkAttachmentDescription*>(uintptr_t{1});
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.pDependencies = reinterpret_cast<const VkSubpassDependency*>(uintptr_t{1});
        const auto bytes = wire::encode(info);
        const std::array<uint32_t, 19> expected{
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 0, 0, 1, 1, 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        ASSERT_EQ(bytes.size(), sizeof(expected));
        EXPECT_EQ(std::memcmp(bytes.data(), expected.data(), sizeof(expected)), 0);
        wire::reader reader(bytes);
        VkRenderPassCreateInfo decoded{};
        wire::decode(reader, decoded);
        EXPECT_EQ(decoded.pAttachments, nullptr);
        EXPECT_EQ(decoded.pDependencies, nullptr);
        EXPECT_EQ(decoded.pSubpasses[0].pInputAttachments, nullptr);
        EXPECT_EQ(decoded.pSubpasses[0].pColorAttachments, nullptr);
        EXPECT_EQ(decoded.pSubpasses[0].pResolveAttachments, nullptr);
        EXPECT_EQ(decoded.pSubpasses[0].pPreserveAttachments, nullptr);
    }

    TEST(VulkanLegacyRenderPassWireTest, RejectsMalformedPacketsAndMissingRequiredArrays)
    {
        VkSubpassDescription subpass{};
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        auto bytes = wire::encode(info);
        for (size_t size = 0; size < bytes.size(); ++size)
        {
            wire::reader reader(std::span<const std::byte>(bytes.data(), size));
            VkRenderPassCreateInfo decoded{};
            EXPECT_THROW(wire::decode(reader, decoded), wire::error) << size;
        }
        bytes.push_back(std::byte{});
        wire::reader reader(bytes);
        VkRenderPassCreateInfo decoded{};
        EXPECT_THROW(wire::decode(reader, decoded), wire::error);
        info.attachmentCount = wire::max_elements + 1;
        EXPECT_THROW(wire::encode(info), wire::error);
        info.attachmentCount = 1;
        EXPECT_THROW(wire::encode(info), wire::error);
        info.attachmentCount = 0;
        subpass.colorAttachmentCount = 1;
        EXPECT_THROW(wire::encode(info), wire::error);
        const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        subpass.pColorAttachments = &color;
        EXPECT_NO_THROW(wire::encode(info));
        subpass.inputAttachmentCount = 1;
        EXPECT_THROW(wire::encode(info), wire::error);
        subpass.inputAttachmentCount = 0;
        subpass.preserveAttachmentCount = 1;
        EXPECT_THROW(wire::encode(info), wire::error);
    }

    TEST(VulkanLegacyRenderPassWireTest, RejectsUnknownWrongParentDuplicateAndCyclicChainsExplicitly)
    {
        VkBaseInStructure unknown{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr};
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, &unknown};
        try
        {
            wire::encode(info);
            FAIL() << "unsupported chain was accepted";
        }
        catch (const wire::error& error)
        {
            EXPECT_EQ(error.result, VK_ERROR_FEATURE_NOT_PRESENT);
        }
        VkAttachmentDescriptionStencilLayout wrong{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT};
        info.pNext = &wrong;
        EXPECT_THROW(wire::encode(info), wire::error);
        VkRenderPassMultiviewCreateInfo views{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
        VkRenderPassMultiviewCreateInfo duplicate{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
        info.pNext = &views;
        views.pNext = &duplicate;
        EXPECT_THROW(wire::encode(info), wire::error);
        views.pNext = &views;
        EXPECT_THROW(wire::encode(info), wire::error);
        views.pNext = nullptr;
        VkRenderPassCreateInfo2 info2{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, &views};
        EXPECT_THROW(wire::encode(info2), wire::error);
    }
}
