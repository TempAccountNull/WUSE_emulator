#include <gtest/gtest.h>
#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include "vulkan_descriptor_copy_spirv.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace sogen::test
{
    TEST(VulkanDescriptorCopyHostTest, WireCarriesCopiesAfterWritesIncludingCopyOnlyRecord)
    {
        using namespace gpu_bridge;
        static_assert(sizeof(update_descriptor_sets_request) == 24);
        static_assert(sizeof(descriptor_copy) == 40);
        const update_descriptor_sets_request first{.device = 7, .write_count = 1, .copy_count = 1};
        const descriptor_write write{.dst_set = 11, .dst_binding = 2, .descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLER,
                                     .sampler = 13};
        const descriptor_copy copy{.src_set = 11, .src_binding = 2, .dst_set = 17,
                                   .dst_binding = 2, .descriptor_count = 1};
        const update_descriptor_sets_request second{.device = 7, .write_count = 0, .copy_count = 1};
        std::vector<std::byte> batch;
        const auto append = [&](const auto& value) {
            const auto* ptr = reinterpret_cast<const std::byte*>(&value);
            batch.insert(batch.end(), ptr, ptr + sizeof(value));
        };
        append(first); append(write); append(copy); append(second); append(copy);
        size_t offset = 0;
        update_descriptor_sets_request decoded{};
        std::memcpy(&decoded, batch.data() + offset, sizeof(decoded));
        EXPECT_EQ(decoded.write_count, 1u);
        EXPECT_EQ(decoded.copy_count, 1u);
        offset += sizeof(decoded) + decoded.write_count * sizeof(descriptor_write);
        descriptor_copy decoded_copy{};
        std::memcpy(&decoded_copy, batch.data() + offset, sizeof(decoded_copy));
        EXPECT_EQ(decoded_copy.src_set, 11u);
        EXPECT_EQ(decoded_copy.dst_set, 17u);
        offset += decoded.copy_count * sizeof(descriptor_copy) + decoded.inline_uniform_data_size;
        std::memcpy(&decoded, batch.data() + offset, sizeof(decoded));
        EXPECT_EQ(decoded.write_count, 0u);
        EXPECT_EQ(decoded.copy_count, 1u);
        offset += sizeof(decoded) + decoded.copy_count * sizeof(descriptor_copy);
        EXPECT_EQ(offset, batch.size());
    }

    TEST(VulkanDescriptorCopyHostTest, Rx6700AcceptsWriteThenCopyAndCopyOnly)
    {
        vulkan_host host;
        if (!host.available()) GTEST_SKIP() << "Host Vulkan unavailable";
        uint64_t instance = 0, device = 0, layout = 0, pool = 0, sampler = 0;
        struct cleanup
        {
            vulkan_host& host;
            uint64_t& instance; uint64_t& device; uint64_t& layout; uint64_t& pool; uint64_t& sampler;
            ~cleanup()
            {
                if (device)
                {
                    if (pool) host.destroy_descriptor_pool(device, pool);
                    if (layout) host.destroy_descriptor_set_layout(device, layout);
                    if (sampler) host.destroy_sampler(device, sampler);
                    host.destroy_device(device);
                }
                if (instance) host.destroy_instance(instance);
            }
        } owned{host, instance, device, layout, pool, sampler};
        ASSERT_EQ(host.create_instance(instance), VK_SUCCESS);
        std::array<uint64_t, 32> physicals{};
        uint32_t count = 0;
        ASSERT_EQ(host.enumerate_physical_devices(instance, physicals, count), VK_SUCCESS);
        uint64_t physical = 0;
        for (uint32_t i = 0; i < std::min(count, static_cast<uint32_t>(physicals.size())); ++i)
        {
            VkPhysicalDeviceProperties properties{};
            ASSERT_EQ(host.get_physical_device_properties(physicals[i], &properties, sizeof(properties), false), VK_SUCCESS);
            if (properties.vendorID == 0x1002)
            {
                physical = physicals[i];
                std::cout << "Descriptor-copy native oracle: " << properties.deviceName << '\n';
                break;
            }
        }
        if (!physical) GTEST_SKIP() << "AMD Vulkan device unavailable";
        const gpu_bridge::device_queue_create_entry queue{.queue_family_index = 0, .queue_count = 1};
        ASSERT_EQ(host.create_device(physical, &queue, 1, nullptr, 0, 0, nullptr, 0, 0, device), VK_SUCCESS);
        ASSERT_EQ(host.create_sampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_FALSE, VK_COMPARE_OP_ALWAYS, VK_FALSE,
                                      VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, 0.0f, 1.0f, 0.0f, 1.0f, sampler), VK_SUCCESS);
        const vulkan_host::descriptor_binding binding{.binding = 0, .descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLER,
                                                       .descriptor_count = 1, .stage_flags = VK_SHADER_STAGE_FRAGMENT_BIT};
        ASSERT_EQ(host.create_descriptor_set_layout(device, 0, std::span{&binding, 1}, layout), VK_SUCCESS);
        const vulkan_host::descriptor_pool_size size{.descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptor_count = 3};
        ASSERT_EQ(host.create_descriptor_pool(device, 3, 0, 0, std::span{&size, 1}, pool), VK_SUCCESS);
        const std::array<uint64_t, 3> layouts{layout, layout, layout};
        std::array<uint64_t, 3> sets{};
        uint32_t allocated = 0;
        ASSERT_EQ(host.allocate_descriptor_sets(device, pool, layouts, {}, sets, allocated), VK_SUCCESS);
        ASSERT_EQ(allocated, sets.size());
        const vulkan_host::descriptor_write write{.dst_set = sets[0], .dst_binding = 0,
                                                   .descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLER, .sampler = sampler};
        const vulkan_host::descriptor_copy first{.src_set = sets[0], .dst_set = sets[1], .descriptor_count = 1};
        // The native AMD driver receives this as one vkUpdateDescriptorSets call, so its copy sees the write.
        EXPECT_EQ(host.update_descriptor_sets(device, std::span{&write, 1}, std::span{&first, 1}), VK_SUCCESS);
        const vulkan_host::descriptor_copy second{.src_set = sets[1], .dst_set = sets[2], .descriptor_count = 1};
        EXPECT_EQ(host.update_descriptor_sets(device, {}, std::span{&second, 1}), VK_SUCCESS);
        const vulkan_host::descriptor_copy foreign{.src_set = UINT64_MAX, .dst_set = sets[2], .descriptor_count = 1};
        EXPECT_EQ(host.update_descriptor_sets(device, {}, std::span{&foreign, 1}), VK_ERROR_VALIDATION_FAILED_EXT);
        const vulkan_host::descriptor_copy missing_binding{.src_set = sets[0], .src_binding = 99,
                                                            .dst_set = sets[1], .descriptor_count = 1};
        EXPECT_EQ(host.update_descriptor_sets(device, {}, std::span{&missing_binding, 1}), VK_ERROR_VALIDATION_FAILED_EXT);
    }

    TEST(VulkanDescriptorCopyHostTest, Rx6700CopiedDescriptorDrivesComputeReadback)
    {
        vulkan_host host;
        if (!host.available()) GTEST_SKIP() << "Host Vulkan unavailable";
        struct resources
        {
            vulkan_host& host;
            uint64_t instance{}, device{}, layout{}, descriptor_pool{}, pipeline_layout{}, pipeline{}, shader{};
            uint64_t command_pool{}, command_buffer{};
            std::vector<uint64_t> buffers, memories;
            ~resources()
            {
                if (device)
                {
                    host.device_wait_idle(device);
                    if (command_buffer) host.free_command_buffer(device, command_pool, command_buffer);
                    if (command_pool) host.destroy_command_pool(device, command_pool);
                    if (pipeline) host.destroy_pipeline(device, pipeline);
                    if (pipeline_layout) host.destroy_pipeline_layout(device, pipeline_layout);
                    if (shader) host.destroy_shader_module(device, shader);
                    if (descriptor_pool) host.destroy_descriptor_pool(device, descriptor_pool);
                    if (layout) host.destroy_descriptor_set_layout(device, layout);
                    for (auto buffer : buffers) host.destroy_buffer(device, buffer);
                    for (auto memory : memories) host.free_memory(device, memory);
                    host.destroy_device(device);
                }
                if (instance) host.destroy_instance(instance);
            }
        } owned{host};
        ASSERT_EQ(host.create_instance(owned.instance), VK_SUCCESS);
        std::array<uint64_t, 32> physicals{};
        uint32_t physical_count = 0;
        ASSERT_EQ(host.enumerate_physical_devices(owned.instance, physicals, physical_count), VK_SUCCESS);
        uint64_t physical = 0;
        for (uint32_t i = 0; i < std::min(physical_count, static_cast<uint32_t>(physicals.size())); ++i)
        {
            VkPhysicalDeviceProperties properties{};
            ASSERT_EQ(host.get_physical_device_properties(physicals[i], &properties, sizeof(properties), false), VK_SUCCESS);
            if (properties.vendorID == 0x1002)
            {
                physical = physicals[i];
                std::cout << "Descriptor-copy GPU readback: " << properties.deviceName << '\n';
                break;
            }
        }
        if (!physical) GTEST_SKIP() << "AMD Vulkan device unavailable";
        const gpu_bridge::device_queue_create_entry queue_info{.queue_family_index = 0, .queue_count = 1};
        ASSERT_EQ(host.create_device(physical, &queue_info, 1, nullptr, 0, 0, nullptr, 0, 0, owned.device), VK_SUCCESS);
        uint64_t queue = 0;
        ASSERT_EQ(host.get_device_queue(owned.device, 0, 0, queue), VK_SUCCESS);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        ASSERT_EQ(host.get_physical_device_memory_properties(physical, &memory_properties, sizeof(memory_properties)), VK_SUCCESS);
        const auto make_buffer = [&](uint32_t initial, uint64_t& buffer, uint64_t& memory) {
            auto result = host.create_buffer(owned.device, sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, buffer);
            if (result != VK_SUCCESS) return result;
            owned.buffers.push_back(buffer);
            uint64_t bytes = 0, alignment = 0;
            uint32_t bits = 0;
            result = host.get_buffer_memory_requirements(owned.device, buffer, bytes, alignment, bits);
            if (result != VK_SUCCESS) return result;
            for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
            {
                const auto flags = memory_properties.memoryTypes[i].propertyFlags;
                if ((bits & (1u << i)) && (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                                              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                {
                    result = host.allocate_memory(owned.device, bytes, i, 0, 0, memory);
                    if (result != VK_SUCCESS) return result;
                    owned.memories.push_back(memory);
                    result = host.bind_buffer_memory(owned.device, buffer, memory, 0);
                    if (result != VK_SUCCESS) return result;
                    return host.upload_memory(owned.device, memory, 0, sizeof(initial), &initial, sizeof(initial));
                }
            }
            return int32_t(VK_ERROR_MEMORY_MAP_FAILED);
        };
        uint64_t source_buffer = 0, source_memory = 0, decoy_buffer = 0, decoy_memory = 0;
        uint64_t output1_buffer = 0, output1_memory = 0, output2_buffer = 0, output2_memory = 0;
        constexpr uint32_t source_value = 0x10203040u, decoy_value = 0xdeadbeefu;
        ASSERT_EQ(make_buffer(source_value, source_buffer, source_memory), VK_SUCCESS);
        ASSERT_EQ(make_buffer(decoy_value, decoy_buffer, decoy_memory), VK_SUCCESS);
        ASSERT_EQ(make_buffer(0, output1_buffer, output1_memory), VK_SUCCESS);
        ASSERT_EQ(make_buffer(0, output2_buffer, output2_memory), VK_SUCCESS);

        const std::array<vulkan_host::descriptor_binding, 2> bindings{{
            {.binding = 0, .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptor_count = 1,
             .stage_flags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 1, .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptor_count = 1,
             .stage_flags = VK_SHADER_STAGE_COMPUTE_BIT},
        }};
        ASSERT_EQ(host.create_descriptor_set_layout(owned.device, 0, bindings, owned.layout), VK_SUCCESS);
        const vulkan_host::descriptor_pool_size pool_size{.descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptor_count = 6};
        ASSERT_EQ(host.create_descriptor_pool(owned.device, 3, 0, 0, std::span{&pool_size, 1}, owned.descriptor_pool), VK_SUCCESS);
        const std::array<uint64_t, 3> layouts{owned.layout, owned.layout, owned.layout};
        std::array<uint64_t, 3> sets{};
        uint32_t set_count = 0;
        ASSERT_EQ(host.allocate_descriptor_sets(owned.device, owned.descriptor_pool, layouts, {}, sets, set_count), VK_SUCCESS);
        ASSERT_EQ(set_count, 3u);
        const std::array<vulkan_host::descriptor_write, 5> writes{{
            {.dst_set = sets[0], .dst_binding = 0, .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .buffer_or_view = source_buffer, .range = sizeof(uint32_t)},
            {.dst_set = sets[1], .dst_binding = 0, .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .buffer_or_view = decoy_buffer, .range = sizeof(uint32_t)},
            {.dst_set = sets[1], .dst_binding = 1, .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .buffer_or_view = output1_buffer, .range = sizeof(uint32_t)},
            {.dst_set = sets[2], .dst_binding = 0, .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .buffer_or_view = decoy_buffer, .range = sizeof(uint32_t)},
            {.dst_set = sets[2], .dst_binding = 1, .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .buffer_or_view = output2_buffer, .range = sizeof(uint32_t)},
        }};
        const vulkan_host::descriptor_copy first{.src_set = sets[0], .dst_set = sets[1], .descriptor_count = 1};
        ASSERT_EQ(host.update_descriptor_sets(owned.device, writes, std::span{&first, 1}), VK_SUCCESS);
        const vulkan_host::descriptor_copy second{.src_set = sets[1], .dst_set = sets[2], .descriptor_count = 1};
        ASSERT_EQ(host.update_descriptor_sets(owned.device, {}, std::span{&second, 1}), VK_SUCCESS);

        ASSERT_EQ(host.create_shader_module(owned.device, 0, descriptor_copy_shader::compute.data(),
                                            sizeof(descriptor_copy_shader::compute), owned.shader), VK_SUCCESS);
        ASSERT_EQ(host.create_pipeline_layout(owned.device, 0, 0, std::span{&owned.layout, 1}, owned.pipeline_layout), VK_SUCCESS);
        const vulkan_host::shader_stage_source compute{.module = owned.shader};
        ASSERT_EQ(host.create_compute_pipeline(owned.device, 0, owned.pipeline_layout, compute, 0, owned.pipeline), VK_SUCCESS);
        ASSERT_EQ(host.create_command_pool(owned.device, 0, 0, owned.command_pool), VK_SUCCESS);
        ASSERT_EQ(host.allocate_command_buffer(owned.device, owned.command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               owned.command_buffer), VK_SUCCESS);
        ASSERT_EQ(host.begin_command_buffer(owned.command_buffer, 0, false, 0, {}, 0, 0, 1, 0), VK_SUCCESS);
        ASSERT_EQ(host.cmd_bind_pipeline(owned.command_buffer, owned.pipeline, VK_PIPELINE_BIND_POINT_COMPUTE), VK_SUCCESS);
        ASSERT_EQ(host.cmd_bind_descriptor_sets(owned.command_buffer, owned.pipeline_layout, 0, std::span{&sets[1], 1},
                                                VK_PIPELINE_BIND_POINT_COMPUTE, {}), VK_SUCCESS);
        ASSERT_EQ(host.cmd_dispatch(owned.command_buffer, 1, 1, 1), VK_SUCCESS);
        ASSERT_EQ(host.cmd_bind_descriptor_sets(owned.command_buffer, owned.pipeline_layout, 0, std::span{&sets[2], 1},
                                                VK_PIPELINE_BIND_POINT_COMPUTE, {}), VK_SUCCESS);
        ASSERT_EQ(host.cmd_dispatch(owned.command_buffer, 1, 1, 1), VK_SUCCESS);
        ASSERT_EQ(host.end_command_buffer(owned.command_buffer), VK_SUCCESS);
        ASSERT_EQ(host.queue_submit(queue, owned.command_buffer, 0), VK_SUCCESS);
        ASSERT_EQ(host.queue_wait_idle(queue), VK_SUCCESS);
        uint32_t first_result = 0, second_result = 0;
        ASSERT_EQ(host.download_memory(owned.device, output1_memory, 0, sizeof(first_result), &first_result, sizeof(first_result)),
                  VK_SUCCESS);
        ASSERT_EQ(host.download_memory(owned.device, output2_memory, 0, sizeof(second_result), &second_result, sizeof(second_result)),
                  VK_SUCCESS);
        std::cout << "Copied descriptor shader outputs: first=0x" << std::hex << first_result << " second=0x"
                  << second_result << std::dec << '\n';
        EXPECT_EQ(first_result, source_value);
        EXPECT_EQ(second_result, source_value);
        EXPECT_NE(first_result, decoy_value);
    }
}
