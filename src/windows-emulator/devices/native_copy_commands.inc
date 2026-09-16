static native_present_sync::dispatch native_sync_dispatch(const device_data& dev)
{
    return {.create_fence = dev.create_fence,
            .destroy_fence = dev.destroy_fence,
            .get_fence_status = dev.get_fence_status,
            .wait_for_fences = dev.wait_for_fences,
            .destroy_pool = dev.destroy_command_pool,
            .destroy_semaphore = dev.destroy_semaphore,
            .create_semaphore = dev.create_semaphore,
            .queue_submit = dev.queue_submit,
            .queue_wait_idle = dev.queue_wait_idle};
}

bool native_references(uint64_t device_id, VkSemaphore semaphore) const
{
    for (const auto& proof : this->devices.at(device_id).native_acquisitions)
    {
        if (proof->references(semaphore))
        {
            return true;
        }
    }
    for (const auto& [id, state] : this->swapchains)
    {
        if (state.device_id == device_id)
        {
            for (const auto& batch : state.copies)
            {
                if (batch->references(semaphore))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool native_references_fence(uint64_t device_id, VkFence fence) const
{
    for (const auto& proof : this->devices.at(device_id).native_acquisitions)
    {
        if (proof->references_fence(fence))
        {
            return true;
        }
    }
    return false;
}

void collect_native_guest_sync(uint64_t device_id)
{
    auto& dev = this->devices.at(device_id);
    if (dev.native_presentation_failed)
    {
        return;
    }
    for (auto it = this->semaphores.begin(); it != this->semaphores.end();)
    {
        if (it->second.device_id == device_id && it->second.native_destroy_requested && !native_references(device_id, it->second.handle))
        {
            dev.destroy_semaphore(dev.handle, it->second.handle, nullptr);
            it = this->semaphores.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = this->fences.begin(); it != this->fences.end();)
    {
        if (it->second.device_id == device_id && it->second.native_destroy_requested &&
            !native_references_fence(device_id, it->second.handle))
        {
            dev.destroy_fence(dev.handle, it->second.handle, nullptr);
            it = this->fences.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

VkResult retire_native_acquisitions(device_data& dev, bool wait)
{
    for (auto it = dev.native_acquisitions.begin(); it != dev.native_acquisitions.end();)
    {
        const auto result = (*it)->complete(wait);
        if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
        {
            it = dev.native_acquisitions.erase(it);
            if (result == VK_ERROR_DEVICE_LOST)
            {
                dev.native_presentation_failed = true;
            }
        }
        else if (!wait && result == VK_NOT_READY)
        {
            ++it;
        }
        else
        {
            return result;
        }
    }
    return VK_SUCCESS;
}

VkResult retire_native_presentations(swapchain_data& swapchain, bool wait)
{
    for (auto it = swapchain.copies.begin(); it != swapchain.copies.end();)
    {
        const auto result = wait ? (*it)->wait() : (*it)->poll();
        if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
        {
            it = swapchain.copies.erase(it);
            if (result == VK_ERROR_DEVICE_LOST)
            {
                this->devices.at(swapchain.device_id).native_presentation_failed = true;
            }
        }
        else if (!wait && result == VK_NOT_READY)
        {
            ++it;
        }
        else
        {
            return result;
        }
    }
    return VK_SUCCESS;
}

VkResult drain_native_device(uint64_t device_id)
{
    auto& dev = this->devices.at(device_id);
    if (dev.native_presentation_failed)
    {
        return VK_ERROR_DEVICE_LOST;
    }
    auto result = retire_native_acquisitions(dev, false);
    if (result != VK_SUCCESS)
    {
        return result;
    }
    if (dev.native_presentation_failed)
    {
        return VK_ERROR_DEVICE_LOST;
    }
    VkResult retained = dev.native_acquisitions.empty() ? VK_SUCCESS : VK_NOT_READY;
    for (auto& [id, state] : this->swapchains)
    {
        if (state.device_id != device_id || !state.native)
        {
            continue;
        }
        result = retire_native_presentations(state, false);
        if (result != VK_SUCCESS)
        {
            return result;
        }
        if (dev.native_presentation_failed)
        {
            return VK_ERROR_DEVICE_LOST;
        }
        if (!state.copies.empty())
        {
            retained = VK_NOT_READY;
        }
    }
    if (retained != VK_SUCCESS)
    {
        return retained;
    }
    if (!dev.native_shutdown_started)
    {
        dev.native_shutdown_fences.clear();
        for (const auto& [id, queue] : this->queues)
        {
            if (queue.device_id != device_id)
            {
                continue;
            }
            auto proof = std::make_shared<native_present_sync::acquisition>(native_sync_dispatch(dev), dev.handle, queue.handle);
            proof->initialize(false);
            dev.native_shutdown_fences.emplace_back(queue.handle, std::move(proof));
        }
        dev.native_shutdown_started = true;
        for (const auto& [queue, proof] : dev.native_shutdown_fences)
        {
            result = dev.queue_submit(queue, 0, nullptr, proof->fence());
            proof->acquired(result);
            if (result != VK_SUCCESS)
            {
                dev.native_presentation_failed = true;
                return result;
            }
        }
    }
    for (const auto& [queue, proof] : dev.native_shutdown_fences)
    {
        result = proof->complete(false);
        if (result != VK_SUCCESS)
        {
            return result;
        }
    }
    dev.native_shutdown_fences.clear();
    return VK_SUCCESS;
}

void observe_native_acquire(swapchain_data& state, uint32_t index, const std::shared_ptr<native_present_sync::acquisition>& proof)
{
    uint64_t completed_serial{};
    VkQueue completed_queue{};
    for (const auto& batch : state.copies)
    {
        if (!batch->uses(state.native, index))
        {
            continue;
        }
        batch->attach(state.native, proof, &index);
        if (batch->serial > completed_serial)
        {
            completed_serial = batch->serial;
            completed_queue = batch->queue;
        }
    }
    if (!completed_serial)
    {
        return;
    }
    // A completed successor present can retire earlier generations on that surface. A
    // successful acquire alone is not proof: every attached record still waits its fence.
    for (auto& [id, old] : this->swapchains)
    {
        if (&old == &state || old.device_id != state.device_id || old.surface_id != state.surface_id || !old.retired)
        {
            continue;
        }
        for (const auto& batch : old.copies)
        {
            if (batch->serial < completed_serial && batch->queue == completed_queue)
            {
                batch->attach(old.native, proof);
            }
        }
    }
}

void collect_native_generations(uint64_t device_id)
{
    for (auto it = this->swapchains.begin(); it != this->swapchains.end();)
    {
        auto& state = it->second;
        if (state.device_id == device_id && state.native && state.destroy_requested)
        {
            const auto result = retire_native_presentations(state, false);
            if (result != VK_SUCCESS)
            {
                throw native_copy_error{result};
            }
            if (state.copies.empty())
            {
                this->destroy_swapchain_resources(state);
                it = this->swapchains.erase(it);
                continue;
            }
        }
        ++it;
    }
    collect_native_guest_sync(device_id);
}

std::shared_ptr<native_copy_submission> prepare_native_copy(const queue_data& queue, const std::span<swapchain_data*> states,
                                                            const std::span<const uint32_t> indices,
                                                            const std::span<const VkSemaphore> waits)
{
    auto& dev = this->devices.at(queue.device_id);
    if (dev.native_presentation_failed)
    {
        return {};
    }
    const auto acquisitions = retire_native_acquisitions(dev, false);
    if (acquisitions != VK_SUCCESS)
    {
        throw native_copy_error{acquisitions};
    }
    collect_native_generations(queue.device_id);
    size_t retained{};
    for (auto& [id, state] : this->swapchains)
    {
        if (state.device_id != queue.device_id)
        {
            continue;
        }
        const auto result = retire_native_presentations(state, false);
        if (result != VK_SUCCESS)
        {
            throw native_copy_error{result};
        }
        retained += state.copies.size();
    }
    if (retained + states.size() > native_present_sync::max_retained_submissions)
    {
        std::fprintf(stderr, "[gpu-bridge] native present retention budget exhausted: device=%llu records=%zu limit=%zu\n",
                     static_cast<unsigned long long>(queue.device_id), retained, native_present_sync::max_retained_submissions);
        throw native_copy_error{VK_ERROR_OUT_OF_HOST_MEMORY};
    }
    for (auto* state : states)
    {
        const auto result = retire_native_presentations(*state, false);
        if (result != VK_SUCCESS)
        {
            throw native_copy_error{result};
        }
        if (dev.native_presentation_failed)
        {
            throw native_copy_error{VK_ERROR_DEVICE_LOST};
        }
    }
    const auto api = native_sync_dispatch(dev);
    auto copy = std::make_shared<native_copy_submission>(api, dev.handle, queue.handle);
    std::vector<VkSwapchainKHR> native_swapchains;
    for (const auto* state : states)
    {
        native_swapchains.push_back(state->native);
    }
    copy->initialize(native_swapchains, indices, this->gpu_copy, dev.native_present_fences);
    copy->guest_waits(waits);
    copy->serial = ++dev.native_present_serial;
    for (auto* state : states)
    {
        state->copies.reserve(state->copies.size() + 1);
    }
    const auto retain = [&] {
        for (auto* state : states)
        {
            state->copies.push_back(copy);
        }
    };
    if (!this->gpu_copy)
    {
        retain();
        return copy;
    }
    if (queue.family != dev.queue_family_index)
    {
        return {};
    }
    auto& instance = this->instances.at(dev.instance_id);
    uint32_t count{};
    instance.get_queue_family_properties(dev.physical_device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    instance.get_queue_family_properties(dev.physical_device, &count, families.data());
    if (queue.family >= count ||
        (families[queue.family].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0)
    {
        return {};
    }
    if (!dev.create_command_pool || !dev.allocate_command_buffers || !dev.begin_command_buffer || !dev.end_command_buffer ||
        !dev.cmd_pipeline_barrier || !dev.cmd_copy_image || !dev.queue_submit || !dev.create_semaphore || !dev.destroy_command_pool ||
        !dev.destroy_semaphore)
    {
        return {};
    }
    const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .pNext = nullptr,
                                            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                            .queueFamilyIndex = queue.family};
    auto result = dev.create_command_pool(dev.handle, &pool_info, nullptr, &copy->pool);
    if (result != VK_SUCCESS)
    {
        throw native_copy_error{result};
    }
    const VkCommandBufferAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                               .pNext = nullptr,
                                               .commandPool = copy->pool,
                                               .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               .commandBufferCount = 1};
    result = dev.allocate_command_buffers(dev.handle, &allocate, &copy->commands);
    if (result != VK_SUCCESS)
    {
        throw native_copy_error{result};
    }
    const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    result = dev.create_semaphore(dev.handle, &semaphore_info, nullptr, &copy->ready);
    if (result != VK_SUCCESS)
    {
        throw native_copy_error{result};
    }
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .pNext = nullptr,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                         .pInheritanceInfo = nullptr};
    result = dev.begin_command_buffer(copy->commands, &begin);
    if (result != VK_SUCCESS)
    {
        throw native_copy_error{result};
    }
    for (size_t i = 0; i < states.size(); ++i)
    {
        auto& swapchain = *states[i];
        const auto index = indices[i];
        const auto source = this->images.at(swapchain.image_ids[index]).handle;
        const auto destination = swapchain.native_images[index];
        const VkImageSubresourceRange range{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                            .baseMipLevel = 0,
                                            .levelCount = 1,
                                            .baseArrayLayer = 0,
                                            .layerCount = swapchain.array_layers};
        std::array<VkImageMemoryBarrier, 2> barriers{};
        barriers[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                       nullptr,
                       VK_ACCESS_MEMORY_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_QUEUE_FAMILY_IGNORED,
                       VK_QUEUE_FAMILY_IGNORED,
                       source,
                       range};
        barriers[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                       nullptr,
                       0,
                       VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_QUEUE_FAMILY_IGNORED,
                       VK_QUEUE_FAMILY_IGNORED,
                       destination,
                       range};
        dev.cmd_pipeline_barrier(copy->commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());
        VkImageCopy region{};
        region.srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = swapchain.array_layers};
        region.dstSubresource = region.srcSubresource;
        region.extent = {.width = swapchain.width, .height = swapchain.height, .depth = 1};
        dev.cmd_copy_image(copy->commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);
        const VkImageMemoryBarrier present{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                           nullptr,
                                           VK_ACCESS_TRANSFER_WRITE_BIT,
                                           0,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                           VK_QUEUE_FAMILY_IGNORED,
                                           VK_QUEUE_FAMILY_IGNORED,
                                           destination,
                                           range};
        dev.cmd_pipeline_barrier(copy->commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &present);
    }
    result = dev.end_command_buffer(copy->commands);
    if (result != VK_SUCCESS)
    {
        throw native_copy_error{result};
    }
    std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              nullptr,
                              static_cast<uint32_t>(waits.size()),
                              waits.data(),
                              stages.data(),
                              1,
                              &copy->commands,
                              1,
                              &copy->ready};
    retain();
    result = dev.queue_submit(queue.handle, 1, &submit, copy->copy_fence);
    copy->submitted(result);
    if (result == VK_ERROR_DEVICE_LOST)
    {
        dev.native_presentation_failed = true;
    }
    if (result != VK_SUCCESS)
    {
        throw native_copy_error{result};
    }
    return copy;
}
