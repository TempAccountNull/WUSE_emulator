#include "../std_include.hpp"
#include "network_store_interface.hpp"

#include "../windows_emulator.hpp"

namespace sogen
{
    namespace
    {
        constexpr ULONG get_parameter = 0x120007;
        constexpr ULONG get_all_parameters = 0x12000f;
        constexpr ULONG enumerate_objects = 0x12001b;
        constexpr uint64_t max_query_bytes = 16 * 1024 * 1024;

        NTSTATUS query_status(const uint32_t error)
        {
            switch (error)
            {
            case 0:
                return STATUS_SUCCESS;
            case 2:
            case 1168:
                return STATUS_NOT_FOUND;
            case 5:
                return STATUS_ACCESS_DENIED;
            case 8:
            case 14:
                return STATUS_NO_MEMORY;
            case 50:
                return STATUS_NOT_SUPPORTED;
            case 87:
                return STATUS_INVALID_PARAMETER;
            case 122:
                return STATUS_BUFFER_TOO_SMALL;
            case 234:
                return STATUS_MORE_ENTRIES;
            default:
                return STATUS_UNSUCCESSFUL;
            }
        }

        struct network_store_interface_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                network::network_store_query query{};
                switch (c.io_control_code)
                {
                case get_parameter:
                    query.operation = network::network_store_operation::parameter;
                    break;
                case get_all_parameters:
                    query.operation = network::network_store_operation::all_parameters;
                    break;
                case enumerate_objects:
                    query.operation = network::network_store_operation::enumerate;
                    break;
                default:
                    return STATUS_NOT_SUPPORTED;
                }

                const bool wow64 = win_emu.process.is_wow64_process;
                const uint32_t width = wow64 ? 4 : 8;
                const uint32_t module_offset = 2 * width;
                const uint32_t flags_offset = 4 * width;
                const uint32_t key_offset = flags_offset + 8;
                const uint32_t count_offset = key_offset + 8 * width;
                const bool single = query.operation == network::network_store_operation::parameter;
                const bool enumerate = query.operation == network::network_store_operation::enumerate;
                const uint32_t required = single ? key_offset + 4 * width + 8 : count_offset + (enumerate ? width : 0);
                if (!c.input_buffer || c.input_buffer_length < required)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto read_u32 = [&](const uint32_t offset) { return win_emu.emu().read_memory<uint32_t>(c.input_buffer + offset); };
                const auto read_pointer = [&](const uint32_t offset) -> uint64_t {
                    return wow64 ? read_u32(offset) : win_emu.emu().read_memory<uint64_t>(c.input_buffer + offset);
                };
                const auto module_address = read_pointer(module_offset);
                if (!module_address)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                win_emu.emu().read_memory(module_address, query.module.data(), query.module.size());
                uint16_t module_length{};
                uint32_t module_type{};
                std::memcpy(&module_length, query.module.data(), sizeof(module_length));
                std::memcpy(&module_type, query.module.data() + 4, sizeof(module_type));
                if (module_length != query.module.size() || module_type != 1)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                query.table = read_u32(module_offset + width);
                query.flags = read_u32(flags_offset);
                query.second_flags = read_u32(flags_offset + 4);
                if (enumerate)
                {
                    query.count = read_u32(count_offset);
                }

                std::array<uint64_t, 4> addresses{};
                uint64_t total_bytes{};
                for (size_t i = 0; i < (single ? 2U : 4U); ++i)
                {
                    const auto offset = key_offset + static_cast<uint32_t>(i) * 2 * width + (single && i == 1 ? width : 0);
                    auto& buffer = query.buffers[i];
                    addresses[i] = read_pointer(offset);
                    buffer.element_size = read_u32(offset + width);
                    const uint64_t bytes = static_cast<uint64_t>(buffer.element_size) * query.count;
                    if (bytes > max_query_bytes - total_bytes || (bytes && (!addresses[i] || addresses[i] > UINT64_MAX - bytes)))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    total_bytes += bytes;
                    buffer.bytes.resize(static_cast<size_t>(bytes));
                    if (bytes)
                    {
                        win_emu.emu().read_memory(addresses[i], buffer.bytes.data(), buffer.bytes.size());
                    }
                }
                if (single)
                {
                    query.parameter_type = read_u32(key_offset + 2 * width);
                    query.parameter_offset = read_u32(key_offset + 4 * width + 4);
                }
                const auto error = win_emu.socket_factory().query_network_store(query);
                const auto status = query_status(error);
                win_emu.log.info("NSI query: ioctl=0x%X table=%u result=%u count=%u\n", c.io_control_code, query.table, error, query.count);
                if (status == STATUS_SUCCESS || status == STATUS_MORE_ENTRIES)
                {
                    for (size_t i = enumerate ? 0 : 1; i < query.buffers.size(); ++i)
                    {
                        const auto& bytes = query.buffers[i].bytes;
                        if (!bytes.empty())
                        {
                            win_emu.emu().write_memory(addresses[i], bytes.data(), bytes.size());
                        }
                    }
                }
                if (enumerate)
                {
                    win_emu.emu().write_memory<uint32_t>(c.input_buffer + count_offset, query.count);
                }
                if (c.io_status_block)
                {
                    IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                    block.Status = status;
                    c.io_status_block.write(block);
                }
                return status;
            }
        };
    }

    std::unique_ptr<io_device> create_network_store_interface(const device_creation_context&)
    {
        return std::make_unique<network_store_interface_device>();
    }
}
