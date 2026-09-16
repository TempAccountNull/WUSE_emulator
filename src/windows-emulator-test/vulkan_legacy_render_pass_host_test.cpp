#include <gtest/gtest.h>
#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include <vk_render_pass.hpp>
#include <algorithm>
#include <array>

namespace sogen::test
{
    namespace legacy_wire = gpu_bridge::render_pass_wire;

    class VulkanLegacyRenderPassHostTest : public testing::Test
    {
      protected:
        vulkan_host host;
        uint64_t instance{};
        uint64_t device{};
        uint32_t api_version{};

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
            VkPhysicalDeviceProperties properties{};
            ASSERT_EQ(host.get_physical_device_properties(physical[0], &properties, sizeof(properties), false), VK_SUCCESS);
            api_version = properties.apiVersion;
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
            ASSERT_EQ(host.create_device(physical[0], &entry, 1, nullptr, 0, 0, nullptr, 0, 0, device), VK_SUCCESS);
        }

        void TearDown() override
        {
            if (device)
            {
                host.destroy_device(device);
            }
            if (instance)
            {
                host.destroy_instance(instance);
            }
        }
    };

    TEST_F(VulkanLegacyRenderPassHostTest, CreatesAttachmentlessPassAndQueriesNativeGranularity)
    {
        const VkSubpassDescription subpass{};
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        const auto packet = legacy_wire::encode(info);
        uint64_t render_pass = 0;
        ASSERT_EQ(host.create_render_pass_full(device, packet, render_pass), VK_SUCCESS);
        ASSERT_NE(render_pass, 0u);
        uint32_t width = 0;
        uint32_t height = 0;
        EXPECT_EQ(host.get_render_area_granularity(device, render_pass, width, height), VK_SUCCESS);
        EXPECT_GT(width, 0u);
        EXPECT_GT(height, 0u);
        host.destroy_render_pass(device, render_pass);
        EXPECT_EQ(host.get_render_area_granularity(device, render_pass, width, height), VK_ERROR_INITIALIZATION_FAILED);
    }

    TEST_F(VulkanLegacyRenderPassHostTest, CreatesReorderedColorAndInputAttachmentsWithLegacyAspectChain)
    {
        if (api_version < VK_API_VERSION_1_1)
        {
            GTEST_SKIP() << "Legacy multiview/aspect structures require Vulkan 1.1 on this fixture's unextended device";
        }
        std::array<VkAttachmentDescription, 3> attachments{};
        for (auto& attachment : attachments)
        {
            attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        const std::array<VkAttachmentReference, 2> colors{{{.attachment = 2, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                                           {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}};
        const VkAttachmentReference input{2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const uint32_t preserve = 1;
        std::array<VkSubpassDescription, 2> subpasses{};
        subpasses[0].colorAttachmentCount = 2;
        subpasses[0].pColorAttachments = colors.data();
        subpasses[1].inputAttachmentCount = 1;
        subpasses[1].pInputAttachments = &input;
        subpasses[1].colorAttachmentCount = 1;
        subpasses[1].pColorAttachments = &colors[1];
        subpasses[1].preserveAttachmentCount = 1;
        subpasses[1].pPreserveAttachments = &preserve;
        const VkSubpassDependency dependency{0,
                                             1,
                                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                             VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                             VK_DEPENDENCY_BY_REGION_BIT};
        const VkInputAttachmentAspectReference aspect_reference{1, 0, VK_IMAGE_ASPECT_COLOR_BIT};
        const VkRenderPassInputAttachmentAspectCreateInfo aspect{VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO, nullptr,
                                                                 1, &aspect_reference};
        const std::array<uint32_t, 2> view_masks{};
        const int32_t view_offset = 0;
        const VkRenderPassMultiviewCreateInfo views{
            VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO, &aspect, 2, view_masks.data(), 1, &view_offset, 0, nullptr};
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, &views};
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = static_cast<uint32_t>(subpasses.size());
        info.pSubpasses = subpasses.data();
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        const auto bytes = legacy_wire::encode(info);
        uint64_t render_pass = 0;
        ASSERT_EQ(host.create_render_pass_full(device, bytes, render_pass), VK_SUCCESS);
        ASSERT_NE(render_pass, 0u);
        EXPECT_EQ(legacy_wire::encode(info), bytes);
        host.destroy_render_pass(device, render_pass);
    }

    TEST_F(VulkanLegacyRenderPassHostTest, RejectsInvalidDeviceAndPacketWithoutPublishingAnObject)
    {
        const VkSubpassDescription subpass{};
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        const auto bytes = legacy_wire::encode(info);
        uint64_t render_pass = UINT64_MAX;
        EXPECT_EQ(host.create_render_pass_full(0, bytes, render_pass), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(render_pass, 0u);
        render_pass = UINT64_MAX;
        EXPECT_EQ(host.create_render_pass_full(device, {}, render_pass), VK_ERROR_UNKNOWN);
        EXPECT_EQ(render_pass, 0u);
        auto truncated = bytes;
        truncated.pop_back();
        render_pass = UINT64_MAX;
        EXPECT_EQ(host.create_render_pass_full(device, truncated, render_pass), VK_ERROR_UNKNOWN);
        EXPECT_EQ(render_pass, 0u);
        ASSERT_EQ(host.create_render_pass_full(device, bytes, render_pass), VK_SUCCESS);
        ASSERT_NE(render_pass, 0u);
        const auto destroyed_device = device;
        host.destroy_device(device);
        device = 0;
        render_pass = UINT64_MAX;
        EXPECT_EQ(host.create_render_pass_full(destroyed_device, bytes, render_pass), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(render_pass, 0u);
    }
}
