#include "../std_include.hpp"
#include "security_support_provider.hpp"
#include "cng_protocol.hpp"
#include "cng_resolver.hpp"
#include "../windows_emulator.hpp"

namespace sogen
{
    namespace
    {
        constexpr auto illegal_function = static_cast<NTSTATUS>(0xc00000af);

        bool clear_output(windows_emulator& emu, uint64_t address, size_t count)
        {
            const std::array<uint8_t, 4096> zeros{};
            while (count)
            {
                const auto amount = std::min(count, zeros.size());
                if (!emu.memory.try_write_memory(address, zeros.data(), amount))
                {
                    return false;
                }
                address += amount;
                count -= amount;
            }
            return true;
        }

        NTSTATUS finish_request(windows_emulator& emu, const io_device_context& context, NTSTATUS status,
                                const std::span<const uint8_t> payload = {})
        {
            std::array<uint32_t, 2> error{};
            const void* output = payload.data();
            size_t length = payload.size();
            if (status == STATUS_SUCCESS && payload.size() > context.output_buffer_length)
            {
                status = STATUS_BUFFER_TOO_SMALL;
                error[1] = static_cast<uint32_t>(payload.size());
            }
            if (static_cast<int32_t>(status) < 0)
            {
                error[0] = static_cast<uint32_t>(status);
                output = error.data();
                length = status == STATUS_BUFFER_TOO_SMALL ? 8 : 4;
                status = STATUS_BUFFER_OVERFLOW;
            }
            if (length && !emu.memory.try_write_memory(context.output_buffer, output, length))
            {
                return STATUS_ACCESS_VIOLATION;
            }
            if (!clear_output(emu, context.output_buffer + length, context.output_buffer_length - length))
            {
                return STATUS_ACCESS_VIOLATION;
            }
            if (context.io_status_block)
            {
                context.io_status_block.access([&](auto& block) { block.Information = length; });
            }
            return status;
        }

        struct security_support_provider : stateless_device
        {
            NTSTATUS io_control(windows_emulator& emu, const io_device_context& context) override
            {
                if (context.io_control_code != 0x390400)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                if (!context.output_buffer || context.output_buffer_length < 8)
                {
                    if (context.output_buffer && !clear_output(emu, context.output_buffer, context.output_buffer_length))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    return STATUS_BUFFER_TOO_SMALL;
                }
                if (!context.input_buffer || context.input_buffer_length < 8)
                {
                    return illegal_function;
                }
                std::array<uint32_t, 2> header{};
                if (!emu.memory.try_read_memory(context.input_buffer, header.data(), sizeof(header)))
                {
                    return STATUS_ACCESS_VIOLATION;
                }
                if (header[0] != cng::transport_magic)
                {
                    return illegal_function;
                }
                try
                {
                    std::vector<uint8_t> input(context.input_buffer_length);
                    if (!emu.memory.try_read_memory(context.input_buffer, input.data(), input.size()))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    switch (header[1])
                    {
                    case cng::resolve_providers: {
                        const auto request = cng::unpack_resolution(input);
                        if (!request)
                        {
                            return finish_request(emu, context, STATUS_INTERNAL_ERROR);
                        }
                        const auto result = cng::resolve(emu.registry, *request);
                        if (result.status != STATUS_SUCCESS)
                        {
                            return finish_request(emu, context, result.status);
                        }
                        const auto payload = cng::pack_provider_refs(result.providers);
                        return finish_request(emu, context, STATUS_SUCCESS, payload);
                    }
                    case cng::register_notification:
                    case cng::unregister_notification: {
                        const auto event = cng::unpack_notification(input);
                        if (!event)
                        {
                            return finish_request(emu, context, STATUS_INTERNAL_ERROR);
                        }
                        const auto status = header[1] == cng::register_notification ? emu.cng_changes.subscribe(emu, make_handle(*event))
                                                                                    : emu.cng_changes.unsubscribe(emu, make_handle(*event));
                        return finish_request(emu, context, status);
                    }
                    default:
                        return illegal_function;
                    }
                }
                catch (const std::bad_alloc&)
                {
                    return STATUS_NO_MEMORY;
                }
                catch (const std::length_error&)
                {
                    return finish_request(emu, context, STATUS_INTEGER_OVERFLOW);
                }
            }
        };
    }

    std::unique_ptr<io_device> create_security_support_provider(const device_creation_context&)
    {
        return std::make_unique<security_support_provider>();
    }
}
