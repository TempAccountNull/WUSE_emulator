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
        constexpr size_t idle_input_limit = 4096;
        constexpr size_t idle_summary_limit = 128;
        constexpr uint64_t idle_summary_batch = 256;
        constexpr uint64_t payload_hash_limit = 16ull * 1024 * 1024;
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
            std::optional<std::string> endpoint;
            std::optional<std::string> local_endpoint;
            std::optional<std::string> remote_endpoint;
            std::optional<uint64_t> requested_bytes;
            std::vector<guest_buffer> buffers;
            uint64_t datagram_address{};
            uint64_t datagram_address_length{};
            uint64_t accept_handle{};
            int32_t accept_sequence{};
            std::chrono::system_clock::time_point captured_at;
            std::chrono::steady_clock::time_point started;
            std::optional<std::string> idle_key;
            std::string deferred_request;
            std::string deferred_result;
            bool receive{};
        };

        std::optional<std::string> capture_endpoint(windows_emulator& win_emu, const uint64_t address, const uint64_t length,
                                                    const std::string_view role, const std::string_view source)
        {
            if (!address || length < sizeof(win_sockaddr))
            {
                return {};
            }
            std::array<std::byte, 28> bytes{};
            uint16_t family{};
            if (!win_emu.memory.try_read_memory(address, &family, sizeof(family)))
            {
                return {};
            }
            const size_t required = family == 2 ? 16 : family == 23 ? 28 : 0;
            if (!required || length < required || !win_emu.memory.try_read_memory(address, bytes.data(), required))
            {
                return {};
            }

            const auto octet = [&](const size_t index) { return static_cast<unsigned>(bytes[index]); };
            const unsigned port = (octet(2) << 8) | octet(3);
            std::string ip;
            if (family == 2)
            {
                for (size_t index = 4; index < 8; ++index)
                {
                    if (index != 4)
                    {
                        ip.push_back('.');
                    }
                    ip += std::to_string(octet(index));
                }
            }
            else
            {
                for (size_t index = 8; index < 24; index += 2)
                {
                    if (index != 8)
                    {
                        ip.push_back(':');
                    }
                    ip += hex((octet(index) << 8) | octet(index + 1)).substr(2);
                }
            }
            json_fields endpoint;
            endpoint.string("role", role);
            endpoint.string("source", source);
            endpoint.string("family", family == 2 ? "ipv4" : "ipv6");
            endpoint.string("address", ip);
            endpoint.number("port", port);
            endpoint.string("sockaddr_hex", hex_bytes(std::span{bytes.data(), required}));
            return endpoint.finish();
        }

        template <typename Traits>
        void capture_request_endpoint(windows_emulator& win_emu, const io_device_context& request, request_metadata& metadata)
        {
            const auto operation = _AFD_REQUEST(request.io_control_code);
            if (operation == AFD_BIND || operation == AFD_CONNECT)
            {
                const size_t offset = operation == AFD_BIND ? 4 : offsetof(AFD_CONNECT_JOIN_INFO_TL<Traits>, RemoteAddress);
                if (request.input_buffer && request.input_buffer_length >= offset + sizeof(win_sockaddr) &&
                    request.input_buffer <= std::numeric_limits<uint64_t>::max() - offset)
                {
                    metadata.endpoint = capture_endpoint(win_emu, request.input_buffer + offset, request.input_buffer_length - offset,
                                                         operation == AFD_BIND ? "local" : "remote", "guest_ioctl_input_at_request");
                }
            }
            else if (operation == AFD_SEND_DATAGRAM)
            {
                AFD_SEND_DATAGRAM_INFO<Traits> info{};
                if (request.input_buffer_length >= sizeof(info) && request.input_buffer &&
                    win_emu.memory.try_read_memory(request.input_buffer, &info, sizeof(info)) && info.TdiConnInfo.RemoteAddressLength > 0)
                {
                    metadata.endpoint = capture_endpoint(win_emu, info.TdiConnInfo.RemoteAddress,
                                                         static_cast<uint64_t>(info.TdiConnInfo.RemoteAddressLength), "remote",
                                                         "guest_datagram_destination_at_request");
                }
            }
            else if (operation == AFD_RECEIVE_DATAGRAM)
            {
                AFD_RECV_DATAGRAM_INFO<Traits> info{};
                if (request.input_buffer_length >= sizeof(info) && request.input_buffer &&
                    win_emu.memory.try_read_memory(request.input_buffer, &info, sizeof(info)))
                {
                    metadata.datagram_address = info.Address;
                    metadata.datagram_address_length = info.AddressLength;
                }
            }
        }

        void capture_accept_target(windows_emulator& win_emu, const io_device_context& request, request_metadata& metadata)
        {
            if (_AFD_REQUEST(request.io_control_code) != AFD_ACCEPT || request.input_buffer_length < sizeof(AFD_ACCEPT_INFO) ||
                !request.input_buffer)
            {
                return;
            }
            AFD_ACCEPT_INFO info{};
            if (win_emu.memory.try_read_memory(request.input_buffer, &info, sizeof(info)))
            {
                metadata.accept_handle = info.AcceptHandle.bits;
                metadata.accept_sequence = info.Sequence;
            }
        }

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

        std::optional<uint64_t> hash_payload(windows_emulator& win_emu, const request_metadata& metadata, const uint64_t count)
        {
            if (count > payload_hash_limit || !metadata.requested_bytes || count > *metadata.requested_bytes)
            {
                return {};
            }
            uint64_t hash = 14695981039346656037ull;
            uint64_t remaining = count;
            std::array<std::byte, 4096> bytes{};
            for (const auto& buffer : metadata.buffers)
            {
                uint64_t in_buffer = std::min<uint64_t>(buffer.length, remaining);
                uint64_t offset = 0;
                while (in_buffer)
                {
                    const auto chunk = static_cast<size_t>(std::min<uint64_t>(in_buffer, bytes.size()));
                    if (!buffer.address || buffer.address > std::numeric_limits<uint64_t>::max() - offset ||
                        !win_emu.memory.try_read_memory(buffer.address + offset, bytes.data(), chunk))
                    {
                        return {};
                    }
                    for (size_t index = 0; index < chunk; ++index)
                    {
                        hash ^= static_cast<uint8_t>(bytes[index]);
                        hash *= 1099511628211ull;
                    }
                    in_buffer -= chunk;
                    remaining -= chunk;
                    offset += chunk;
                }
                if (!remaining)
                {
                    return hash;
                }
            }
            return remaining ? std::optional<uint64_t>{} : std::optional<uint64_t>{hash};
        }

        void capture_payload_hash(json_fields& event, windows_emulator& win_emu, const request_metadata& metadata, const uint64_t count,
                                  const std::string_view source)
        {
            if (const auto hash = hash_payload(win_emu, metadata, count))
            {
                event.string("payload_fnv1a64", hex(*hash));
                event.number("payload_hashed_bytes", count);
                event.string("payload_hash_source", source);
            }
            else
            {
                event.string("payload_hash_unavailable",
                             count > payload_hash_limit ? "exceeds_16_mib_limit" : "descriptors_incomplete_or_guest_buffer_unreadable");
            }
        }

        void add_common(json_fields& record, const std::string_view channel, const std::string_view phase, const uint64_t request_id,
                        const std::chrono::system_clock::time_point captured_at = std::chrono::system_clock::now())
        {
            record.number("schema", 1);
            record.string("type", "network_io");
            record.string("channel", channel);
            record.string("phase", phase);
            record.number("request_id", request_id);
            const auto now = captured_at.time_since_epoch();
            record.number("time_ms", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()));
            record.number("time_us", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count()));
        }

        uint64_t timestamp_us(const std::chrono::system_clock::time_point time)
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count());
        }

        std::string accept_key(const uint64_t listener, const int32_t sequence)
        {
            return std::to_string(listener) + ":" + std::to_string(sequence);
        }
    }

    struct network_debug_logger::implementation
    {
        struct idle_summary
        {
            std::string operation;
            uint32_t status{};
            uint64_t file_handle{};
            uint64_t baseline_request_id{};
            uint64_t first_request_id{};
            uint64_t last_request_id{};
            uint64_t first_time_us{};
            uint64_t last_time_us{};
            uint64_t count{};
        };

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
        std::unordered_map<uint64_t, std::string> remote_endpoints;
        std::unordered_map<uint64_t, std::string> local_endpoints;
        std::unordered_map<std::string, std::string> pending_accept_endpoints;
        std::unordered_map<std::string, idle_summary> idle_summaries;

        ~implementation()
        {
            for (const auto& [key, summary] : idle_summaries)
            {
                (void)key;
                flush_idle(summary);
            }
            for (const auto& [id, metadata] : active)
            {
                (void)id;
                if (!metadata.deferred_request.empty())
                {
                    write(metadata.deferred_request);
                    if (!metadata.deferred_result.empty())
                    {
                        write(metadata.deferred_result);
                    }
                }
            }
        }

        void flush_idle(const idle_summary& summary)
        {
            if (!summary.count)
            {
                return;
            }
            json_fields event;
            add_common(event, "afd", "summary", summary.last_request_id);
            event.string("kind", "afd_idle_repeat");
            event.string("operation", summary.operation);
            event.string("status", hex(summary.status));
            event.string("file_handle", hex(summary.file_handle));
            event.number("baseline_request_id", summary.baseline_request_id);
            event.number("first_request_id", summary.first_request_id);
            event.number("last_request_id", summary.last_request_id);
            event.number("first_time_us", summary.first_time_us);
            event.number("last_time_us", summary.last_time_us);
            event.number("count", summary.count);
            write(event.finish());
        }

        void write_idle(const std::string& key, const request_metadata& metadata, const uint64_t request_id, const uint64_t file_handle,
                        const int32_t status, std::string completion)
        {
            if (const auto it = idle_summaries.find(key); it != idle_summaries.end())
            {
                auto& summary = it->second;
                if (!summary.count)
                {
                    summary.first_request_id = request_id;
                    summary.first_time_us = timestamp_us(metadata.captured_at);
                }
                summary.last_request_id = request_id;
                summary.last_time_us = timestamp_us(metadata.captured_at);
                ++summary.count;
                if (summary.count >= idle_summary_batch || summary.last_time_us - summary.first_time_us >= 5'000'000)
                {
                    flush_idle(summary);
                    summary.count = 0;
                }
                return;
            }
            if (idle_summaries.size() >= idle_summary_limit)
            {
                for (const auto& [old_key, summary] : idle_summaries)
                {
                    (void)old_key;
                    flush_idle(summary);
                }
                idle_summaries.clear();
            }
            write(metadata.deferred_request);
            write(metadata.deferred_result);
            write(std::move(completion));
            idle_summaries.emplace(key, idle_summary{.operation = metadata.operation,
                                                     .status = static_cast<uint32_t>(status),
                                                     .file_handle = file_handle,
                                                     .baseline_request_id = request_id});
        }

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
        const auto captured_at = std::chrono::system_clock::now();
        const auto started = std::chrono::steady_clock::now();
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
            metadata.captured_at = captured_at;
            metadata.started = started;
            if (win_emu.process.is_wow64_process)
            {
                capture_descriptors<EmulatorTraits<Emu32>>(win_emu, request, metadata);
                capture_request_endpoint<EmulatorTraits<Emu32>>(win_emu, request, metadata);
            }
            else
            {
                capture_descriptors<EmulatorTraits<Emu64>>(win_emu, request, metadata);
                capture_request_endpoint<EmulatorTraits<Emu64>>(win_emu, request, metadata);
            }
            capture_accept_target(win_emu, request, metadata);
            if (metadata.endpoint)
            {
                (metadata.operation == "bind" ? metadata.local_endpoint : metadata.remote_endpoint) = metadata.endpoint;
            }
            if ((metadata.operation == "receive_datagram" || metadata.operation == "poll") && request.input_buffer &&
                request.input_buffer_length <= idle_input_limit)
            {
                std::array<char, idle_input_limit> input{};
                if (win_emu.memory.try_read_memory(request.input_buffer, input.data(), request.input_buffer_length))
                {
                    metadata.idle_key = std::to_string(request.file_handle.bits) + "|" + metadata.operation + "|" + metadata.origin + "|" +
                                        std::to_string(request.input_buffer_length) + "|";
                    metadata.idle_key->append(input.data(), request.input_buffer_length);
                }
            }

            std::scoped_lock lock(impl_->mutex);
            if (!metadata.local_endpoint)
            {
                if (const auto it = impl_->local_endpoints.find(request.file_handle.bits); it != impl_->local_endpoints.end())
                {
                    metadata.local_endpoint = it->second;
                }
            }
            if (!metadata.remote_endpoint)
            {
                if (const auto it = impl_->remote_endpoints.find(request.file_handle.bits); it != impl_->remote_endpoints.end())
                {
                    metadata.remote_endpoint = it->second;
                }
            }
            if (!metadata.endpoint)
            {
                metadata.endpoint = metadata.remote_endpoint ? metadata.remote_endpoint : metadata.local_endpoint;
            }
            request.network_request_id = ++impl_->next_id;
            json_fields event;
            add_common(event, "afd", "request", request.network_request_id, captured_at);
            event.string("kind", "afd_request");
            event.number("thread_id", request.issuer_thread_id);
            event.string("file_handle", hex(request.file_handle.bits));
            event.string("ioctl", hex(request.io_control_code));
            event.string("operation", metadata.operation);
            event.string("syscall", metadata.syscall);
            if (metadata.endpoint)
            {
                event.raw("endpoint", *metadata.endpoint);
            }
            else
            {
                event.string("endpoint_unavailable", "no_address_in_request_or_known_connection");
            }
            if (metadata.local_endpoint)
            {
                event.raw("local_endpoint", *metadata.local_endpoint);
            }
            if (metadata.remote_endpoint)
            {
                event.raw("remote_endpoint", *metadata.remote_endpoint);
            }
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
                event.string("payload_direction", "outbound");
                capture_payload_hash(event, win_emu, metadata, *metadata.requested_bytes, "guest_send_buffer_at_request");
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
                if (metadata.receive)
                {
                    event.string("payload_direction", "inbound");
                }
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
            if (impl_->active.size() < active_request_limit)
            {
                if (metadata.idle_key)
                {
                    metadata.deferred_request = event.finish();
                }
                else
                {
                    impl_->write(event.finish());
                }
                impl_->active.emplace(request.network_request_id, std::move(metadata));
            }
            else
            {
                impl_->write(event.finish());
            }
        }
        catch (...)
        {
        }
    }

    void network_debug_logger::afd_result(windows_emulator&, const io_device_context& request, const int32_t status) noexcept
    {
        const auto captured_at = std::chrono::system_clock::now();
        if (!impl_ || !request.network_request_id)
        {
            return;
        }
        try
        {
            std::scoped_lock lock(impl_->mutex);
            json_fields event;
            add_common(event, "afd", "result", request.network_request_id, captured_at);
            event.string("kind", "afd_result");
            event.string("status", hex(static_cast<uint32_t>(status)));
            event.string("operation", afd_operation(request.io_control_code));
            event.string("ioctl", hex(request.io_control_code));
            event.number("thread_id", request.issuer_thread_id);
            impl_->origin_for(event, request.network_request_id);
            if (const auto it = impl_->active.find(request.network_request_id);
                it != impl_->active.end() && it->second.idle_key &&
                (status == STATUS_PENDING || (it->second.operation == "receive_datagram" && status == STATUS_DEVICE_NOT_READY) ||
                 (it->second.operation == "poll" && status == STATUS_TIMEOUT)))
            {
                it->second.deferred_result = event.finish();
            }
            else
            {
                if (const auto pending = impl_->active.find(request.network_request_id);
                    pending != impl_->active.end() && !pending->second.deferred_request.empty())
                {
                    impl_->write(std::move(pending->second.deferred_request));
                    pending->second.idle_key.reset();
                }
                impl_->write(event.finish());
            }
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
        const auto captured_at = std::chrono::system_clock::now();
        const auto completed_at = std::chrono::steady_clock::now();
        if (!impl_ || !request.network_request_id)
        {
            return;
        }
        try
        {
            std::scoped_lock lock(impl_->mutex);
            json_fields event;
            add_common(event, "afd", "completion", request.network_request_id, captured_at);
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
                event.number("elapsed_us",
                             static_cast<uint64_t>(
                                 std::chrono::duration_cast<std::chrono::microseconds>(completed_at - it->second.started).count()));
                if (it->second.requested_bytes)
                {
                    event.number("requested_bytes", *it->second.requested_bytes);
                    if (block && (it->second.receive || it->second.operation == "send" || it->second.operation == "send_datagram"))
                    {
                        event.boolean("completed_exceeds_requested", block->Information > *it->second.requested_bytes);
                    }
                }
                auto endpoint = it->second.endpoint;
                auto local_endpoint = it->second.local_endpoint;
                auto remote_endpoint = it->second.remote_endpoint;
                if (it->second.operation == "receive_datagram" && it->second.datagram_address && it->second.datagram_address_length &&
                    status == STATUS_SUCCESS)
                {
                    ULONG address_length{};
                    if (win_emu.memory.try_read_memory(it->second.datagram_address_length, &address_length, sizeof(address_length)))
                    {
                        endpoint = capture_endpoint(win_emu, it->second.datagram_address, address_length, "remote",
                                                    "guest_datagram_source_at_completion");
                        remote_endpoint = endpoint;
                    }
                }
                if (status == STATUS_SUCCESS && block && request.output_buffer && request.output_buffer_length)
                {
                    if (it->second.operation == "get_address")
                    {
                        local_endpoint = capture_endpoint(win_emu, request.output_buffer,
                                                          std::min<uint64_t>(block->Information, request.output_buffer_length), "local",
                                                          "guest_get_address_output_at_completion");
                        endpoint = local_endpoint;
                    }
                    else if (it->second.operation == "wait_for_listen" && block->Information >= sizeof(int32_t) + sizeof(win_sockaddr) &&
                             request.output_buffer_length >= sizeof(int32_t) + sizeof(win_sockaddr) &&
                             request.output_buffer <= std::numeric_limits<uint64_t>::max() - sizeof(int32_t))
                    {
                        int32_t sequence{};
                        if (win_emu.memory.try_read_memory(request.output_buffer, &sequence, sizeof(sequence)))
                        {
                            remote_endpoint =
                                capture_endpoint(win_emu, request.output_buffer + sizeof(sequence),
                                                 std::min<uint64_t>(block->Information, request.output_buffer_length) - sizeof(sequence),
                                                 "remote", "guest_wait_for_listen_output_at_completion");
                            endpoint = remote_endpoint;
                            if (remote_endpoint)
                            {
                                if (impl_->pending_accept_endpoints.size() >= active_request_limit)
                                {
                                    impl_->pending_accept_endpoints.clear();
                                }
                                impl_->pending_accept_endpoints[accept_key(request.file_handle.bits, sequence)] = *remote_endpoint;
                            }
                        }
                    }
                }
                if (status == STATUS_SUCCESS && it->second.operation == "accept" && it->second.accept_handle)
                {
                    if (const auto accepted =
                            impl_->pending_accept_endpoints.find(accept_key(request.file_handle.bits, it->second.accept_sequence));
                        accepted != impl_->pending_accept_endpoints.end())
                    {
                        remote_endpoint = accepted->second;
                        endpoint = remote_endpoint;
                        impl_->remote_endpoints[it->second.accept_handle] = *remote_endpoint;
                        impl_->pending_accept_endpoints.erase(accepted);
                    }
                    if (local_endpoint)
                    {
                        impl_->local_endpoints[it->second.accept_handle] = *local_endpoint;
                    }
                }
                if (endpoint)
                {
                    event.raw("endpoint", *endpoint);
                }
                else
                {
                    event.string("endpoint_unavailable", "no_address_in_request_or_completed_response");
                }
                if (local_endpoint)
                {
                    event.raw("local_endpoint", *local_endpoint);
                }
                if (remote_endpoint)
                {
                    event.raw("remote_endpoint", *remote_endpoint);
                }
                if (it->second.receive && block && status == STATUS_SUCCESS)
                {
                    event.string("payload_direction", "inbound");
                    capture_payload_hash(event, win_emu, it->second, block->Information, "guest_receive_buffer_at_completion");
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
                if (!it->second.receive && it->second.requested_bytes && block && status == STATUS_SUCCESS &&
                    (it->second.operation == "send" || it->second.operation == "send_datagram"))
                {
                    event.string("payload_direction", "outbound");
                    capture_payload_hash(event, win_emu, it->second, block->Information, "guest_send_buffer_at_completion");
                }
                if (status == STATUS_SUCCESS && it->second.operation == "connect" && endpoint)
                {
                    if (impl_->remote_endpoints.size() >= active_request_limit)
                    {
                        impl_->remote_endpoints.clear();
                    }
                    impl_->remote_endpoints[request.file_handle.bits] = *endpoint;
                }
                if (status == STATUS_SUCCESS && (it->second.operation == "bind" || it->second.operation == "get_address") && local_endpoint)
                {
                    if (impl_->local_endpoints.size() >= active_request_limit)
                    {
                        impl_->local_endpoints.clear();
                    }
                    impl_->local_endpoints[request.file_handle.bits] = *local_endpoint;
                }
                if (impl_->recent_origins.size() >= active_request_limit)
                {
                    impl_->recent_origins.clear();
                }
                impl_->recent_origins.emplace(request.network_request_id, it->second.origin);
            }
            else
            {
                event.raw("origin", "{\"unavailable\":\"request_metadata_not_present_or_restored\"}");
                event.string("preview_unavailable", "request_metadata_not_present_or_restored");
            }
            const auto operation = _AFD_REQUEST(request.io_control_code);
            const bool transfer =
                operation == AFD_SEND || operation == AFD_SEND_DATAGRAM || operation == AFD_RECEIVE || operation == AFD_RECEIVE_DATAGRAM;
            if (!transfer && status == STATUS_SUCCESS && block && request.output_buffer && request.output_buffer_length)
            {
                const size_t available = static_cast<size_t>(std::min<uint64_t>(block->Information, request.output_buffer_length));
                const size_t count = std::min(available, preview_limit);
                if (count)
                {
                    std::array<std::byte, preview_limit> bytes{};
                    if (win_emu.memory.try_read_memory(request.output_buffer, bytes.data(), count))
                    {
                        event.string("control_output_preview_hex", hex_bytes(std::span{bytes.data(), count}));
                        event.boolean("control_output_preview_truncated", available > count);
                        event.string("control_output_preview_source", "guest_ioctl_output_at_completion");
                    }
                    else
                    {
                        event.string("control_output_preview_unavailable", "output_buffer_unreadable");
                    }
                }
            }
            const auto it = impl_->active.find(request.network_request_id);
            if (it != impl_->active.end() && it->second.idle_key &&
                ((it->second.operation == "receive_datagram" && status == STATUS_DEVICE_NOT_READY) ||
                 (it->second.operation == "poll" && status == STATUS_TIMEOUT)))
            {
                auto key = *it->second.idle_key + "|" + std::to_string(static_cast<uint32_t>(status)) + "|" +
                           std::to_string(block ? block->Information : 0) + "|" + std::to_string(was_pending);
                impl_->write_idle(key, it->second, request.network_request_id, request.file_handle.bits, status, event.finish());
            }
            else
            {
                if (it != impl_->active.end())
                {
                    if (!it->second.deferred_request.empty())
                    {
                        impl_->write(std::move(it->second.deferred_request));
                    }
                    if (!it->second.deferred_result.empty())
                    {
                        impl_->write(std::move(it->second.deferred_result));
                    }
                }
                impl_->write(event.finish());
            }
            if (it != impl_->active.end())
            {
                impl_->active.erase(it);
            }
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
