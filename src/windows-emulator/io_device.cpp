#include "std_include.hpp"
#include "io_device.hpp"
#include "windows_emulator.hpp"
#include "devices/afd_endpoint.hpp"
#include "devices/mount_point_manager.hpp"
#include "devices/security_support_provider.hpp"
#include "devices/named_pipe.hpp"
#include "devices/network_store_interface.hpp"
#include "devices/gpu_bridge.hpp"
#include "devices/steam_bridge.hpp"
#include "devices/console.hpp"
#include "devices/cm_api.hpp"
#include <iostream>

namespace sogen
{

    namespace
    {
        struct dummy_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator&, const io_device_context&) override
            {
                return STATUS_SUCCESS;
            }
        };

        // A device whose handle opens but that fails every ioctl. Used for the PnP DevQuery interface that
        // audioses probes for device properties: a clean error makes the caller fall back to its defaults,
        // whereas returning success with no data leads it to read uninitialized output.
        struct unsupported_io_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator&, const io_device_context&) override
            {
                return STATUS_NOT_SUPPORTED;
            }
        };

        struct transport_stub_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override
            {
                if (context.output_buffer && context.output_buffer_length)
                {
                    win_emu.emu().set_memory(context.output_buffer, 0, context.output_buffer_length);
                }

                if (context.io_status_block)
                {
                    IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                    block.Information = context.output_buffer_length;
                    context.io_status_block.write(block);
                }

                return STATUS_SUCCESS;
            }
        };

        // Factories for the devices defined locally in this file, matching the shared create_*
        // (device_creation_context) signature so they slot straight into the registry.
        std::unique_ptr<io_device> create_dummy_device(const device_creation_context&)
        {
            return std::make_unique<dummy_device>();
        }

        std::unique_ptr<io_device> create_transport_stub_device(const device_creation_context&)
        {
            return std::make_unique<transport_stub_device>();
        }

        std::unique_ptr<io_device> create_unsupported_io_device(const device_creation_context&)
        {
            return std::make_unique<unsupported_io_device>();
        }

        std::unique_ptr<io_device> create_named_pipe_device(const device_creation_context&)
        {
            return std::make_unique<named_pipe>();
        }

    }

    const std::map<std::u16string_view, device_factory>& get_device_registry()
    {
        using namespace std::string_view_literals;

        static const std::map<std::u16string_view, device_factory> registry = {
            // Dummy
            {u"CNG"sv, create_dummy_device},
            {u"RasAcd"sv, create_dummy_device},
            {u"PcwDrv"sv, create_dummy_device},
            {u"DeviceApi\\CMApi"sv, create_cm_api},
            {u"DeviceApi\\CMNotify"sv, create_dummy_device},
            {u"ConDrv\\Server"sv, create_dummy_device},
            {u"DeviceApi\\Dev\\Query"sv, create_unsupported_io_device},
            // Generic
            {u"Console"sv, create_console_device},
            // Multimedia Class Scheduler. avrt!AvSetMmThreadCharacteristics opens this to raise the audio
            // render worker's scheduling priority; the emulator has no priority classes, so accepting the open
            // and succeeding its ioctls is enough for the worker to proceed.
            {u"MMCSS\\MmThread"sv, create_dummy_device},
            {u"Nsi"sv, create_network_store_interface},
            {u"MountPointManager"sv, create_mount_point_manager},
            {u"KsecDD"sv, create_security_support_provider},
            {u"NamedPipe"sv, create_named_pipe_device},
            {u"SogenGpu"sv, create_gpu_bridge},
            {u"SogenSteam"sv, create_steam_bridge},
            // AFD
            {u"Afd\\Endpoint"sv, create_afd_endpoint},
            {u"Afd\\AsyncConnectHlp"sv, create_afd_async_connect_hlp},
            // Transport
            {u"Tcp"sv, create_transport_stub_device},
            {u"Tcp6"sv, create_transport_stub_device},
            {u"Udp"sv, create_transport_stub_device},
            {u"RawIp"sv, create_transport_stub_device},
        };
        return registry;
    }

    bool needs_32_bit_devices(const windows_emulator& win_emu)
    {
        return win_emu.process.is_wow64_process;
    }

    std::unique_ptr<io_device> create_device(const std::u16string_view device, const device_creation_context& context)
    {
        const auto& registry = get_device_registry();
        const auto it = registry.find(device);
        if (it == registry.end())
        {
            throw std::runtime_error("Unsupported device: " + u16_to_u8(device));
        }

        return it->second(context);
    }

    emulator_thread& io_device_context::thread() const
    {
        if (!this->vcpu)
        {
            throw std::runtime_error("I/O request has no issuing vCPU");
        }

        return this->vcpu->thread();
    }

    void complete_device_ioctl(windows_emulator& win_emu, const io_device_context& c, const NTSTATUS status,
                               const bool completed_synchronously)
    {
        if (status == STATUS_PENDING)
        {
            return;
        }
        if (c.event.bits)
        {
            if (auto* event = win_emu.process.events.get(c.event))
            {
                event->signaled = true;
            }
        }
        const auto block = c.io_status_block ? c.io_status_block.read() : IO_STATUS_BLOCK<EmulatorTraits<Emu64>>{.Status = status};
        if (c.apc_routine)
        {
            const auto issuer = win_emu.process.thread_handles_by_id.find(c.issuer_thread_id);
            if (issuer != win_emu.process.thread_handles_by_id.end())
            {
                if (auto* thread = win_emu.process.threads.get(issuer->second))
                {
                    thread->pending_apcs.push_back({
                        .flags = 0,
                        .apc_routine = c.apc_routine,
                        .apc_argument1 = c.apc_context,
                        .apc_argument2 = c.io_status_block.value(),
                        .apc_argument3 = 0,
                        .restamp_io_status_block = win_emu.process.is_wow64_process && c.io_status_block,
                        .io_status = static_cast<int32_t>(static_cast<ULONG>(status)),
                        .io_information = static_cast<uint32_t>(block.Information),
                    });
                }
            }
            return;
        }
        const auto* file_object = win_emu.process.devices.get(c.file_handle);
        if (!file_object || !file_object->completion_port.bits)
        {
            return;
        }
        constexpr ULONG skip_port_on_success = 0x1;
        if (completed_synchronously &&
            (status != STATUS_SUCCESS || (file_object->completion_notification_flags & skip_port_on_success) != 0))
        {
            return;
        }
        if (auto* port = win_emu.process.io_completions.get(file_object->completion_port))
        {
            io_completion_message message{};
            message.key_context = file_object->completion_key;
            message.apc_context = c.apc_context;
            message.io_status_block = block;
            port->enqueue(message);
        }
    }

    NTSTATUS io_device::execute_ioctl(windows_emulator& win_emu, const io_device_context& c)
    {
        const auto original_io_status = this->may_return_pending() && c.io_status_block
                                            ? std::optional{c.io_status_block.read()}
                                            : std::optional<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>{};
        if (c.io_status_block)
        {
            c.io_status_block.write({});
        }

        const auto result = this->io_control(win_emu, c);
        if (result == STATUS_PENDING)
        {
            // Native AFD leaves the caller's status block untouched until completion.
            if (original_io_status)
            {
                c.io_status_block.write(*original_io_status);
            }
            return result;
        }

        write_io_status(c.io_status_block, result);
        complete_device_ioctl(win_emu, c, result, !c.completing_pending);
        return result;
    }

    NTSTATUS io_device_container::io_control(windows_emulator& win_emu, const io_device_context& context)
    {
        this->assert_validity();
        win_emu.callbacks.on_ioctrl(*this->device_, this->device_name_, context.io_control_code);
        return this->device_->io_control(win_emu, context);
    }

    void io_device_container::work(windows_emulator& win_emu)
    {
        this->assert_validity();
        this->device_->work(win_emu);
    }

    uint32_t io_device_container::cancel_pending_io(windows_emulator& win_emu, const uint64_t io_status_block,
                                                    const uint32_t issuer_thread_id)
    {
        this->assert_validity();
        return this->device_->cancel_pending_io(win_emu, io_status_block, issuer_thread_id);
    }

    void io_device_container::rebase_steady_deadlines(const std::chrono::steady_clock::duration offset)
    {
        this->assert_validity();
        this->device_->rebase_steady_deadlines(offset);
    }

    void io_device_container::serialize_object(utils::buffer_serializer& buffer) const
    {
        this->assert_validity();

        buffer.write(this->is_32_bit_);
        buffer.write_string(this->device_name_);
        buffer.write(this->completion_port);
        buffer.write(this->completion_key);
        buffer.write(this->completion_notification_flags);
        this->device_->serialize(buffer);
    }

    void io_device_container::deserialize_object(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->is_32_bit_);
        buffer.read_string(this->device_name_);
        buffer.read(this->completion_port);
        buffer.read(this->completion_key);
        buffer.read(this->completion_notification_flags);

        this->setup();
        this->device_->deserialize(buffer);
    }

} // namespace sogen
