#include <gtest/gtest.h>
#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include "vulkan_multi_draw_shaders.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace sogen::test
{
    TEST(VulkanConditionalRenderingHostTest, AmdZeroSuppressesDrawAndOneRendersPixels)
    {
        vulkan_host host;
        if (!host.available()) GTEST_SKIP() << "Host Vulkan unavailable";
        struct resources
        {
            vulkan_host& host;
            uint64_t instance{}, device{}, pool{}, render_pass{}, image{}, view{}, framebuffer{};
            uint64_t image_memory{}, predicate_buffer{}, predicate_memory{}, readback_buffer{}, readback_memory{}, query_pool{};
            uint64_t layout{}, pipeline{}, vertex_shader{}, fragment_shader{};
            ~resources()
            {
                if (device)
                {
                    host.device_wait_idle(device);
                    if (query_pool) host.destroy_query_pool(device, query_pool);
                    if (framebuffer) host.destroy_framebuffer(device, framebuffer);
                    if (pipeline) host.destroy_pipeline(device, pipeline);
                    if (layout) host.destroy_pipeline_layout(device, layout);
                    if (vertex_shader) host.destroy_shader_module(device, vertex_shader);
                    if (fragment_shader) host.destroy_shader_module(device, fragment_shader);
                    if (view) host.destroy_image_view(device, view);
                    if (render_pass) host.destroy_render_pass(device, render_pass);
                    if (image) host.destroy_image(device, image);
                    if (predicate_buffer) host.destroy_buffer(device, predicate_buffer);
                    if (readback_buffer) host.destroy_buffer(device, readback_buffer);
                    if (pool) host.destroy_command_pool(device, pool);
                    if (image_memory) host.free_memory(device, image_memory);
                    if (predicate_memory) host.free_memory(device, predicate_memory);
                    if (readback_memory) host.free_memory(device, readback_memory);
                    host.destroy_device(device);
                }
                if (instance) host.destroy_instance(instance);
            }
        } owned{host};

        ASSERT_EQ(host.create_instance(owned.instance), VK_SUCCESS);
        std::array<uint64_t, 32> physical_devices{};
        uint32_t count = 0;
        ASSERT_EQ(host.enumerate_physical_devices(owned.instance, physical_devices, count), VK_SUCCESS);
        uint64_t physical = 0;
        VkPhysicalDeviceProperties properties{};
        for (uint32_t i = 0; i < std::min(count, static_cast<uint32_t>(physical_devices.size())); ++i)
        {
            VkPhysicalDeviceProperties candidate{};
            ASSERT_EQ(host.get_physical_device_properties(physical_devices[i], &candidate, sizeof(candidate), false), VK_SUCCESS);
            if (candidate.vendorID == 0x1002) { physical = physical_devices[i]; properties = candidate; break; }
        }
        if (!physical) GTEST_SKIP() << "No AMD Vulkan device";
        SCOPED_TRACE(properties.deviceName);

        uint32_t extension_count = 0;
        ASSERT_EQ(host.enumerate_device_extension_properties(physical, nullptr, 0, extension_count), VK_SUCCESS);
        std::vector<VkExtensionProperties> extensions(extension_count);
        ASSERT_EQ(host.enumerate_device_extension_properties(physical, extensions.data(),
                                                              extensions.size() * sizeof(extensions[0]), extension_count), VK_SUCCESS);
        const bool extension_available = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME) == 0;
        });
        if (!extension_available) GTEST_SKIP() << "AMD driver lacks VK_EXT_conditional_rendering";

        constexpr gpu_bridge::feature_chain_record feature_record{
            .s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .body_size = sizeof(VkPhysicalDeviceFeatures)};
        std::vector<std::byte> feature_blob;
        ASSERT_EQ(host.get_physical_device_features2(physical, &feature_record, sizeof(feature_record), 1, feature_blob), VK_SUCCESS);
        ASSERT_EQ(feature_blob.size(), sizeof(feature_record) + sizeof(VkPhysicalDeviceFeatures));
        VkPhysicalDeviceFeatures supported{};
        std::memcpy(&supported, feature_blob.data() + sizeof(feature_record), sizeof(supported));
        if (supported.pipelineStatisticsQuery != VK_TRUE)
            GTEST_SKIP() << "AMD device lacks pipeline-statistics queries";
        VkPhysicalDeviceFeatures enabled{};
        enabled.pipelineStatisticsQuery = VK_TRUE;
        std::memcpy(feature_blob.data() + sizeof(feature_record), &enabled, sizeof(enabled));
        constexpr gpu_bridge::feature_chain_record conditional_record{
            .s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT,
            .body_size = 2 * sizeof(VkBool32)};
        std::vector<std::byte> conditional_blob;
        ASSERT_EQ(host.get_physical_device_features2(physical, &conditional_record, sizeof(conditional_record), 1,
                                                     conditional_blob), VK_SUCCESS);
        ASSERT_EQ(conditional_blob.size(), sizeof(conditional_record) + 2 * sizeof(VkBool32));
        VkBool32 conditional_available = VK_FALSE;
        std::memcpy(&conditional_available, conditional_blob.data() + sizeof(conditional_record), sizeof(VkBool32));
        if (conditional_available != VK_TRUE) GTEST_SKIP() << "AMD driver has conditionalRendering feature disabled";
        const std::array<VkBool32, 2> conditional_enabled{VK_TRUE, VK_FALSE};
        std::memcpy(conditional_blob.data() + sizeof(conditional_record), conditional_enabled.data(),
                    sizeof(conditional_enabled));
        feature_blob.insert(feature_blob.end(), conditional_blob.begin(), conditional_blob.end());
        std::array<gpu_bridge::queue_family_properties, 64> families{};
        uint32_t family_count = 0;
        ASSERT_EQ(host.get_queue_family_properties(physical, false, families.data(), sizeof(families), family_count), VK_SUCCESS);
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < std::min(family_count, static_cast<uint32_t>(families.size())); ++i)
            if (families[i].queue_count && (families[i].queue_flags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
        ASSERT_NE(family, UINT32_MAX);
        const gpu_bridge::device_queue_create_entry queue{.queue_family_index = family, .queue_count = 1};
        constexpr char extension_name[] = VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME;
        ASSERT_EQ(host.create_device(physical, &queue, 1, extension_name, sizeof(extension_name), 1,
                                     feature_blob.data(), feature_blob.size(), 2,
                                     owned.device), VK_SUCCESS);

        VkPhysicalDeviceMemoryProperties memory_properties{};
        ASSERT_EQ(host.get_physical_device_memory_properties(physical, &memory_properties, sizeof(memory_properties)), VK_SUCCESS);
        const auto allocate = [&](uint64_t size, uint32_t bits, uint32_t flags, uint64_t& memory) {
            for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & flags) == flags)
                    return host.allocate_memory(owned.device, size, i, 0, 0, memory);
            return int32_t(VK_ERROR_MEMORY_MAP_FAILED);
        };

        constexpr uint32_t width = 32, height = 32;
        ASSERT_EQ(host.create_render_pass(owned.device, VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, owned.render_pass), VK_SUCCESS);
        ASSERT_EQ(host.create_image(owned.device, VK_FORMAT_R8G8B8A8_UNORM, width, height,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    VK_IMAGE_TILING_OPTIMAL, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TYPE_2D, 1, 1, 1, 0,
                                    owned.image), VK_SUCCESS);
        uint64_t size = 0, alignment = 0;
        uint32_t bits = 0;
        ASSERT_EQ(host.get_image_memory_requirements(owned.device, owned.image, size, alignment, bits), VK_SUCCESS);
        ASSERT_EQ(allocate(size, bits, 0, owned.image_memory), VK_SUCCESS);
        ASSERT_EQ(host.bind_image_memory(owned.device, owned.image, owned.image_memory, 0), VK_SUCCESS);
        ASSERT_EQ(host.create_image_view(owned.device, owned.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                         VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1, VK_COMPONENT_SWIZZLE_IDENTITY,
                                         VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                         VK_COMPONENT_SWIZZLE_IDENTITY, owned.view), VK_SUCCESS);
        ASSERT_EQ(host.create_framebuffer(owned.device, owned.render_pass, owned.view, 0, width, height, owned.framebuffer), VK_SUCCESS);
        ASSERT_EQ(host.create_shader_module(owned.device, 0, multi_draw_shaders::vert.data(),
                                            sizeof(multi_draw_shaders::vert), owned.vertex_shader), VK_SUCCESS);
        ASSERT_EQ(host.create_shader_module(owned.device, 0, multi_draw_shaders::frag.data(),
                                            sizeof(multi_draw_shaders::frag), owned.fragment_shader), VK_SUCCESS);
        ASSERT_EQ(host.create_pipeline_layout(owned.device, 0, 0, {}, owned.layout), VK_SUCCESS);
        const vulkan_host::shader_stage_source vertex{.module = owned.vertex_shader};
        const vulkan_host::shader_stage_source fragment{.module = owned.fragment_shader};
        const vulkan_host::depth_state no_depth{};
        const vulkan_host::specialization no_specialization{};
        ASSERT_EQ(host.create_graphics_pipeline(owned.device, 0, owned.render_pass, owned.layout, vertex, fragment,
                                                0, width, height, {}, {}, {}, no_depth, {}, 0, 0,
                                                VK_SAMPLE_COUNT_1_BIT, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0, 0,
                                                {}, no_specialization, no_specialization, {}, owned.pipeline), VK_SUCCESS);

        const std::array<uint32_t, 2> predicates{0u, 1u};
        ASSERT_EQ(host.create_buffer(owned.device, sizeof(predicates), VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT,
                                     owned.predicate_buffer), VK_SUCCESS);
        ASSERT_EQ(host.get_buffer_memory_requirements(owned.device, owned.predicate_buffer, size, alignment, bits), VK_SUCCESS);
        ASSERT_EQ(allocate(size, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           owned.predicate_memory), VK_SUCCESS);
        ASSERT_EQ(host.bind_buffer_memory(owned.device, owned.predicate_buffer, owned.predicate_memory, 0), VK_SUCCESS);
        ASSERT_EQ(host.upload_memory(owned.device, owned.predicate_memory, 0, sizeof(predicates), predicates.data(),
                                     sizeof(predicates)), VK_SUCCESS);
        ASSERT_EQ(host.create_buffer(owned.device, width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     owned.readback_buffer), VK_SUCCESS);
        ASSERT_EQ(host.get_buffer_memory_requirements(owned.device, owned.readback_buffer, size, alignment, bits), VK_SUCCESS);
        ASSERT_EQ(allocate(size, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           owned.readback_memory), VK_SUCCESS);
        ASSERT_EQ(host.bind_buffer_memory(owned.device, owned.readback_buffer, owned.readback_memory, 0), VK_SUCCESS);
        ASSERT_EQ(host.create_query_pool(owned.device, VK_QUERY_TYPE_PIPELINE_STATISTICS, 1,
                                         VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT, owned.query_pool), VK_SUCCESS);
        ASSERT_EQ(host.create_command_pool(owned.device, family, 0, owned.pool), VK_SUCCESS);
        uint64_t queue_id = 0;
        ASSERT_EQ(host.get_device_queue(owned.device, family, 0, queue_id), VK_SUCCESS);
        for (uint32_t predicate = 0; predicate != 2; ++predicate)
        {
            uint64_t command_buffer = 0;
            ASSERT_EQ(host.allocate_command_buffer(owned.device, owned.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                   command_buffer), VK_SUCCESS);
            ASSERT_EQ(host.begin_command_buffer(command_buffer, 0, false, 0, {}, 0, 0, 1, 0), VK_SUCCESS);
            ASSERT_EQ(host.cmd_reset_query_pool(command_buffer, owned.query_pool, 0, 1), VK_SUCCESS);
            ASSERT_EQ(host.cmd_begin_query(command_buffer, owned.query_pool, 0, 0), VK_SUCCESS);
            ASSERT_EQ(host.cmd_begin_render_pass(command_buffer, owned.render_pass, owned.framebuffer, width, height,
                                                 0, 0, 0, 1, 1), VK_SUCCESS);
            ASSERT_EQ(host.cmd_bind_pipeline(command_buffer, owned.pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS), VK_SUCCESS);
            ASSERT_EQ(host.cmd_begin_conditional_rendering(command_buffer, owned.predicate_buffer, 4 * predicate, 0), VK_SUCCESS);
            ASSERT_EQ(host.cmd_draw(command_buffer, 3, 1, 0, 0), VK_SUCCESS);
            ASSERT_EQ(host.cmd_end_conditional_rendering(command_buffer), VK_SUCCESS);
            ASSERT_EQ(host.cmd_end_render_pass(command_buffer), VK_SUCCESS);
            ASSERT_EQ(host.cmd_end_query(command_buffer, owned.query_pool, 0), VK_SUCCESS);
            const vulkan_host::subresource_range range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ASSERT_EQ(host.cmd_pipeline_barrier(command_buffer, owned.image, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                                VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, range), VK_SUCCESS);
            ASSERT_EQ(host.cmd_copy_image_to_buffer(command_buffer, owned.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                    owned.readback_buffer, width, height, VK_IMAGE_ASPECT_COLOR_BIT), VK_SUCCESS);
            ASSERT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
            ASSERT_EQ(host.queue_submit(queue_id, command_buffer, 0), VK_SUCCESS);
            ASSERT_EQ(host.queue_wait_idle(queue_id), VK_SUCCESS);
            uint64_t vertices = 0;
            size_t written = 0;
            ASSERT_EQ(host.get_query_pool_results(owned.device, owned.query_pool, 0, 1,
                                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT,
                                                  &vertices, sizeof(vertices), sizeof(vertices), written), VK_SUCCESS);
            ASSERT_EQ(written, sizeof(vertices));
            std::array<uint8_t, width * height * 4> pixels{};
            ASSERT_EQ(host.download_memory(owned.device, owned.readback_memory, 0, pixels.size(), pixels.data(),
                                           pixels.size()), VK_SUCCESS);
            uint32_t colored = 0;
            for (size_t i = 0; i < pixels.size(); i += 4)
                if (pixels[i] || pixels[i + 1] || pixels[i + 2]) ++colored;
            std::cout << "Native " << properties.deviceName << " conditional predicate=" << predicate
                      << ": IA vertices=" << vertices << ", colored pixels=" << colored << '/' << width * height << '\n';
            EXPECT_EQ(vertices, predicate ? 3u : 0u);
            if (predicate) EXPECT_GE(colored, 100u) << "Nonzero predicate must permit the triangle";
            else EXPECT_EQ(colored, 0u) << "Zero predicate must suppress the triangle";
        }
    }
}
