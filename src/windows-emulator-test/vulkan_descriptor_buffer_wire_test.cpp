#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gtest/gtest.h>
#include <vk_descriptor_buffer_wire.hpp>
#include <gpu_bridge_protocol.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sogen::test
{
    namespace wire = gpu_bridge::descriptor_buffer_wire;

    TEST(VulkanDescriptorBufferWireTest, EverySupportedDescriptorVariantHasAStableTaggedRoundTrip)
    {
        constexpr std::array types{
            VK_DESCRIPTOR_TYPE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV,
        };
        for (auto type : types)
        {
            wire::get_info source{};
            source.type = type;
            source.kind = wire::kind_for(type);
            source.present = true;
            source.values[0] =
                source.kind == wire::payload_kind::image && type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? 0 : 0x1122334455667788ull;
            source.values[1] =
                source.kind == wire::payload_kind::image || source.kind == wire::payload_kind::address ? 0x8877665544332211ull : 0;
            source.values[2] = source.kind == wire::payload_kind::image || source.kind == wire::payload_kind::address ? 37 : 0;
            const auto bytes = wire::encode(source);
            static_assert(wire::get_info_byte_count == 40);
            const auto restored = wire::decode(bytes);
            EXPECT_EQ(restored.type, source.type);
            EXPECT_EQ(restored.kind, source.kind);
            EXPECT_EQ(restored.present, source.present);
            EXPECT_EQ(restored.values, source.values);
        }
    }

    TEST(VulkanDescriptorBufferWireTest, FixedLittleEndianEncodingCarriesNoNativePointerLayout)
    {
        wire::get_info source{
            .type = VK_DESCRIPTOR_TYPE_SAMPLER,
            .kind = wire::payload_kind::sampler,
            .present = true,
            .values = {0x1122334455667788ull, 0, 0},
        };
        const auto bytes = wire::encode(source);
        EXPECT_EQ(std::to_integer<uint8_t>(bytes[4]), 1);
        EXPECT_EQ(std::to_integer<uint8_t>(bytes[8]), 1);
        EXPECT_EQ(std::to_integer<uint8_t>(bytes[12]), 0);
        constexpr std::array<uint8_t, 8> sampler_id{0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
        for (size_t i = 0; i < sampler_id.size(); ++i)
        {
            EXPECT_EQ(std::to_integer<uint8_t>(bytes[16 + i]), sampler_id[i]);
        }
        EXPECT_EQ(bytes.size(), 40u);
    }

    TEST(VulkanDescriptorBufferWireTest, RejectsUnknownTypesAndMismatchedTaggedPayloads)
    {
        wire::get_info source{
            .type = VK_DESCRIPTOR_TYPE_SAMPLER,
            .kind = wire::payload_kind::sampler,
            .present = true,
            .values = {7, 0, 0},
        };
        EXPECT_THROW(wire::kind_for(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC), std::invalid_argument);
        EXPECT_THROW(wire::kind_for(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC), std::invalid_argument);
        EXPECT_THROW(wire::kind_for(VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK), std::invalid_argument);
        source.kind = wire::payload_kind::image;
        EXPECT_THROW(wire::encode(source), std::invalid_argument);
        source.kind = wire::payload_kind::sampler;
        source.values[1] = 1;
        EXPECT_THROW(wire::encode(source), std::invalid_argument);
    }

    TEST(VulkanDescriptorBufferWireTest, RejectsCorruptOrTruncatedPackets)
    {
        wire::get_info source{
            .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .kind = wire::payload_kind::image,
            .present = true,
            .values = {0, 7, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        auto bytes = wire::encode(source);
        EXPECT_THROW(wire::decode(std::span<const std::byte>(bytes.data(), bytes.size() - 1)), std::invalid_argument);
        bytes[12] = std::byte{1};
        EXPECT_THROW(wire::decode(bytes), std::invalid_argument);
        bytes[12] = std::byte{0};
        bytes[8] = std::byte{2};
        EXPECT_THROW(wire::decode(bytes), std::invalid_argument);
        bytes[8] = std::byte{1};
        bytes[4] = std::byte{3};
        EXPECT_THROW(wire::decode(bytes), std::invalid_argument);
        bytes = wire::encode(source);
        bytes[36] = std::byte{1};
        EXPECT_THROW(wire::decode(bytes), std::invalid_argument);
    }

    TEST(VulkanDescriptorBufferWireTest, PreservesOptionalNullPayloadWithoutLeakingData)
    {
        for (const auto type : {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER})
        {
            wire::get_info source{};
            source.type = type;
            source.kind = wire::kind_for(type);
            EXPECT_THROW(wire::encode(source), std::invalid_argument);
            EXPECT_FALSE(wire::decode(wire::encode(source, true), true).present);
            source.values[0] = 1;
            EXPECT_THROW(wire::encode(source, true), std::invalid_argument);
        }
        wire::get_info sampler{};
        sampler.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        sampler.kind = wire::payload_kind::sampler;
        EXPECT_THROW(wire::encode(sampler), std::invalid_argument);
    }

    TEST(VulkanDescriptorBufferWireTest, UsesNativeDescriptorSizeAndRejectsOversizedReadback)
    {
        EXPECT_NO_THROW(wire::validate_descriptor_data_size(32, 32));
        EXPECT_THROW(wire::validate_descriptor_data_size(0, 0), std::invalid_argument);
        EXPECT_THROW(wire::validate_descriptor_data_size(32, 16), std::invalid_argument);
        EXPECT_THROW(wire::validate_descriptor_data_size(wire::max_descriptor_bytes + 1, wire::max_descriptor_bytes + 1),
                     std::invalid_argument);
        EXPECT_NO_THROW(wire::validate_descriptor_data_size(wire::max_descriptor_bytes, wire::max_descriptor_bytes));
    }

    namespace
    {
        template <typename Handle>
        Handle guest_handle(uint64_t id)
        {
            if constexpr (std::is_pointer_v<Handle>)
            {
                return reinterpret_cast<Handle>(static_cast<uintptr_t>(id));
            }
            else
            {
                return static_cast<Handle>(id);
            }
        }

        template <typename Handle>
        uint64_t guest_id(Handle handle)
        {
            if constexpr (std::is_pointer_v<Handle>)
            {
                return reinterpret_cast<uintptr_t>(handle);
            }
            else
            {
                return static_cast<uint64_t>(handle);
            }
        }
    }

    TEST(VulkanDescriptorBufferWireTest, SnapshotsNestedGuestStructuresWithoutTransmittingPointers)
    {
        VkSampler sampler = guest_handle<VkSampler>(0x1234);
        VkDescriptorGetInfoEXT source{};
        source.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
        source.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        source.data.pSampler = &sampler;
        const auto mapped_sampler = wire::snapshot(source, [](auto handle) { return guest_id(handle); });
        EXPECT_EQ(mapped_sampler.values[0], 0x1234u);
        sampler = guest_handle<VkSampler>(0x9876);
        EXPECT_EQ(wire::decode(wire::encode(mapped_sampler)).values[0], 0x1234u);

        VkDescriptorImageInfo image{};
        image.sampler = sampler;
        image.imageView = guest_handle<VkImageView>(0x4321);
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        source.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        source.data.pCombinedImageSampler = &image;
        const auto mapped_image = wire::snapshot(source, [](auto handle) { return guest_id(handle); });
        EXPECT_EQ(mapped_image.values[0], 0x9876u);
        EXPECT_EQ(mapped_image.values[1], 0x4321u);
        EXPECT_EQ(mapped_image.values[2], static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

        VkDescriptorAddressInfoEXT address{};
        address.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT;
        address.address = 0x123456780000ull;
        address.range = 256;
        address.format = VK_FORMAT_UNDEFINED;
        source.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        source.data.pUniformBuffer = &address;
        const auto mapped_address = wire::snapshot(source, [](auto handle) { return guest_id(handle); });
        EXPECT_EQ(mapped_address.values[0], address.address);
        EXPECT_EQ(mapped_address.values[1], 256u);
        EXPECT_EQ(wire::decode(wire::encode(mapped_address)).values[0], address.address);
    }

    TEST(VulkanDescriptorBufferWireTest, RejectsUnsupportedChainsAndNullPayloadWithoutFeature)
    {
        VkDescriptorGetInfoEXT source{};
        source.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
        source.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        source.data.pSampledImage = nullptr;
        EXPECT_THROW(wire::snapshot(source, [](auto handle) { return guest_id(handle); }), std::invalid_argument);
        EXPECT_FALSE(wire::snapshot(source, [](auto handle) { return guest_id(handle); }, true).present);

        source.pNext = &source;
        EXPECT_THROW(wire::snapshot(source, [](auto handle) { return guest_id(handle); }, true), std::invalid_argument);
        source.pNext = nullptr;
        source.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        EXPECT_THROW(wire::snapshot(source, [](auto handle) { return guest_id(handle); }, true), std::invalid_argument);

        source.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
        source.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        VkDescriptorAddressInfoEXT address{};
        address.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT;
        address.pNext = &source;
        source.data.pUniformBuffer = &address;
        EXPECT_THROW(wire::snapshot(source, [](auto handle) { return guest_id(handle); }), std::invalid_argument);
    }

    TEST(VulkanDescriptorBufferWireTest, RejectsMissingRequiredHandlesAndGatesNullDescriptor)
    {
        wire::get_info sampler{.type = VK_DESCRIPTOR_TYPE_SAMPLER, .kind = wire::payload_kind::sampler, .present = true};
        EXPECT_THROW(wire::encode(sampler), std::invalid_argument);

        wire::get_info combined{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .kind = wire::payload_kind::image,
                                .present = true,
                                .values = {0, 7, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        EXPECT_THROW(wire::encode(combined), std::invalid_argument);

        wire::get_info sampled{.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                               .kind = wire::payload_kind::image,
                               .present = true,
                               .values = {0, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        EXPECT_THROW(wire::encode(sampled), std::invalid_argument);
        EXPECT_NO_THROW(wire::encode(sampled, true));

        wire::get_info address{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                               .kind = wire::payload_kind::address,
                               .present = true,
                               .values = {0, 256, VK_FORMAT_UNDEFINED}};
        EXPECT_THROW(wire::encode(address), std::invalid_argument);
        EXPECT_NO_THROW(wire::encode(address, true));
    }

    TEST(VulkanDescriptorBufferWireTest, ProtocolCarriesBoundedDescriptorBytesAfterFixedRequest)
    {
        namespace gb = gpu_bridge;
        EXPECT_EQ(static_cast<uint32_t>(gb::command::get_descriptor), 0x8b7u);
        EXPECT_EQ(gb::ioctl_get_descriptor, gb::make_ioctl(0x8b7u));
        EXPECT_EQ(sizeof(gb::get_descriptor_request), 16u);
        EXPECT_EQ(sizeof(gb::get_descriptor_response), 8u);
        EXPECT_EQ(gb::max_get_descriptor_bytes, wire::max_descriptor_bytes);
        const gb::get_descriptor_request request{.device = 7, .data_size = 16, .wire_size = wire::get_info_byte_count};
        EXPECT_EQ(request.device, 7u);
        EXPECT_EQ(request.data_size, 16u);
        EXPECT_EQ(request.wire_size, 40u);
    }

    TEST(VulkanDescriptorBufferWireTest, NativeRx6700ReturnsStableSamplerAndUniformBufferDescriptors)
    {
        struct native_context
        {
            HMODULE module{};
            VkInstance instance{};
            VkDevice device{};
            VkSampler sampler{};
            VkBuffer buffer{};
            VkDeviceMemory memory{};
            PFN_vkDestroyInstance destroy_instance{};
            PFN_vkDestroyDevice destroy_device{};
            PFN_vkDestroySampler destroy_sampler{};
            PFN_vkDestroyBuffer destroy_buffer{};
            PFN_vkFreeMemory free_memory{};

            ~native_context()
            {
                if (buffer && destroy_buffer)
                {
                    destroy_buffer(device, buffer, nullptr);
                }
                if (memory && free_memory)
                {
                    free_memory(device, memory, nullptr);
                }
                if (sampler && destroy_sampler)
                {
                    destroy_sampler(device, sampler, nullptr);
                }
                if (device && destroy_device)
                {
                    destroy_device(device, nullptr);
                }
                if (instance && destroy_instance)
                {
                    destroy_instance(instance, nullptr);
                }
                if (module)
                {
                    FreeLibrary(module);
                }
            }
        } native;

        std::array<wchar_t, MAX_PATH> system_directory{};
        const UINT directory_length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
        ASSERT_GT(directory_length, 0u);
        ASSERT_LT(directory_length, system_directory.size());
        const std::wstring loader_path = std::wstring(system_directory.data(), directory_length) + L"\\vulkan-1.dll";
        native.module = LoadLibraryW(loader_path.c_str());
        if (!native.module)
        {
            GTEST_SKIP() << "System Vulkan loader unavailable";
        }
        const auto get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(native.module, "vkGetInstanceProcAddr"));
        ASSERT_NE(get_instance_proc, nullptr);
        const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc(VK_NULL_HANDLE, "vkCreateInstance"));
        ASSERT_NE(create_instance, nullptr);

        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &application;
        if (create_instance(&instance_info, nullptr, &native.instance) != VK_SUCCESS)
        {
            GTEST_SKIP() << "Vulkan 1.2 instance unavailable";
        }
        native.destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get_instance_proc(native.instance, "vkDestroyInstance"));
        ASSERT_NE(native.destroy_instance, nullptr);

        const auto enumerate_physical_devices =
            reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_instance_proc(native.instance, "vkEnumeratePhysicalDevices"));
        const auto get_physical_device_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_instance_proc(native.instance, "vkGetPhysicalDeviceProperties"));
        const auto get_physical_device_properties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get_instance_proc(native.instance, "vkGetPhysicalDeviceProperties2"));
        const auto get_physical_device_features2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(get_instance_proc(native.instance, "vkGetPhysicalDeviceFeatures2"));
        const auto enumerate_device_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            get_instance_proc(native.instance, "vkEnumerateDeviceExtensionProperties"));
        const auto get_queue_families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            get_instance_proc(native.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        const auto get_memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            get_instance_proc(native.instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto create_device = reinterpret_cast<PFN_vkCreateDevice>(get_instance_proc(native.instance, "vkCreateDevice"));
        const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_instance_proc(native.instance, "vkGetDeviceProcAddr"));
        ASSERT_NE(enumerate_physical_devices, nullptr);
        ASSERT_NE(get_physical_device_properties, nullptr);
        ASSERT_NE(get_physical_device_properties2, nullptr);
        ASSERT_NE(get_physical_device_features2, nullptr);
        ASSERT_NE(enumerate_device_extensions, nullptr);
        ASSERT_NE(get_queue_families, nullptr);
        ASSERT_NE(get_memory_properties, nullptr);
        ASSERT_NE(create_device, nullptr);
        ASSERT_NE(get_device_proc, nullptr);

        uint32_t physical_count = 0;
        ASSERT_EQ(enumerate_physical_devices(native.instance, &physical_count, nullptr), VK_SUCCESS);
        std::vector<VkPhysicalDevice> physical_devices(physical_count);
        ASSERT_EQ(enumerate_physical_devices(native.instance, &physical_count, physical_devices.data()), VK_SUCCESS);
        VkPhysicalDevice rx6700 = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties device_properties{};
        for (const auto physical_device : physical_devices)
        {
            VkPhysicalDeviceProperties properties{};
            get_physical_device_properties(physical_device, &properties);
            if (properties.vendorID == 0x1002 && std::strstr(properties.deviceName, "6700"))
            {
                rx6700 = physical_device;
                device_properties = properties;
                break;
            }
        }
        if (!rx6700)
        {
            GTEST_SKIP() << "RX 6700 Vulkan device unavailable";
        }

        uint32_t extension_count = 0;
        ASSERT_EQ(enumerate_device_extensions(rx6700, nullptr, &extension_count, nullptr), VK_SUCCESS);
        std::vector<VkExtensionProperties> extensions(extension_count);
        ASSERT_EQ(enumerate_device_extensions(rx6700, nullptr, &extension_count, extensions.data()), VK_SUCCESS);
        if (std::none_of(extensions.begin(), extensions.end(), [](const auto& extension) {
                return std::strcmp(extension.extensionName, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME) == 0;
            }))
        {
            GTEST_SKIP() << "RX 6700 lacks VK_EXT_descriptor_buffer";
        }

        VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_features{};
        descriptor_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
        VkPhysicalDeviceBufferDeviceAddressFeatures address_features{};
        address_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        descriptor_features.pNext = &address_features;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &descriptor_features;
        get_physical_device_features2(rx6700, &features);
        if (!descriptor_features.descriptorBuffer || !address_features.bufferDeviceAddress)
        {
            GTEST_SKIP() << "RX 6700 descriptorBuffer or bufferDeviceAddress feature unavailable";
        }

        VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_properties{};
        descriptor_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &descriptor_properties;
        get_physical_device_properties2(rx6700, &properties2);
        ASSERT_NO_THROW(
            wire::validate_descriptor_data_size(descriptor_properties.samplerDescriptorSize, descriptor_properties.samplerDescriptorSize));
        ASSERT_NO_THROW(wire::validate_descriptor_data_size(descriptor_properties.uniformBufferDescriptorSize,
                                                            descriptor_properties.uniformBufferDescriptorSize));

        uint32_t queue_count = 0;
        get_queue_families(rx6700, &queue_count, nullptr);
        ASSERT_GT(queue_count, 0u);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        get_queue_families(rx6700, &queue_count, queues.data());
        uint32_t queue_family = queue_count;
        for (uint32_t i = 0; i < queue_count; ++i)
        {
            if (queues[i].queueCount)
            {
                queue_family = i;
                break;
            }
        }
        ASSERT_LT(queue_family, queue_count);
        const float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;
        VkPhysicalDeviceDescriptorBufferFeaturesEXT enabled_descriptor_features{};
        enabled_descriptor_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
        enabled_descriptor_features.descriptorBuffer = VK_TRUE;
        VkPhysicalDeviceBufferDeviceAddressFeatures enabled_address_features{};
        enabled_address_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        enabled_address_features.bufferDeviceAddress = VK_TRUE;
        enabled_descriptor_features.pNext = &enabled_address_features;
        const char* descriptor_extension = VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME;
        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = &enabled_descriptor_features;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = &descriptor_extension;
        ASSERT_EQ(create_device(rx6700, &device_info, nullptr, &native.device), VK_SUCCESS);

        native.destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(get_device_proc(native.device, "vkDestroyDevice"));
        native.destroy_sampler = reinterpret_cast<PFN_vkDestroySampler>(get_device_proc(native.device, "vkDestroySampler"));
        native.destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(get_device_proc(native.device, "vkDestroyBuffer"));
        native.free_memory = reinterpret_cast<PFN_vkFreeMemory>(get_device_proc(native.device, "vkFreeMemory"));
        const auto create_sampler = reinterpret_cast<PFN_vkCreateSampler>(get_device_proc(native.device, "vkCreateSampler"));
        const auto get_descriptor = reinterpret_cast<PFN_vkGetDescriptorEXT>(get_device_proc(native.device, "vkGetDescriptorEXT"));
        const auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(get_device_proc(native.device, "vkCreateBuffer"));
        const auto get_buffer_requirements =
            reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get_device_proc(native.device, "vkGetBufferMemoryRequirements"));
        const auto allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(get_device_proc(native.device, "vkAllocateMemory"));
        const auto bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(get_device_proc(native.device, "vkBindBufferMemory"));
        const auto get_buffer_address =
            reinterpret_cast<PFN_vkGetBufferDeviceAddress>(get_device_proc(native.device, "vkGetBufferDeviceAddress"));
        ASSERT_NE(native.destroy_device, nullptr);
        ASSERT_NE(native.destroy_sampler, nullptr);
        ASSERT_NE(native.destroy_buffer, nullptr);
        ASSERT_NE(native.free_memory, nullptr);
        ASSERT_NE(create_sampler, nullptr);
        ASSERT_NE(get_descriptor, nullptr);
        ASSERT_NE(create_buffer, nullptr);
        ASSERT_NE(get_buffer_requirements, nullptr);
        ASSERT_NE(allocate_memory, nullptr);
        ASSERT_NE(bind_buffer_memory, nullptr);
        ASSERT_NE(get_buffer_address, nullptr);

        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 1.0f;
        ASSERT_EQ(create_sampler(native.device, &sampler_info, nullptr, &native.sampler), VK_SUCCESS);
        VkDescriptorGetInfoEXT sampler_descriptor{};
        sampler_descriptor.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
        sampler_descriptor.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        sampler_descriptor.data.pSampler = &native.sampler;
        const auto sampler_size = descriptor_properties.samplerDescriptorSize;
        std::vector<std::byte> sampler_first(sampler_size, std::byte{0xa5});
        std::vector<std::byte> sampler_second(sampler_size, std::byte{0x5a});
        get_descriptor(native.device, &sampler_descriptor, sampler_size, sampler_first.data());
        get_descriptor(native.device, &sampler_descriptor, sampler_size, sampler_second.data());
        EXPECT_EQ(sampler_first, sampler_second);
        EXPECT_NE(sampler_first, std::vector<std::byte>(sampler_size, std::byte{0xa5}));
        EXPECT_NE(sampler_second, std::vector<std::byte>(sampler_size, std::byte{0x5a}));

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = 256;
        buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ASSERT_EQ(create_buffer(native.device, &buffer_info, nullptr, &native.buffer), VK_SUCCESS);
        VkMemoryRequirements requirements{};
        get_buffer_requirements(native.device, native.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        get_memory_properties(rx6700, &memory_properties);
        uint32_t memory_type = memory_properties.memoryTypeCount;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        {
            if (requirements.memoryTypeBits & (1u << i))
            {
                memory_type = i;
                break;
            }
        }
        ASSERT_LT(memory_type, memory_properties.memoryTypeCount);
        VkMemoryAllocateFlagsInfo allocation_flags{};
        allocation_flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocation_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocation_info{};
        allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation_info.pNext = &allocation_flags;
        allocation_info.allocationSize = requirements.size;
        allocation_info.memoryTypeIndex = memory_type;
        ASSERT_EQ(allocate_memory(native.device, &allocation_info, nullptr, &native.memory), VK_SUCCESS);
        ASSERT_EQ(bind_buffer_memory(native.device, native.buffer, native.memory, 0), VK_SUCCESS);
        VkBufferDeviceAddressInfo address_info{};
        address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        address_info.buffer = native.buffer;
        const VkDeviceAddress buffer_address = get_buffer_address(native.device, &address_info);
        ASSERT_NE(buffer_address, 0u);

        VkDescriptorAddressInfoEXT uniform_address{};
        uniform_address.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT;
        uniform_address.address = buffer_address;
        uniform_address.range = buffer_info.size;
        uniform_address.format = VK_FORMAT_UNDEFINED;
        VkDescriptorGetInfoEXT uniform_descriptor{};
        uniform_descriptor.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
        uniform_descriptor.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uniform_descriptor.data.pUniformBuffer = &uniform_address;
        const auto uniform_size = descriptor_properties.uniformBufferDescriptorSize;
        std::vector<std::byte> uniform_first(uniform_size, std::byte{0xa5});
        std::vector<std::byte> uniform_second(uniform_size, std::byte{0x5a});
        get_descriptor(native.device, &uniform_descriptor, uniform_size, uniform_first.data());
        get_descriptor(native.device, &uniform_descriptor, uniform_size, uniform_second.data());
        EXPECT_EQ(uniform_first, uniform_second);
        EXPECT_NE(uniform_first, std::vector<std::byte>(uniform_size, std::byte{0xa5}));
        EXPECT_NE(uniform_second, std::vector<std::byte>(uniform_size, std::byte{0x5a}));

        auto print_bytes = [](const std::vector<std::byte>& descriptor) {
            std::ostringstream output;
            for (const auto byte : descriptor)
            {
                output << std::hex << std::setfill('0') << std::setw(2) << std::to_integer<unsigned>(byte);
            }
            return output.str();
        };
        std::cout << "Native " << device_properties.deviceName << " vkGetDescriptorEXT samplerSize=" << sampler_size
                  << " samplerBytes=" << print_bytes(sampler_first) << " uniformBufferSize=" << uniform_size
                  << " uniformBufferBytes=" << print_bytes(uniform_first) << '\n';
    }

}
