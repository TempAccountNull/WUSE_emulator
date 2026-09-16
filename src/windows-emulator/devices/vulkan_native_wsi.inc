void vulkan_host::enable_native_wsi(const bool gpu_copy)
{
#ifdef _WIN32
    if (!this->impl_->instances.empty())
    {
        throw std::logic_error("Native WSI must be selected before instance creation");
    }
    this->impl_->native_wsi = true;
    this->impl_->gpu_copy = gpu_copy;
#else
    throw std::runtime_error("Native WSI currently requires Win32");
#endif
}

std::vector<std::byte> vulkan_host::native_wsi_request(const std::span<const std::byte> packet, native_presentation_window_lease lease)
try
{
    namespace wire = gpu_bridge::native_wsi;
    static_assert(sizeof(VkSurfaceCapabilitiesKHR) == 52 && sizeof(VkSurfaceFormatKHR) == 8 && sizeof(VkResult) == 4);
    wire::request request{};
    std::vector<std::byte> output(sizeof(wire::response));
    auto finish = [&](const int32_t result, const uint32_t count = 0, const uint64_t object = 0) {
        const wire::response header{.result = result,
                                    .count = count,
                                    .object = object,
                                    .payload_bytes = static_cast<uint32_t>(output.size() - sizeof(wire::response)),
                                    .reserved = 0};
        std::memcpy(output.data(), &header, sizeof(header));
        return std::move(output);
    };
    if (!this->impl_->native_wsi || !wire::decode(packet, request))
    {
        return finish(VK_ERROR_UNKNOWN);
    }
    auto& host = *this->impl_;
    if (request.op == wire::operation::destroy_device)
    {
        this->destroy_device(request.owner);
        return finish(VK_SUCCESS);
    }
    if (request.op == wire::operation::destroy_instance)
    {
        this->destroy_instance(request.owner);
        return finish(VK_SUCCESS);
    }
    if (request.op == wire::operation::create_surface)
    {
#ifdef _WIN32
        const auto instance = host.instances.find(request.owner);
        if (instance == host.instances.end() || !lease || !lease.live() || lease.guest_window() != request.object)
        {
            return finish(VK_ERROR_UNKNOWN);
        }
        const auto create = host.load_instance_proc<PFN_vkCreateWin32SurfaceKHR>(instance->second.handle, "vkCreateWin32SurfaceKHR");
        const auto destroy = host.load_instance_proc<PFN_vkDestroySurfaceKHR>(instance->second.handle, "vkDestroySurfaceKHR");
        if (!create || !destroy)
        {
            return finish(VK_ERROR_EXTENSION_NOT_PRESENT);
        }
        const auto target = lease.target();
        const VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0,
                                               reinterpret_cast<HINSTANCE>(target.instance), reinterpret_cast<HWND>(target.window)};
        VkSurfaceKHR surface{};
        const auto result = create(instance->second.handle, &info, nullptr, &surface);
        if (result != VK_SUCCESS)
        {
            return finish(result);
        }
        const auto id = host.next_id++;
        try
        {
            impl::surface_data value{};
            value.hwnd = request.object;
            value.native = surface;
            value.instance_id = request.owner;
            value.lease = std::move(lease);
            host.surfaces.emplace(id, std::move(value));
        }
        catch (...)
        {
            destroy(instance->second.handle, surface, nullptr);
            throw;
        }
        return finish(VK_SUCCESS, 0, id);
#else
        return finish(VK_ERROR_EXTENSION_NOT_PRESENT);
#endif
    }
    if (request.op == wire::operation::destroy_surface)
    {
        const auto surface = host.surfaces.find(request.object);
        if (surface == host.surfaces.end() || surface->second.instance_id != request.owner)
        {
            return finish(VK_ERROR_UNKNOWN);
        }
        if (std::ranges::any_of(host.swapchains, [&](const auto& item) { return item.second.surface_id == request.object; }))
        {
            return finish(VK_ERROR_UNKNOWN);
        }
        this->destroy_surface(request.object);
        return finish(VK_SUCCESS);
    }
    if (request.op == wire::operation::query_surface)
    {
        const auto physical = host.physical_devices.find(request.owner);
        if (physical == host.physical_devices.end())
        {
            return finish(VK_ERROR_UNKNOWN);
        }
        auto& instance = host.instances.at(physical->second.instance_id);
        const auto query = static_cast<wire::query>(request.words[0]);
        VkSurfaceKHR native_surface{};
        if (query != wire::query::win32_support)
        {
            const auto surface = host.surfaces.find(request.object);
            if (surface == host.surfaces.end() || surface->second.instance_id != physical->second.instance_id ||
                !surface->second.lease.live())
            {
                return finish(VK_ERROR_SURFACE_LOST_KHR);
            }
            native_surface = surface->second.native;
        }
        const bool entries = request.words[3] != 0;
        uint32_t count = entries ? request.words[2] : 0;
        const auto capacity = count;
        const size_t stride = query == wire::query::formats ? sizeof(VkSurfaceFormatKHR) : sizeof(uint32_t);
        size_t bytes = sizeof(wire::response);
        if (!wire::append_size(bytes, capacity, stride))
        {
            return finish(VK_ERROR_OUT_OF_HOST_MEMORY);
        }
        if (query == wire::query::capabilities)
        {
            const auto fn = host.load_instance_proc<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
                instance.handle, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
            if (!fn)
            {
                return finish(VK_ERROR_EXTENSION_NOT_PRESENT);
            }
            VkSurfaceCapabilitiesKHR value{};
            const auto result = fn(physical->second.handle, native_surface, &value);
            if (result == VK_SUCCESS)
            {
                output.resize(sizeof(wire::response) + sizeof(value));
                std::memcpy(output.data() + sizeof(wire::response), &value, sizeof(value));
            }
            return finish(result);
        }
        if (query == wire::query::support || query == wire::query::win32_support)
        {
            VkBool32 value{};
            VkResult result = VK_SUCCESS;
            uint32_t families{};
            instance.get_queue_family_properties(physical->second.handle, &families, nullptr);
            if (request.words[1] >= families)
            {
                return finish(VK_ERROR_UNKNOWN);
            }
            if (query == wire::query::support)
            {
                const auto fn = host.load_instance_proc<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(instance.handle,
                                                                                                  "vkGetPhysicalDeviceSurfaceSupportKHR");
                if (!fn)
                {
                    return finish(VK_ERROR_EXTENSION_NOT_PRESENT);
                }
                result = fn(physical->second.handle, request.words[1], native_surface, &value);
            }
            else
            {
#ifdef _WIN32
                const auto fn = host.load_instance_proc<PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR>(
                    instance.handle, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
                if (!fn)
                {
                    return finish(VK_ERROR_EXTENSION_NOT_PRESENT);
                }
                value = fn(physical->second.handle, request.words[1]);
#else
                return finish(VK_ERROR_EXTENSION_NOT_PRESENT);
#endif
            }
            if (result == VK_SUCCESS)
            {
                output.resize(sizeof(wire::response) + sizeof(value));
                std::memcpy(output.data() + sizeof(wire::response), &value, sizeof(value));
            }
            return finish(result);
        }
        output.resize(bytes);
        VkResult result{};
        if (query == wire::query::formats)
        {
            const auto fn =
                host.load_instance_proc<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(instance.handle, "vkGetPhysicalDeviceSurfaceFormatsKHR");
            if (!fn)
            {
                return finish(VK_ERROR_EXTENSION_NOT_PRESENT);
            }
            VkSurfaceFormatKHR dummy{};
            auto* data = capacity ? reinterpret_cast<VkSurfaceFormatKHR*>(output.data() + sizeof(wire::response)) : &dummy;
            result = fn(physical->second.handle, native_surface, &count, entries ? data : nullptr);
        }
        else if (query == wire::query::modes)
        {
            const auto fn = host.load_instance_proc<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
                instance.handle, "vkGetPhysicalDeviceSurfacePresentModesKHR");
            if (!fn)
            {
                return finish(VK_ERROR_EXTENSION_NOT_PRESENT);
            }
            VkPresentModeKHR dummy{};
            auto* data = capacity ? reinterpret_cast<VkPresentModeKHR*>(output.data() + sizeof(wire::response)) : &dummy;
            result = fn(physical->second.handle, native_surface, &count, entries ? data : nullptr);
        }
        else
        {
            return finish(VK_ERROR_UNKNOWN);
        }
        const bool defined = result == VK_SUCCESS || result == VK_INCOMPLETE;
        output.resize(sizeof(wire::response) + (defined && entries ? std::min(count, capacity) * stride : 0));
        return finish(result, defined ? count : 0);
    }
    if (request.op == wire::operation::create_swapchain)
    {
        const auto device = host.devices.find(request.owner);
        const auto surface = host.surfaces.find(request.object);
        if (device == host.devices.end() || surface == host.surfaces.end() || device->second.instance_id != surface->second.instance_id ||
            !surface->second.lease.live())
        {
            return finish(VK_ERROR_SURFACE_LOST_KHR);
        }
        auto& dev = device->second;
        if (dev.native_presentation_failed || dev.native_shutdown_started)
        {
            return finish(VK_ERROR_DEVICE_LOST);
        }
        host.collect_native_generations(request.owner);
        const auto retained_swapchains = std::ranges::count_if(
            host.swapchains, [&](const auto& item) { return item.second.device_id == request.owner && item.second.native; });
        if (retained_swapchains >= 16)
        {
            std::fprintf(stderr, "[gpu-bridge] native swapchain retention budget exhausted: device=%llu limit=16\n",
                         static_cast<unsigned long long>(request.owner));
            return finish(VK_ERROR_OUT_OF_DEVICE_MEMORY);
        }
        if (!dev.native_create_swapchain || !dev.native_images || !dev.native_destroy_swapchain)
        {
            return finish(VK_ERROR_EXTENSION_NOT_PRESENT);
        }
        impl::swapchain_data* predecessor{};
        uint64_t predecessor_id{};
        if (request.related)
        {
            const auto previous = host.swapchains.find(request.related);
            if (previous == host.swapchains.end() || !previous->second.explicit_predecessor(request.owner, request.object))
            {
                return finish(VK_ERROR_UNKNOWN);
            }
            predecessor = &previous->second;
            predecessor_id = request.related;
        }
        else
        {
            std::vector<native_present_sync::swapchain_generation*> generations;
            generations.reserve(host.swapchains.size());
            for (auto& item : host.swapchains)
            {
                generations.push_back(&item.second);
            }
            const auto selected = native_present_sync::select_deferred_generation(generations, request.owner, request.object);
            if (selected.ambiguous)
            {
                std::fprintf(stderr, "[gpu-bridge] native swapchain predecessor is ambiguous: device=%llu surface=%llu\n",
                             static_cast<unsigned long long>(request.owner), static_cast<unsigned long long>(request.object));
                return finish(VK_ERROR_INITIALIZATION_FAILED);
            }
            if (selected.value)
            {
                predecessor = static_cast<impl::swapchain_data*>(selected.value);
                for (const auto& [id, state] : host.swapchains)
                {
                    if (&state == predecessor)
                    {
                        predecessor_id = id;
                        break;
                    }
                }
                std::fprintf(stderr,
                             "[gpu-bridge] reusing deferred native swapchain: device=%llu surface=%llu guest-swapchain=%llu "
                             "native=0x%llx records=%zu\n",
                             static_cast<unsigned long long>(request.owner), static_cast<unsigned long long>(request.object),
                             static_cast<unsigned long long>(predecessor_id),
                             static_cast<unsigned long long>(native_present_sync::native_handle_bits(predecessor->native)),
                             predecessor->copies.size());
            }
        }
        std::vector<uint32_t> families(request.words[wire::family_count]);
        std::vector<VkFormat> formats(request.words[wire::format_count]);
        const auto* arrays = packet.data() + sizeof(request);
        if (!families.empty())
        {
            std::memcpy(families.data(), arrays, families.size() * sizeof(uint32_t));
        }
        if (!formats.empty())
        {
            std::memcpy(formats.data(), arrays + families.size() * sizeof(uint32_t), formats.size() * sizeof(VkFormat));
        }
        const VkImageFormatListCreateInfo format_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, nullptr,
                                                      static_cast<uint32_t>(formats.size()), formats.data()};
        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.pNext = formats.empty() ? nullptr : &format_list;
        info.flags = request.words[wire::flags];
        info.surface = surface->second.native;
        info.minImageCount = request.words[wire::min_images];
        info.imageFormat = static_cast<VkFormat>(request.words[wire::format]);
        info.imageColorSpace = static_cast<VkColorSpaceKHR>(request.words[wire::color_space]);
        info.imageExtent = {.width = request.words[wire::width], .height = request.words[wire::height]};
        info.imageArrayLayers = request.words[wire::layers];
        info.imageUsage = request.words[wire::usage];
        info.imageSharingMode = static_cast<VkSharingMode>(request.words[wire::sharing]);
        info.queueFamilyIndexCount = static_cast<uint32_t>(families.size());
        info.pQueueFamilyIndices = families.data();
        info.preTransform = static_cast<VkSurfaceTransformFlagBitsKHR>(request.words[wire::transform]);
        info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(request.words[wire::alpha]);
        info.presentMode = static_cast<VkPresentModeKHR>(request.words[wire::present_mode]);
        info.clipped = request.words[wire::clipped];
        info.oldSwapchain = predecessor ? predecessor->native : VK_NULL_HANDLE;
        impl::swapchain_data value{};
        value.device_id = request.owner;
        value.surface_id = request.object;
        value.hwnd = surface->second.hwnd;
        value.lease = surface->second.lease;
        value.width = info.imageExtent.width;
        value.height = info.imageExtent.height;
        value.format = info.imageFormat;
        value.array_layers = info.imageArrayLayers;
        if (host.gpu_copy)
        {
            if (!dev.create_image || !dev.destroy_image || !dev.get_image_memory_requirements || !dev.allocate_memory ||
                !dev.bind_image_memory || !dev.free_memory ||
                !host.instances.at(dev.instance_id).get_physical_device_image_format_properties ||
                !host.instances.at(dev.instance_id).get_physical_device_memory_properties)
            {
                return finish(VK_ERROR_FEATURE_NOT_PRESENT);
            }
            if (info.flags & ~VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
            {
                return finish(VK_ERROR_FEATURE_NOT_PRESENT);
            }
            VkSurfaceCapabilitiesKHR capabilities{};
            const auto& instance = host.instances.at(dev.instance_id);
            const auto caps = host.load_instance_proc<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
                instance.handle, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
            if (!caps)
            {
                return finish(VK_ERROR_EXTENSION_NOT_PRESENT);
            }
            const auto status = caps(dev.physical_device, info.surface, &capabilities);
            if (status != VK_SUCCESS)
            {
                return finish(status);
            }
            if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
            {
                return finish(VK_ERROR_FORMAT_NOT_SUPPORTED);
            }
            info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        }
        auto result = dev.native_create_swapchain(dev.handle, &info, nullptr, &value.native);
        if (result != VK_SUCCESS)
        {
            std::fprintf(stderr,
                         "[gpu-bridge] native swapchain create failed: result=%d device=%llu surface=%llu format=%d color-space=%d "
                         "present-mode=%d flags=0x%x usage=0x%x extent=%ux%u images=%u old=0x%llx formats=%zu\n",
                         result, static_cast<unsigned long long>(request.owner), static_cast<unsigned long long>(request.object),
                         static_cast<int>(info.imageFormat), static_cast<int>(info.imageColorSpace), static_cast<int>(info.presentMode),
                         info.flags, info.imageUsage, info.imageExtent.width, info.imageExtent.height, info.minImageCount,
                         static_cast<unsigned long long>(native_present_sync::native_handle_bits(info.oldSwapchain)), formats.size());
            return finish(result);
        }
        if (predecessor)
        {
            predecessor->replaced();
        }
        std::vector<uint64_t> registered;
        std::vector<VkDeviceMemory> allocated;
        try
        {
            uint32_t count{};
            std::vector<VkImage> images;
            bool enumerated{};
            for (uint32_t attempt = 0; attempt < 8; ++attempt)
            {
                result = dev.native_images(dev.handle, value.native, &count, nullptr);
                if (result != VK_SUCCESS)
                {
                    throw native_copy_error{result};
                }
                if (!count || count > 64)
                {
                    throw native_copy_error{VK_ERROR_OUT_OF_DEVICE_MEMORY};
                }
                images.resize(count);
                result = dev.native_images(dev.handle, value.native, &count, images.data());
                if (result == VK_INCOMPLETE)
                {
                    continue;
                }
                if (result != VK_SUCCESS)
                {
                    throw native_copy_error{result};
                }
                if (!count || count > images.size())
                {
                    throw native_copy_error{VK_ERROR_INITIALIZATION_FAILED};
                }
                images.resize(count);
                enumerated = true;
                break;
            }
            if (!enumerated)
            {
                throw native_copy_error{VK_ERROR_INITIALIZATION_FAILED};
            }
            value.image_ids.reserve(count);
            value.image_memory.reserve(count);
            value.native_images = images;
            registered.reserve(count);
            allocated.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                VkImage guest_image = images[i];
                VkDeviceMemory memory{};
                if (host.gpu_copy)
                {
                    VkImageCreateInfo image_info{};
                    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                    image_info.pNext = formats.empty() ? nullptr : &format_list;
                    image_info.flags = (info.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
                                           ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT
                                           : 0;
                    image_info.imageType = VK_IMAGE_TYPE_2D;
                    image_info.format = info.imageFormat;
                    image_info.extent = {.width = info.imageExtent.width, .height = info.imageExtent.height, .depth = 1};
                    image_info.mipLevels = 1;
                    image_info.arrayLayers = info.imageArrayLayers;
                    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
                    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
                    image_info.usage = request.words[wire::usage] | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                    image_info.sharingMode = info.imageSharingMode;
                    image_info.queueFamilyIndexCount = info.queueFamilyIndexCount;
                    image_info.pQueueFamilyIndices = info.pQueueFamilyIndices;
                    VkImageFormatProperties supported{};
                    auto& instance = host.instances.at(dev.instance_id);
                    result = instance.get_physical_device_image_format_properties(dev.physical_device, image_info.format,
                                                                                  image_info.imageType, image_info.tiling, image_info.usage,
                                                                                  image_info.flags, &supported);
                    if (result != VK_SUCCESS)
                    {
                        throw native_copy_error{result};
                    }
                    result = dev.create_image(dev.handle, &image_info, nullptr, &guest_image);
                    if (result != VK_SUCCESS)
                    {
                        throw native_copy_error{result};
                    }
                    VkMemoryRequirements requirements{};
                    dev.get_image_memory_requirements(dev.handle, guest_image, &requirements);
                    if (requirements.size > native_present_sync::max_copy_allocation_bytes - dev.native_copy_allocation_bytes -
                                                value.native_copy_allocation_bytes)
                    {
                        dev.destroy_image(dev.handle, guest_image, nullptr);
                        std::fprintf(stderr, "[gpu-bridge] native copy allocation budget exhausted: device=%llu limit=%llu\n",
                                     static_cast<unsigned long long>(request.owner),
                                     static_cast<unsigned long long>(native_present_sync::max_copy_allocation_bytes));
                        throw native_copy_error{VK_ERROR_OUT_OF_DEVICE_MEMORY};
                    }
                    const auto memory_type = host.find_memory_type(dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    if (memory_type == UINT32_MAX)
                    {
                        dev.destroy_image(dev.handle, guest_image, nullptr);
                        throw native_copy_error{VK_ERROR_FEATURE_NOT_PRESENT};
                    }
                    const VkMemoryAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                        .pNext = nullptr,
                                                        .allocationSize = requirements.size,
                                                        .memoryTypeIndex = memory_type};
                    result = dev.allocate_memory(dev.handle, &allocate, nullptr, &memory);
                    if (result == VK_SUCCESS)
                    {
                        result = dev.bind_image_memory(dev.handle, guest_image, memory, 0);
                    }
                    if (result != VK_SUCCESS)
                    {
                        dev.destroy_image(dev.handle, guest_image, nullptr);
                        if (memory)
                        {
                            dev.free_memory(dev.handle, memory, nullptr);
                        }
                        throw native_copy_error{result};
                    }
                    value.native_copy_allocation_bytes += requirements.size;
                }
                const auto id = host.next_id++;
                try
                {
                    host.images.emplace(id, impl::image_data{.handle = guest_image,
                                                             .device_id = request.owner,
                                                             .samples = 1,
                                                             .borrowed = !host.gpu_copy,
                                                             .swapchain_image = true});
                }
                catch (...)
                {
                    if (host.gpu_copy)
                    {
                        dev.destroy_image(dev.handle, guest_image, nullptr);
                        dev.free_memory(dev.handle, memory, nullptr);
                    }
                    throw;
                }
                registered.push_back(id);
                value.image_ids.push_back(id);
                if (memory)
                {
                    value.image_memory.push_back(memory);
                    allocated.push_back(memory);
                }
            }
            const auto id = host.next_id++;
            const auto copy_bytes = value.native_copy_allocation_bytes;
            host.swapchains.emplace(id, std::move(value));
            dev.native_copy_allocation_bytes += copy_bytes;
            return finish(VK_SUCCESS, count, id);
        }
        catch (...)
        {
            for (const auto id : registered)
            {
                const auto image = host.images.find(id);
                if (image != host.images.end())
                {
                    if (!image->second.borrowed)
                    {
                        dev.destroy_image(dev.handle, image->second.handle, nullptr);
                    }
                    host.images.erase(image);
                }
            }
            for (auto memory : allocated)
            {
                dev.free_memory(dev.handle, memory, nullptr);
            }
            dev.native_destroy_swapchain(dev.handle, value.native, nullptr);
            throw;
        }
    }
    if (request.op == wire::operation::present)
    {
        const auto queue = host.queues.find(request.owner);
        if (queue == host.queues.end())
        {
            return finish(VK_ERROR_UNKNOWN);
        }
        auto& dev = host.devices.at(queue->second.device_id);
        if (dev.native_presentation_failed || dev.native_shutdown_started)
        {
            return finish(VK_ERROR_DEVICE_LOST);
        }
        if (!dev.native_present)
        {
            return finish(VK_ERROR_EXTENSION_NOT_PRESENT);
        }
        const auto waits_count = request.words[0];
        const auto count = request.words[1];
        if (count == 0)
        {
            return finish(VK_ERROR_UNKNOWN);
        }
        std::vector<VkSemaphore> waits(waits_count);
        std::vector<VkSwapchainKHR> swapchains(count);
        std::vector<uint32_t> indices(count);
        std::vector<VkResult> results(count, VK_SUCCESS);
        std::vector<impl::swapchain_data*> states(count);
        const auto* arrays = packet.data() + sizeof(request);
        for (uint32_t i = 0; i < waits_count; ++i)
        {
            uint64_t id{};
            std::memcpy(&id, arrays + static_cast<size_t>(i) * sizeof(id), sizeof(id));
            const auto sem = host.semaphores.find(id);
            if (sem == host.semaphores.end() || sem->second.device_id != queue->second.device_id)
            {
                return finish(VK_ERROR_UNKNOWN);
            }
            waits[i] = sem->second.handle;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            wire::present_entry entry{};
            std::memcpy(&entry, arrays + static_cast<size_t>(waits_count) * sizeof(uint64_t) + static_cast<size_t>(i) * sizeof(entry),
                        sizeof(entry));
            const auto swapchain = host.swapchains.find(entry.swapchain);
            if (swapchain == host.swapchains.end() || swapchain->second.device_id != queue->second.device_id || !swapchain->second.native ||
                swapchain->second.destroy_requested || entry.image_index >= swapchain->second.image_ids.size())
            {
                return finish(VK_ERROR_UNKNOWN);
            }
            swapchains[i] = swapchain->second.native;
            indices[i] = entry.image_index;
            states[i] = &swapchain->second;
            for (uint32_t previous = 0; previous < i; ++previous)
            {
                if (states[previous] == states[i])
                {
                    return finish(VK_ERROR_VALIDATION_FAILED_EXT);
                }
            }
        }
        output.resize(sizeof(wire::response) + results.size() * sizeof(VkResult));
        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                              nullptr,
                              waits_count,
                              waits.data(),
                              count,
                              swapchains.data(),
                              indices.data(),
                              results.data()};
        auto copy = host.prepare_native_copy(queue->second, states, indices, waits);
        if (!copy)
        {
            return finish(VK_ERROR_FEATURE_NOT_PRESENT);
        }
        const auto present_fences = copy->fence_info(info.pNext);
        if (dev.native_present_fences)
        {
            info.pNext = &present_fences;
        }
        if (host.gpu_copy)
        {
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &copy->ready;
        }
        const auto native_result = dev.native_present(queue->second.handle, &info);
        const auto completion = copy->presented(native_result);
        if (completion.logical_device_lost)
        {
            dev.native_presentation_failed = true;
            std::fill(results.begin(), results.end(), VK_ERROR_DEVICE_LOST);
        }
        std::memcpy(output.data() + sizeof(wire::response), results.data(), results.size() * sizeof(VkResult));
        return finish(completion.result, count);
    }
    const auto swapchain = host.swapchains.find(request.object);
    if (swapchain == host.swapchains.end() || swapchain->second.device_id != request.owner || !swapchain->second.native)
    {
        return finish(VK_ERROR_UNKNOWN);
    }
    auto& value = swapchain->second;
    auto& dev = host.devices.at(request.owner);
    if (request.op == wire::operation::destroy_swapchain)
    {
        this->destroy_swapchain(request.owner, request.object);
        return finish(VK_SUCCESS);
    }
    if (request.op == wire::operation::images)
    {
        const auto available = static_cast<uint32_t>(value.image_ids.size());
        if (!request.words[1])
        {
            return finish(VK_SUCCESS, available);
        }
        const auto count = std::min(available, request.words[0]);
        output.resize(sizeof(wire::response) + static_cast<size_t>(count) * sizeof(uint64_t));
        if (count)
        {
            std::memcpy(output.data() + sizeof(wire::response), value.image_ids.data(), static_cast<size_t>(count) * sizeof(uint64_t));
        }
        return finish(count < available ? VK_INCOMPLETE : VK_SUCCESS, count);
    }
    if (request.op == wire::operation::acquire)
    {
        if (dev.native_presentation_failed || dev.native_shutdown_started)
        {
            return finish(VK_ERROR_DEVICE_LOST);
        }
        if (value.retired || value.destroy_requested)
        {
            return finish(VK_ERROR_OUT_OF_DATE_KHR);
        }
        if (!value.lease.live())
        {
            return finish(VK_ERROR_SURFACE_LOST_KHR);
        }
        VkSemaphore semaphore{};
        VkFence fence{};
        if (request.related)
        {
            const auto sem = host.semaphores.find(request.related);
            if (sem == host.semaphores.end() || sem->second.device_id != request.owner)
            {
                return finish(VK_ERROR_UNKNOWN);
            }
            semaphore = sem->second.handle;
        }
        if (request.extra)
        {
            const auto found = host.fences.find(request.extra);
            if (found == host.fences.end() || found->second.device_id != request.owner)
            {
                return finish(VK_ERROR_UNKNOWN);
            }
            fence = found->second.handle;
        }
        if (!dev.native_acquire || (!semaphore && !fence))
        {
            return finish(VK_ERROR_UNKNOWN);
        }
        const auto retired = host.retire_native_acquisitions(dev, false);
        if (retired != VK_SUCCESS)
        {
            return finish(retired);
        }
        if (dev.native_presentation_failed || dev.native_shutdown_started)
        {
            return finish(VK_ERROR_DEVICE_LOST);
        }
        if (dev.native_acquisitions.size() >= native_present_sync::max_retained_acquisitions)
        {
            std::fprintf(stderr, "[gpu-bridge] native acquire retention budget exhausted: device=%llu limit=%zu\n",
                         static_cast<unsigned long long>(request.owner), native_present_sync::max_retained_acquisitions);
            return finish(VK_ERROR_OUT_OF_HOST_MEMORY);
        }
        VkQueue forward_queue{};
        if (fence)
        {
            if (!dev.get_device_queue)
            {
                return finish(VK_ERROR_FEATURE_NOT_PRESENT);
            }
            dev.get_device_queue(dev.handle, dev.queue_family_index, 0, &forward_queue);
            if (!forward_queue)
            {
                return finish(VK_ERROR_FEATURE_NOT_PRESENT);
            }
        }
        auto proof = std::make_shared<native_present_sync::acquisition>(impl::native_sync_dispatch(dev), dev.handle, forward_queue);
        proof->initialize(fence != VK_NULL_HANDLE);
        proof->guest_targets(semaphore, fence);
        dev.native_acquisitions.reserve(dev.native_acquisitions.size() + 1);
        uint32_t index{};
        const auto result = dev.native_acquire(dev.handle, value.native, 0, proof->semaphore(semaphore), proof->fence(), &index);
        proof->acquired(result);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        {
            dev.native_acquisitions.push_back(proof);
            host.observe_native_acquire(value, index, proof);
            if (proof->forward(semaphore, fence) != VK_SUCCESS)
            {
                dev.native_presentation_failed = true;
                return finish(VK_ERROR_DEVICE_LOST);
            }
        }
        else if (result == VK_ERROR_DEVICE_LOST)
        {
            dev.native_presentation_failed = true;
        }
        return finish(result, result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ? index : 0);
    }
    return finish(VK_ERROR_UNKNOWN);
}
catch (const native_copy_error& error)
{
    const gpu_bridge::native_wsi::response response{.result = error.result, .count = 0, .object = 0, .payload_bytes = 0, .reserved = 0};
    std::vector<std::byte> bytes(sizeof(response));
    std::memcpy(bytes.data(), &response, sizeof(response));
    return bytes;
}
catch (const std::bad_alloc&)
{
    const gpu_bridge::native_wsi::response response{
        .result = VK_ERROR_OUT_OF_HOST_MEMORY, .count = 0, .object = 0, .payload_bytes = 0, .reserved = 0};
    std::vector<std::byte> bytes(sizeof(response));
    std::memcpy(bytes.data(), &response, sizeof(response));
    return bytes;
}
