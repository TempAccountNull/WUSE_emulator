#include <gtest/gtest.h>

#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <vk_debug_utils_command_wire.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    namespace wire = sogen::gpu_bridge::debug_utils_command_wire;

    template <typename Function>
    Function global(PFN_vkGetInstanceProcAddr get, const char* name)
    {
        return reinterpret_cast<Function>(get(VK_NULL_HANDLE, name));
    }

    template <typename Function>
    Function instance(PFN_vkGetInstanceProcAddr get, VkInstance handle, const char* name)
    {
        return reinterpret_cast<Function>(get(handle, name));
    }

    template <typename Function>
    Function device(PFN_vkGetDeviceProcAddr get, VkDevice handle, const char* name)
    {
        return reinterpret_cast<Function>(get(handle, name));
    }

    TEST(VulkanDebugUtilsCommandWireTest, ObjectNameAndOpaqueTagRoundTripWithoutPointers)
    {
        wire::command name{};
        name.op = wire::operation::set_object_name;
        name.dispatch_id = 0x1234567887654321;
        name.object_id = 0xabcdef0123456789;
        name.object_type = VK_OBJECT_TYPE_BUFFER;
        name.name = "guest resource";
        auto bytes = wire::encode(name);
        ASSERT_EQ(bytes.size(), 64 + name.name.size());
        auto recovered = wire::decode(bytes);
        EXPECT_EQ(recovered.dispatch_id, name.dispatch_id);
        EXPECT_EQ(recovered.object_id, name.object_id);
        EXPECT_EQ(recovered.object_type, VK_OBJECT_TYPE_BUFFER);
        EXPECT_EQ(recovered.name, name.name);

        name.null_name = true;
        name.name.clear();
        recovered = wire::decode(wire::encode(name));
        EXPECT_TRUE(recovered.null_name);
        EXPECT_TRUE(recovered.name.empty());
        name.object_id = 0;
        EXPECT_NO_THROW((void)wire::encode(name));
        name.object_type = VK_OBJECT_TYPE_UNKNOWN;
        EXPECT_THROW((void)wire::encode(name), std::invalid_argument);
        name.object_id = 0xabcdef0123456789;
        name.object_type = VK_OBJECT_TYPE_BUFFER;

        wire::command tag{};
        tag.op = wire::operation::set_object_tag;
        tag.dispatch_id = name.dispatch_id;
        tag.object_id = name.object_id;
        tag.object_type = name.object_type;
        tag.tag_name = 0xFEDCBA9876543210;
        tag.tag = {std::byte{0}, std::byte{0xff}, std::byte{0}, std::byte{0x42}};
        recovered = wire::decode(wire::encode(tag));
        EXPECT_EQ(recovered.tag_name, tag.tag_name);
        EXPECT_EQ(recovered.tag, tag.tag);
    }

    TEST(VulkanDebugUtilsCommandWireTest, QueueAndCommandLabelsKeepColorsAndRejectCorruption)
    {
        for (const auto op : {wire::operation::queue_begin_label, wire::operation::queue_insert_label, wire::operation::command_begin_label,
                              wire::operation::command_insert_label})
        {
            wire::command label{};
            label.op = op;
            label.dispatch_id = 99;
            label.name = "marker";
            label.color = {0.125f, 0.5f, 1.0f, 0.0f};
            const auto result = wire::decode(wire::encode(label));
            EXPECT_EQ(result.name, label.name);
            EXPECT_EQ(result.color, label.color);
        }
        wire::command end{};
        end.op = wire::operation::command_end_label;
        end.dispatch_id = 99;
        auto bytes = wire::encode(end);
        EXPECT_EQ(wire::decode(bytes).op, wire::operation::command_end_label);
        bytes.pop_back();
        EXPECT_THROW((void)wire::decode(bytes), std::invalid_argument);

        end.name = "not allowed";
        EXPECT_THROW((void)wire::encode(end), std::invalid_argument);
        end.name.clear();
        end.op = wire::operation::set_object_tag;
        end.object_id = 1;
        end.object_type = VK_OBJECT_TYPE_BUFFER;
        EXPECT_THROW((void)wire::encode(end), std::invalid_argument);
    }

    TEST(VulkanDebugUtilsCommandWireTest, MarshalsGuestVulkanStructsWithExplicitLengths)
    {
        VkDebugUtilsObjectNameInfoEXT name{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        name.objectType = VK_OBJECT_TYPE_BUFFER;
        name.objectHandle = 88;
        name.pObjectName = "guest-buffer";
        auto result = wire::decode(wire::marshal_object_name(7, name));
        EXPECT_EQ(result.name, "guest-buffer");
        EXPECT_FALSE(result.null_name);
        name.pObjectName = nullptr;
        result = wire::decode(wire::marshal_object_name(7, name));
        EXPECT_TRUE(result.null_name);
        EXPECT_TRUE(result.name.empty());
        name.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        EXPECT_THROW((void)wire::marshal_object_name(7, name), std::invalid_argument);

        const std::array<std::byte, 4> tag_bytes{std::byte{0}, std::byte{0xff}, std::byte{0x55}, std::byte{0}};
        VkDebugUtilsObjectTagInfoEXT tag{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT};
        tag.objectType = VK_OBJECT_TYPE_BUFFER;
        tag.objectHandle = 88;
        tag.tagName = 123;
        tag.tagSize = tag_bytes.size();
        tag.pTag = tag_bytes.data();
        result = wire::decode(wire::marshal_object_tag(7, tag));
        EXPECT_EQ(result.tag, std::vector<std::byte>(tag_bytes.begin(), tag_bytes.end()));
        tag.tagSize = wire::max_payload_bytes + 1;
        EXPECT_THROW((void)wire::marshal_object_tag(7, tag), std::invalid_argument);

        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = "guest-label";
        label.color[0] = 0.25f;
        result = wire::decode(wire::marshal_label(wire::operation::command_begin_label, 99, label));
        EXPECT_EQ(result.name, "guest-label");
        EXPECT_EQ(result.color[0], 0.25f);
        EXPECT_EQ(wire::decode(wire::marshal_label_end(wire::operation::command_end_label, 99)).op, wire::operation::command_end_label);
        label.pLabelName = nullptr;
        EXPECT_THROW((void)wire::marshal_label(wire::operation::command_begin_label, 99, label), std::invalid_argument);
    }

    TEST(VulkanDebugUtilsCommandWireTest, NativeAmdObjectNamesTagsAndLabelsReachRealDriver)
    {
        const auto system = [] {
            std::array<wchar_t, MAX_PATH> path{};
            const auto length = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
            if (length == 0 || length + 14 >= path.size())
            {
                return static_cast<HMODULE>(nullptr);
            }
            wcscat_s(path.data(), path.size(), L"\\vulkan-1.dll");
            return LoadLibraryW(path.data());
        }();
        ASSERT_NE(system, nullptr);

        struct library_cleanup
        {
            HMODULE handle;

            ~library_cleanup()
            {
                FreeLibrary(handle);
            }
        } library{system};

        const auto get =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(system, "vkGetInstanceProcAddr")));
        ASSERT_NE(get, nullptr);
        const auto enumerate_extensions = global<PFN_vkEnumerateInstanceExtensionProperties>(get, "vkEnumerateInstanceExtensionProperties");
        const auto create_instance = global<PFN_vkCreateInstance>(get, "vkCreateInstance");
        ASSERT_NE(enumerate_extensions, nullptr);
        ASSERT_NE(create_instance, nullptr);
        uint32_t count = 0;
        ASSERT_EQ(enumerate_extensions(nullptr, &count, nullptr), VK_SUCCESS);
        std::vector<VkExtensionProperties> extensions(count);
        ASSERT_EQ(enumerate_extensions(nullptr, &count, extensions.data()), VK_SUCCESS);
        if (std::none_of(extensions.begin(), extensions.end(),
                         [](const auto& item) { return std::strcmp(item.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0; }))
        {
            GTEST_SKIP() << "Native loader does not expose VK_EXT_debug_utils";
        }

        const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.enabledExtensionCount = 1;
        instance_info.ppEnabledExtensionNames = &extension;
        VkInstance vk_instance{};
        ASSERT_EQ(create_instance(&instance_info, nullptr, &vk_instance), VK_SUCCESS);
        const auto destroy_instance = instance<PFN_vkDestroyInstance>(get, vk_instance, "vkDestroyInstance");
        ASSERT_NE(destroy_instance, nullptr);

        struct instance_cleanup
        {
            VkInstance handle;
            PFN_vkDestroyInstance destroy;

            ~instance_cleanup()
            {
                destroy(handle, nullptr);
            }
        } instance_guard{vk_instance, destroy_instance};

        const auto enumerate_devices = instance<PFN_vkEnumeratePhysicalDevices>(get, vk_instance, "vkEnumeratePhysicalDevices");
        const auto get_properties = instance<PFN_vkGetPhysicalDeviceProperties>(get, vk_instance, "vkGetPhysicalDeviceProperties");
        const auto get_queues =
            instance<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(get, vk_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        const auto create_device = instance<PFN_vkCreateDevice>(get, vk_instance, "vkCreateDevice");
        ASSERT_NE(enumerate_devices, nullptr);
        ASSERT_NE(get_properties, nullptr);
        ASSERT_NE(get_queues, nullptr);
        ASSERT_NE(create_device, nullptr);
        ASSERT_EQ(enumerate_devices(vk_instance, &count, nullptr), VK_SUCCESS);
        std::vector<VkPhysicalDevice> physical_devices(count);
        ASSERT_EQ(enumerate_devices(vk_instance, &count, physical_devices.data()), VK_SUCCESS);
        VkPhysicalDevice amd{};
        for (auto candidate : physical_devices)
        {
            VkPhysicalDeviceProperties properties{};
            get_properties(candidate, &properties);
            if (properties.vendorID == 0x1002)
            {
                amd = candidate;
                break;
            }
        }
        if (!amd)
        {
            GTEST_SKIP() << "No AMD Vulkan physical device";
        }
        get_queues(amd, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(count);
        get_queues(amd, &count, queue_families.data());
        const auto found = std::find_if(queue_families.begin(), queue_families.end(), [](const auto& family) {
            return family.queueCount && (family.queueFlags & VK_QUEUE_GRAPHICS_BIT);
        });
        ASSERT_NE(found, queue_families.end());
        const uint32_t family = static_cast<uint32_t>(found - queue_families.begin());
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        VkDevice vk_device{};
        ASSERT_EQ(create_device(amd, &device_info, nullptr, &vk_device), VK_SUCCESS);
        const auto get_device = instance<PFN_vkGetDeviceProcAddr>(get, vk_instance, "vkGetDeviceProcAddr");
        ASSERT_NE(get_device, nullptr);
        const auto destroy_device = device<PFN_vkDestroyDevice>(get_device, vk_device, "vkDestroyDevice");
        ASSERT_NE(destroy_device, nullptr);

        struct device_cleanup
        {
            VkDevice handle;
            PFN_vkDestroyDevice destroy;

            ~device_cleanup()
            {
                destroy(handle, nullptr);
            }
        } device_guard{vk_device, destroy_device};

        const auto set_name = device<PFN_vkSetDebugUtilsObjectNameEXT>(get_device, vk_device, "vkSetDebugUtilsObjectNameEXT");
        const auto set_tag = device<PFN_vkSetDebugUtilsObjectTagEXT>(get_device, vk_device, "vkSetDebugUtilsObjectTagEXT");
        const auto queue_begin = device<PFN_vkQueueBeginDebugUtilsLabelEXT>(get_device, vk_device, "vkQueueBeginDebugUtilsLabelEXT");
        const auto queue_insert = device<PFN_vkQueueInsertDebugUtilsLabelEXT>(get_device, vk_device, "vkQueueInsertDebugUtilsLabelEXT");
        const auto queue_end = device<PFN_vkQueueEndDebugUtilsLabelEXT>(get_device, vk_device, "vkQueueEndDebugUtilsLabelEXT");
        const auto get_device_queue = device<PFN_vkGetDeviceQueue>(get_device, vk_device, "vkGetDeviceQueue");
        ASSERT_NE(set_name, nullptr);
        ASSERT_NE(set_tag, nullptr);
        ASSERT_NE(queue_begin, nullptr);
        ASSERT_NE(queue_insert, nullptr);
        ASSERT_NE(queue_end, nullptr);
        ASSERT_NE(get_device_queue, nullptr);

        wire::command name{};
        name.op = wire::operation::set_object_name;
        name.dispatch_id = reinterpret_cast<uint64_t>(vk_device);
        name.object_id = name.dispatch_id;
        name.object_type = VK_OBJECT_TYPE_DEVICE;
        name.name = "sogen-native-amd-device";
        auto decoded_name = wire::decode(wire::encode(name));
        VkDebugUtilsObjectNameInfoEXT name_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        name_info.objectType = decoded_name.object_type;
        name_info.objectHandle = decoded_name.object_id;
        name_info.pObjectName = decoded_name.name.c_str();
        EXPECT_EQ(set_name(vk_device, &name_info), VK_SUCCESS);

        wire::command tag{};
        tag.op = wire::operation::set_object_tag;
        tag.dispatch_id = name.dispatch_id;
        tag.object_id = name.object_id;
        tag.object_type = VK_OBJECT_TYPE_DEVICE;
        tag.tag_name = 0x20260925;
        tag.tag = {std::byte{0x12}, std::byte{0}, std::byte{0xfe}};
        auto decoded_tag = wire::decode(wire::encode(tag));
        VkDebugUtilsObjectTagInfoEXT tag_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT};
        tag_info.objectType = decoded_tag.object_type;
        tag_info.objectHandle = decoded_tag.object_id;
        tag_info.tagName = decoded_tag.tag_name;
        tag_info.tagSize = decoded_tag.tag.size();
        tag_info.pTag = decoded_tag.tag.data();
        EXPECT_EQ(set_tag(vk_device, &tag_info), VK_SUCCESS);

        VkQueue vk_queue{};
        get_device_queue(vk_device, family, 0, &vk_queue);
        ASSERT_NE(vk_queue, VK_NULL_HANDLE);
        wire::command label{};
        label.op = wire::operation::queue_begin_label;
        label.dispatch_id = reinterpret_cast<uint64_t>(vk_queue);
        label.name = "sogen-native-amd-label";
        label.color = {0.1f, 0.2f, 0.3f, 1.0f};
        auto decoded_label = wire::decode(wire::encode(label));
        VkDebugUtilsLabelEXT label_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label_info.pLabelName = decoded_label.name.c_str();
        std::copy(decoded_label.color.begin(), decoded_label.color.end(), label_info.color);
        queue_begin(vk_queue, &label_info);
        queue_insert(vk_queue, &label_info);
        queue_end(vk_queue);

        const auto create_pool = device<PFN_vkCreateCommandPool>(get_device, vk_device, "vkCreateCommandPool");
        const auto destroy_pool = device<PFN_vkDestroyCommandPool>(get_device, vk_device, "vkDestroyCommandPool");
        const auto allocate_commands = device<PFN_vkAllocateCommandBuffers>(get_device, vk_device, "vkAllocateCommandBuffers");
        const auto begin_command = device<PFN_vkBeginCommandBuffer>(get_device, vk_device, "vkBeginCommandBuffer");
        const auto end_command = device<PFN_vkEndCommandBuffer>(get_device, vk_device, "vkEndCommandBuffer");
        const auto cmd_begin = device<PFN_vkCmdBeginDebugUtilsLabelEXT>(get_device, vk_device, "vkCmdBeginDebugUtilsLabelEXT");
        const auto cmd_insert = device<PFN_vkCmdInsertDebugUtilsLabelEXT>(get_device, vk_device, "vkCmdInsertDebugUtilsLabelEXT");
        const auto cmd_end = device<PFN_vkCmdEndDebugUtilsLabelEXT>(get_device, vk_device, "vkCmdEndDebugUtilsLabelEXT");
        ASSERT_NE(create_pool, nullptr);
        ASSERT_NE(destroy_pool, nullptr);
        ASSERT_NE(allocate_commands, nullptr);
        ASSERT_NE(begin_command, nullptr);
        ASSERT_NE(end_command, nullptr);
        ASSERT_NE(cmd_begin, nullptr);
        ASSERT_NE(cmd_insert, nullptr);
        ASSERT_NE(cmd_end, nullptr);
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = family;
        VkCommandPool pool{};
        ASSERT_EQ(create_pool(vk_device, &pool_info, nullptr, &pool), VK_SUCCESS);

        struct pool_cleanup
        {
            VkDevice device;
            VkCommandPool pool;
            PFN_vkDestroyCommandPool destroy;

            ~pool_cleanup()
            {
                destroy(device, pool, nullptr);
            }
        } pool_guard{vk_device, pool, destroy_pool};

        VkCommandBufferAllocateInfo allocate_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate_info.commandPool = pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        VkCommandBuffer command_buffer{};
        ASSERT_EQ(allocate_commands(vk_device, &allocate_info, &command_buffer), VK_SUCCESS);
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        ASSERT_EQ(begin_command(command_buffer, &begin_info), VK_SUCCESS);
        label.op = wire::operation::command_begin_label;
        label.dispatch_id = reinterpret_cast<uint64_t>(command_buffer);
        decoded_label = wire::decode(wire::encode(label));
        label_info.pLabelName = decoded_label.name.c_str();
        cmd_begin(command_buffer, &label_info);
        label.op = wire::operation::command_insert_label;
        decoded_label = wire::decode(wire::encode(label));
        label_info.pLabelName = decoded_label.name.c_str();
        cmd_insert(command_buffer, &label_info);
        cmd_end(command_buffer);
        EXPECT_EQ(end_command(command_buffer), VK_SUCCESS);
    }
}
