#include <gtest/gtest.h>
#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include <vk_render_pass.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <vector>

namespace sogen::test
{
    namespace allocation_wire = gpu_bridge::render_pass_wire;

    TEST(VulkanMemoryPriorityWireTest, FixedPriorityRequestPreservesHandlesAndFloatBits)
    {
        const gpu_bridge::set_device_memory_priority_request request{.device = 0x1234567887654321ULL,
                                                                     .memory = 0xabcdef0123456789ULL,
                                                                     .priority_bits = std::bit_cast<uint32_t>(0.625f),
                                                                     .reserved = 0};
        const std::array<uint32_t, 6> expected{0x87654321, 0x12345678, 0x23456789, 0xabcdef01, 0x3f200000, 0};
        static_assert(sizeof(float) == sizeof(uint32_t));
        ASSERT_EQ(sizeof(request), sizeof(expected));
        EXPECT_EQ(std::memcmp(&request, expected.data(), sizeof(request)), 0);
    }

    TEST(VulkanMemoryPriorityWireTest, OwnsCompleteFlagsPriorityAndDedicatedChain)
    {
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.buffer = allocation_wire::handle_value<VkBuffer>(0x1234567887654321ULL);
        const VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT, &dedicated, 0.625f};
        const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &priority,
                                              VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT | VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT, 0x80000001};
        const VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags, 0x123456789abcdef0ULL, 19};
        const auto packet = allocation_wire::encode(info);
        const std::array<uint32_t, 15> expected{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                3,
                                                VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                                VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT | VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT,
                                                0x80000001,
                                                VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT,
                                                0x3f200000,
                                                VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                                0,
                                                0,
                                                0x87654321,
                                                0x12345678,
                                                0x9abcdef0,
                                                0x12345678,
                                                19};
        ASSERT_EQ(packet.size(), sizeof(expected));
        EXPECT_EQ(std::memcmp(packet.data(), expected.data(), sizeof(expected)), 0);
        allocation_wire::reader reader(packet);
        VkMemoryAllocateInfo decoded{};
        allocation_wire::decode(reader, decoded);
        const auto* decoded_flags = static_cast<const VkMemoryAllocateFlagsInfo*>(decoded.pNext);
        ASSERT_NE(decoded_flags, nullptr);
        EXPECT_NE(decoded_flags, &flags);
        EXPECT_EQ(decoded_flags->flags, flags.flags);
        EXPECT_EQ(decoded_flags->deviceMask, flags.deviceMask);
        const auto* decoded_priority = static_cast<const VkMemoryPriorityAllocateInfoEXT*>(decoded_flags->pNext);
        ASSERT_NE(decoded_priority, nullptr);
        EXPECT_FLOAT_EQ(decoded_priority->priority, priority.priority);
        const auto* decoded_dedicated = static_cast<const VkMemoryDedicatedAllocateInfo*>(decoded_priority->pNext);
        ASSERT_NE(decoded_dedicated, nullptr);
        EXPECT_EQ(allocation_wire::handle_id(decoded_dedicated->buffer), 0x1234567887654321ULL);
        EXPECT_EQ(decoded_dedicated->image, VK_NULL_HANDLE);
        EXPECT_EQ(decoded_dedicated->pNext, nullptr);
        EXPECT_EQ(decoded.allocationSize, info.allocationSize);
        EXPECT_EQ(decoded.memoryTypeIndex, info.memoryTypeIndex);
        EXPECT_EQ(allocation_wire::encode(decoded), packet);
    }

    TEST(VulkanMemoryPriorityWireTest, RejectsNonfiniteOutOfRangeAndUnsupportedAllocationChains)
    {
        VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &priority, 4096, 0};
        for (const float value : {-0.01f, 1.01f, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                  std::numeric_limits<float>::quiet_NaN()})
        {
            priority.priority = value;
            EXPECT_THROW(allocation_wire::encode(info), allocation_wire::error);
        }
        for (const float value : {-0.0f, 0.0f, 0.5f, 1.0f})
        {
            priority.priority = value;
            EXPECT_NO_THROW(allocation_wire::encode(info));
        }
        const VkExportMemoryAllocateInfo unsupported{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr,
                                                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
        info.pNext = &unsupported;
        EXPECT_THROW(allocation_wire::encode(info), allocation_wire::error);
        info.pNext = &priority;
        priority.pNext = &priority;
        EXPECT_THROW(allocation_wire::encode(info), allocation_wire::error);
        const VkMemoryPriorityAllocateInfoEXT duplicate{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT, nullptr, 0.5f};
        priority.pNext = &duplicate;
        EXPECT_THROW(allocation_wire::encode(info), allocation_wire::error);
        priority.pNext = nullptr;
        const auto packet = allocation_wire::encode(info);
        for (size_t count = 0; count < packet.size(); ++count)
        {
            allocation_wire::reader reader(std::span<const std::byte>(packet.data(), count));
            VkMemoryAllocateInfo decoded{};
            EXPECT_THROW(allocation_wire::decode(reader, decoded), allocation_wire::error);
        }
        auto forged = packet;
        const uint32_t nan_bits = 0x7fc00000;
        std::memcpy(forged.data() + 12, &nan_bits, sizeof(nan_bits));
        allocation_wire::reader reader(forged);
        VkMemoryAllocateInfo decoded{};
        EXPECT_THROW(allocation_wire::decode(reader, decoded), allocation_wire::error);
    }

    class VulkanMemoryPriorityHostTest : public testing::Test
    {
      protected:
        vulkan_host host;
        uint64_t instance{};
        std::array<uint64_t, 4> devices{};
        std::array<uint64_t, 4> buffers{};
        std::array<uint64_t, 4> memories{};
        std::array<uint64_t, 4> sizes{};
        std::array<uint32_t, 4> types{};

        static std::vector<std::byte> features(bool priority, bool pageable)
        {
            std::vector<std::byte> blob;
            const auto add = [&](uint32_t type, bool enabled) {
                const gpu_bridge::feature_chain_record record{.s_type = type, .body_size = sizeof(uint32_t)};
                const uint32_t value = enabled ? VK_TRUE : VK_FALSE;
                const auto offset = blob.size();
                blob.resize(offset + sizeof(record) + sizeof(value));
                std::memcpy(blob.data() + offset, &record, sizeof(record));
                std::memcpy(blob.data() + offset + sizeof(record), &value, sizeof(value));
            };
            add(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT, priority);
            add(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT, pageable);
            return blob;
        }

        void SetUp() override
        {
            if (!host.available())
            {
                GTEST_SKIP() << "Vulkan driver unavailable";
            }
            ASSERT_EQ(host.create_instance(instance), VK_SUCCESS);
            uint32_t version{};
            ASSERT_EQ(host.get_instance_api_version(instance, version), VK_SUCCESS);
            if (version < VK_API_VERSION_1_1)
            {
                GTEST_SKIP() << "Feature-query fixture requires native instance API 1.1";
            }
            std::array<uint64_t, 32> physical{};
            uint32_t count{};
            ASSERT_EQ(host.enumerate_physical_devices(instance, physical, count), VK_SUCCESS);
            if (!count)
            {
                GTEST_SKIP() << "No physical devices";
            }
            VkPhysicalDeviceProperties physical_properties{};
            ASSERT_EQ(host.get_physical_device_properties(physical[0], &physical_properties, sizeof(physical_properties), false),
                      VK_SUCCESS);
            if (physical_properties.apiVersion < VK_API_VERSION_1_1)
            {
                GTEST_SKIP() << "Dedicated allocation fixture requires physical-device API 1.1";
            }
            ASSERT_EQ(host.enumerate_device_extension_properties(physical[0], nullptr, 0, count), VK_SUCCESS);
            std::vector<VkExtensionProperties> properties(count);
            ASSERT_EQ(host.enumerate_device_extension_properties(physical[0], properties.data(), properties.size() * sizeof(properties[0]),
                                                                 count),
                      VK_SUCCESS);
            const auto has_extension = [&](const char* name) {
                return std::ranges::any_of(properties,
                                           [&](const auto& property) { return std::strcmp(property.extensionName, name) == 0; });
            };
            if (!has_extension(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME) || !has_extension(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME))
            {
                GTEST_SKIP() << "Memory-priority/pageable extensions unavailable";
            }
            const std::array<gpu_bridge::feature_chain_record, 2> query{{
                {.s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT, .body_size = sizeof(uint32_t)},
                {.s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT, .body_size = sizeof(uint32_t)},
            }};
            std::vector<std::byte> available;
            ASSERT_EQ(host.get_physical_device_features2(physical[0], query.data(), sizeof(query), 2, available), VK_SUCCESS);
            size_t offset = 0;
            for (const auto& expected : query)
            {
                ASSERT_GE(available.size() - offset, sizeof(expected) + sizeof(uint32_t));
                gpu_bridge::feature_chain_record record{};
                uint32_t supported{};
                std::memcpy(&record, available.data() + offset, sizeof(record));
                ASSERT_EQ(record.s_type, expected.s_type);
                ASSERT_EQ(record.body_size, sizeof(uint32_t));
                std::memcpy(&supported, available.data() + offset + sizeof(record), sizeof(supported));
                offset += sizeof(record) + sizeof(supported);
                if (supported != VK_TRUE)
                {
                    GTEST_SKIP() << "Positive enabled-feature fixture unsupported by this device";
                }
            }
            std::array<gpu_bridge::queue_family_properties, 64> families{};
            ASSERT_EQ(host.get_queue_family_properties(physical[0], false, families.data(), sizeof(families), count), VK_SUCCESS);
            uint32_t family = UINT32_MAX;
            for (uint32_t i = 0; i < std::min(count, static_cast<uint32_t>(families.size())); ++i)
            {
                if (families[i].queue_count)
                {
                    family = i;
                    break;
                }
            }
            ASSERT_NE(family, UINT32_MAX);
            const gpu_bridge::device_queue_create_entry queue{.queue_family_index = family, .queue_count = 1};
            constexpr auto extensions =
                std::to_array(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME "\0" VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
            for (uint32_t i = 0; i < devices.size(); ++i)
            {
                const auto blob = features(i == 0 || i == 2, i == 0);
                ASSERT_EQ(host.create_device(physical[0], &queue, 1, i == 3 ? nullptr : extensions.data(), i == 3 ? 0 : extensions.size(),
                                             i == 3 ? 0 : 2, i == 3 ? nullptr : blob.data(), i == 3 ? 0 : blob.size(), i == 3 ? 0 : 2,
                                             devices[i]),
                          VK_SUCCESS);
                ASSERT_EQ(host.create_buffer(devices[i], 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, buffers[i]), VK_SUCCESS);
                uint64_t alignment{};
                uint32_t type_bits{};
                ASSERT_EQ(host.get_buffer_memory_requirements(devices[i], buffers[i], sizes[i], alignment, type_bits), VK_SUCCESS);
                ASSERT_NE(type_bits, 0u);
                types[i] = static_cast<uint32_t>(std::countr_zero(type_bits));
                const VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, sizes[i], types[i]};
                ASSERT_EQ(host.allocate_memory_full(devices[i], allocation_wire::encode(info), memories[i]), VK_SUCCESS);
            }
        }

        void TearDown() override
        {
            if (instance)
            {
                host.destroy_instance(instance);
            }
        }
    };

    TEST_F(VulkanMemoryPriorityHostTest, NativeSetterAcceptsEnabledAndDisabledPageableFeatures)
    {
        for (uint32_t i = 0; i < 3; ++i)
        {
            for (const float priority : {-0.0f, 0.25f, 0.5f, 1.0f})
            {
                EXPECT_EQ(host.set_device_memory_priority(devices[i], memories[i], priority), VK_SUCCESS);
            }
        }
    }

    TEST_F(VulkanMemoryPriorityHostTest, RejectsAbsentExtensionInvalidStaleAndForeignOwners)
    {
        EXPECT_EQ(host.set_device_memory_priority(devices[3], memories[3], 0.5f), VK_ERROR_FEATURE_NOT_PRESENT);
        EXPECT_EQ(host.set_device_memory_priority(0, memories[0], 0.5f), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(host.set_device_memory_priority(devices[0], 0, 0.5f), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(host.set_device_memory_priority(devices[0], memories[1], 0.5f), VK_ERROR_INITIALIZATION_FAILED);
        host.free_memory(devices[0], memories[0]);
        EXPECT_EQ(host.set_device_memory_priority(devices[0], memories[0], 0.5f), VK_ERROR_INITIALIZATION_FAILED);
    }

    TEST_F(VulkanMemoryPriorityHostTest, RejectsNonfiniteAndOutOfRangePrioritiesBeforeNativeCall)
    {
        for (const float priority : {-0.01f, 1.01f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                     -std::numeric_limits<float>::infinity()})
        {
            EXPECT_EQ(host.set_device_memory_priority(devices[0], memories[0], priority), VK_ERROR_VALIDATION_FAILED_EXT);
        }
    }

    TEST_F(VulkanMemoryPriorityHostTest, AllocationPriorityValidatesEnabledFeatureAndExtension)
    {
        const VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT, nullptr, 0.75f};
        for (uint32_t i = 0; i < devices.size(); ++i)
        {
            const VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &priority, sizes[i], types[i]};
            uint64_t memory = UINT64_MAX;
            const auto result = host.allocate_memory_full(devices[i], allocation_wire::encode(info), memory);
            if (i == 0 || i == 2)
            {
                EXPECT_EQ(result, VK_SUCCESS);
                EXPECT_NE(memory, 0u);
                host.free_memory(devices[i], memory);
            }
            else
            {
                EXPECT_EQ(result, VK_ERROR_FEATURE_NOT_PRESENT);
                EXPECT_EQ(memory, 0u);
            }
        }
    }

    TEST_F(VulkanMemoryPriorityHostTest, DedicatedBufferChainTranslatesOwnerAndRejectsDestroyedOrForeignResources)
    {
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.buffer = allocation_wire::handle_value<VkBuffer>(buffers[0]);
        const VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT, &dedicated, 0.75f};
        const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &priority, VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT,
                                              1};
        const VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags, sizes[0], types[0]};
        uint64_t memory{};
        ASSERT_EQ(host.allocate_memory_full(devices[0], allocation_wire::encode(info), memory), VK_SUCCESS);
        EXPECT_EQ(host.bind_buffer_memory(devices[0], buffers[0], memory, 0), VK_SUCCESS);
        EXPECT_EQ(host.set_device_memory_priority(devices[0], memory, 1.0f), VK_SUCCESS);
        dedicated.buffer = allocation_wire::handle_value<VkBuffer>(buffers[1]);
        EXPECT_EQ(host.allocate_memory_full(devices[0], allocation_wire::encode(info), memory), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(memory, 0u);
        // Use an enabled owner for the stale-resource assertion so feature gating cannot mask it.
        host.destroy_buffer(devices[0], buffers[0]);
        dedicated.buffer = allocation_wire::handle_value<VkBuffer>(buffers[0]);
        EXPECT_EQ(host.allocate_memory_full(devices[0], allocation_wire::encode(info), memory), VK_ERROR_INITIALIZATION_FAILED);
    }

    TEST_F(VulkanMemoryPriorityHostTest, DedicatedImageAndMalformedPacketChecks)
    {
        std::array<uint64_t, 2> images{};
        for (uint32_t i = 0; i < images.size(); ++i)
        {
            ASSERT_EQ(host.create_image(devices[i], VK_FORMAT_R8G8B8A8_UNORM, 8, 8, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                        VK_IMAGE_TILING_OPTIMAL, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TYPE_2D, 1, 1, 1, 0, images[i]),
                      VK_SUCCESS);
        }
        uint64_t size{};
        uint64_t alignment{};
        uint32_t type_bits{};
        ASSERT_EQ(host.get_image_memory_requirements(devices[0], images[0], size, alignment, type_bits), VK_SUCCESS);
        ASSERT_NE(type_bits, 0u);
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = allocation_wire::handle_value<VkImage>(images[0]);
        const VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated, size,
                                        static_cast<uint32_t>(std::countr_zero(type_bits))};
        uint64_t memory{};
        ASSERT_EQ(host.allocate_memory_full(devices[0], allocation_wire::encode(info), memory), VK_SUCCESS);
        EXPECT_EQ(host.bind_image_memory(devices[0], images[0], memory, 0), VK_SUCCESS);
        dedicated.image = allocation_wire::handle_value<VkImage>(images[1]);
        EXPECT_EQ(host.allocate_memory_full(devices[0], allocation_wire::encode(info), memory), VK_ERROR_INITIALIZATION_FAILED);
        host.destroy_image(devices[1], images[1]);
        EXPECT_EQ(host.allocate_memory_full(devices[1], allocation_wire::encode(info), memory), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(host.allocate_memory_full(devices[0], {}, memory), VK_ERROR_UNKNOWN);
        EXPECT_EQ(memory, 0u);
    }
}
