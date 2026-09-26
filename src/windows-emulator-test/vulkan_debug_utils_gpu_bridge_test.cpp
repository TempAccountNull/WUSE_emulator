#include <gtest/gtest.h>

#include "emulation_test_utils.hpp"
#include "../windows-emulator/io_device.hpp"
#include <windows_emulator.hpp>
#include <gpu_bridge_protocol.hpp>
#include <vk_debug_utils_command_wire.hpp>
#include <vk_debug_utils_messenger_wire.hpp>
#include <vk_debug_utils_wire.hpp>

#include <vulkan/vulkan_core.h>

#include <cstring>
#include <vector>

namespace sogen::test
{
    namespace messenger_wire = gpu_bridge::debug_utils_messenger_wire;
    namespace callback_wire = gpu_bridge::debug_utils_wire;

    VKAPI_ATTR VkBool32 VKAPI_CALL unreachable_guest_callback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                                VkDebugUtilsMessageTypeFlagsEXT,
                                                                const VkDebugUtilsMessengerCallbackDataEXT*, void*)
    {
        ADD_FAILURE() << "Host IOCTL must queue callback, never invoke a guest address";
        return VK_FALSE;
    }

    TEST(VulkanDebugUtilsGpuBridgeTest, RealAmdCallbackCrossesIoctlAsBoundedGuestThreadPacket)
    {
        emulator_settings settings{.disable_logging = true};
        settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        windows_emulator win = create_sample_emulator(std::move(settings), {}, {}, std::move(interfaces));
        win.setup_process_if_necessary();
        const uint64_t memory = win.memory.allocate_memory(2 * 1024 * 1024, memory_permission::read_write);
        ASSERT_NE(memory, 0u);
        io_device_container bridge{u"SogenGpu", win, {}};

        const auto ioctl = [&](uint32_t code, const void* input, size_t size, size_t output_size) {
            if (size) win.emu().write_memory(memory, input, size);
            io_device_context request{win.memory};
            request.vcpu = &win.vcpu(0);
            request.io_control_code = code;
            request.input_buffer = size ? memory : 0;
            request.input_buffer_length = static_cast<ULONG>(size);
            request.output_buffer = memory + 0x1000;
            request.output_buffer_length = static_cast<ULONG>(output_size);
            return bridge.io_control(win, request);
        };

        ASSERT_EQ(ioctl(gpu_bridge::ioctl_debug_utils_capabilities, nullptr, 0,
                        sizeof(gpu_bridge::debug_utils_capabilities_response)), STATUS_SUCCESS);
        if (!win.emu().read_memory<gpu_bridge::debug_utils_capabilities_response>(memory + 0x1000).available)
            GTEST_SKIP() << "Native Vulkan loader lacks VK_EXT_debug_utils";

        const gpu_bridge::create_instance_request instance_request{
            .magic = gpu_bridge::create_instance_request_magic, .extension_bits = gpu_bridge::instance_ext_debug_utils};
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_create_instance, &instance_request, sizeof(instance_request),
                        sizeof(gpu_bridge::create_instance_response)), STATUS_SUCCESS);
        const auto created = win.emu().read_memory<gpu_bridge::create_instance_response>(memory + 0x1000);
        ASSERT_EQ(created.vk_result, VK_SUCCESS);
        ASSERT_NE(created.instance, 0u);

        VkDebugUtilsMessengerCreateInfoEXT create_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        create_info.pfnUserCallback = unreachable_guest_callback;
        create_info.pUserData = reinterpret_cast<void*>(0x50607080);
        constexpr uint64_t messenger_id = 0x8000000000000011ULL;
        const auto packet = messenger_wire::marshal_create(created.instance, messenger_id, create_info, 8);
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_debug_utils_messenger, packet.data(), packet.size(),
                        sizeof(gpu_bridge::object_response)), STATUS_SUCCESS);
        const auto messenger = win.emu().read_memory<gpu_bridge::object_response>(memory + 0x1000);
        ASSERT_EQ(messenger.vk_result, VK_SUCCESS);
        EXPECT_EQ(messenger.object, messenger_id);

        VkDebugUtilsMessengerCallbackDataEXT message{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
        message.pMessageIdName = "ioctl-amd-relay";
        message.messageIdNumber = 227;
        message.pMessage = "callback-from-native-driver";
        const auto submit = messenger_wire::marshal_submit(created.instance, 8, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                                                            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, message);
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_debug_utils_messenger, submit.data(), submit.size(),
                        sizeof(gpu_bridge::object_response)), STATUS_SUCCESS);
        EXPECT_EQ(win.emu().read_memory<gpu_bridge::object_response>(memory + 0x1000).vk_result, VK_SUCCESS);
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_debug_utils_poll, nullptr, 0,
                        sizeof(gpu_bridge::debug_utils_poll_response) + callback_wire::max_packet_bytes), STATUS_SUCCESS);
        const auto reply = win.emu().read_memory<gpu_bridge::debug_utils_poll_response>(memory + 0x1000);
        ASSERT_EQ(reply.pending, 1u);
        EXPECT_EQ(reply.guest_pointer_bytes, 8u);
        EXPECT_EQ(reply.instance_id, created.instance);
        EXPECT_EQ(reply.callback_address, reinterpret_cast<uintptr_t>(unreachable_guest_callback));
        EXPECT_EQ(reply.user_data, 0x50607080u);
        ASSERT_LE(reply.packet_size, callback_wire::max_packet_bytes);
        std::vector<std::byte> callback(reply.packet_size);
        win.emu().read_memory(memory + 0x1000 + sizeof(reply), callback.data(), callback.size());
        const auto decoded = callback_wire::decode(callback);
        EXPECT_STREQ(decoded->data.pMessageIdName, "ioctl-amd-relay");
        EXPECT_STREQ(decoded->data.pMessage, "callback-from-native-driver");
        EXPECT_EQ(decoded->data.messageIdNumber, 227);
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_debug_utils_poll, nullptr, 0,
                        sizeof(gpu_bridge::debug_utils_poll_response) + callback_wire::max_packet_bytes), STATUS_SUCCESS);
        EXPECT_EQ(win.emu().read_memory<gpu_bridge::debug_utils_poll_response>(memory + 0x1000).pending, 0u);

        const auto destroy = messenger_wire::marshal_destroy(created.instance, messenger_id, 8);
        EXPECT_EQ(ioctl(gpu_bridge::ioctl_debug_utils_messenger, destroy.data(), destroy.size(),
                        sizeof(gpu_bridge::object_response)), STATUS_SUCCESS);
        const gpu_bridge::destroy_instance_request destroy_instance{.instance = created.instance};
        EXPECT_EQ(ioctl(gpu_bridge::ioctl_destroy_instance, &destroy_instance, sizeof(destroy_instance), 0), STATUS_SUCCESS);
    }

    TEST(VulkanDebugUtilsGpuBridgeTest, AmdObjectAndLabelCommandsCrossIoctlAndRecordedCommandStream)
    {
        namespace wire = gpu_bridge::debug_utils_command_wire;
        emulator_settings settings{.disable_logging = true};
        settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        windows_emulator win = create_sample_emulator(std::move(settings), {}, {}, std::move(interfaces));
        win.setup_process_if_necessary();
        const uint64_t memory = win.memory.allocate_memory(2 * 1024 * 1024, memory_permission::read_write);
        ASSERT_NE(memory, 0u);
        io_device_container bridge{u"SogenGpu", win, {}};
        const auto ioctl = [&](uint32_t code, const void* input, size_t size, size_t output_size) {
            if (size) win.emu().write_memory(memory, input, size);
            io_device_context request{win.memory};
            request.vcpu = &win.vcpu(0);
            request.io_control_code = code;
            request.input_buffer = size ? memory : 0;
            request.input_buffer_length = static_cast<ULONG>(size);
            request.output_buffer = memory + 0x1000;
            request.output_buffer_length = static_cast<ULONG>(output_size);
            return bridge.io_control(win, request);
        };
        const auto result = [&] { return win.emu().read_memory<gpu_bridge::result_response>(memory + 0x1000).vk_result; };
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_debug_utils_capabilities, nullptr, 0,
                        sizeof(gpu_bridge::debug_utils_capabilities_response)), STATUS_SUCCESS);
        if (!win.emu().read_memory<gpu_bridge::debug_utils_capabilities_response>(memory + 0x1000).available)
            GTEST_SKIP() << "Native Vulkan loader lacks VK_EXT_debug_utils";

        const gpu_bridge::create_instance_request instance_request{
            .magic = gpu_bridge::create_instance_request_magic, .extension_bits = gpu_bridge::instance_ext_debug_utils};
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_create_instance, &instance_request, sizeof(instance_request),
                        sizeof(gpu_bridge::create_instance_response)), STATUS_SUCCESS);
        const auto instance = win.emu().read_memory<gpu_bridge::create_instance_response>(memory + 0x1000);
        ASSERT_EQ(instance.vk_result, VK_SUCCESS);
        const gpu_bridge::enumerate_physical_devices_request enumerate{.instance = instance.instance, .max_count = 32};
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_enumerate_physical_devices, &enumerate, sizeof(enumerate),
                        sizeof(gpu_bridge::enumerate_physical_devices_response) + 32 * sizeof(uint64_t)), STATUS_SUCCESS);
        const auto device_count = win.emu().read_memory<gpu_bridge::enumerate_physical_devices_response>(memory + 0x1000).count;
        std::vector<uint64_t> candidates;
        for (uint32_t i = 0; i < device_count && i < 32; ++i)
            candidates.push_back(win.emu().read_memory<uint64_t>(
                memory + 0x1000 + sizeof(gpu_bridge::enumerate_physical_devices_response) + 8 * i));
        uint64_t physical = 0;
        for (const uint64_t candidate : candidates)
        {
            const gpu_bridge::get_physical_device_properties_request properties{.physical_device = candidate};
            ASSERT_EQ(ioctl(gpu_bridge::ioctl_get_physical_device_properties, &properties, sizeof(properties),
                            sizeof(VkPhysicalDeviceProperties)), STATUS_SUCCESS);
            if (win.emu().read_memory<uint32_t>(memory + 0x1000 + offsetof(VkPhysicalDeviceProperties, vendorID)) == 0x1002)
                physical = candidate;
        }
        if (!physical) GTEST_SKIP() << "No AMD Vulkan physical device";
        const gpu_bridge::get_queue_family_properties_request families{.physical_device = physical, .max_count = 32};
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_get_queue_family_properties, &families, sizeof(families),
                        sizeof(gpu_bridge::get_queue_family_properties_response) + 32 * sizeof(gpu_bridge::queue_family_properties)), STATUS_SUCCESS);
        const auto family_count = win.emu().read_memory<gpu_bridge::get_queue_family_properties_response>(memory + 0x1000).count;
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < family_count && i < 32; ++i)
        {
            const auto item = win.emu().read_memory<gpu_bridge::queue_family_properties>(
                memory + 0x1000 + sizeof(gpu_bridge::get_queue_family_properties_response) + i * sizeof(gpu_bridge::queue_family_properties));
            if (item.queue_count && (item.queue_flags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
        }
        ASSERT_NE(family, UINT32_MAX);
        struct device_request { gpu_bridge::create_device_request header{}; gpu_bridge::device_queue_create_entry queue{}; } create;
        static_assert(offsetof(device_request, queue) == sizeof(gpu_bridge::create_device_request));
        create.header.physical_device = physical;
        create.header.queue_create_count = 1;
        create.queue.queue_family_index = family;
        create.queue.queue_count = 1;
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_create_device, &create, sizeof(create), sizeof(gpu_bridge::create_device_response)), STATUS_SUCCESS);
        const auto device = win.emu().read_memory<gpu_bridge::create_device_response>(memory + 0x1000);
        ASSERT_EQ(device.vk_result, VK_SUCCESS);
        ASSERT_NE(device.device, 0u);

        wire::command command{};
        command.op = wire::operation::set_object_name;
        command.dispatch_id = device.device;
        command.object_id = device.device;
        command.object_type = VK_OBJECT_TYPE_DEVICE;
        command.name = "sogen-ioctl-amd-device";
        auto packet = wire::encode(command);
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_debug_utils_command, packet.data(), packet.size(), sizeof(gpu_bridge::result_response)), STATUS_SUCCESS);
        EXPECT_EQ(result(), VK_SUCCESS);
        command.op = wire::operation::set_object_tag;
        command.name.clear();
        command.tag_name = 0x20260926;
        command.tag = {std::byte{0x12}, std::byte{0xfe}};
        packet = wire::encode(command);
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_debug_utils_command, packet.data(), packet.size(), sizeof(gpu_bridge::result_response)), STATUS_SUCCESS);
        EXPECT_EQ(result(), VK_SUCCESS);

        const gpu_bridge::get_device_queue_request queue_request{.device = device.device, .queue_family_index = family};
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_get_device_queue, &queue_request, sizeof(queue_request),
                        sizeof(gpu_bridge::get_device_queue_response)), STATUS_SUCCESS);
        const uint64_t queue = win.emu().read_memory<gpu_bridge::get_device_queue_response>(memory + 0x1000).queue;
        ASSERT_NE(queue, 0u);
        command = {};
        command.dispatch_id = queue;
        command.name = "sogen-ioctl-amd-label";
        command.color = {0.1f, 0.2f, 0.3f, 1.0f};
        for (const auto op : {wire::operation::queue_begin_label, wire::operation::queue_insert_label,
                              wire::operation::queue_end_label})
        {
            command.op = op;
            if (op == wire::operation::queue_end_label) command.name.clear();
            packet = wire::encode(command);
            ASSERT_EQ(ioctl(gpu_bridge::ioctl_debug_utils_command, packet.data(), packet.size(), sizeof(gpu_bridge::result_response)), STATUS_SUCCESS);
            EXPECT_EQ(result(), VK_SUCCESS);
        }

        const gpu_bridge::create_command_pool_request pool_request{.device = device.device, .queue_family_index = family};
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_create_command_pool, &pool_request, sizeof(pool_request),
                        sizeof(gpu_bridge::create_command_pool_response)), STATUS_SUCCESS);
        const auto pool = win.emu().read_memory<gpu_bridge::create_command_pool_response>(memory + 0x1000);
        ASSERT_EQ(pool.vk_result, VK_SUCCESS);
        const gpu_bridge::allocate_command_buffer_request allocation{.device = device.device, .command_pool = pool.command_pool};
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_allocate_command_buffer, &allocation, sizeof(allocation),
                        sizeof(gpu_bridge::allocate_command_buffer_response)), STATUS_SUCCESS);
        const auto buffer = win.emu().read_memory<gpu_bridge::allocate_command_buffer_response>(memory + 0x1000);
        ASSERT_EQ(buffer.vk_result, VK_SUCCESS);
        std::vector<std::byte> stream;
        const auto append = [&](gpu_bridge::command op, const void* bytes, size_t size) {
            const gpu_bridge::command_record_header header{.command = static_cast<uint32_t>(op), .size = static_cast<uint32_t>(size)};
            auto body = std::as_bytes(std::span(static_cast<const std::byte*>(bytes), size));
            const auto* first = reinterpret_cast<const std::byte*>(&header);
            stream.insert(stream.end(), first, first + sizeof(header));
            stream.insert(stream.end(), body.begin(), body.end());
        };
        const gpu_bridge::begin_command_buffer_request begin{.command_buffer = buffer.command_buffer};
        append(gpu_bridge::command::begin_command_buffer, &begin, sizeof(begin));
        command.dispatch_id = buffer.command_buffer;
        command.name = "sogen-ioctl-amd-command";
        for (const auto op : {wire::operation::command_begin_label, wire::operation::command_insert_label,
                              wire::operation::command_end_label})
        {
            command.op = op;
            if (op == wire::operation::command_end_label) command.name.clear();
            packet = wire::encode(command);
            append(gpu_bridge::command::debug_utils_command, packet.data(), packet.size());
        }
        const gpu_bridge::end_command_buffer_request end{.command_buffer = buffer.command_buffer};
        append(gpu_bridge::command::end_command_buffer, &end, sizeof(end));
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_record_commands, stream.data(), stream.size(), sizeof(gpu_bridge::result_response)), STATUS_SUCCESS);
        EXPECT_EQ(result(), VK_SUCCESS);

        const gpu_bridge::destroy_command_pool_request destroy_pool{.device = device.device, .command_pool = pool.command_pool};
        EXPECT_EQ(ioctl(gpu_bridge::ioctl_destroy_command_pool, &destroy_pool, sizeof(destroy_pool), 0), STATUS_SUCCESS);
        const gpu_bridge::destroy_device_request destroy_device{.device = device.device};
        EXPECT_EQ(ioctl(gpu_bridge::ioctl_destroy_device, &destroy_device, sizeof(destroy_device), 0), STATUS_SUCCESS);
        const gpu_bridge::destroy_instance_request destroy_instance{.instance = instance.instance};
        EXPECT_EQ(ioctl(gpu_bridge::ioctl_destroy_instance, &destroy_instance, sizeof(destroy_instance), 0), STATUS_SUCCESS);
    }

}
