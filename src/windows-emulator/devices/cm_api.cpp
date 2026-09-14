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

        struct interface_list_request
        {
            uint32_t size;
            uint32_t flags;
            GUID interface_class;
            uint64_t device_id;
            uint32_t device_id_bytes;
            uint32_t result_size;
        };

        static_assert(sizeof(interface_list_request) == 40);

        std::u16string guid_name(const GUID& guid)
        {
            return u8_to_u16(utils::string::va("{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", guid.Data1, guid.Data2, guid.Data3,
                                               guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
                                               guid.Data4[6], guid.Data4[7]));
        }

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

            NTSTATUS read_interface_request(windows_emulator& win_emu, const io_device_context& c, interface_list_request& request) const
            {
                const auto required = this->is_32_bit ? 36u : 40u;
                if (!c.input_buffer || c.input_buffer_length < required)
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
                    request.size = 40;
                    request.flags = input[1];
                    memcpy(&request.interface_class, input.data() + 2, sizeof(GUID));
                    request.device_id = input[6];
                    request.device_id_bytes = input[7];
                    request.result_size = input[8];
                }
                else if (!win_emu.memory.try_read_memory(c.input_buffer, &request, sizeof(request)))
                {
                    return STATUS_ACCESS_VIOLATION;
                }
                return request.size == 40 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
            }

            static std::u16string registered_interfaces(windows_emulator& win_emu, const interface_list_request& request,
                                                        const std::u16string& device_id)
            {
                std::u16string list;
                // Enabled interfaces are PnP runtime objects, not the devices saved in an offline SYSTEM hive.
                // Sogen currently has no interface activation path; only ALL_DEVICES includes saved registrations.
                if (request.flags == 0)
                {
                    const auto path =
                        uR"(\Registry\Machine\System\CurrentControlSet\Control\DeviceClasses\)" + guid_name(request.interface_class);
                    const auto interface_class = win_emu.registry.get_key({path});
                    if (interface_class)
                    {
                        const auto count = win_emu.registry.get_sub_key_count(*interface_class);
                        for (size_t index = 0; index < count; ++index)
                        {
                            const auto name = win_emu.registry.get_sub_key_name(*interface_class, index);
                            if (!name || !name->starts_with("##?#"))
                            {
                                continue;
                            }
                            const auto instance_name = u8_to_u16(*name);
                            auto instance_path = path;
                            instance_path += u'\\';
                            instance_path += instance_name;
                            const auto instance = win_emu.registry.get_key({instance_path});
                            if (!instance)
                            {
                                continue;
                            }
                            if (!device_id.empty())
                            {
                                const auto value = win_emu.registry.get_value(*instance, "DeviceInstance");
                                const auto id = value ? value->as_string() : std::nullopt;
                                if (!id || utils::string::to_lower(*id) != utils::string::to_lower(device_id))
                                {
                                    continue;
                                }
                            }
                            const auto references = win_emu.registry.get_sub_key_count(*instance);
                            for (size_t reference = 0; reference < references; ++reference)
                            {
                                const auto ref = win_emu.registry.get_sub_key_name(*instance, reference);
                                if (!ref || !ref->starts_with('#'))
                                {
                                    continue;
                                }
                                auto symbolic_link = u"\\\\?\\" + instance_name.substr(4);
                                if (ref->size() > 1)
                                {
                                    symbolic_link += u'\\';
                                    symbolic_link += u8_to_u16(ref->substr(1));
                                }
                                list += symbolic_link;
                                list += u'\0';
                            }
                        }
                    }
                }
                list += u'\0';
                return list;
            }

            NTSTATUS get_interface_list(windows_emulator& win_emu, const io_device_context& c) const
            {
                interface_list_request request{};
                auto status = this->read_interface_request(win_emu, c, request);
                if (status != STATUS_SUCCESS)
                {
                    return status;
                }
                std::u16string device_id;
                if (request.device_id)
                {
                    if (request.device_id_bytes < 2 || request.device_id_bytes > 0xfffe || request.device_id_bytes % 2)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    if (request.device_id % 2)
                    {
                        return STATUS_DATATYPE_MISALIGNMENT;
                    }
                    device_id.resize(request.device_id_bytes / 2);
                    if (!win_emu.memory.try_read_memory(request.device_id, device_id.data(), request.device_id_bytes))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    device_id.back() = u'\0';
                    device_id.resize(device_id.find(u'\0'));
                }
                else if (request.device_id_bytes)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (!c.output_buffer || c.output_buffer_length < 20 || request.result_size != 20)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (c.output_buffer % 4)
                {
                    return STATUS_DATATYPE_MISALIGNMENT;
                }
                std::u16string list;
                if (request.flags & ~0x10000u)
                {
                    status = STATUS_INVALID_PARAMETER;
                }
                else
                {
                    list = registered_interfaces(win_emu, request, device_id);
                }
                const auto bytes = list.size() * sizeof(char16_t);
                if (bytes > UINT32_MAX - 20)
                {
                    status = STATUS_INSUFFICIENT_RESOURCES;
                    list.clear();
                }
                const auto required = static_cast<uint32_t>(list.size() * sizeof(char16_t));
                if (status == STATUS_SUCCESS && c.output_buffer_length - 20 < required)
                {
                    status = STATUS_BUFFER_TOO_SMALL;
                }
                const auto copied = status == STATUS_SUCCESS ? required : 0u;
                std::vector<std::byte> result(std::max(20u, 16u + copied));
                const std::array<uint32_t, 4> header{20, static_cast<uint32_t>(status), required, 0};
                memcpy(result.data(), header.data(), sizeof(header));
                if (copied)
                {
                    // PiCMReturnBufferResultData starts payload at 16 but includes 20 header bytes in Information.
                    memcpy(result.data() + 16, list.data(), copied);
                }
                if (!win_emu.memory.try_write_memory(c.output_buffer, result.data(), result.size()))
                {
                    return STATUS_ACCESS_VIOLATION;
                }
                if (c.io_status_block)
                {
                    c.io_status_block.access([&](auto& block) { block.Information = 20u + copied; });
                }
                win_emu.log.info("CMApi interface list %s (%s): %u bytes, status 0x%08X\n",
                                 u16_to_u8(guid_name(request.interface_class)).c_str(), request.flags == 0 ? "all registered" : "active",
                                 required, static_cast<uint32_t>(status));
                return STATUS_SUCCESS;
            }

            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                if (c.io_control_code == 0x470807)
                {
                    return get_interface_list(win_emu, c);
                }
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
