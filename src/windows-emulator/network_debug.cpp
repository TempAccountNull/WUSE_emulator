#include "network_debug.hpp"

#include "windows_emulator.hpp"
#include "io_device.hpp"
#include "syscall_utils.hpp"
#include "devices/afd_types.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr size_t preview_limit = 64;
        constexpr size_t descriptor_limit = 256;
        constexpr size_t active_request_limit = 4096;
        constexpr uint64_t output_limit = 64ull * 1024 * 1024;

        std::string hex(const uint64_t value)
        {
            std::array<char, 32> buffer{};
            const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
            return "0x" + std::string(buffer.data(), result.ptr);
        }

        std::string hex_bytes(const std::span<const std::byte> bytes)
        {
            static constexpr char digits[] = "0123456789abcdef";
            std::string result;
            result.reserve(bytes.size() * 2);
            for (const auto value : bytes)
            {
                const auto byte = static_cast<unsigned>(value);
                result.push_back(digits[byte >> 4]);
                result.push_back(digits[byte & 0xf]);
            }
            return result;
        }

        void append_json_string(std::string& output, const std::string_view value)
        {
            output.push_back('"');
            for (const unsigned char ch : value)
            {
                switch (ch)
                {
                case '"':
                    output += "\\\"";
                    break;
                case '\\':
                    output += "\\\\";
                    break;
                case '\n':
                    output += "\\n";
                    break;
                case '\r':
                    output += "\\r";
                    break;
                case '\t':
                    output += "\\t";
                    break;
                default:
                    if (ch < 0x20)
                    {
                        output += "\\u00";
                        static constexpr char digits[] = "0123456789abcdef";
                        output.push_back(digits[ch >> 4]);
                        output.push_back(digits[ch & 0xf]);
                    }
                    else
                    {
                        output.push_back(static_cast<char>(ch));
                    }
                    break;
                }
            }
            output.push_back('"');
        }

        struct json_fields
        {
            std::string text{"{"};
            bool first{true};

            void key(const std::string_view name)
            {
                if (!first)
                {
                    text.push_back(',');
                }
                first = false;
                append_json_string(text, name);
                text.push_back(':');
            }

            void string(const std::string_view name, const std::string_view value)
            {
                key(name);
                append_json_string(text, value);
            }

            void number(const std::string_view name, const uint64_t value)
            {
                key(name);
                text += std::to_string(value);
            }

            void boolean(const std::string_view name, const bool value)
            {
                key(name);
                text += value ? "true" : "false";
            }

            void raw(const std::string_view name, const std::string_view value)
            {
                key(name);
                text += value;
            }

            std::string finish()
            {
                text.push_back('}');
                return std::move(text);
            }
        };

        template <typename Traits>
        void capture_transport_ioctl_input(json_fields& event, windows_emulator& win_emu, const io_device_context& request)
        {
            using wrapper_type = AFD_WINSOCK_TRANSPORT_IOCTL<Traits>;
            if (request.input_buffer_length < sizeof(wrapper_type))
            {
                event.string("transport_wrapper_unavailable", "input_too_short");
                event.string("transport_input_preview_unavailable", "wrapper_unavailable");
                return;
            }
            wrapper_type wrapper{};
            if (!request.input_buffer || !win_emu.memory.try_read_memory(request.input_buffer, &wrapper, sizeof(wrapper)))
            {
                event.string("transport_wrapper_unavailable", "input_buffer_unreadable");
                event.string("transport_input_preview_unavailable", "wrapper_unavailable");
                return;
            }

            event.number("transport_type", wrapper.Type);
            event.string("transport_control_code", hex(wrapper.ControlCode));
            event.number("transport_input_length", static_cast<uint64_t>(wrapper.InputBufferLength));
            const size_t count = std::min<size_t>(wrapper.InputBufferLength, preview_limit);
            event.boolean("transport_input_preview_truncated", wrapper.InputBufferLength > count);
            if (!wrapper.InputBufferLength)
            {
                event.string("transport_input_preview_unavailable", "empty_input");
                return;
            }
            if (!wrapper.InputBuffer)
            {
                event.string("transport_input_preview_unavailable", "null_input_buffer");
                return;
            }
            std::array<std::byte, preview_limit> bytes{};
            if (!win_emu.memory.try_read_memory(wrapper.InputBuffer, bytes.data(), count))
            {
                event.string("transport_input_preview_unavailable", "nested_input_unreadable");
                return;
            }
            event.string("transport_input_preview_hex", hex_bytes(std::span{bytes.data(), count}));
            event.string("transport_input_preview_source", "guest_transport_input_at_request");
        }

        struct guest_buffer
        {
            uint64_t address{};
            uint32_t length{};
        };

        struct request_metadata
        {
            std::string syscall;
            std::string operation;
            std::string origin;
            std::optional<uint64_t> requested_bytes;
            std::vector<guest_buffer> buffers;
            bool receive{};
        };

        std::string afd_operation(const ULONG ioctl)
        {
            switch (_AFD_REQUEST(ioctl))
            {
            case AFD_BIND:
                return "bind";
            case AFD_CONNECT:
                return "connect";
            case AFD_START_LISTEN:
                return "start_listen";
            case AFD_WAIT_FOR_LISTEN:
                return "wait_for_listen";
            case AFD_ACCEPT:
                return "accept";
            case AFD_RECEIVE:
                return "receive";
            case AFD_RECEIVE_DATAGRAM:
                return "receive_datagram";
            case AFD_SEND:
                return "send";
            case AFD_SEND_DATAGRAM:
                return "send_datagram";
            case AFD_POLL:
                return "poll";
            case AFD_PARTIAL_DISCONNECT:
                return "partial_disconnect";
            case AFD_GET_ADDRESS:
                return "get_address";
            case AFD_QUERY_RECEIVE_INFO:
                return "query_receive_info";
            case AFD_QUERY_HANDLES:
                return "query_handles";
            case AFD_SET_INFORMATION:
                return "set_information";
            case AFD_GET_REMOTE_ADDRESS:
                return "get_remote_address";
            case AFD_GET_CONTEXT:
                return "get_context";
            case AFD_SET_CONTEXT:
                return "set_context";
            case AFD_SET_CONNECT_DATA:
                return "set_connect_data";
            case AFD_SET_CONNECT_OPTIONS:
                return "set_connect_options";
            case AFD_SET_DISCONNECT_DATA:
                return "set_disconnect_data";
            case AFD_SET_DISCONNECT_OPTIONS:
                return "set_disconnect_options";
            case AFD_GET_CONNECT_DATA:
                return "get_connect_data";
            case AFD_GET_CONNECT_OPTIONS:
                return "get_connect_options";
            case AFD_GET_DISCONNECT_DATA:
                return "get_disconnect_data";
            case AFD_GET_DISCONNECT_OPTIONS:
                return "get_disconnect_options";
            case AFD_SIZE_CONNECT_DATA:
                return "size_connect_data";
            case AFD_SIZE_CONNECT_OPTIONS:
                return "size_connect_options";
            case AFD_SIZE_DISCONNECT_DATA:
                return "size_disconnect_data";
            case AFD_SIZE_DISCONNECT_OPTIONS:
                return "size_disconnect_options";
            case AFD_GET_INFORMATION:
                return "get_information";
            case AFD_TRANSMIT_FILE:
                return "transmit_file";
            case AFD_SUPER_ACCEPT:
                return "super_accept";
            case AFD_EVENT_SELECT:
                return "event_select";
            case AFD_ENUM_NETWORK_EVENTS:
                return "enum_network_events";
            case AFD_DEFER_ACCEPT:
                return "defer_accept";
            case AFD_WAIT_FOR_LISTEN_LIFO:
                return "wait_for_listen_lifo";
            case AFD_SET_QOS:
                return "set_qos";
            case AFD_GET_QOS:
                return "get_qos";
            case AFD_NO_OPERATION:
                return "no_operation";
            case AFD_VALIDATE_GROUP:
                return "validate_group";
            case AFD_GET_UNACCEPTED_CONNECT_DATA:
                return "get_unaccepted_connect_data";
            case AFD_ROUTING_INTERFACE_QUERY:
                return "routing_interface_query";
            case AFD_ROUTING_INTERFACE_CHANGE:
                return "routing_interface_change";
            case AFD_ADDRESS_LIST_QUERY:
                return "address_list_query";
            case AFD_ADDRESS_LIST_CHANGE:
                return "address_list_change";
            case AFD_JOIN_LEAF:
                return "join_leaf";
            case AFD_TRANSPORT_IOCTL:
                return "transport_ioctl";
            default:
                return "afd_request_" + std::to_string(_AFD_REQUEST(ioctl));
            }
        }

        std::string capture_origin(windows_emulator& win_emu, const syscall_context& issuer)
        {
            json_fields origin;
            origin.string("source", "syscall_stack_return_address");
            const auto rip = issuer.emu.reg(x86_register::rip);
            const auto rsp = issuer.emu.reg(x86_register::rsp);
            origin.string("syscall_rip", hex(rip));
            origin.string("stack_pointer", hex(rsp));

            json_fields registers;
            for (const auto& [name, reg] : std::array<std::pair<std::string_view, x86_register>, 8>{{{"rip", x86_register::rip},
                                                                                                     {"rsp", x86_register::rsp},
                                                                                                     {"rbp", x86_register::rbp},
                                                                                                     {"rax", x86_register::rax},
                                                                                                     {"rcx", x86_register::rcx},
                                                                                                     {"rdx", x86_register::rdx},
                                                                                                     {"r8", x86_register::r8},
                                                                                                     {"r9", x86_register::r9}}})
            {
                registers.string(name, hex(issuer.emu.reg(reg)));
            }
            origin.raw("registers", registers.finish());

            std::array<uint64_t, 8> stack{};
            std::array<bool, 8> valid{};
            for (size_t i = 0; i < stack.size(); ++i)
            {
                valid[i] = rsp <= std::numeric_limits<uint64_t>::max() - i * sizeof(uint64_t) &&
                           win_emu.memory.try_read_memory(rsp + i * sizeof(uint64_t), &stack[i], sizeof(stack[i]));
            }
            std::string words{"["};
            for (size_t i = 0; i < stack.size(); ++i)
            {
                if (i)
                {
                    words.push_back(',');
                }
                if (valid[i])
                {
                    append_json_string(words, hex(stack[i]));
                }
                else
                {
                    words += "null";
                }
            }
            words.push_back(']');
            origin.raw("stack", words);
            origin.raw("stack_words", words);
            origin.string("stack_kind", "raw_qwords_not_unwound");

            if (!valid[0])
            {
                origin.string("unavailable", "guest_return_address_unreadable");
            }
            else
            {
                origin.string("address", hex(stack[0]));
                if (const auto* module = win_emu.mod_manager.find_by_address(stack[0]))
                {
                    origin.string("module", std::string_view{module->name}.substr(0, 256));
                    origin.string("rva", hex(stack[0] - module->image_base));
                }
                else
                {
                    origin.string("unavailable", "guest_return_address_unmapped");
                }
            }
            return origin.finish();
        }

        template <typename Traits>
        void capture_descriptors(windows_emulator& win_emu, const io_device_context& request, request_metadata& metadata)
        {
            uint64_t array{};
            ULONG count{};
            switch (_AFD_REQUEST(request.io_control_code))
            {
            case AFD_SEND: {
                AFD_SEND_INFO<Traits> info{};
                if (request.input_buffer_length < sizeof(info) ||
                    !win_emu.memory.try_read_memory(request.input_buffer, &info, sizeof(info)))
                {
                    return;
                }
                array = info.BufferArray;
                count = info.BufferCount;
                break;
            }
            case AFD_RECEIVE: {
                AFD_RECV_INFO<Traits> info{};
                if (request.input_buffer_length < sizeof(info) ||
                    !win_emu.memory.try_read_memory(request.input_buffer, &info, sizeof(info)))
                {
                    return;
                }
                array = info.BufferArray;
                count = info.BufferCount;
                metadata.receive = true;
                break;
            }
            case AFD_SEND_DATAGRAM: {
                AFD_SEND_DATAGRAM_INFO<Traits> info{};
                if (request.input_buffer_length < sizeof(info) ||
                    !win_emu.memory.try_read_memory(request.input_buffer, &info, sizeof(info)))
                {
                    return;
                }
                array = info.BufferArray;
                count = info.BufferCount;
                break;
            }
            case AFD_RECEIVE_DATAGRAM: {
                AFD_RECV_DATAGRAM_INFO<Traits> info{};
                if (request.input_buffer_length < sizeof(info) ||
                    !win_emu.memory.try_read_memory(request.input_buffer, &info, sizeof(info)))
                {
                    return;
                }
                array = info.BufferArray;
                count = info.BufferCount;
                metadata.receive = true;
                break;
            }
            default:
                return;
            }
            if (!array || !count || count > descriptor_limit ||
                array > std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(count) * sizeof(EMU_WSABUF<Traits>))
            {
                return;
            }
            std::vector<EMU_WSABUF<Traits>> descriptors(count);
            if (!win_emu.memory.try_read_memory(array, descriptors.data(), descriptors.size() * sizeof(descriptors.front())))
            {
                return;
            }
            uint64_t total = 0;
            for (const auto& buffer : descriptors)
            {
                total += buffer.len;
                metadata.buffers.push_back({static_cast<uint64_t>(buffer.buf), buffer.len});
            }
            metadata.requested_bytes = total;
        }

        std::optional<std::string> capture_preview(windows_emulator& win_emu, const request_metadata& metadata, const uint64_t count)
        {
            std::array<std::byte, preview_limit> bytes{};
            size_t copied = 0;
            uint64_t remaining = count;
            for (const auto& buffer : metadata.buffers)
            {
                if (copied == bytes.size() || remaining == 0)
                {
                    break;
                }
                const auto n = static_cast<size_t>(std::min<uint64_t>({buffer.length, bytes.size() - copied, remaining}));
                if (n && (!buffer.address || !win_emu.memory.try_read_memory(buffer.address, bytes.data() + copied, n)))
                {
                    return {};
                }
                copied += n;
                remaining -= n;
            }
            if (remaining && copied < std::min<uint64_t>(count, preview_limit))
            {
                return {};
            }
            return hex_bytes(std::span{bytes.data(), copied});
        }

        void add_common(json_fields& record, const std::string_view channel, const std::string_view phase, const uint64_t request_id)
        {
            record.number("schema", 1);
            record.string("type", "network_io");
            record.string("channel", channel);
            record.string("phase", phase);
            record.number("request_id", request_id);
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            record.number("time_ms", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()));
        }
    }

    struct network_debug_logger::implementation
    {
        explicit implementation(const std::filesystem::path& path)
            : file(path, std::ios::binary | std::ios::out | std::ios::trunc)
        {
            if (!file)
            {
                throw std::runtime_error("Failed to open network debug file: " + path.string());
            }
            const auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            next_id = static_cast<uint64_t>(seconds) * 1'000'000;
        }

        std::mutex mutex;
        std::ofstream file;
        uint64_t next_id{};
        uint64_t bytes_written{};
        uint32_t records_since_flush{};
        bool limit_reported{};
        std::unordered_map<uint64_t, request_metadata> active;
        std::unordered_map<uint64_t, std::string> recent_origins;

        void origin_for(json_fields& event, const uint64_t request_id)
        {
            if (const auto it = active.find(request_id); it != active.end())
            {
                event.raw("origin", it->second.origin);
            }
            else if (const auto recent = recent_origins.find(request_id); recent != recent_origins.end())
            {
                event.raw("origin", recent->second);
            }
            else
            {
                event.raw("origin", "{\"unavailable\":\"origin_not_captured_or_restored\"}");
            }
        }

        void write(std::string line)
        {
            // Reserve room for one complete, flushed truncation marker inside the cap.
            if (bytes_written + line.size() + 1 > output_limit - 256)
            {
                if (!limit_reported)
                {
                    file << "{\"type\":\"network_diagnostic\",\"kind\":\"output_limit_reached\",\"limit_bytes\":" << output_limit << "}\n";
                    file.flush();
                    limit_reported = true;
                }
                return;
            }
            file << line << '\n';
            bytes_written += line.size() + 1;
            if (++records_since_flush >= 32)
            {
                file.flush();
                records_since_flush = 0;
            }
        }
    };

    network_debug_logger::network_debug_logger() = default;
    network_debug_logger::~network_debug_logger() = default;

    void network_debug_logger::open(const std::filesystem::path& path)
    {
        if (!path.empty())
        {
            impl_ = std::make_unique<implementation>(path);
        }
    }

    bool network_debug_logger::enabled() const noexcept
    {
        return impl_ != nullptr;
    }

    void network_debug_logger::begin_afd_request(windows_emulator& win_emu, io_device_context& request, const syscall_context& issuer,
                                                 const std::string_view syscall_name) noexcept
    {
        if (!impl_ || _AFD_BASE(request.io_control_code) != FSCTL_AFD_BASE)
        {
            return;
        }
        try
        {
            request_metadata metadata;
            metadata.syscall = syscall_name;
            metadata.operation = afd_operation(request.io_control_code);
            metadata.origin = capture_origin(win_emu, issuer);
            if (win_emu.process.is_wow64_process)
            {
                capture_descriptors<EmulatorTraits<Emu32>>(win_emu, request, metadata);
            }
            else
            {
                capture_descriptors<EmulatorTraits<Emu64>>(win_emu, request, metadata);
            }

            std::scoped_lock lock(impl_->mutex);
            request.network_request_id = ++impl_->next_id;
            json_fields event;
            add_common(event, "afd", "request", request.network_request_id);
            event.string("kind", "afd_request");
            event.number("thread_id", request.issuer_thread_id);
            event.string("file_handle", hex(request.file_handle.bits));
            event.string("ioctl", hex(request.io_control_code));
            event.string("operation", metadata.operation);
            event.string("syscall", metadata.syscall);
            event.number("input_length", request.input_buffer_length);
            event.number("output_length", request.output_buffer_length);
            if (metadata.requested_bytes)
            {
                event.number("requested_bytes", *metadata.requested_bytes);
            }
            else
            {
                event.string("requested_bytes_unavailable", "no_valid_payload_descriptors_or_non_transfer_operation");
            }
            if (metadata.requested_bytes && !metadata.receive)
            {
                if (const auto preview = capture_preview(win_emu, metadata, *metadata.requested_bytes))
                {
                    event.string("preview_hex", *preview);
                    event.boolean("preview_truncated", *metadata.requested_bytes > preview_limit);
                    event.string("preview_source", "guest_send_buffer_at_request");
                }
                else
                {
                    event.string("preview_unavailable", "guest_send_buffer_unreadable");
                }
            }
            else
            {
                event.string("preview_unavailable", metadata.receive ? "receive_not_completed" : "no_payload");
            }
            const auto operation = _AFD_REQUEST(request.io_control_code);
            const bool transfer =
                operation == AFD_SEND || operation == AFD_SEND_DATAGRAM || operation == AFD_RECEIVE || operation == AFD_RECEIVE_DATAGRAM;
            if (!transfer)
            {
                if (!request.input_buffer_length)
                {
                    event.string("control_input_preview_unavailable", "empty_input");
                }
                else
                {
                    std::array<std::byte, preview_limit> control{};
                    const size_t count = std::min<size_t>(request.input_buffer_length, control.size());
                    if (request.input_buffer && win_emu.memory.try_read_memory(request.input_buffer, control.data(), count))
                    {
                        event.string("control_input_preview_hex", hex_bytes(std::span{control.data(), count}));
                        event.boolean("control_input_preview_truncated", request.input_buffer_length > count);
                        event.string("control_input_preview_source", "guest_ioctl_input_at_request");
                    }
                    else
                    {
                        event.string("control_input_preview_unavailable", "input_buffer_unreadable");
                    }
                }
            }
            if (operation == AFD_TRANSPORT_IOCTL)
            {
                if (win_emu.process.is_wow64_process)
                {
                    capture_transport_ioctl_input<EmulatorTraits<Emu32>>(event, win_emu, request);
                }
                else
                {
                    capture_transport_ioctl_input<EmulatorTraits<Emu64>>(event, win_emu, request);
                }
            }
            event.raw("origin", metadata.origin);
            impl_->write(event.finish());
            if (impl_->active.size() < active_request_limit)
            {
                impl_->active.emplace(request.network_request_id, std::move(metadata));
            }
        }
        catch (...)
        {
        }
    }

    void network_debug_logger::afd_result(windows_emulator&, const io_device_context& request, const int32_t status) noexcept
    {
        if (!impl_ || !request.network_request_id)
        {
            return;
        }
        try
        {
            std::scoped_lock lock(impl_->mutex);
            json_fields event;
            add_common(event, "afd", "result", request.network_request_id);
            event.string("kind", "afd_result");
            event.string("status", hex(static_cast<uint32_t>(status)));
            event.string("operation", afd_operation(request.io_control_code));
            event.string("ioctl", hex(request.io_control_code));
            event.number("thread_id", request.issuer_thread_id);
            impl_->origin_for(event, request.network_request_id);
            impl_->write(event.finish());
            if (status != STATUS_PENDING && status == STATUS_INVALID_PARAMETER)
            {
                impl_->active.erase(request.network_request_id);
            }
        }
        catch (...)
        {
        }
    }

    void network_debug_logger::afd_completion(windows_emulator& win_emu, const io_device_context& request, const int32_t status,
                                              const bool was_pending) noexcept
    {
        if (!impl_ || !request.network_request_id)
        {
            return;
        }
        try
        {
            std::scoped_lock lock(impl_->mutex);
            json_fields event;
            add_common(event, "afd", "completion", request.network_request_id);
            event.string("kind", was_pending ? "afd_pending_complete" : "afd_complete");
            event.string("status", hex(static_cast<uint32_t>(status)));
            event.string("operation", afd_operation(request.io_control_code));
            event.string("ioctl", hex(request.io_control_code));
            event.number("thread_id", request.issuer_thread_id);
            event.string("file_handle", hex(request.file_handle.bits));
            const auto block = request.io_status_block.try_read();
            if (block)
            {
                event.number("completed_bytes", block->Information);
            }
            else
            {
                event.string("completed_bytes_unavailable", "io_status_block_unreadable");
            }
            if (const auto it = impl_->active.find(request.network_request_id); it != impl_->active.end())
            {
                event.string("syscall", it->second.syscall);
                event.raw("origin", it->second.origin);
                if (it->second.requested_bytes)
                {
                    event.number("requested_bytes", *it->second.requested_bytes);
                }
                if (it->second.receive && block && status == STATUS_SUCCESS)
                {
                    if (const auto preview = capture_preview(win_emu, it->second, block->Information))
                    {
                        event.string("preview_hex", *preview);
                        event.boolean("preview_truncated", block->Information > preview_limit);
                        event.string("preview_source", "guest_receive_buffer_at_completion");
                    }
                    else
                    {
                        event.string("preview_unavailable", "guest_receive_buffer_unreadable");
                    }
                }
                else
                {
                    event.string("preview_unavailable", it->second.receive ? "receive_not_successful" : "send_preview_on_request");
                }
                if (impl_->recent_origins.size() >= active_request_limit)
                {
                    impl_->recent_origins.clear();
                }
                impl_->recent_origins.emplace(request.network_request_id, it->second.origin);
                impl_->active.erase(it);
            }
            else
            {
                event.raw("origin", "{\"unavailable\":\"request_metadata_not_present_or_restored\"}");
                event.string("preview_unavailable", "request_metadata_not_present_or_restored");
            }
            impl_->write(event.finish());
        }
        catch (...)
        {
        }
    }

    void network_debug_logger::apc_queue(const uint64_t request_id, const uint32_t thread_id, const int32_t status,
                                         const uint64_t information) noexcept
    {
        if (!impl_ || !request_id)
        {
            return;
        }
        try
        {
            std::scoped_lock lock(impl_->mutex);
            json_fields event;
            add_common(event, "apc", "queue", request_id);
            event.string("kind", "apc_queue");
            event.number("thread_id", thread_id);
            event.string("status", hex(static_cast<uint32_t>(status)));
            event.number("completed_bytes", information);
            impl_->origin_for(event, request_id);
            impl_->write(event.finish());
        }
        catch (...)
        {
        }
    }

    void network_debug_logger::apc_dispatch(const uint64_t request_id, const uint32_t thread_id) noexcept
    {
        if (!impl_ || !request_id)
        {
            return;
        }
        try
        {
            std::scoped_lock lock(impl_->mutex);
            json_fields event;
            add_common(event, "apc", "dequeue", request_id);
            event.string("kind", "apc_dispatch");
            event.number("thread_id", thread_id);
            impl_->origin_for(event, request_id);
            impl_->write(event.finish());
        }
        catch (...)
        {
        }
    }

    void network_debug_logger::iocp_queue(const uint64_t request_id, const uint64_t port, const uint64_t key, const int32_t status,
                                          const uint64_t information) noexcept
    {
        if (!impl_ || !request_id)
        {
            return;
        }
        try
        {
            std::scoped_lock lock(impl_->mutex);
            json_fields event;
            add_common(event, "iocp", "queue", request_id);
            event.string("kind", "iocp_queue");
            event.string("port", hex(port));
            event.string("key", hex(key));
            event.string("status", hex(static_cast<uint32_t>(status)));
            event.number("completed_bytes", information);
            impl_->origin_for(event, request_id);
            impl_->write(event.finish());
        }
        catch (...)
        {
        }
    }

    void network_debug_logger::iocp_dequeue(const io_completion_message& message, const uint64_t port) noexcept
    {
        if (!impl_ || !message.network_request_id)
        {
            return;
        }
        try
        {
            std::scoped_lock lock(impl_->mutex);
            json_fields event;
            add_common(event, "iocp", "dequeue", message.network_request_id);
            event.string("kind", "iocp_dequeue");
            event.string("port", hex(port));
            event.string("key", hex(message.key_context));
            event.string("status", hex(static_cast<uint32_t>(message.io_status_block.Status)));
            event.number("completed_bytes", message.io_status_block.Information);
            impl_->origin_for(event, message.network_request_id);
            impl_->write(event.finish());
        }
        catch (...)
        {
        }
    }
}
