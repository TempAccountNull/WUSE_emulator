#include <gtest/gtest.h>
#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include "vulkan_multi_draw_shaders.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace sogen::test
{
    TEST(VulkanMultiDrawHostTest, BothNativeCommandsRenderReadablePixelsOnSupportedGpu)
    {
        vulkan_host host;
        if (!host.available())
        {
            GTEST_SKIP() << "Host Vulkan unavailable";
        }
        struct owned_resources
        {
            vulkan_host& host;
            uint64_t instance{};
            uint64_t device{};
            uint64_t pool{};
            uint64_t render_pass{};
            uint64_t statistics_pool{};
            uint64_t pipeline_layout{};
            uint64_t pipeline{};
            uint64_t vertex_shader{};
            uint64_t fragment_shader{};
            std::vector<uint64_t> buffers;
            std::vector<uint64_t> images;
            std::vector<uint64_t> views;
            std::vector<uint64_t> framebuffers;
            std::vector<uint64_t> memories;
            ~owned_resources()
            {
                if (device)
                {
                    host.device_wait_idle(device);
                    for (auto id : framebuffers) host.destroy_framebuffer(device, id);
                    if (statistics_pool) host.destroy_query_pool(device, statistics_pool);
                    if (pipeline) host.destroy_pipeline(device, pipeline);
                    if (pipeline_layout) host.destroy_pipeline_layout(device, pipeline_layout);
                    if (vertex_shader) host.destroy_shader_module(device, vertex_shader);
                    if (fragment_shader) host.destroy_shader_module(device, fragment_shader);
                    for (auto id : views) host.destroy_image_view(device, id);
                    if (render_pass) host.destroy_render_pass(device, render_pass);
                    for (auto id : images) host.destroy_image(device, id);
                    for (auto id : buffers) host.destroy_buffer(device, id);
                    if (pool) host.destroy_command_pool(device, pool);
                    for (auto id : memories) host.free_memory(device, id);
                    host.destroy_device(device);
                }
                if (instance) host.destroy_instance(instance);
            }
        } owned{host};

        ASSERT_EQ(host.create_instance(owned.instance), VK_SUCCESS);
        std::array<uint64_t, 32> physical_devices{};
        uint32_t physical_count = 0;
        ASSERT_EQ(host.enumerate_physical_devices(owned.instance, physical_devices, physical_count), VK_SUCCESS);
        uint64_t physical_device = 0;
        VkPhysicalDeviceProperties properties{};
        for (uint32_t i = 0; i < std::min(physical_count, static_cast<uint32_t>(physical_devices.size())); ++i)
        {
            VkPhysicalDeviceProperties candidate{};
            ASSERT_EQ(host.get_physical_device_properties(physical_devices[i], &candidate, sizeof(candidate), false), VK_SUCCESS);
            uint32_t candidate_extension_count = 0;
            ASSERT_EQ(host.enumerate_device_extension_properties(physical_devices[i], nullptr, 0, candidate_extension_count), VK_SUCCESS);
            std::vector<VkExtensionProperties> candidate_extensions(candidate_extension_count);
            ASSERT_EQ(host.enumerate_device_extension_properties(physical_devices[i], candidate_extensions.data(),
                                                                  candidate_extensions.size() * sizeof(candidate_extensions[0]),
                                                                  candidate_extension_count), VK_SUCCESS);
            ASSERT_LE(candidate_extension_count, candidate_extensions.size());
            candidate_extensions.resize(candidate_extension_count);
            const bool supports_extension = std::any_of(candidate_extensions.begin(), candidate_extensions.end(), [](const auto& extension) {
                return std::strcmp(extension.extensionName, VK_EXT_MULTI_DRAW_EXTENSION_NAME) == 0;
            });
            std::cout << candidate.deviceName << " VK_EXT_multi_draw=" << supports_extension << '\n';
            if (supports_extension && (!physical_device || candidate.vendorID == 0x1002))
            {
                physical_device = physical_devices[i];
                properties = candidate;
            }
        }
        if (!physical_device) GTEST_SKIP() << "No Vulkan device exposes VK_EXT_multi_draw";
        std::cout << "Multi-draw device: " << properties.deviceName << '\n';

        constexpr gpu_bridge::feature_chain_record feature_record{
            .s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT, .body_size = sizeof(VkBool32)};
        std::vector<std::byte> feature_blob;
        ASSERT_EQ(host.get_physical_device_features2(physical_device, &feature_record, sizeof(feature_record), 1, feature_blob),
                  VK_SUCCESS);
        ASSERT_EQ(feature_blob.size(), sizeof(feature_record) + sizeof(VkBool32));
        VkBool32 supported = VK_FALSE;
        std::memcpy(&supported, feature_blob.data() + sizeof(feature_record), sizeof(supported));
        if (supported != VK_TRUE) GTEST_SKIP() << "Selected driver does not enable multiDraw feature";

        constexpr gpu_bridge::feature_chain_record base_record{
            .s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .body_size = sizeof(VkPhysicalDeviceFeatures)};
        std::vector<std::byte> base_blob;
        ASSERT_EQ(host.get_physical_device_features2(physical_device, &base_record, sizeof(base_record), 1, base_blob), VK_SUCCESS);
        ASSERT_EQ(base_blob.size(), sizeof(base_record) + sizeof(VkPhysicalDeviceFeatures));
        VkPhysicalDeviceFeatures base_features{};
        std::memcpy(&base_features, base_blob.data() + sizeof(base_record), sizeof(base_features));
        const bool have_statistics = base_features.pipelineStatisticsQuery == VK_TRUE;
        base_features = {};
        base_features.pipelineStatisticsQuery = have_statistics ? VK_TRUE : VK_FALSE;
        std::memcpy(base_blob.data() + sizeof(base_record), &base_features, sizeof(base_features));

        constexpr gpu_bridge::feature_chain_record property_record{
            .s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_PROPERTIES_EXT, .body_size = sizeof(uint32_t)};
        std::vector<std::byte> property_blob;
        ASSERT_EQ(host.get_physical_device_properties2(physical_device, &property_record, sizeof(property_record), 1, property_blob),
                  VK_SUCCESS);
        ASSERT_EQ(property_blob.size(), sizeof(property_record) + sizeof(uint32_t));
        uint32_t maximum = 0;
        std::memcpy(&maximum, property_blob.data() + sizeof(property_record), sizeof(maximum));
        ASSERT_GE(maximum, 2u);
        std::cout << "maxMultiDrawCount: " << maximum << '\n';

        std::array<gpu_bridge::queue_family_properties, 64> families{};
        uint32_t family_count = 0;
        ASSERT_EQ(host.get_queue_family_properties(physical_device, false, families.data(), sizeof(families), family_count), VK_SUCCESS);
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < std::min(family_count, static_cast<uint32_t>(families.size())); ++i)
        {
            if (families[i].queue_count && (families[i].queue_flags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
        }
        ASSERT_NE(family, UINT32_MAX);
        const gpu_bridge::device_queue_create_entry queue{.queue_family_index = family, .queue_count = 1};
        constexpr char extension_name[] = VK_EXT_MULTI_DRAW_EXTENSION_NAME;
        base_blob.insert(base_blob.end(), feature_blob.begin(), feature_blob.end());
        ASSERT_EQ(host.create_device(physical_device, &queue, 1, extension_name, sizeof(extension_name), 1,
                                     base_blob.data(), base_blob.size(), 2, owned.device), VK_SUCCESS);
        ASSERT_TRUE(host.supports_multi_draw(owned.device)) << "Native multi-draw function pointers unavailable";
        if (have_statistics)
        {
            ASSERT_EQ(host.create_query_pool(owned.device, VK_QUERY_TYPE_PIPELINE_STATISTICS, 1,
                                             VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT, owned.statistics_pool), VK_SUCCESS);
        }

        VkPhysicalDeviceMemoryProperties memory_properties{};
        ASSERT_EQ(host.get_physical_device_memory_properties(physical_device, &memory_properties, sizeof(memory_properties)), VK_SUCCESS);
        auto allocate = [&](uint64_t size, uint32_t type_bits, uint32_t required, uint64_t& out) {
            for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
            {
                if ((type_bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & required) == required)
                {
                    const auto result = host.allocate_memory(owned.device, size, i, 0, 0, out);
                    if (result == VK_SUCCESS) owned.memories.push_back(out);
                    return result;
                }
            }
            return int32_t(VK_ERROR_MEMORY_MAP_FAILED);
        };
        auto make_buffer = [&](uint64_t size, uint32_t usage, uint64_t& buffer, uint64_t& memory) {
            auto result = host.create_buffer(owned.device, size, usage, buffer);
            if (result != VK_SUCCESS) return result;
            owned.buffers.push_back(buffer);
            uint64_t bytes = 0, alignment = 0;
            uint32_t bits = 0;
            result = host.get_buffer_memory_requirements(owned.device, buffer, bytes, alignment, bits);
            if (result != VK_SUCCESS) return result;
            result = allocate(bytes, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memory);
            return result == VK_SUCCESS ? host.bind_buffer_memory(owned.device, buffer, memory, 0) : result;
        };

        constexpr uint32_t width = 32, height = 32;
        ASSERT_EQ(host.create_render_pass(owned.device, VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                           VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, owned.render_pass), VK_SUCCESS);
        ASSERT_EQ(host.create_shader_module(owned.device, 0, multi_draw_shaders::vert.data(),
                                            sizeof(multi_draw_shaders::vert), owned.vertex_shader), VK_SUCCESS);
        ASSERT_EQ(host.create_shader_module(owned.device, 0, multi_draw_shaders::frag.data(),
                                            sizeof(multi_draw_shaders::frag), owned.fragment_shader), VK_SUCCESS);
        ASSERT_EQ(host.create_pipeline_layout(owned.device, 0, 0, {}, owned.pipeline_layout), VK_SUCCESS);
        const vulkan_host::shader_stage_source vertex{.module = owned.vertex_shader};
        const vulkan_host::shader_stage_source fragment{.module = owned.fragment_shader};
        const vulkan_host::depth_state no_depth{};
        const vulkan_host::specialization no_spec{};
        ASSERT_EQ(host.create_graphics_pipeline(owned.device, 0, owned.render_pass, owned.pipeline_layout, vertex, fragment,
                                                0, width, height, {}, {}, {}, no_depth, {}, 0, 0,
                                                VK_SAMPLE_COUNT_1_BIT, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0, 0,
                                                {}, no_spec, no_spec, {}, owned.pipeline), VK_SUCCESS);
        ASSERT_EQ(host.create_command_pool(owned.device, family, 0, owned.pool), VK_SUCCESS);
        uint64_t queue_id = 0;
        ASSERT_EQ(host.get_device_queue(owned.device, family, 0, queue_id), VK_SUCCESS);

        uint64_t index_buffer = 0, index_memory = 0;
        ASSERT_EQ(make_buffer(12, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer, index_memory), VK_SUCCESS);
        const std::array<uint16_t, 6> indices{3, 4, 5, 3, 4, 5};
        ASSERT_EQ(host.upload_memory(owned.device, index_memory, 0, sizeof(indices), indices.data(), sizeof(indices)), VK_SUCCESS);

        for (int draw_mode = 0; draw_mode < 3; ++draw_mode)
        {
            const bool indexed = draw_mode != 0;
            uint64_t image = 0, image_memory = 0, view = 0, framebuffer = 0, readback_buffer = 0, readback_memory = 0;
            ASSERT_EQ(host.create_image(owned.device, VK_FORMAT_R8G8B8A8_UNORM, width, height,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                        VK_IMAGE_TILING_OPTIMAL, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TYPE_2D, 1, 1, 1, 0, image), VK_SUCCESS);
            owned.images.push_back(image);
            uint64_t bytes = 0, alignment = 0;
            uint32_t bits = 0;
            ASSERT_EQ(host.get_image_memory_requirements(owned.device, image, bytes, alignment, bits), VK_SUCCESS);
            ASSERT_EQ(allocate(bytes, bits, 0, image_memory), VK_SUCCESS);
            ASSERT_EQ(host.bind_image_memory(owned.device, image, image_memory, 0), VK_SUCCESS);
            ASSERT_EQ(host.create_image_view(owned.device, image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                             VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1, VK_COMPONENT_SWIZZLE_IDENTITY,
                                             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                             VK_COMPONENT_SWIZZLE_IDENTITY, view), VK_SUCCESS);
            owned.views.push_back(view);
            ASSERT_EQ(host.create_framebuffer(owned.device, owned.render_pass, view, 0, width, height, framebuffer), VK_SUCCESS);
            owned.framebuffers.push_back(framebuffer);
            ASSERT_EQ(make_buffer(width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback_buffer, readback_memory), VK_SUCCESS);
            uint64_t command_buffer = 0;
            ASSERT_EQ(host.allocate_command_buffer(owned.device, owned.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, command_buffer), VK_SUCCESS);
            ASSERT_EQ(host.begin_command_buffer(command_buffer, 0, false, 0, {}, 0, 0, 1, 0), VK_SUCCESS);
            if (have_statistics)
            {
                ASSERT_EQ(host.cmd_reset_query_pool(command_buffer, owned.statistics_pool, 0, 1), VK_SUCCESS);
                ASSERT_EQ(host.cmd_begin_query(command_buffer, owned.statistics_pool, 0, 0), VK_SUCCESS);
            }
            ASSERT_EQ(host.cmd_begin_render_pass(command_buffer, owned.render_pass, framebuffer, width, height, 0, 0, 0, 1, 1), VK_SUCCESS);
            ASSERT_EQ(host.cmd_bind_pipeline(command_buffer, owned.pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS), VK_SUCCESS);
            if (indexed)
            {
                ASSERT_EQ(host.cmd_bind_index_buffer(command_buffer, index_buffer, 0, VK_INDEX_TYPE_UINT16), VK_SUCCESS);
                const int32_t entry_offset = draw_mode == 1 ? -3 : 123;
                const std::array<vulkan_host::multi_draw_indexed_info, 2> draws{{{0, 3, entry_offset}, {3, 3, entry_offset}}};
                const int32_t offset = -3;
                ASSERT_EQ(host.cmd_draw_multi_indexed(command_buffer, draws, 1, 0, draw_mode == 1 ? nullptr : &offset), VK_SUCCESS);
            }
            else
            {
                const std::array<vulkan_host::multi_draw_info, 2> draws{{{0, 3}, {0, 3}}};
                ASSERT_EQ(host.cmd_draw_multi(command_buffer, draws, 1, 0), VK_SUCCESS);
            }
            ASSERT_EQ(host.cmd_end_render_pass(command_buffer), VK_SUCCESS);
            if (have_statistics)
            {
                ASSERT_EQ(host.cmd_end_query(command_buffer, owned.statistics_pool, 0), VK_SUCCESS);
            }
            const vulkan_host::subresource_range range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ASSERT_EQ(host.cmd_pipeline_barrier(command_buffer, image, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                                VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, range), VK_SUCCESS);
            ASSERT_EQ(host.cmd_copy_image_to_buffer(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                    readback_buffer, width, height, VK_IMAGE_ASPECT_COLOR_BIT), VK_SUCCESS);
            ASSERT_EQ(host.end_command_buffer(command_buffer), VK_SUCCESS);
            ASSERT_EQ(host.queue_submit(queue_id, command_buffer, 0), VK_SUCCESS);
            ASSERT_EQ(host.queue_wait_idle(queue_id), VK_SUCCESS);
            if (have_statistics)
            {
                uint64_t vertices = 0;
                size_t written = 0;
                ASSERT_EQ(host.get_query_pool_results(owned.device, owned.statistics_pool, 0, 1,
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT,
                                                      &vertices, sizeof(vertices), sizeof(vertices), written), VK_SUCCESS);
                ASSERT_EQ(written, sizeof(vertices));
                EXPECT_EQ(vertices, 6u) << "Both three-vertex records must execute in mode " << draw_mode;
                std::cout << "multi-draw mode " << draw_mode << " input-assembly vertices: " << vertices << '\n';
            }
            std::array<uint8_t, width * height * 4> pixels{};
            ASSERT_EQ(host.download_memory(owned.device, readback_memory, 0, pixels.size(), pixels.data(), pixels.size()), VK_SUCCESS);
            const auto pixel = [&](uint32_t x, uint32_t y) { return pixels.data() + 4 * (y * width + x); };
            EXPECT_EQ(pixel(0, 0)[0], 0) << (indexed ? "indexed" : "non-indexed");
            EXPECT_EQ(pixel(0, 0)[1], 0);
            EXPECT_EQ(pixel(0, 0)[2], 0);
            uint32_t colored_pixels = 0;
            for (size_t p = 0; p < pixels.size(); p += 4)
            {
                colored_pixels += pixels[p] != 0 || pixels[p + 1] != 0 || pixels[p + 2] != 0;
            }
            EXPECT_GT(colored_pixels, 0u) << "multi-draw mode " << draw_mode << " produced no color";
            std::cout << "multi-draw mode " << draw_mode << " colored pixels: " << colored_pixels << '\n';
        }
    }
}
