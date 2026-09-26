    bool vulkan_host::debug_utils_available() const
    {
        if (!this->available())
            return false;
        const auto enumerate = this->impl_->load_instance_proc<PFN_vkEnumerateInstanceExtensionProperties>(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
        if (!enumerate)
            return false;
        uint32_t count{};
        if (enumerate(nullptr, &count, nullptr) != VK_SUCCESS || count > 65536)
            return false;
        std::vector<VkExtensionProperties> extensions(count);
        if (count && enumerate(nullptr, &count, extensions.data()) != VK_SUCCESS)
            return false;
        if (count > extensions.size())
            return false;
        return std::ranges::any_of(extensions.begin(), extensions.begin() + count, [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
        });
    }

    void vulkan_host::set_debug_utils_sink(debug_utils_sink sink)
    {
        this->impl_->debug_sink = std::move(sink);
    }

    uint32_t vulkan_host::exchange_debug_utils_guest_thread(uint32_t guest_thread_id)
    {
        return std::exchange(debug_utils_current_guest_thread, guest_thread_id);
    }

    int32_t vulkan_host::debug_utils_messenger(std::span<const std::byte> packet, uint64_t& out_messenger)
    {
        out_messenger = 0;
        gpu_bridge::debug_utils_messenger_wire::request request;
        try
        {
            request = gpu_bridge::debug_utils_messenger_wire::decode(packet);
        }
        catch (const std::exception&)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto instance = this->impl_->instances.find(request.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.debug_utils_enabled)
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        const VkInstance native_instance = instance->second.handle;
        if (request.op == gpu_bridge::debug_utils_messenger_wire::operation::create)
        {
            if (this->impl_->messengers.contains(request.messenger_id))
                return VK_ERROR_INITIALIZATION_FAILED;
            const auto create = this->impl_->load_instance_proc<PFN_vkCreateDebugUtilsMessengerEXT>(
                native_instance, "vkCreateDebugUtilsMessengerEXT");
            if (!create)
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            auto state = std::make_shared<impl::debug_callback_state>();
            state->owner = this->impl_.get();
            state->instance_id = request.instance_id;
            state->callback_address = request.callback_address;
            state->user_data = request.user_data;
            state->guest_pointer_bytes = request.guest_pointer_bytes;
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = request.severity;
            info.messageType = request.types;
            info.pfnUserCallback = impl::debug_callback;
            info.pUserData = state.get();
            VkDebugUtilsMessengerEXT native{};
            const VkResult result = create(native_instance, &info, nullptr, &native);
            if (result != VK_SUCCESS)
                return result;
            const uint64_t id = request.messenger_id;
            this->impl_->messengers.emplace(id, impl::messenger_data{native, request.instance_id, std::move(state)});
            out_messenger = id;
            return VK_SUCCESS;
        }
        if (request.op == gpu_bridge::debug_utils_messenger_wire::operation::destroy)
        {
            const auto messenger = this->impl_->messengers.find(request.messenger_id);
            if (messenger == this->impl_->messengers.end() || messenger->second.instance_id != request.instance_id)
                return VK_ERROR_INITIALIZATION_FAILED;
            const auto destroy = this->impl_->load_instance_proc<PFN_vkDestroyDebugUtilsMessengerEXT>(
                native_instance, "vkDestroyDebugUtilsMessengerEXT");
            if (!destroy)
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            destroy(native_instance, messenger->second.handle, nullptr);
            this->impl_->messengers.erase(messenger);
            return VK_SUCCESS;
        }
        if (request.op == gpu_bridge::debug_utils_messenger_wire::operation::submit && request.message)
        {
            const auto submit = this->impl_->load_instance_proc<PFN_vkSubmitDebugUtilsMessageEXT>(
                native_instance, "vkSubmitDebugUtilsMessageEXT");
            if (!submit)
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            submit(native_instance, request.message->severity, request.message->types, &request.message->data);
            return VK_SUCCESS;
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    int32_t vulkan_host::debug_utils_command(std::span<const std::byte> packet)
    {
        gpu_bridge::debug_utils_command_wire::command request;
        try
        {
            request = gpu_bridge::debug_utils_command_wire::decode(packet);
        }
        catch (const std::exception&)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        using operation = gpu_bridge::debug_utils_command_wire::operation;
        uint64_t device_id = request.dispatch_id;
        VkQueue queue{};
        VkCommandBuffer command_buffer{};
        if (request.op >= operation::queue_begin_label && request.op <= operation::queue_end_label)
        {
            const auto entry = this->impl_->queues.find(request.dispatch_id);
            if (entry == this->impl_->queues.end())
                return VK_ERROR_INITIALIZATION_FAILED;
            device_id = entry->second.device_id;
            queue = entry->second.handle;
        }
        else if (request.op >= operation::command_begin_label && request.op <= operation::command_end_label)
        {
            const auto entry = this->impl_->command_buffers.find(request.dispatch_id);
            if (entry == this->impl_->command_buffers.end())
                return VK_ERROR_INITIALIZATION_FAILED;
            device_id = entry->second.device_id;
            command_buffer = entry->second.handle;
        }
        const auto device = this->impl_->devices.find(device_id);
        if (device == this->impl_->devices.end())
            return VK_ERROR_INITIALIZATION_FAILED;
        const auto instance = this->impl_->instances.find(device->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.debug_utils_enabled || !instance->second.get_device_proc_addr)
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        const auto proc = [&](const char* name) {
            return instance->second.get_device_proc_addr(device->second.handle, name);
        };
        if (request.op == operation::set_object_name || request.op == operation::set_object_tag)
        {
            const auto native_value = [&](auto handle) -> uint64_t {
                if constexpr (std::is_pointer_v<decltype(handle)>)
                    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
                else
                    return static_cast<uint64_t>(handle);
            };
            const auto mapped = [&](const auto& objects) -> uint64_t {
                const auto item = objects.find(request.object_id);
                return item != objects.end() && item->second.device_id == device_id ? native_value(item->second.handle) : 0;
            };
            uint64_t native{};
            switch (request.object_type)
            {
            case VK_OBJECT_TYPE_INSTANCE: {
                const auto item = this->impl_->instances.find(request.object_id);
                native = item != this->impl_->instances.end() && request.object_id == device->second.instance_id
                             ? native_value(item->second.handle) : 0;
                break;
            }
            case VK_OBJECT_TYPE_PHYSICAL_DEVICE: {
                const auto item = this->impl_->physical_devices.find(request.object_id);
                native = item != this->impl_->physical_devices.end() && item->second.instance_id == device->second.instance_id &&
                                 item->second.handle == device->second.physical_device
                             ? native_value(item->second.handle) : 0;
                break;
            }
            case VK_OBJECT_TYPE_DEVICE:
                native = request.object_id == device_id ? native_value(device->second.handle) : 0;
                break;
            case VK_OBJECT_TYPE_QUEUE: native = mapped(this->impl_->queues); break;
            case VK_OBJECT_TYPE_COMMAND_BUFFER: native = mapped(this->impl_->command_buffers); break;
            case VK_OBJECT_TYPE_COMMAND_POOL: native = mapped(this->impl_->command_pools); break;
            case VK_OBJECT_TYPE_FENCE: native = mapped(this->impl_->fences); break;
            case VK_OBJECT_TYPE_EVENT: native = mapped(this->impl_->events); break;
            case VK_OBJECT_TYPE_SEMAPHORE: native = mapped(this->impl_->semaphores); break;
            case VK_OBJECT_TYPE_DEVICE_MEMORY: native = mapped(this->impl_->memories); break;
            case VK_OBJECT_TYPE_BUFFER: native = mapped(this->impl_->buffers); break;
            case VK_OBJECT_TYPE_IMAGE: native = mapped(this->impl_->images); break;
            case VK_OBJECT_TYPE_SAMPLER: native = mapped(this->impl_->samplers); break;
            case VK_OBJECT_TYPE_SHADER_MODULE: native = mapped(this->impl_->shader_modules); break;
            case VK_OBJECT_TYPE_IMAGE_VIEW: native = mapped(this->impl_->image_views); break;
            case VK_OBJECT_TYPE_BUFFER_VIEW: native = mapped(this->impl_->buffer_views); break;
            case VK_OBJECT_TYPE_QUERY_POOL: native = mapped(this->impl_->query_pools); break;
            case VK_OBJECT_TYPE_RENDER_PASS: native = mapped(this->impl_->render_passes); break;
            case VK_OBJECT_TYPE_FRAMEBUFFER: native = mapped(this->impl_->framebuffers); break;
            case VK_OBJECT_TYPE_PIPELINE_LAYOUT: native = mapped(this->impl_->pipeline_layouts); break;
            case VK_OBJECT_TYPE_PIPELINE: native = mapped(this->impl_->pipelines); break;
            case VK_OBJECT_TYPE_PIPELINE_CACHE: native = mapped(this->impl_->pipeline_caches); break;
            case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT: native = mapped(this->impl_->descriptor_set_layouts); break;
            case VK_OBJECT_TYPE_DESCRIPTOR_POOL: native = mapped(this->impl_->descriptor_pools); break;
            case VK_OBJECT_TYPE_DESCRIPTOR_SET: native = mapped(this->impl_->descriptor_sets); break;
            case VK_OBJECT_TYPE_SWAPCHAIN_KHR: {
                const auto item = this->impl_->swapchains.find(request.object_id);
                native = item != this->impl_->swapchains.end() && item->second.device_id == device_id
                             ? native_value(item->second.native) : 0;
                break;
            }
            default: return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            if (!native)
                return VK_ERROR_INITIALIZATION_FAILED;
            if (request.op == operation::set_object_name)
            {
                const auto function = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(proc("vkSetDebugUtilsObjectNameEXT"));
                if (!function)
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
                info.objectType = request.object_type;
                info.objectHandle = native;
                info.pObjectName = request.null_name ? nullptr : request.name.c_str();
                return function(device->second.handle, &info);
            }
            const auto function = reinterpret_cast<PFN_vkSetDebugUtilsObjectTagEXT>(proc("vkSetDebugUtilsObjectTagEXT"));
            if (!function)
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            VkDebugUtilsObjectTagInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT};
            info.objectType = request.object_type;
            info.objectHandle = native;
            info.tagName = request.tag_name;
            info.tagSize = request.tag.size();
            info.pTag = request.tag.data();
            return function(device->second.handle, &info);
        }
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = request.name.c_str();
        std::copy(request.color.begin(), request.color.end(), label.color);
        switch (request.op)
        {
        case operation::queue_begin_label: {
            const auto function = reinterpret_cast<PFN_vkQueueBeginDebugUtilsLabelEXT>(proc("vkQueueBeginDebugUtilsLabelEXT"));
            if (!function) return VK_ERROR_EXTENSION_NOT_PRESENT;
            function(queue, &label); return VK_SUCCESS;
        }
        case operation::queue_insert_label: {
            const auto function = reinterpret_cast<PFN_vkQueueInsertDebugUtilsLabelEXT>(proc("vkQueueInsertDebugUtilsLabelEXT"));
            if (!function) return VK_ERROR_EXTENSION_NOT_PRESENT;
            function(queue, &label); return VK_SUCCESS;
        }
        case operation::queue_end_label: {
            const auto function = reinterpret_cast<PFN_vkQueueEndDebugUtilsLabelEXT>(proc("vkQueueEndDebugUtilsLabelEXT"));
            if (!function) return VK_ERROR_EXTENSION_NOT_PRESENT;
            function(queue); return VK_SUCCESS;
        }
        case operation::command_begin_label: {
            const auto function = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(proc("vkCmdBeginDebugUtilsLabelEXT"));
            if (!function) return VK_ERROR_EXTENSION_NOT_PRESENT;
            function(command_buffer, &label); return VK_SUCCESS;
        }
        case operation::command_insert_label: {
            const auto function = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(proc("vkCmdInsertDebugUtilsLabelEXT"));
            if (!function) return VK_ERROR_EXTENSION_NOT_PRESENT;
            function(command_buffer, &label); return VK_SUCCESS;
        }
        case operation::command_end_label: {
            const auto function = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(proc("vkCmdEndDebugUtilsLabelEXT"));
            if (!function) return VK_ERROR_EXTENSION_NOT_PRESENT;
            function(command_buffer); return VK_SUCCESS;
        }
        default: return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
