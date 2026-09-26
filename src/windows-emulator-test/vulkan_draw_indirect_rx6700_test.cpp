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
    TEST(VulkanDrawIndirectHostTest, AmdStridedDrawsProduceSixInputAssemblyVertices)
    {
        vulkan_host host;
        if (!host.available()) GTEST_SKIP() << "Host Vulkan unavailable";
        struct resources
        {
            vulkan_host& host;
            uint64_t instance{}, device{}, pool{}, render_pass{}, image{}, view{}, framebuffer{};
            uint64_t image_memory{}, indirect_buffer{}, indirect_memory{}, readback_buffer{}, readback_memory{}, query_pool{};
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
                    if (indirect_buffer) host.destroy_buffer(device, indirect_buffer);
                    if (readback_buffer) host.destroy_buffer(device, readback_buffer);
                    if (pool) host.destroy_command_pool(device, pool);
                    if (image_memory) host.free_memory(device, image_memory);
                    if (indirect_memory) host.free_memory(device, indirect_memory);
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
        std::array<gpu_bridge::queue_family_properties, 64> families{};
        uint32_t family_count = 0;
        ASSERT_EQ(host.get_queue_family_properties(physical, false, families.data(), sizeof(families), family_count), VK_SUCCESS);
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < std::min(family_count, static_cast<uint32_t>(families.size())); ++i)
            if (families[i].queue_count && (families[i].queue_flags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
        ASSERT_NE(family, UINT32_MAX);
        const gpu_bridge::device_queue_create_entry queue{.queue_family_index = family, .queue_count = 1};
        ASSERT_EQ(host.create_device(physical, &queue, 1, nullptr, 0, 0, feature_blob.data(), feature_blob.size(), 1,
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

        // The first record is deliberately empty. The actual draws start at offset 16 and have a
        // 24-byte stride, so observing six IA vertices proves both offset and stride were consumed.
        std::array<std::byte, 64> commands{};
        const VkDrawIndirectCommand draw{.vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0};
        std::memcpy(commands.data() + 16, &draw, sizeof(draw));
        std::memcpy(commands.data() + 40, &draw, sizeof(draw));
        ASSERT_EQ(host.create_buffer(owned.device, commands.size(), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                     owned.indirect_buffer), VK_SUCCESS);
        ASSERT_EQ(host.get_buffer_memory_requirements(owned.device, owned.indirect_buffer, size, alignment, bits), VK_SUCCESS);
        ASSERT_EQ(allocate(size, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           owned.indirect_memory), VK_SUCCESS);
        ASSERT_EQ(host.bind_buffer_memory(owned.device, owned.indirect_buffer, owned.indirect_memory, 0), VK_SUCCESS);
        ASSERT_EQ(host.upload_memory(owned.device, owned.indirect_memory, 0, commands.size(), commands.data(),
                                     commands.size()), VK_SUCCESS);
        ASSERT_EQ(host.create_buffer(owned.device, width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     owned.readback_buffer), VK_SUCCESS);
        ASSERT_EQ(host.get_buffer_memory_requirements(owned.device, owned.readback_buffer, size, alignment, bits), VK_SUCCESS);
        ASSERT_EQ(allocate(size, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           owned.readback_memory), VK_SUCCESS);
        ASSERT_EQ(host.bind_buffer_memory(owned.device, owned.readback_buffer, owned.readback_memory, 0), VK_SUCCESS);
        ASSERT_EQ(host.create_query_pool(owned.device, VK_QUERY_TYPE_PIPELINE_STATISTICS, 1,
                                         VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT, owned.query_pool), VK_SUCCESS);
        ASSERT_EQ(host.create_command_pool(owned.device, family, 0, owned.pool), VK_SUCCESS);
        uint64_t command_buffer = 0, queue_id = 0;
        ASSERT_EQ(host.allocate_command_buffer(owned.device, owned.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, command_buffer), VK_SUCCESS);
        ASSERT_EQ(host.get_device_queue(owned.device, family, 0, queue_id), VK_SUCCESS);
        ASSERT_EQ(host.begin_command_buffer(command_buffer, 0, false, 0, {}, 0, 0, 1, 0), VK_SUCCESS);
        ASSERT_EQ(host.cmd_reset_query_pool(command_buffer, owned.query_pool, 0, 1), VK_SUCCESS);
        ASSERT_EQ(host.cmd_begin_query(command_buffer, owned.query_pool, 0, 0), VK_SUCCESS);
        ASSERT_EQ(host.cmd_begin_render_pass(command_buffer, owned.render_pass, owned.framebuffer, width, height,
                                             0, 0, 0, 1, 1), VK_SUCCESS);
        ASSERT_EQ(host.cmd_bind_pipeline(command_buffer, owned.pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS), VK_SUCCESS);
        ASSERT_EQ(host.cmd_draw_indirect(command_buffer, owned.indirect_buffer, 16, 2, 24), VK_SUCCESS);
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
        EXPECT_EQ(vertices, 6u) << "Two indirect records with a 24-byte stride must each draw three vertices";
        std::array<uint8_t, width * height * 4> pixels{};
        ASSERT_EQ(host.download_memory(owned.device, owned.readback_memory, 0, pixels.size(), pixels.data(),
                                       pixels.size()), VK_SUCCESS);
        uint32_t colored = 0;
        for (size_t i = 0; i < pixels.size(); i += 4)
            if (pixels[i] || pixels[i + 1] || pixels[i + 2]) ++colored;
        std::cout << "Native " << properties.deviceName << " indirect offset=16 stride=24 drawCount=2: IA vertices="
                  << vertices << ", colored pixels=" << colored << '/' << width * height << '\n';
        EXPECT_GE(colored, 100u) << "Indirect draws must produce visible triangle pixels";
        EXPECT_LT(colored, width * height) << "The clear background must remain visible";
    }
}
