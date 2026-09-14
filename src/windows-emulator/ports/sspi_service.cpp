#include "../std_include.hpp"
#include "sspi_service.hpp"

#include "binary_writer.hpp"
#include "../registry/registry_utils.hpp"
#include "../windows_emulator.hpp"

namespace sogen
{
    namespace
    {
        constexpr std::array<uint8_t, 16> sspi_interface = {0xc8, 0xad, 0x32, 0x4f, 0x52, 0x60, 0x04, 0x4a,
                                                            0x87, 0x01, 0x29, 0x3c, 0xcf, 0x20, 0x96, 0xf0};
        constexpr std::array<uint8_t, 8> context_prefix = {0x53, 0x4f, 0x47, 0x45, 0x4e, 0x53, 0x53, 0x50};
        constexpr uint32_t sam_compatible_name = 2;

        struct sspi_service_port final : rpc_port
        {
            uint64_t next_context{1};
            std::vector<uint64_t> contexts;

            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                rpc_port::serialize_object(buffer);
                buffer.write(next_context);
                buffer.write(contexts);
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                rpc_port::deserialize_object(buffer);
                buffer.read(next_context);
                buffer.read(contexts);
            }

            static void write_context(utils::aligned_binary_writer& writer, const uint64_t id)
            {
                writer.write<uint32_t>(0);
                if (!id)
                {
                    writer.pad(16);
                    return;
                }
                writer.write(context_prefix.data(), context_prefix.size());
                writer.write(&id, sizeof(id));
            }

            std::optional<uint64_t> read_context(windows_emulator& win_emu, const lpc_request_context& c) const
            {
                if (c.send_buffer_length < 20)
                {
                    return std::nullopt;
                }
                std::array<uint8_t, 20> wire{};
                win_emu.memory.read_memory(c.send_buffer, wire.data(), wire.size());
                uint32_t attributes{};
                uint64_t id{};
                std::memcpy(&attributes, wire.data(), sizeof(attributes));
                std::memcpy(&id, wire.data() + 12, sizeof(id));
                if (attributes || !std::equal(context_prefix.begin(), context_prefix.end(), wire.begin() + 4) ||
                    std::ranges::find(contexts, id) == contexts.end())
                {
                    return std::nullopt;
                }
                return id;
            }

            NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>&) override
            {
                if (bound_interface() != sspi_interface || win_emu.process.is_wow64_process)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                win_emu.log.print(color::gray, "SSPI RPC: procedure %u, request %u bytes\n", procedure, c.send_buffer_length);
                switch (procedure)
                {
                case 0: {
                    if (c.send_buffer_length < 12)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    const auto name_pointer = win_emu.emu().read_memory<uint64_t>(c.send_buffer);
                    const auto mode = win_emu.emu().read_memory<uint32_t>(c.send_buffer + 8);
                    if (name_pointer || mode != 2)
                    {
                        return STATUS_NOT_SUPPORTED;
                    }
                    const auto id = next_context++;
                    contexts.push_back(id);
                    writer.write<uint32_t>(0);
                    writer.write<uint32_t>(0);
                    write_context(writer, id);
                    writer.write(STATUS_SUCCESS);
                    return STATUS_SUCCESS;
                }
                case 1: {
                    const auto id = read_context(win_emu, c);
                    if (!id)
                    {
                        return STATUS_INVALID_HANDLE;
                    }
                    std::erase(contexts, *id);
                    write_context(writer, 0);
                    writer.write(STATUS_SUCCESS);
                    return STATUS_SUCCESS;
                }
                case 14: {
                    if (c.send_buffer_length < 44 || !read_context(win_emu, c))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    const auto process_id = win_emu.emu().read_memory<uint64_t>(c.send_buffer + 24);
                    const auto format = win_emu.emu().read_memory<uint32_t>(c.send_buffer + 40);
                    if (process_id != process_context::process_id || format != sam_compatible_name)
                    {
                        return STATUS_NOT_SUPPORTED;
                    }
                    const auto name =
                        registry_utils::get_account_domain(win_emu.registry) + u"\\" + registry_utils::get_user_name(win_emu.registry);
                    if (name.size() > 32766)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    writer.align_to(8);
                    writer.write(static_cast<uint16_t>(name.size() * sizeof(char16_t)));
                    writer.write(static_cast<uint16_t>((name.size() + 1) * sizeof(char16_t)));
                    writer.write_ndr_pointer(true);
                    writer.write_ndr_u16string(name, false);
                    writer.write<uint32_t>(0);
                    writer.write(STATUS_SUCCESS);
                    return STATUS_SUCCESS;
                }
                default:
                    return STATUS_NOT_SUPPORTED;
                }
            }
        };
    }

    std::unique_ptr<port> create_sspi_service_port()
    {
        return std::make_unique<sspi_service_port>();
    }
}
