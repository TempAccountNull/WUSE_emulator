#include "../std_include.hpp"
#include "cm_api.hpp"
#include "../windows_emulator.hpp"

namespace sogen
{
    namespace
    {
        struct registry_request
        {
            uint32_t size;
            uint32_t operation;
            uint32_t key_type;
            uint32_t padding;
            uint64_t name;
            uint32_t name_bytes;
            uint32_t desired_access;
            uint32_t disposition;
            uint32_t flags;
            uint32_t result_size;
            uint32_t reserved;
        };

        struct handle_result
        {
            uint32_t size{16};
            NTSTATUS status{};
            uint64_t key{};
        };

        static_assert(sizeof(registry_request) == 48);
        static_assert(sizeof(handle_result) == 16);

        bool is_class_guid(const std::u16string_view value)
        {
            if (value.size() != 38 || value.front() != u'{' || value.back() != u'}')
            {
                return false;
            }
            for (size_t i = 1; i < 37; ++i)
            {
                if (i == 9 || i == 14 || i == 19 || i == 24)
                {
                    if (value[i] != u'-')
                    {
                        return false;
                    }
                }
                else if (!((value[i] >= u'0' && value[i] <= u'9') || (value[i] >= u'a' && value[i] <= u'f') ||
                           (value[i] >= u'A' && value[i] <= u'F')))
                {
                    return false;
                }
            }
            return true;
        }

        struct cm_api : stateless_device
        {
            bool is_32_bit;

            explicit cm_api(const bool wow64)
                : is_32_bit(wow64)
            {
            }

            NTSTATUS read_request(windows_emulator& win_emu, const io_device_context& c, registry_request& request) const
            {
                const auto required = this->is_32_bit ? 36u : 48u;
                if (c.input_buffer == 0 || c.input_buffer_length < required)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (c.input_buffer % (this->is_32_bit ? 4 : 8) != 0)
                {
                    return STATUS_DATATYPE_MISALIGNMENT;
                }
                if (this->is_32_bit)
                {
                    std::array<uint32_t, 9> input{};
                    if (!win_emu.memory.try_read_memory(c.input_buffer, input.data(), sizeof(input)))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    if (input[0] != required)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    request = {.size = 48,
                               .operation = input[1],
                               .key_type = input[2],
                               .padding = 0,
                               .name = input[3],
                               .name_bytes = input[4],
                               .desired_access = input[5],
                               .disposition = input[6],
                               .flags = input[7],
                               .result_size = input[8],
                               .reserved = 0};
                }
                else if (!win_emu.memory.try_read_memory(c.input_buffer, &request, sizeof(request)))
                {
                    return STATUS_ACCESS_VIOLATION;
                }
                return request.size == 48 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
            }

            static NTSTATUS open_class_key(windows_emulator& win_emu, const registry_request& request, uint64_t& key_handle)
            {
                if (request.operation != 0 || request.flags != 0 || (request.key_type != 2 && request.key_type != 3))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                std::u16string path = uR"(\Registry\Machine\System\CurrentControlSet\Control\)";
                path += request.key_type == 3 ? u"DeviceClasses" : u"Class";
                if (request.name)
                {
                    if (request.name_bytes != 78 || request.name % 2 != 0)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    std::array<char16_t, 39> name{};
                    if (!win_emu.memory.try_read_memory(request.name, name.data(), sizeof(name)))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    const std::u16string_view guid{name.data(), 38};
                    if (!is_class_guid(guid))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    path += u'\\';
                    path += guid;
                }
                else if (request.name_bytes != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                win_emu.callbacks.on_generic_access("Registry key", path);
                auto key = win_emu.registry.get_key({path});
                if (!key && request.name && request.disposition == 1)
                {
                    key = win_emu.registry.create_key({path});
                }
                if (!key)
                {
                    return STATUS_OBJECT_NAME_NOT_FOUND;
                }
                const auto opened = win_emu.process.registry_keys.store(std::move(*key));
                key_handle = opened.bits;
                win_emu.log.info("CMApi class key %s -> 0x%" PRIx64 " (access: 0x%X)\n", u16_to_u8(path).c_str(), key_handle,
                                 request.desired_access);
                return STATUS_SUCCESS;
            }

            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                if (c.io_control_code != 0x470863)
                {
                    win_emu.log.warn("Unsupported CMApi ioctl: 0x%X\n", c.io_control_code);
                    return STATUS_NOT_SUPPORTED;
                }
                registry_request request{};
                const auto status = this->read_request(win_emu, c, request);
                if (status != STATUS_SUCCESS)
                {
                    return status;
                }
                if (c.output_buffer == 0 || c.output_buffer_length < sizeof(handle_result) || request.result_size != sizeof(handle_result))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (c.output_buffer % 4 != 0)
                {
                    return STATUS_DATATYPE_MISALIGNMENT;
                }

                // PiCMReturnHandleResultData returns transport success with the operation's NTSTATUS in the output.
                handle_result result{};
                result.status = open_class_key(win_emu, request, result.key);
                if (!win_emu.memory.try_write_memory(c.output_buffer, &result, sizeof(result)))
                {
                    if (result.key)
                    {
                        win_emu.process.registry_keys.erase(result.key);
                    }
                    return STATUS_ACCESS_VIOLATION;
                }
                if (c.io_status_block)
                {
                    c.io_status_block.access([](auto& block) { block.Information = sizeof(handle_result); });
                }
                return STATUS_SUCCESS;
            }
        };
    }

    std::unique_ptr<io_device> create_cm_api(const device_creation_context& context)
    {
        return std::make_unique<cm_api>(context.is_32_bit);
    }
}
