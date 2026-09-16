#pragma once

#include "../io_device.hpp"
#include <new>
#include <span>

namespace sogen
{
    inline NTSTATUS deliver_native_wsi_output(memory_interface& memory, const io_device_context& context,
                                              const std::span<const std::byte> bytes)
    {
        try
        {
            NTSTATUS status = STATUS_SUCCESS;
            if (bytes.size() > context.output_buffer_length)
            {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            else if (!memory.try_write_memory(context.output_buffer, bytes.data(), bytes.size()))
            {
                status = STATUS_ACCESS_VIOLATION;
            }

            if (context.io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Status = status;
                block.Information = status == STATUS_SUCCESS ? bytes.size() : 0;
                if (!memory.try_write_memory(context.io_status_block.value(), &block, sizeof(block)))
                {
                    return STATUS_ACCESS_VIOLATION;
                }
            }
            return status;
        }
        catch (const std::bad_alloc&)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        catch (...)
        {
            return STATUS_UNSUCCESSFUL;
        }
    }
}
