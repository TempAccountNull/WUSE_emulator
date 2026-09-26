namespace
{
    std::mutex g_debug_utils_mutex;
    std::unordered_set<gb::object_id> g_debug_utils_instances;
    std::unordered_set<gb::object_id> g_debug_utils_creation_callbacks;
    std::unordered_map<gb::object_id, gb::object_id> g_debug_utils_physical_instances;
    std::unordered_map<gb::object_id, gb::object_id> g_debug_utils_devices;
    std::unordered_map<gb::object_id, gb::object_id> g_debug_utils_messengers;
    std::atomic_uint64_t g_next_debug_messenger_id{0x8000000000000001ULL};

    bool debug_utils_supported_by_bridge()
    {
        // Native loader support is insufficient; keep this hidden until the full guest/host
        // callback path passes a fresh emulator test.
        constexpr bool guest_bridge_validated = false;
        if (!guest_bridge_validated) return false;
        gb::debug_utils_capabilities_response response{};
        return bridge_call(gb::ioctl_debug_utils_capabilities, nullptr, 0, &response, sizeof(response)) && response.available != 0;
    }

    bool debug_utils_instance_enabled(gb::object_id instance)
    {
        std::lock_guard lock(g_debug_utils_mutex);
        return g_debug_utils_instances.contains(instance);
    }

    bool debug_utils_device_enabled(gb::object_id device)
    {
        std::lock_guard lock(g_debug_utils_mutex);
        return g_debug_utils_devices.contains(device);
    }

    bool debug_utils_function_name(const char* name)
    {
        if (!name) return false;
        static constexpr std::array names{
            "vkCreateDebugUtilsMessengerEXT", "vkDestroyDebugUtilsMessengerEXT", "vkSubmitDebugUtilsMessageEXT",
            "vkSetDebugUtilsObjectNameEXT", "vkSetDebugUtilsObjectTagEXT",
            "vkQueueBeginDebugUtilsLabelEXT", "vkQueueInsertDebugUtilsLabelEXT", "vkQueueEndDebugUtilsLabelEXT",
            "vkCmdBeginDebugUtilsLabelEXT", "vkCmdInsertDebugUtilsLabelEXT", "vkCmdEndDebugUtilsLabelEXT"};
        return std::ranges::any_of(names, [&](const char* candidate) { return std::strcmp(name, candidate) == 0; });
    }

    VkResult debug_utils_send_command(std::span<const std::byte> packet)
    {
        gb::result_response response{};
        if (packet.size() > UINT32_MAX ||
            !bridge_call(gb::ioctl_debug_utils_command, packet.data(), static_cast<DWORD>(packet.size()), &response, sizeof(response)))
            return VK_ERROR_INITIALIZATION_FAILED;
        return static_cast<VkResult>(response.vk_result);
    }

    void drain_debug_utils_callbacks()
    {
        if (g_debug_callback_count.load(std::memory_order_acquire) == 0)
            return;
        const HANDLE handle = bridge();
        if (handle == INVALID_HANDLE_VALUE)
            return;
        thread_local std::vector<std::byte> output(sizeof(gb::debug_utils_poll_response) + gb::debug_utils_wire::max_packet_bytes);
        for (size_t index = 0; index < gb::debug_utils_callback_relay::queue::max_queued_packets; ++index)
        {
            DWORD returned{};
            if (!DeviceIoControl(handle, gb::ioctl_debug_utils_poll, nullptr, 0, output.data(),
                                 static_cast<DWORD>(output.size()), &returned, nullptr) ||
                returned < sizeof(gb::debug_utils_poll_response))
            {
                shim_log("vulkan-shim: debug-utils callback poll failed\n");
                return;
            }
            gb::debug_utils_poll_response response{};
            std::memcpy(&response, output.data(), sizeof(response));
            if (!response.pending)
                return;
            if (response.reserved || response.guest_pointer_bytes != sizeof(void*) ||
                !response.callback_address || response.packet_size > gb::debug_utils_wire::max_packet_bytes ||
                returned != sizeof(response) + response.packet_size)
            {
                shim_log("vulkan-shim: invalid debug-utils callback reply\n");
                return;
            }
            try
            {
                const auto packet = std::span(output.data() + sizeof(response), response.packet_size);
                auto decoded = gb::debug_utils_wire::decode(packet);
                auto* callback = reinterpret_cast<PFN_vkDebugUtilsMessengerCallbackEXT>(
                    static_cast<uintptr_t>(response.callback_address));
                callback(decoded->severity, decoded->types, &decoded->data,
                         reinterpret_cast<void*>(static_cast<uintptr_t>(response.user_data)));
            }
            catch (const std::exception& error)
            {
                std::array<char, 256> message{};
                std::snprintf(message.data(), message.size(), "vulkan-shim: debug-utils callback decode failed: %.160s\n", error.what());
                shim_log(message.data());
            }
        }
        shim_log("vulkan-shim: debug-utils callback drain limit reached\n");
    }
}

extern "C"
{
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkCreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* info,
                                   const VkAllocationCallbacks* allocator, VkDebugUtilsMessengerEXT* out)
    {
        if (!out || !info || allocator || !debug_utils_instance_enabled(to_object_id(instance)))
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        try
        {
            const gb::object_id id = g_next_debug_messenger_id.fetch_add(1, std::memory_order_relaxed);
            const auto packet = gb::debug_utils_messenger_wire::marshal_create(to_object_id(instance), id, *info, sizeof(void*));
            g_debug_callback_count.fetch_add(1, std::memory_order_release);
            gb::object_response response{};
            if (!bridge_call(gb::ioctl_debug_utils_messenger, packet.data(), static_cast<DWORD>(packet.size()), &response,
                             sizeof(response)))
            {
                g_debug_callback_count.fetch_sub(1, std::memory_order_release);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            if (response.vk_result != VK_SUCCESS || response.object != id)
            {
                g_debug_callback_count.fetch_sub(1, std::memory_order_release);
                return static_cast<VkResult>(response.vk_result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : response.vk_result);
            }
            *out = to_handle<VkDebugUtilsMessengerEXT>(id);
            {
                std::lock_guard lock(g_debug_utils_mutex);
                g_debug_utils_messengers.emplace(id, to_object_id(instance));
            }
            return VK_SUCCESS;
        }
        catch (const std::exception&)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL
    vkDestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger, const VkAllocationCallbacks* allocator)
    {
        if (allocator || !messenger) return;
        try
        {
            const auto packet = gb::debug_utils_messenger_wire::marshal_destroy(to_object_id(instance), to_object_id(messenger),
                                                                                 sizeof(void*));
            gb::object_response response{};
            if (!bridge_call(gb::ioctl_debug_utils_messenger, packet.data(), static_cast<DWORD>(packet.size()), &response,
                             sizeof(response)) || response.vk_result != VK_SUCCESS)
            {
                shim_log("vulkan-shim: debug-utils messenger destruction failed; callback retained\n");
                return;
            }
            std::lock_guard lock(g_debug_utils_mutex);
            if (g_debug_utils_messengers.erase(to_object_id(messenger)))
                g_debug_callback_count.fetch_sub(1, std::memory_order_release);
        }
        catch (const std::exception&)
        {
            shim_log("vulkan-shim: invalid debug-utils messenger destruction\n");
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL
    vkSubmitDebugUtilsMessageEXT(VkInstance instance, VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                 VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data)
    {
        if (!data || !debug_utils_instance_enabled(to_object_id(instance))) return;
        try
        {
            const auto packet = gb::debug_utils_messenger_wire::marshal_submit(to_object_id(instance), sizeof(void*),
                                                                                 severity, types, *data);
            gb::object_response response{};
            bridge_call(gb::ioctl_debug_utils_messenger, packet.data(), static_cast<DWORD>(packet.size()), &response, sizeof(response));
        }
        catch (const std::exception& error)
        {
            std::array<char, 256> message{};
            std::snprintf(message.data(), message.size(), "vulkan-shim: invalid debug-utils message: %.160s\n", error.what());
            shim_log(message.data());
        }
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkSetDebugUtilsObjectNameEXT(VkDevice device, const VkDebugUtilsObjectNameInfoEXT* info)
    {
        if (!info || !debug_utils_device_enabled(to_object_id(device))) return VK_ERROR_EXTENSION_NOT_PRESENT;
        try { return debug_utils_send_command(gb::debug_utils_command_wire::marshal_object_name(to_object_id(device), *info)); }
        catch (const std::exception&) { return VK_ERROR_INITIALIZATION_FAILED; }
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkSetDebugUtilsObjectTagEXT(VkDevice device, const VkDebugUtilsObjectTagInfoEXT* info)
    {
        if (!info || !debug_utils_device_enabled(to_object_id(device))) return VK_ERROR_EXTENSION_NOT_PRESENT;
        try { return debug_utils_send_command(gb::debug_utils_command_wire::marshal_object_tag(to_object_id(device), *info)); }
        catch (const std::exception&) { return VK_ERROR_INITIALIZATION_FAILED; }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkQueueBeginDebugUtilsLabelEXT(VkQueue queue, const VkDebugUtilsLabelEXT* label)
    {
        if (!label) return;
        try { (void)debug_utils_send_command(gb::debug_utils_command_wire::marshal_label(
                  gb::debug_utils_command_wire::operation::queue_begin_label, to_object_id(queue), *label)); }
        catch (const std::exception&) { shim_log("vulkan-shim: invalid queue debug label\n"); }
    }
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkQueueInsertDebugUtilsLabelEXT(VkQueue queue, const VkDebugUtilsLabelEXT* label)
    {
        if (!label) return;
        try { (void)debug_utils_send_command(gb::debug_utils_command_wire::marshal_label(
                  gb::debug_utils_command_wire::operation::queue_insert_label, to_object_id(queue), *label)); }
        catch (const std::exception&) { shim_log("vulkan-shim: invalid queue debug label\n"); }
    }
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkQueueEndDebugUtilsLabelEXT(VkQueue queue)
    {
        try { (void)debug_utils_send_command(gb::debug_utils_command_wire::marshal_label_end(
                  gb::debug_utils_command_wire::operation::queue_end_label, to_object_id(queue))); }
        catch (const std::exception&) { shim_log("vulkan-shim: invalid queue debug label end\n"); }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBeginDebugUtilsLabelEXT(VkCommandBuffer buffer, const VkDebugUtilsLabelEXT* label)
    {
        if (!label) return;
        try {
            const auto packet = gb::debug_utils_command_wire::marshal_label(
                gb::debug_utils_command_wire::operation::command_begin_label, to_object_id(buffer), *label);
            record_command(to_object_id(buffer), gb::command::debug_utils_command, packet.data(), packet.size());
        }
        catch (const std::exception&) { fail_recorded_command(to_object_id(buffer), VK_ERROR_INITIALIZATION_FAILED); }
    }
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdInsertDebugUtilsLabelEXT(VkCommandBuffer buffer, const VkDebugUtilsLabelEXT* label)
    {
        if (!label) return;
        try {
            const auto packet = gb::debug_utils_command_wire::marshal_label(
                gb::debug_utils_command_wire::operation::command_insert_label, to_object_id(buffer), *label);
            record_command(to_object_id(buffer), gb::command::debug_utils_command, packet.data(), packet.size());
        }
        catch (const std::exception&) { fail_recorded_command(to_object_id(buffer), VK_ERROR_INITIALIZATION_FAILED); }
    }
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndDebugUtilsLabelEXT(VkCommandBuffer buffer)
    {
        try {
            const auto packet = gb::debug_utils_command_wire::marshal_label_end(
                gb::debug_utils_command_wire::operation::command_end_label, to_object_id(buffer));
            record_command(to_object_id(buffer), gb::command::debug_utils_command, packet.data(), packet.size());
        }
        catch (const std::exception&) { fail_recorded_command(to_object_id(buffer), VK_ERROR_INITIALIZATION_FAILED); }
    }
}
