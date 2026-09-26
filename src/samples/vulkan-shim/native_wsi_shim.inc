namespace
{
    namespace nw = sogen::gpu_bridge::native_wsi;

    struct native_reply
    {
        nw::response header{.result = VK_ERROR_UNKNOWN, .count = 0, .object = 0, .payload_bytes = 0, .reserved = 0};
        std::vector<std::byte> payload;
    };

    native_reply native_exchange(nw::request request, const std::span<const std::byte> trailing = {}, const size_t capacity = 0)
    {
        native_reply result;
        if (capacity > nw::max_packet - sizeof(nw::response))
        {
            return result;
        }
        auto packet = nw::encode(request, trailing);
        if (packet.empty())
        {
            return result;
        }
        std::vector<std::byte> output(sizeof(nw::response) + capacity);
        if (!bridge_call(nw::ioctl, packet.data(), static_cast<DWORD>(packet.size()), output.data(), static_cast<DWORD>(output.size())))
        {
            return result;
        }
        std::memcpy(&result.header, output.data(), sizeof(result.header));
        if (result.header.reserved != 0 || result.header.payload_bytes > capacity)
        {
            result.header = {.result = VK_ERROR_UNKNOWN, .count = 0, .object = 0, .payload_bytes = 0, .reserved = 0};
            return result;
        }
        result.payload.assign(output.begin() + sizeof(nw::response), output.begin() + sizeof(nw::response) + result.header.payload_bytes);
        return result;
    }

    bool native_wsi_enabled()
    {
        if (passthrough_active())
        {
            return false;
        }
        static const bool enabled = [] {
            nw::request request{};
            request.op = nw::operation::enabled;
            const auto result = native_exchange(request);
            return result.header.result == VK_SUCCESS && result.header.count == 1;
        }();
        return enabled;
    }

    VkResult native_destroy(const nw::operation op, const uint64_t owner, const uint64_t object = 0)
    {
        nw::request request{};
        request.op = op;
        request.owner = owner;
        request.object = object;
        return static_cast<VkResult>(native_exchange(request).header.result);
    }

    VkResult native_create_surface(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* info, VkSurfaceKHR* surface)
    {
        if (!info || !surface || info->sType != VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR || info->pNext || info->flags)
        {
            return VK_ERROR_UNKNOWN;
        }
        nw::request request{};
        request.op = nw::operation::create_surface;
        request.owner = to_object_id(instance);
        request.object = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info->hwnd));
        const auto result = native_exchange(request);
        if (result.header.result == VK_SUCCESS)
        {
            *surface = to_handle<VkSurfaceKHR>(result.header.object);
        }
        return static_cast<VkResult>(result.header.result);
    }

    VkResult native_query(const nw::query query, VkPhysicalDevice physical, VkSurfaceKHR surface, const uint32_t family, uint32_t* count,
                          void* values, const size_t stride)
    {
        if (!count)
        {
            return VK_ERROR_UNKNOWN;
        }
        const auto capacity = values ? *count : 0;
        size_t bytes = sizeof(nw::response);
        if (!nw::append_size(bytes, capacity, stride))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        nw::request request{};
        request.op = nw::operation::query_surface;
        request.owner = to_object_id(physical);
        request.object = to_object_id(surface);
        request.words[0] = static_cast<uint32_t>(query);
        request.words[1] = family;
        request.words[2] = capacity;
        request.words[3] = values ? 1 : 0;
        auto result = native_exchange(request, {}, bytes - sizeof(nw::response));
        const auto status = static_cast<VkResult>(result.header.result);
        if (status == VK_SUCCESS || status == VK_INCOMPLETE)
        {
            const auto written = query == nw::query::capabilities || query == nw::query::support || query == nw::query::win32_support
                                     ? 1u
                                     : result.header.count;
            if (values && (written > capacity || result.payload.size() != static_cast<size_t>(written) * stride))
            {
                return VK_ERROR_UNKNOWN;
            }
            if (values && !result.payload.empty())
            {
                std::memcpy(values, result.payload.data(), result.payload.size());
            }
            *count = written;
        }
        return status;
    }

    VkResult native_formats2(VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR* info, uint32_t* count,
                             VkSurfaceFormat2KHR* formats)
    {
        if (!info || !count || info->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR || info->pNext)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        if (!formats)
        {
            return native_query(nw::query::formats, physical, info->surface, 0, count, nullptr, sizeof(VkSurfaceFormatKHR));
        }
        const uint32_t capacity = *count;
        size_t bytes = sizeof(nw::response);
        if (!nw::append_size(bytes, capacity, sizeof(VkSurfaceFormatKHR)))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        for (uint32_t i = 0; i < capacity; ++i)
        {
            if (formats[i].sType != VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR || formats[i].pNext)
            {
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
        }
        std::vector<VkSurfaceFormatKHR> values(capacity);
        VkSurfaceFormatKHR dummy{};
        uint32_t written = capacity;
        const auto result = native_query(nw::query::formats, physical, info->surface, 0, &written, capacity ? values.data() : &dummy,
                                         sizeof(VkSurfaceFormatKHR));
        if (result == VK_SUCCESS || result == VK_INCOMPLETE)
        {
            for (uint32_t i = 0; i < written; ++i)
            {
                formats[i].surfaceFormat = values[i];
            }
            *count = written;
        }
        return result;
    }

    VkResult native_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info, VkSwapchainKHR* swapchain)
    {
        if (!info || !swapchain || info->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR)
        {
            return VK_ERROR_UNKNOWN;
        }
        const VkImageFormatListCreateInfo* formats{};
        for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext)
        {
            if (node->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO || formats)
            {
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
            formats = reinterpret_cast<const VkImageFormatListCreateInfo*>(node);
        }
        const uint32_t families = info->imageSharingMode == VK_SHARING_MODE_CONCURRENT ? info->queueFamilyIndexCount : 0;
        const uint32_t format_count = formats ? formats->viewFormatCount : 0;
        if ((families && !info->pQueueFamilyIndices) || (format_count && !formats->pViewFormats))
        {
            return VK_ERROR_UNKNOWN;
        }
        size_t bytes = sizeof(nw::request);
        if (!nw::append_size(bytes, families, sizeof(uint32_t)) || !nw::append_size(bytes, format_count, sizeof(uint32_t)))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        nw::request request{};
        request.op = nw::operation::create_swapchain;
        request.owner = to_object_id(device);
        request.object = to_object_id(info->surface);
        request.related = to_object_id(info->oldSwapchain);
        request.words = {info->flags,
                         info->minImageCount,
                         static_cast<uint32_t>(info->imageFormat),
                         static_cast<uint32_t>(info->imageColorSpace),
                         info->imageExtent.width,
                         info->imageExtent.height,
                         info->imageArrayLayers,
                         info->imageUsage,
                         static_cast<uint32_t>(info->imageSharingMode),
                         static_cast<uint32_t>(info->preTransform),
                         static_cast<uint32_t>(info->compositeAlpha),
                         static_cast<uint32_t>(info->presentMode),
                         info->clipped,
                         families,
                         format_count,
                         0};
        std::vector<std::byte> arrays(bytes - sizeof(request));
        if (families)
        {
            std::memcpy(arrays.data(), info->pQueueFamilyIndices, static_cast<size_t>(families) * sizeof(uint32_t));
        }
        if (format_count)
        {
            std::memcpy(arrays.data() + static_cast<size_t>(families) * sizeof(uint32_t), formats->pViewFormats,
                        static_cast<size_t>(format_count) * sizeof(uint32_t));
        }
        const auto result = native_exchange(request, arrays);
        if (result.header.result == VK_SUCCESS)
        {
            *swapchain = to_handle<VkSwapchainKHR>(result.header.object);
        }
        return static_cast<VkResult>(result.header.result);
    }

    VkResult native_images(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images)
    {
        if (!count)
        {
            return VK_ERROR_UNKNOWN;
        }
        const auto capacity = images ? *count : 0;
        size_t bytes = sizeof(nw::response);
        if (!nw::append_size(bytes, capacity, sizeof(uint64_t)))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        nw::request request{};
        request.op = nw::operation::images;
        request.owner = to_object_id(device);
        request.object = to_object_id(swapchain);
        request.words[0] = capacity;
        request.words[1] = images ? 1 : 0;
        const auto result = native_exchange(request, {}, bytes - sizeof(nw::response));
        if (result.header.result == VK_SUCCESS || result.header.result == VK_INCOMPLETE)
        {
            if (images)
            {
                if (result.header.count > capacity || result.payload.size() != static_cast<size_t>(result.header.count) * sizeof(uint64_t))
                {
                    return VK_ERROR_UNKNOWN;
                }
                for (uint32_t i = 0; i < result.header.count; ++i)
                {
                    uint64_t id{};
                    std::memcpy(&id, result.payload.data() + static_cast<size_t>(i) * sizeof(id), sizeof(id));
                    images[i] = to_handle<VkImage>(id);
                }
            }
            *count = result.header.count;
        }
        return static_cast<VkResult>(result.header.result);
    }

    VkResult native_acquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                            uint32_t* index)
    {
        if (!index)
        {
            return VK_ERROR_UNKNOWN;
        }
        nw::request request{};
        request.op = nw::operation::acquire;
        request.owner = to_object_id(device);
        request.object = to_object_id(swapchain);
        request.related = to_object_id(semaphore);
        request.extra = to_object_id(fence);
        request.timeout = timeout;
        const auto result = native_exchange(request);
        if (result.header.result == VK_SUCCESS || result.header.result == VK_SUBOPTIMAL_KHR)
        {
            *index = result.header.count;
        }
        return static_cast<VkResult>(result.header.result);
    }

    VkResult native_present(VkQueue queue, const VkPresentInfoKHR* info)
    {
        if (!info || info->sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR || info->pNext || !info->swapchainCount || !info->pSwapchains ||
            !info->pImageIndices || (info->waitSemaphoreCount && !info->pWaitSemaphores))
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        size_t bytes = sizeof(nw::request);
        if (!nw::append_size(bytes, info->waitSemaphoreCount, sizeof(uint64_t)) ||
            !nw::append_size(bytes, info->swapchainCount, sizeof(nw::present_entry)))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        nw::request request{};
        request.op = nw::operation::present;
        request.owner = to_object_id(queue);
        request.words[0] = info->waitSemaphoreCount;
        request.words[1] = info->swapchainCount;
        std::vector<std::byte> arrays(bytes - sizeof(request));
        size_t offset{};
        for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i)
        {
            const auto id = to_object_id(info->pWaitSemaphores[i]);
            std::memcpy(arrays.data() + offset, &id, sizeof(id));
            offset += sizeof(id);
        }
        for (uint32_t i = 0; i < info->swapchainCount; ++i)
        {
            const nw::present_entry entry{
                .swapchain = to_object_id(info->pSwapchains[i]), .image_index = info->pImageIndices[i], .reserved = 0};
            std::memcpy(arrays.data() + offset, &entry, sizeof(entry));
            offset += sizeof(entry);
        }
        const auto result = native_exchange(request, arrays, static_cast<size_t>(info->swapchainCount) * sizeof(int32_t));
        if (info->pResults)
        {
            if (result.header.count == info->swapchainCount &&
                result.payload.size() == static_cast<size_t>(info->swapchainCount) * sizeof(int32_t))
            {
                std::memcpy(info->pResults, result.payload.data(), result.payload.size());
            }
            else
            {
                std::fill_n(info->pResults, info->swapchainCount, static_cast<VkResult>(result.header.result));
            }
        }
        return static_cast<VkResult>(result.header.result);
    }
}
