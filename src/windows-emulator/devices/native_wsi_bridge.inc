struct native_owner_dispatch
{
    inline static std::atomic_uint32_t retained_or_active{};
    static constexpr uint32_t max_owners = 4;
    std::shared_ptr<ui_completion_queue> queue;
    ui_backend* ui{};
    bool slot{};
    bool retained{};

    void acquire_slot()
    {
        if (slot)
        {
            return;
        }
        const auto previous = retained_or_active.fetch_add(1);
        if (previous >= max_owners)
        {
            retained_or_active.fetch_sub(1);
            throw std::runtime_error("Native presentation owner limit reached (including process-exit quarantine)");
        }
        slot = true;
    }

    ~native_owner_dispatch()
    {
        if (slot && !retained)
        {
            retained_or_active.fetch_sub(1);
        }
    }
};

struct native_io_operation
{
    std::shared_ptr<vulkan_host> host;
    std::shared_ptr<ui_completion_queue> queue;
    ui_backend* ui{};
    std::vector<std::byte> packet;
    gpu_bridge::native_wsi::request request;
    using owned_result = ui_owned_completion<std::vector<std::byte>>;
    ui_completion_ticket<std::shared_ptr<owned_result>> ticket;
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    bool gpu_copy{};

    void start()
    {
        ticket = queue->post([owner = host, queue = queue, ui = ui, packet = packet, request = request](const ui_cancellation_token&) {
            auto result = std::make_shared<owned_result>(queue, [owner, request](std::vector<std::byte> bytes) {
                namespace wire = gpu_bridge::native_wsi;
                if (bytes.size() < sizeof(wire::response))
                {
                    return;
                }
                wire::response response{};
                std::memcpy(&response, bytes.data(), sizeof(response));
                if (response.result != 0 || response.object == 0)
                {
                    return;
                }
                wire::request destroy{};
                destroy.owner = request.owner;
                destroy.object = response.object;
                if (request.op == wire::operation::create_surface)
                {
                    destroy.op = wire::operation::destroy_surface;
                }
                else if (request.op == wire::operation::create_swapchain)
                {
                    destroy.op = wire::operation::destroy_swapchain;
                }
                else
                {
                    return;
                }
                owner->native_wsi_request(wire::encode(destroy));
            });
            if (request.op != gpu_bridge::native_wsi::operation::create_surface &&
                request.op != gpu_bridge::native_wsi::operation::create_swapchain)
            {
                result->adopt();
            }
            native_presentation_window_lease lease;
            if (request.op == gpu_bridge::native_wsi::operation::create_surface)
            {
                auto acquired = ui->acquire_native_window_on_ui(request.object);
                if (acquired.status != native_window_acquire_status::success)
                {
                    std::fprintf(stderr, "[gpu-bridge] native surface rejected: guest HWND=%llx reason=%u\n",
                                 static_cast<unsigned long long>(request.object), static_cast<unsigned>(acquired.status));
                    const gpu_bridge::native_wsi::response failure{
                        .result = -3, .count = 0, .object = 0, .payload_bytes = 0, .reserved = 0};
                    std::vector<std::byte> bytes(sizeof(failure));
                    std::memcpy(bytes.data(), &failure, sizeof(failure));
                    result->value = std::move(bytes);
                    return result;
                }
                lease = std::move(acquired.lease);
            }
            result->value = owner->native_wsi_request(packet, std::move(lease));
            return result;
        });
    }
};

std::shared_ptr<native_owner_dispatch> native_dispatch_{std::make_shared<native_owner_dispatch>()};
bool native_wsi_{};
bool gpu_copy_{};
ui_backend* native_ui_{};

NTSTATUS handle_native_wsi(windows_emulator& win_emu, const io_device_context& context)
{
    namespace wire = gpu_bridge::native_wsi;
    if (!context.vcpu || !context.input_buffer || context.input_buffer_length < sizeof(wire::request) ||
        context.input_buffer_length > wire::max_packet || !context.output_buffer || context.output_buffer_length < sizeof(wire::response))
    {
        return STATUS_INVALID_PARAMETER;
    }
    std::vector<std::byte> packet(context.input_buffer_length);
    win_emu.emu().read_memory(context.input_buffer, packet.data(), packet.size());
    wire::request request{};
    if (!wire::decode(packet, request))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (request.op == wire::operation::enabled)
    {
        return write_output(
            win_emu, context,
            wire::response{.result = 0, .count = this->native_wsi_ ? 1u : 0u, .object = 0, .payload_bytes = 0, .reserved = 0});
    }
    if (!this->native_wsi_ || !this->native_dispatch_->queue)
    {
        return STATUS_NOT_SUPPORTED;
    }

    size_t required = sizeof(wire::response);
    if (request.op == wire::operation::query_surface)
    {
        const auto query = static_cast<wire::query>(request.words[0]);
        if (query == wire::query::capabilities)
        {
            required += 52;
        }
        else if (query == wire::query::support || query == wire::query::win32_support)
        {
            required += 4;
        }
        else if (!wire::append_size(required, request.words[3] ? request.words[2] : 0, query == wire::query::formats ? 8 : 4))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }
    else if (request.op == wire::operation::images)
    {
        if (!wire::append_size(required, request.words[1] ? request.words[0] : 0, 8))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }
    else if (request.op == wire::operation::present)
    {
        if (!wire::append_size(required, request.words[1], 4))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (context.output_buffer_length < required)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (request.op == wire::operation::destroy_device || request.op == wire::operation::destroy_instance)
    {
        for (auto it = this->direct_mappings_.begin(); it != this->direct_mappings_.end();)
        {
            const bool owned = request.op == wire::operation::destroy_device ? it->second.device == request.owner
                                                                             : this->vulkan_.owns_device(request.owner, it->second.device);
            if (!owned)
            {
                ++it;
                continue;
            }
            win_emu.memory.revoke_host_memory(it->second.backing);
            this->vulkan_.unmap_memory(it->second.device, it->first);
            it = this->direct_mappings_.erase(it);
        }
    }

    auto pending = std::make_shared<native_io_operation>();
    pending->host = this->vulkan_owner_;
    pending->queue = this->native_dispatch_->queue;
    pending->ui = this->native_ui_;
    pending->packet = std::move(packet);
    pending->request = request;
    pending->gpu_copy = this->gpu_copy_;
    pending->start();
    auto* const waiting = &context.thread();
    waiting->await_host_status = STATUS_SUCCESS;
    waiting->await_host_condition = [pending, activity = this->presentation_activity_, &win_emu, context, waiting]() {
        auto completed = pending->ticket.try_take();
        if (!completed)
        {
            return false;
        }
        std::vector<std::byte> bytes;
        if (completed->status != ui_completion_status::completed || !completed->value)
        {
            wire::response failure{.result = -3, .count = 0, .object = 0, .payload_bytes = 0, .reserved = 0};
            bytes.resize(sizeof(failure));
            std::memcpy(bytes.data(), &failure, sizeof(failure));
        }
        else
        {
            bytes = (*completed->value)->value;
        }
        wire::response response{};
        if (bytes.size() < sizeof(response))
        {
            waiting->await_host_status = STATUS_UNSUCCESSFUL;
            return true;
        }
        std::memcpy(&response, bytes.data(), sizeof(response));
        if (pending->request.op == wire::operation::acquire && (response.result == 1 || response.result == 2) &&
            pending->request.timeout != 0)
        {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pending->started).count();
            if (pending->request.timeout == UINT64_MAX || static_cast<uint64_t>(std::max<int64_t>(elapsed, 0)) < pending->request.timeout)
            {
                pending->start();
                return false;
            }
            response.result = 2;
            std::memcpy(bytes.data(), &response, sizeof(response));
        }
        waiting->await_host_status = deliver_native_wsi_output(win_emu.emu(), context, bytes);
        if (waiting->await_host_status != STATUS_SUCCESS)
        {
            return true;
        }
        if (completed->value)
        {
            (*completed->value)->adopt();
        }
        try
        {
            if (pending->request.op == wire::operation::present && response.count <= pending->request.words[1] &&
                bytes.size() >= sizeof(response) + static_cast<size_t>(response.count) * sizeof(int32_t))
            {
                uint32_t accepted{};
                for (uint32_t index = 0; index < response.count; ++index)
                {
                    int32_t result{};
                    std::memcpy(&result, bytes.data() + sizeof(response) + index * sizeof(result), sizeof(result));
                    if (result == 0 || result == 1000001003) // VK_SUCCESS or VK_SUBOPTIMAL_KHR
                    {
                        ++accepted;
                    }
                }
                activity->record(win_emu, accepted, pending->request.words[1] - accepted);
            }
            if (response.result == 0 && pending->request.op == wire::operation::create_swapchain)
            {
                vulkan_host::render_device_info adapter;
                pending->host->get_swapchain_render_device(pending->request.owner, response.object, adapter);
                const char* kind = "Other";
                switch (adapter.type)
                {
                case 1:
                    kind = "Integrated GPU";
                    break;
                case 2:
                    kind = "Discrete GPU";
                    break;
                case 3:
                    kind = "Virtual GPU";
                    break;
                case 4:
                    kind = "CPU";
                    break;
                default:
                    break;
                }
                const auto message = std::format(
                    "Using device: {} | {} | Vulkan device ID 0x{:X} | Vulkan swapchain ID 0x{:X} | "
                    "presentation: {} | device_luid_valid={} | device_luid_hex={} | device_node_mask={} | guest_window=0x{:X}",
                    kind, adapter.name, pending->request.owner, response.object,
                    pending->gpu_copy ? "GPU copy, device-local images to native Win32 Vulkan WSI" : "native direct Win32 Vulkan WSI",
                    adapter.device_luid_valid, adapter.device_luid_hex, adapter.device_node_mask, adapter.guest_window);
                gpu_bridge_device::publish_gpu_status("render-device-live.txt", message);
                if (win_emu.callbacks.on_generic_activity)
                {
                    win_emu.callbacks.on_generic_activity(message);
                }
                else
                {
                    win_emu.log.info("%s\n", message.c_str());
                }
            }
        }
        catch (...)
        {
            std::fputs("[gpu-bridge] native WSI result delivered; activity logging failed\n", stderr);
        }
        return true;
    };
    win_emu.yield_thread(*context.vcpu, false);
    return STATUS_SUCCESS;
}
