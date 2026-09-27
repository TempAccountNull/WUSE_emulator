#include "../std_include.hpp"
#include "afd_endpoint.hpp"
#include "afd_types.hpp"

#include <deque>

#include "../windows_emulator.hpp"
#include "../network/socket_factory.hpp"

#include <network/address.hpp>
#include <network/socket.hpp>

#include <utils/finally.hpp>
#include <utils/time.hpp>

namespace sogen
{

    namespace
    {
        // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

        std::optional<afd_profile::operation> profiled_operation(const ULONG request)
        {
            using operation = afd_profile::operation;
            switch (request)
            {
            case AFD_CONNECT:
                return operation::connect;
            case AFD_SEND:
                return operation::send;
            case AFD_RECEIVE:
                return operation::receive;
            case AFD_SEND_DATAGRAM:
                return operation::send_datagram;
            case AFD_RECEIVE_DATAGRAM:
                return operation::receive_datagram;
            case AFD_POLL:
                return operation::poll_request;
            default:
                return {};
            }
        }

        struct afd_creation_data
        {
            uint64_t unk1;
            char afd_open_packet_xx[0x10];
            uint64_t unk2;
            int address_family;
            int type;
            int protocol;
            // ...
        };

        struct win_sockaddr
        {
            int16_t sa_family;
            uint8_t sa_data[14];
        };

        struct win_sockaddr_in
        {
            int16_t sin_family;
            uint16_t sin_port;
            in_addr sin_addr;
            uint8_t sin_zero[8];
        };

        struct win_sockaddr_in6
        {
            int16_t sin6_family;
            uint16_t sin6_port;
            uint32_t sin6_flowinfo;
            in6_addr sin6_addr;
            uint32_t sin6_scope_id;
        };

        // NOLINTEND(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

        static_assert(sizeof(win_sockaddr) == 16);
        static_assert(sizeof(win_sockaddr_in) == 16);
        static_assert(sizeof(win_sockaddr_in6) == 28);

        static_assert(sizeof(win_sockaddr_in::sin_addr) == 4);
        static_assert(sizeof(win_sockaddr_in6::sin6_addr) == 16);
        static_assert(sizeof(win_sockaddr_in6::sin6_flowinfo) == sizeof(sockaddr_in6::sin6_flowinfo));
        static_assert(sizeof(win_sockaddr_in6::sin6_scope_id) == sizeof(sockaddr_in6::sin6_scope_id));

        struct socket_mapping
        {
            int win_value;
            int host_value;
        };

        constexpr std::array address_family_map{
            socket_mapping{.win_value = 0, .host_value = AF_UNSPEC},
            socket_mapping{.win_value = 2, .host_value = AF_INET},
            socket_mapping{.win_value = 23, .host_value = AF_INET6},
        };

        constexpr std::array socket_type_map{
            socket_mapping{.win_value = 0, .host_value = 0},          socket_mapping{.win_value = 1, .host_value = SOCK_STREAM},
            socket_mapping{.win_value = 2, .host_value = SOCK_DGRAM}, socket_mapping{.win_value = 3, .host_value = SOCK_RAW},
            socket_mapping{.win_value = 4, .host_value = SOCK_RDM},
        };

        constexpr std::array socket_protocol_map{
            socket_mapping{.win_value = 0, .host_value = 0},
            socket_mapping{.win_value = 6, .host_value = IPPROTO_TCP},
            socket_mapping{.win_value = 17, .host_value = IPPROTO_UDP},
            socket_mapping{.win_value = 255, .host_value = IPPROTO_RAW},
        };

        int16_t translate_host_to_win_address_family(const int host_af)
        {
            for (const auto& entry : address_family_map)
            {
                if (entry.host_value == host_af)
                {
                    return static_cast<int16_t>(entry.win_value);
                }
            }

            throw std::runtime_error("Unknown host address family: " + std::to_string(host_af));
        }

        int translate_win_to_host_address_family(const int win_af)
        {
            for (const auto& entry : address_family_map)
            {
                if (entry.win_value == win_af)
                {
                    return entry.host_value;
                }
            }

            throw std::runtime_error("Unknown address family: " + std::to_string(win_af));
        }

        int translate_win_to_host_type(const int win_type)
        {
            for (const auto& entry : socket_type_map)
            {
                if (entry.win_value == win_type)
                {
                    return entry.host_value;
                }
            }

            throw std::runtime_error("Unknown socket type: " + std::to_string(win_type));
        }

        int translate_win_to_host_protocol(const int win_protocol)
        {
            for (const auto& entry : socket_protocol_map)
            {
                if (entry.win_value == win_protocol)
                {
                    return entry.host_value;
                }
            }

            throw std::runtime_error("Unknown socket protocol: " + std::to_string(win_protocol));
        }

        std::vector<std::byte> convert_to_win_address(const windows_emulator& win_emu, const network::address& a)
        {
            if (a.is_ipv4())
            {
                win_sockaddr_in win_addr{};
                win_addr.sin_family = translate_host_to_win_address_family(a.get_family());
                win_addr.sin_port = htons(win_emu.get_emulator_port(a.get_port()));
                memcpy(&win_addr.sin_addr, &a.get_in_addr().sin_addr, sizeof(win_addr.sin_addr));

                const auto* ptr = reinterpret_cast<std::byte*>(&win_addr);
                return {ptr, ptr + sizeof(win_addr)};
            }

            if (a.is_ipv6())
            {
                win_sockaddr_in6 win_addr{};
                win_addr.sin6_family = translate_host_to_win_address_family(a.get_family());
                win_addr.sin6_port = htons(win_emu.get_emulator_port(a.get_port()));

                const auto& addr = a.get_in6_addr();
                memcpy(&win_addr.sin6_addr, &addr.sin6_addr, sizeof(win_addr.sin6_addr));
                win_addr.sin6_flowinfo = addr.sin6_flowinfo;
                win_addr.sin6_scope_id = addr.sin6_scope_id;

                const auto* ptr = reinterpret_cast<std::byte*>(&win_addr);
                return {ptr, ptr + sizeof(win_addr)};
            }

            throw std::runtime_error("Unsupported host address family for conversion: " + std::to_string(a.get_family()));
        }

        network::address convert_to_host_address(const windows_emulator& win_emu, const std::span<const std::byte> data)
        {
            if (data.size() < sizeof(win_sockaddr))
            {
                throw std::runtime_error("Bad address size");
            }

            win_sockaddr win_addr{};
            memcpy(&win_addr, data.data(), sizeof(win_addr));

            const auto family = translate_win_to_host_address_family(win_addr.sa_family);

            network::address a{};

            if (family == AF_INET)
            {
                if (data.size() < sizeof(win_sockaddr_in))
                {
                    throw std::runtime_error("Bad IPv4 address size");
                }

                win_sockaddr_in win_addr4{};
                memcpy(&win_addr4, data.data(), sizeof(win_addr4));

                a.set_ipv4(win_addr4.sin_addr);
                a.set_port(win_emu.get_host_port(ntohs(win_addr4.sin_port)));

                return a;
            }

            if (family == AF_INET6)
            {
                if (data.size() < sizeof(win_sockaddr_in6))
                {
                    throw std::runtime_error("Bad IPv6 address size");
                }

                win_sockaddr_in6 win_addr6{};
                memcpy(&win_addr6, data.data(), sizeof(win_addr6));

                a.set_ipv6(win_addr6.sin6_addr);
                a.set_port(ntohs(win_addr6.sin6_port));

                auto& addr = a.get_in6_addr();
                addr.sin6_flowinfo = win_addr6.sin6_flowinfo;
                addr.sin6_scope_id = win_addr6.sin6_scope_id;

                return a;
            }

            throw std::runtime_error("Unsupported win address family for conversion: " + std::to_string(family));
        }

        afd_creation_data get_creation_data(windows_emulator& win_emu, const io_device_creation_data& data)
        {
            if (!data.buffer || data.length < sizeof(afd_creation_data))
            {
                throw std::runtime_error("Bad AFD creation data");
            }

            return win_emu.emu().read_memory<afd_creation_data>(data.buffer);
        }

        static_assert(sizeof(EMU_WSABUF<EmulatorTraits<Emu32>>) == 8);
        static_assert(offsetof(EMU_WSABUF<EmulatorTraits<Emu32>>, buf) == 4);
        static_assert(sizeof(EMU_WSABUF<EmulatorTraits<Emu64>>) == 16);
        static_assert(offsetof(EMU_WSABUF<EmulatorTraits<Emu64>>, buf) == 8);

        // Untrusted guest lengths need bounded staging; stream I/O may complete with a prefix.
        constexpr size_t max_stream_transfer_bytes = 64u << 20;
        constexpr ULONG max_stream_buffer_count = 1u << 16;
        constexpr size_t max_datagram_bytes = 0x10000;
        // Windows SDK shared/tdi.h defines TDI_RECEIVE_NORMAL; tdi.h needs kernel-only types.
        constexpr ULONG tdi_receive_normal = 0x20;
        constexpr ULONG tdi_receive_peek = 0x80;

        bool is_writable_guest_range(memory_manager& memory, uint64_t address, uint64_t size)
        {
            if (size > UINT64_MAX - address)
            {
                return false;
            }
            const auto end = address + size;
            while (address < end)
            {
                const auto region = memory.get_region_info(address);
                if (!region.is_committed || (region.permissions.common & memory_permission::write) == memory_permission::none ||
                    region.permissions.extended != memory_permission_ext::none)
                {
                    return false;
                }
                const auto next = region.start + region.length;
                if (next <= address)
                {
                    return false;
                }
                address = std::min(next, end);
            }
            return true;
        }

        template <typename Traits>
        NTSTATUS load_stream_buffers(const memory_interface& memory, const uint64_t array, const ULONG count,
                                     std::vector<EMU_WSABUF<Traits>>& buffers)
        {
            if (!array || !count)
            {
                return STATUS_INVALID_PARAMETER;
            }
            if (count > max_stream_buffer_count)
            {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            const size_t bytes = static_cast<size_t>(count) * sizeof(EMU_WSABUF<Traits>);
            if (array > std::numeric_limits<uint64_t>::max() - bytes)
            {
                return STATUS_ACCESS_VIOLATION;
            }
            try
            {
                buffers.resize(count);
            }
            catch (const std::bad_alloc&)
            {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            if (!memory.try_read_memory(array, buffers.data(), bytes))
            {
                return STATUS_ACCESS_VIOLATION;
            }
            for (const auto& buffer : buffers)
            {
                if (buffer.len != 0 && !buffer.buf)
                {
                    return STATUS_ACCESS_VIOLATION;
                }
            }
            return STATUS_SUCCESS;
        }

        template <typename Traits>
        size_t stream_transfer_size(const std::span<const EMU_WSABUF<Traits>> buffers)
        {
            size_t size = 0;
            for (const auto& buffer : buffers)
            {
                size += std::min<size_t>(buffer.len, max_stream_transfer_bytes - size);
            }
            return size;
        }

        template <typename Traits>
        std::pair<AFD_POLL_INFO<Traits>, std::vector<AFD_POLL_HANDLE_INFO<Traits>>> get_poll_info(windows_emulator& win_emu,
                                                                                                  const io_device_context& c)
        {
            constexpr auto info_size = offsetof(AFD_POLL_INFO<Traits>, Handles);
            if (!c.input_buffer || c.input_buffer_length < info_size || c.input_buffer != c.output_buffer)
            {
                throw std::runtime_error("Bad AFD poll data");
            }

            AFD_POLL_INFO<Traits> poll_info{};
            win_emu.emu().read_memory(c.input_buffer, &poll_info, info_size);

            std::vector<AFD_POLL_HANDLE_INFO<Traits>> handle_info{};

            const emulator_object<AFD_POLL_HANDLE_INFO<Traits>> handle_info_obj{win_emu.emu(), c.input_buffer + info_size};

            if (c.input_buffer_length < (info_size + (sizeof(AFD_POLL_HANDLE_INFO<Traits>) * poll_info.NumberOfHandles)))
            {
                throw std::runtime_error("Bad AFD poll handle data");
            }

            handle_info.reserve(poll_info.NumberOfHandles);
            for (ULONG i = 0; i < poll_info.NumberOfHandles; ++i)
            {
                handle_info.emplace_back(handle_info_obj.read(i));
            }

            return {poll_info, std::move(handle_info)};
        }

        int16_t map_afd_request_events_to_socket(const ULONG poll_events)
        {
            int16_t socket_events{};

            if (poll_events & (AFD_POLL_DISCONNECT | AFD_POLL_ACCEPT | AFD_POLL_RECEIVE))
            {
                socket_events |= POLLRDNORM;
            }

            if (poll_events & AFD_POLL_RECEIVE_EXPEDITED)
            {
                socket_events |= POLLRDBAND;
            }

            if (poll_events & (AFD_POLL_CONNECT | AFD_POLL_CONNECT_FAIL | AFD_POLL_SEND))
            {
                socket_events |= POLLWRNORM;
            }

            return socket_events;
        }

        ULONG map_socket_response_events_to_afd(const int16_t socket_events, const ULONG afd_poll_events, const bool is_listening,
                                                const bool is_connecting)
        {
            ULONG afd_events = 0;

            if (socket_events & POLLRDNORM)
            {
                if (!is_listening && afd_poll_events & AFD_POLL_RECEIVE)
                {
                    afd_events |= AFD_POLL_RECEIVE;
                }
                else if (is_listening && afd_poll_events & AFD_POLL_ACCEPT)
                {
                    afd_events |= AFD_POLL_ACCEPT;
                }
            }

            if (socket_events & POLLRDBAND && afd_poll_events & AFD_POLL_RECEIVE_EXPEDITED)
            {
                afd_events |= AFD_POLL_RECEIVE_EXPEDITED;
            }

            if (socket_events & POLLWRNORM)
            {
                if (!is_connecting && afd_poll_events & AFD_POLL_SEND)
                {
                    afd_events |= AFD_POLL_SEND;
                }
                else if (is_connecting && afd_poll_events & AFD_POLL_CONNECT)
                {
                    afd_events |= AFD_POLL_CONNECT;
                }
            }

            if ((socket_events & (POLLHUP | POLLERR)) == (POLLHUP | POLLERR))
            {
                if (afd_poll_events & AFD_POLL_CONNECT_FAIL)
                {
                    afd_events |= AFD_POLL_CONNECT_FAIL;
                }
                if (afd_poll_events & AFD_POLL_ABORT)
                {
                    afd_events |= AFD_POLL_ABORT;
                }
            }
            else if (socket_events & POLLHUP && afd_poll_events & AFD_POLL_DISCONNECT)
            {
                afd_events |= AFD_POLL_DISCONNECT;
            }

            if (socket_events & POLLNVAL && afd_poll_events & AFD_POLL_LOCAL_CLOSE)
            {
                afd_events |= AFD_POLL_LOCAL_CLOSE;
            }

            return afd_events;
        }

        template <typename Traits>
        struct afd_endpoint : io_device
        {
            using status_block = IO_STATUS_BLOCK<EmulatorTraits<Emu64>>;

            bool may_return_pending() const override
            {
                return true;
            }

            struct pending_connection
            {
                network::address remote_address;
                std::unique_ptr<network::i_socket> accepted_socket;
            };

            std::unique_ptr<network::i_socket> s_{};

            struct pending_request
            {
                io_device_context context;
                std::optional<bool> require_poll{};
                std::optional<std::chrono::steady_clock::time_point> timeout{};
                std::optional<std::chrono::steady_clock::time_point> profile_since{};
                std::optional<afd_profile::operation> profile_operation{};
                std::vector<EMU_WSABUF<Traits>> stream_buffers{};
                ULONG stream_afd_flags{};
                std::vector<EMU_WSABUF<Traits>> datagram_buffers{};
                std::vector<std::byte> datagram_target{};
                ULONG datagram_afd_flags{};
                ULONG datagram_tdi_flags{};
                uint64_t datagram_address{};
                uint64_t datagram_address_length{};
                std::vector<std::byte> connect_input{};
                std::vector<AFD_POLL_HANDLE_INFO<Traits>> poll_handles{};
                bool poll_captured{};

                explicit pending_request(const io_device_context& c)
                    : context(c)
                {
                }

                explicit pending_request(utils::buffer_deserializer& buffer)
                    : context(buffer)
                {
                }

                void serialize(utils::buffer_serializer& buffer) const
                {
                    buffer.write(context);
                    buffer.write_optional(require_poll);
                    buffer.write_optional(timeout);
                    buffer.write_optional(profile_since);
                    buffer.write_optional(profile_operation);
                    buffer.write_vector(stream_buffers);
                    buffer.write(stream_afd_flags);
                    buffer.write_vector(datagram_buffers);
                    buffer.write_vector(datagram_target);
                    buffer.write(datagram_afd_flags);
                    buffer.write(datagram_tdi_flags);
                    buffer.write(datagram_address);
                    buffer.write(datagram_address_length);
                    buffer.write_vector(connect_input);
                    buffer.write_vector(poll_handles);
                    buffer.write(poll_captured);
                }

                void deserialize(utils::buffer_deserializer& buffer)
                {
                    buffer.read(context);
                    buffer.read_optional(require_poll);
                    buffer.read_optional(timeout);
                    buffer.read_optional(profile_since);
                    buffer.read_optional(profile_operation);
                    buffer.read_vector(stream_buffers);
                    buffer.read(stream_afd_flags);
                    buffer.read_vector(datagram_buffers);
                    buffer.read_vector(datagram_target);
                    buffer.read(datagram_afd_flags);
                    buffer.read(datagram_tdi_flags);
                    buffer.read(datagram_address);
                    buffer.read(datagram_address_length);
                    buffer.read_vector(connect_input);
                    buffer.read_vector(poll_handles);
                    buffer.read(poll_captured);
                }
            };

            static constexpr size_t max_pending_requests = 256;
            std::deque<pending_request> pending_requests_{};
            pending_request* active_pending_{};
            bool executing_delayed_ioctl_{};
            std::optional<afd_creation_data> creation_data{};

            std::unordered_map<LONG, pending_connection> pending_connections_{};
            LONG next_sequence_{0};

            std::optional<handle> event_select_event_{};
            ULONG event_select_mask_{0};
            ULONG triggered_events_{0};

            bool non_blocking_{false};

            afd_endpoint()
            {
                network::initialize_wsa();
            }

            afd_endpoint(afd_endpoint&&) = delete;
            afd_endpoint& operator=(afd_endpoint&&) = delete;

            ~afd_endpoint() override = default;

            void create(windows_emulator& win_emu, const io_device_creation_data& data) override
            {
                this->creation_data = get_creation_data(win_emu, data);
                this->setup(win_emu.socket_factory());
            }

            void setup(network::socket_factory& factory)
            {
                if (!this->creation_data)
                {
                    return;
                }

                const auto& data = *this->creation_data;

                const auto af = translate_win_to_host_address_family(data.address_family);
                const auto type = translate_win_to_host_type(data.type);
                const auto protocol = translate_win_to_host_protocol(data.protocol);

                this->s_ = factory.create_socket(af, type, protocol);
                if (!this->s_)
                {
                    throw std::runtime_error("Failed to create socket!");
                }

                this->s_->set_blocking(false);
            }

            NTSTATUS delay_ioctrl(const io_device_context& c, const std::optional<bool> require_poll = {},
                                  const std::optional<std::chrono::steady_clock::time_point> timeout = {})
            {
                if (this->executing_delayed_ioctl_)
                {
                    return STATUS_PENDING;
                }
                if (this->pending_requests_.size() >= max_pending_requests)
                {
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                this->pending_requests_.emplace_back(c);
                auto& request = this->pending_requests_.back();
                request.require_poll = require_poll;
                request.timeout = timeout;
                return STATUS_PENDING;
            }

            void update_shared_info(windows_emulator& win_emu, const io_device_context& c)
            {
                constexpr size_t option_flags_offset = 0x2c;
                constexpr ULONG non_blocking_flag = 1u << 6;
                if (c.input_buffer_length < option_flags_offset + sizeof(ULONG))
                {
                    return;
                }

                const auto option_flags = win_emu.emu().read_memory<ULONG>(c.input_buffer + option_flags_offset);
                this->non_blocking_ = (option_flags & non_blocking_flag) != 0;
            }

            NTSTATUS pend_or_would_block(const io_device_context& c, const bool require_poll, const ULONG flags)
            {
                if ((flags & 2) == 0 && ((flags & 4) != 0 || this->non_blocking_))
                {
                    return STATUS_DEVICE_NOT_READY;
                }

                return this->delay_ioctrl(c, require_poll);
            }

            void rebase_steady_deadlines(const std::chrono::steady_clock::duration offset) override
            {
                for (auto& request : this->pending_requests_)
                {
                    utils::rebase_steady_deadline(request.timeout, offset);
                    utils::rebase_steady_deadline(request.profile_since, offset);
                }
            }

            uint32_t cancel_pending_io(windows_emulator& win_emu, const uint64_t io_status_block, const uint32_t issuer_thread_id) override
            {
                uint32_t cancelled = 0;
                for (size_t index = 0; index < this->pending_requests_.size();)
                {
                    auto& request = this->pending_requests_[index];
                    if ((io_status_block && request.context.io_status_block.value() != io_status_block) ||
                        (issuer_thread_id && request.context.issuer_thread_id != issuer_thread_id))
                    {
                        ++index;
                        continue;
                    }

                    write_io_status(request.context.io_status_block, STATUS_CANCELLED, true);
                    complete_device_ioctl(win_emu, request.context, STATUS_CANCELLED, false);
                    if (request.profile_since && request.profile_operation)
                    {
                        const auto elapsed = std::chrono::steady_clock::now() - *request.profile_since;
                        win_emu.afd_diagnostics.record_completion(
                            *request.profile_operation, static_cast<uint32_t>(STATUS_CANCELLED),
                            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
                    }
                    this->pending_requests_.erase(this->pending_requests_.begin() + static_cast<ptrdiff_t>(index));
                    ++cancelled;
                }
                return cancelled;
            }

            void work(windows_emulator& win_emu) override
            {
                if (!this->s_ || (this->pending_requests_.empty() && !this->event_select_mask_))
                {
                    return;
                }

                network::poll_entry pfd{};
                pfd.s = this->s_.get();
                for (const auto& request : this->pending_requests_)
                {
                    if (request.require_poll.has_value())
                    {
                        pfd.events |= *request.require_poll ? POLLIN : POLLOUT;
                    }
                }
                if (this->event_select_mask_)
                {
                    pfd.events = static_cast<int16_t>(pfd.events | map_afd_request_events_to_socket(this->event_select_mask_));
                }
                pfd.revents = 0;
                if (pfd.events != 0)
                {
                    win_emu.socket_factory().poll_sockets(std::span{&pfd, 1});
                }
                const auto socket_events = pfd.revents;

                if (socket_events && this->event_select_mask_)
                {
                    const bool is_connecting =
                        std::any_of(this->pending_requests_.begin(), this->pending_requests_.end(), [](const pending_request& request) {
                            return _AFD_REQUEST(request.context.io_control_code) == AFD_CONNECT;
                        });
                    ULONG current_events =
                        map_socket_response_events_to_afd(socket_events, this->event_select_mask_, pfd.s->is_listening(), is_connecting);
                    if ((current_events & ~this->triggered_events_) != 0)
                    {
                        this->triggered_events_ |= current_events;
                        if (auto* event = win_emu.process.events.get(*this->event_select_event_))
                        {
                            event->signaled = true;
                        }
                    }
                }

                bool blocked_read = false;
                bool blocked_write = false;
                for (size_t index = 0; index < this->pending_requests_.size();)
                {
                    auto& request = this->pending_requests_[index];
                    const bool expired = request.timeout && *request.timeout <= win_emu.clock().steady_now();
                    bool ready = true;
                    if (request.require_poll.has_value())
                    {
                        const bool read = *request.require_poll;
                        ready = !(read ? blocked_read : blocked_write) &&
                                (socket_events & ((read ? POLLIN : POLLOUT) | POLLHUP | POLLERR)) != 0;
                        if (!ready && !expired)
                        {
                            if (read)
                            {
                                blocked_read = true;
                            }
                            else
                            {
                                blocked_write = true;
                            }
                            ++index;
                            continue;
                        }
                    }

                    this->active_pending_ = &request;
                    request.context.completing_pending = true;
                    this->executing_delayed_ioctl_ = true;
                    const auto reset_active = utils::finally([this] {
                        this->active_pending_ = nullptr;
                        this->executing_delayed_ioctl_ = false;
                    });
                    NTSTATUS status = STATUS_PENDING;
                    if (ready)
                    {
                        status = this->execute_ioctl(win_emu, request.context);
                    }
                    if (status == STATUS_PENDING && !expired)
                    {
                        if (request.require_poll.has_value())
                        {
                            if (*request.require_poll)
                            {
                                blocked_read = true;
                            }
                            else
                            {
                                blocked_write = true;
                            }
                        }
                        ++index;
                        continue;
                    }
                    if (status == STATUS_PENDING)
                    {
                        status = STATUS_TIMEOUT;
                        if (_AFD_REQUEST(request.context.io_control_code) == AFD_POLL)
                        {
                            const ULONG count = 0;
                            const auto number_offset = offsetof(AFD_POLL_INFO<Traits>, NumberOfHandles);
                            if (!win_emu.memory.try_write_memory(request.context.input_buffer + number_offset, &count, sizeof(count)))
                            {
                                status = STATUS_ACCESS_VIOLATION;
                            }
                        }
                        write_io_status(request.context.io_status_block, status, true);
                        complete_device_ioctl(win_emu, request.context, status, false);
                    }

                    if (request.profile_since && request.profile_operation)
                    {
                        const auto elapsed = std::chrono::steady_clock::now() - *request.profile_since;
                        win_emu.afd_diagnostics.record_completion(
                            *request.profile_operation, static_cast<uint32_t>(status),
                            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
                    }
                    this->active_pending_ = nullptr;
                    this->executing_delayed_ioctl_ = false;
                    this->pending_requests_.erase(this->pending_requests_.begin() + static_cast<ptrdiff_t>(index));
                }
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                buffer.read_optional(this->creation_data);
                this->setup(buffer.read<socket_factory_wrapper>());
                buffer.read(this->non_blocking_);
                const auto count = buffer.read<uint64_t>();
                if (count > max_pending_requests)
                {
                    throw std::runtime_error("Serialized AFD pending request count exceeds limit");
                }
                this->pending_requests_.clear();
                for (uint64_t index = 0; index < count; ++index)
                {
                    this->pending_requests_.emplace_back(buffer);
                    buffer.read(this->pending_requests_.back());
                }
            }

            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                buffer.write_optional(this->creation_data);
                buffer.write(this->non_blocking_);
                buffer.write(static_cast<uint64_t>(this->pending_requests_.size()));
                for (const auto& request : this->pending_requests_)
                {
                    buffer.write(request);
                }
            }

            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                if (_AFD_BASE(c.io_control_code) != FSCTL_AFD_BASE)
                {
                    win_emu.log.error("Bad AFD IOCTL: 0x%X\n", static_cast<uint32_t>(c.io_control_code));
                    return STATUS_NOT_SUPPORTED;
                }

                const auto request = _AFD_REQUEST(c.io_control_code);
                const auto operation = profiled_operation(request);
                if (!operation || !win_emu.afd_diagnostics.enabled())
                {
                    return this->dispatch_ioctl(win_emu, c, request);
                }

                const bool retry = this->executing_delayed_ioctl_;
                const auto start = std::chrono::steady_clock::now();
                try
                {
                    const auto status = this->dispatch_ioctl(win_emu, c, request);
                    const auto nanos =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                    win_emu.afd_diagnostics.record_attempt(*operation, static_cast<uint32_t>(status), static_cast<uint64_t>(nanos), retry);
                    if (!retry && status == STATUS_PENDING && !this->pending_requests_.empty())
                    {
                        auto& pending = this->pending_requests_.back();
                        pending.profile_since = start;
                        pending.profile_operation = *operation;
                    }
                    return status;
                }
                catch (...)
                {
                    const auto nanos =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                    win_emu.afd_diagnostics.record_exception(*operation, static_cast<uint64_t>(nanos), retry);
                    throw;
                }
            }

            NTSTATUS dispatch_ioctl(windows_emulator& win_emu, const io_device_context& c, const ULONG request)
            {
                switch (request)
                {
                case AFD_BIND:
                    return this->ioctl_bind(win_emu, c);
                case AFD_CONNECT:
                    return this->ioctl_connect(win_emu, c);
                case AFD_START_LISTEN:
                    return this->ioctl_listen(win_emu, c);
                case AFD_WAIT_FOR_LISTEN:
                    return this->ioctl_wait_for_listen(win_emu, c);
                case AFD_ACCEPT:
                    return this->ioctl_accept(win_emu, c);
                case AFD_SEND:
                    return this->ioctl_send(win_emu, c);
                case AFD_RECEIVE:
                    return this->ioctl_receive(win_emu, c);
                case AFD_SEND_DATAGRAM:
                    return this->ioctl_send_datagram(win_emu, c);
                case AFD_RECEIVE_DATAGRAM:
                    return this->ioctl_receive_datagram(win_emu, c);
                case AFD_POLL:
                    return this->ioctl_poll(win_emu, c);
                case AFD_ADDRESS_LIST_QUERY:
                    return this->ioctl_address_list(win_emu, c);
                case AFD_GET_ADDRESS:
                    return this->ioctl_get_address(win_emu, c);
                case AFD_EVENT_SELECT:
                    return this->ioctl_event_select(win_emu, c);
                case AFD_ENUM_NETWORK_EVENTS:
                    return this->ioctl_enum_network_events(win_emu, c);
                case AFD_SET_CONTEXT:
                    this->update_shared_info(win_emu, c);
                    return STATUS_SUCCESS;
                case AFD_GET_INFORMATION:
                    return this->ioctl_information(win_emu, c, false);
                case AFD_SET_INFORMATION:
                    return this->ioctl_information(win_emu, c, true);
                case AFD_QUERY_HANDLES:
                case AFD_TRANSPORT_IOCTL:
                case AFD_PARTIAL_DISCONNECT:
                    return STATUS_SUCCESS;
                default:
                    win_emu.log.error("Unsupported AFD IOCTL: 0x%X (%u)\n", static_cast<uint32_t>(c.io_control_code),
                                      static_cast<uint32_t>(request));
                    return STATUS_NOT_SUPPORTED;
                }
            }

            NTSTATUS ioctl_address_list(windows_emulator& win_emu, const io_device_context& c)
            {
                if (!this->s_)
                {
                    return STATUS_INVALID_HANDLE;
                }
                if (c.input_buffer_length < sizeof(uint16_t) || c.output_buffer_length < sizeof(uint32_t))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                uint16_t family{};
                if (!win_emu.memory.try_read_memory(c.input_buffer, &family, sizeof(family)))
                {
                    return STATUS_ACCESS_VIOLATION;
                }
                std::vector<std::byte> data;
                const auto result = static_cast<NTSTATUS>(this->s_->query_address_list(family, data));
                if (result != STATUS_SUCCESS)
                {
                    return result;
                }
                if (data.size() < sizeof(uint32_t))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                uint32_t count{};
                memcpy(&count, data.data(), sizeof(count));
                size_t cursor = sizeof(count);
                size_t copied = cursor;
                for (uint32_t index = 0; index < count; ++index)
                {
                    if (data.size() - cursor < 4)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    uint16_t length{};
                    memcpy(&length, data.data() + cursor, sizeof(length));
                    if (data.size() - cursor - 4 < length)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    cursor += 4u + length;
                    if (cursor <= c.output_buffer_length)
                    {
                        copied = cursor;
                    }
                }
                if (cursor != data.size())
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (!win_emu.memory.try_write_memory(c.output_buffer, data.data(), copied))
                {
                    return STATUS_ACCESS_VIOLATION;
                }
                c.io_status_block.access([&](status_block& block) { block.Information = data.size(); });
                const auto status = copied == data.size() ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
                win_emu.log.info("AFD address list: family %u, %u addresses, %zu/%zu bytes, status 0x%08X\n", family, count, copied,
                                 data.size(), static_cast<uint32_t>(status));
                return status;
            }

            NTSTATUS ioctl_information(windows_emulator& win_emu, const io_device_context& c, const bool set)
            {
                if (!this->s_)
                {
                    return STATUS_INVALID_HANDLE;
                }
                if (!c.input_buffer || (!set && !c.output_buffer))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (c.input_buffer_length < sizeof(AFD_INFO) || (!set && c.output_buffer_length < sizeof(AFD_INFO)))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                auto request = win_emu.emu().read_memory<AFD_INFO>(c.input_buffer);
                NTSTATUS status = STATUS_INVALID_PARAMETER;
                if (set)
                {
                    switch (request.InformationClass)
                    {
                    case 2:
                        this->non_blocking_ = (request.Information & 0xff) != 0;
                        status = STATUS_SUCCESS;
                        break;
                    case 1:
                    case 6:
                    case 7:
                    case 11:
                    case 12:
                    case 15:
                        status = static_cast<NTSTATUS>(this->s_->set_information(request.InformationClass, request.Information));
                        break;
                    default:
                        break;
                    }
                }
                else
                {
                    switch (request.InformationClass)
                    {
                    case 4:
                        status = static_cast<NTSTATUS>(this->s_->query_information(4, request.Information));
                        if (status == STATUS_SUCCESS &&
                            std::any_of(this->pending_requests_.begin(), this->pending_requests_.end(), [](const pending_request& pending) {
                                const auto code = _AFD_REQUEST(pending.context.io_control_code);
                                return code == AFD_SEND || code == AFD_SEND_DATAGRAM;
                            }))
                        {
                            ++request.Information;
                        }
                        break;
                    case 5:
                        if (c.input_buffer_length > 0x10000)
                        {
                            return STATUS_INVALID_PARAMETER;
                        }
                        {
                            const auto parameters =
                                win_emu.emu().read_memory(c.input_buffer + sizeof(AFD_INFO), c.input_buffer_length - sizeof(AFD_INFO));
                            status = static_cast<NTSTATUS>(this->s_->query_information(5, request.Information, parameters));
                        }
                        break;
                    case 3:
                    case 6:
                    case 7:
                    case 8:
                    case 10:
                    case 14:
                        status = static_cast<NTSTATUS>(this->s_->query_information(request.InformationClass, request.Information));
                        break;
                    default:
                        break;
                    }
                    if (status == STATUS_SUCCESS)
                    {
                        win_emu.emu().write_memory(c.output_buffer, request);
                        c.io_status_block.access([](status_block& block) { block.Information = sizeof(AFD_INFO); });
                    }
                }
                win_emu.log.info("AFD %s information: class %u, value 0x%" PRIx64 ", status 0x%08X\n", set ? "set" : "get",
                                 request.InformationClass, request.Information, static_cast<uint32_t>(status));
                return status;
            }

            NTSTATUS ioctl_connect(windows_emulator& win_emu, const io_device_context& c)
            {
                if (!this->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint socket!");
                }

                auto data = this->active_pending_ && !this->active_pending_->connect_input.empty()
                                ? this->active_pending_->connect_input
                                : win_emu.emu().read_memory(c.input_buffer, c.input_buffer_length);

                // AFD_CONNECT_JOIN_INFO_TL has pointer-aligned handles before the address.
                constexpr auto address_offset = offsetof(AFD_CONNECT_JOIN_INFO_TL<Traits>, RemoteAddress);

                if (data.size() < address_offset)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto addr = convert_to_host_address(win_emu, std::span(data).subspan(address_offset));

                if (!this->s_->connect(addr))
                {
                    const auto error = this->s_->get_last_error();
                    if (error == SERR(EWOULDBLOCK))
                    {
                        const auto status = this->delay_ioctrl(c, false);
                        if (status == STATUS_PENDING && !this->executing_delayed_ioctl_)
                        {
                            this->pending_requests_.back().connect_input = std::move(data);
                        }
                        return status;
                    }

                    if (this->executing_delayed_ioctl_ && error == SERR(EISCONN))
                    {
                        return STATUS_SUCCESS;
                    }

                    return STATUS_UNSUCCESSFUL;
                }

                return STATUS_SUCCESS;
            }

            NTSTATUS ioctl_bind(windows_emulator& win_emu, const io_device_context& c) const
            {
                if (!this->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint socket!");
                }

                auto data = win_emu.emu().read_memory(c.input_buffer, c.input_buffer_length);

                constexpr auto address_offset = 4;

                if (data.size() < address_offset)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto addr = convert_to_host_address(win_emu, std::span(data).subspan(address_offset));

                if (!this->s_->bind(addr))
                {
                    return STATUS_ADDRESS_ALREADY_ASSOCIATED;
                }

                return STATUS_SUCCESS;
            }

            NTSTATUS ioctl_listen(windows_emulator& win_emu, const io_device_context& c) const
            {
                if (!this->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint socket!");
                }

                if (c.input_buffer_length < sizeof(AFD_LISTEN_INFO))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto listen_info = win_emu.emu().read_memory<AFD_LISTEN_INFO>(c.input_buffer);

                if (!this->s_->listen(static_cast<int>(listen_info.MaximumConnectionQueue)))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                return STATUS_SUCCESS;
            }

            NTSTATUS ioctl_wait_for_listen(windows_emulator& win_emu, const io_device_context& c)
            {
                if (!this->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint socket!");
                }

                if (c.output_buffer_length < sizeof(AFD_LISTEN_RESPONSE_INFO))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                network::address remote_address{};
                auto accepted_socket_ptr = this->s_->accept(remote_address);

                if (!accepted_socket_ptr)
                {
                    const auto error = this->s_->get_last_error();
                    if (error == SERR(EWOULDBLOCK))
                    {
                        return this->delay_ioctrl(c, true);
                    }

                    return STATUS_UNSUCCESSFUL;
                }

                if (!remote_address.is_ipv4())
                {
                    throw std::runtime_error("Unsupported address family");
                }

                pending_connection pending{};
                pending.remote_address = remote_address;
                pending.accepted_socket = std::move(accepted_socket_ptr);

                LONG sequence = next_sequence_++;
                pending_connections_.try_emplace(sequence, std::move(pending));

                AFD_LISTEN_RESPONSE_INFO response{};
                response.Sequence = sequence;

                auto transport_buffer = convert_to_win_address(win_emu, remote_address);
                memcpy(&response.RemoteAddress, transport_buffer.data(), sizeof(win_sockaddr));

                win_emu.emu().write_memory<AFD_LISTEN_RESPONSE_INFO>(c.output_buffer, response);

                if (c.io_status_block)
                {
                    status_block block{};
                    block.Information = sizeof(AFD_LISTEN_RESPONSE_INFO);
                    c.io_status_block.write(block);
                }

                return STATUS_SUCCESS;
            }

            NTSTATUS ioctl_accept(windows_emulator& win_emu, const io_device_context& c)
            {
                if (!this->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint socket!");
                }

                if (c.input_buffer_length < sizeof(AFD_ACCEPT_INFO))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto accept_info = win_emu.emu().read_memory<AFD_ACCEPT_INFO>(c.input_buffer);

                const auto it = pending_connections_.find(accept_info.Sequence);
                if (it == pending_connections_.end())
                {
                    return STATUS_INVALID_PARAMETER;
                }

                auto& accepted_socket = it->second.accepted_socket;

                auto* target_device = win_emu.process.devices.get(accept_info.AcceptHandle);
                if (!target_device)
                {
                    return STATUS_INVALID_HANDLE;
                }

                auto* target_endpoint = target_device->get_internal_device<afd_endpoint>();
                if (!target_endpoint)
                {
                    return STATUS_INVALID_HANDLE;
                }

                target_endpoint->s_ = std::move(accepted_socket);

                pending_connections_.erase(it);

                return STATUS_SUCCESS;
            }

            NTSTATUS ioctl_receive(windows_emulator& win_emu, const io_device_context& c)
            {
                if (!this->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint socket!");
                }

                auto& emu = win_emu.emu();
                if (c.input_buffer_length < sizeof(AFD_RECV_INFO<Traits>))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                std::vector<EMU_WSABUF<Traits>> loaded_buffers;
                const bool use_captured = this->active_pending_ && !this->active_pending_->stream_buffers.empty();
                AFD_RECV_INFO<Traits> receive_info{};
                if (use_captured)
                {
                    receive_info.AfdFlags = this->active_pending_->stream_afd_flags;
                }
                else
                {
                    receive_info = emu.read_memory<AFD_RECV_INFO<Traits>>(c.input_buffer);
                    // TDI_RECEIVE_PEEK must not consume the packet; host i_socket has no peek operation.
                    if ((receive_info.TdiFlags & tdi_receive_peek) != 0)
                    {
                        return STATUS_NOT_SUPPORTED;
                    }
                    if (!this->executing_delayed_ioctl_)
                    {
                        win_emu.afd_diagnostics.record_buffer_count(afd_profile::operation::receive, receive_info.BufferCount);
                    }
                }
                if (!use_captured)
                {
                    const auto status =
                        load_stream_buffers<Traits>(win_emu.memory, receive_info.BufferArray, receive_info.BufferCount, loaded_buffers);
                    if (status != STATUS_SUCCESS)
                    {
                        return status;
                    }
                }
                const auto& buffers = use_captured ? this->active_pending_->stream_buffers : loaded_buffers;

                std::vector<std::byte> host_buffer;
                try
                {
                    host_buffer.resize(stream_transfer_size<Traits>(buffers));
                }
                catch (const std::bad_alloc&)
                {
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                const auto bytes_received = this->s_->recv(host_buffer);
                if (bytes_received < 0)
                {
                    const auto error = this->s_->get_last_error();
                    if (error == SERR(EWOULDBLOCK))
                    {
                        const auto status = this->pend_or_would_block(c, true, receive_info.AfdFlags);
                        if (status == STATUS_PENDING && !this->executing_delayed_ioctl_)
                        {
                            auto& pending = this->pending_requests_.back();
                            pending.stream_buffers = buffers;
                            pending.stream_afd_flags = receive_info.AfdFlags;
                        }
                        return status;
                    }
                    if (error == SERR(ECONNRESET))
                    {
                        return STATUS_CONNECTION_RESET;
                    }
                    return STATUS_UNSUCCESSFUL;
                }
                if (static_cast<size_t>(bytes_received) > host_buffer.size())
                {
                    return STATUS_UNSUCCESSFUL;
                }

                size_t copied = 0;
                for (const auto& buffer : buffers)
                {
                    const auto length = std::min<size_t>(buffer.len, static_cast<size_t>(bytes_received) - copied);
                    if (length && !win_emu.memory.try_write_memory(buffer.buf, host_buffer.data() + copied, length))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    copied += length;
                    if (copied == static_cast<size_t>(bytes_received))
                    {
                        break;
                    }
                }
                win_emu.afd_diagnostics.record_transfer(afd_profile::operation::receive, static_cast<uint64_t>(bytes_received));

                if (c.io_status_block)
                {
                    status_block block{};
                    block.Information = static_cast<uint32_t>(bytes_received);
                    c.io_status_block.write(block);
                }
                return STATUS_SUCCESS;
            }

            NTSTATUS ioctl_send(windows_emulator& win_emu, const io_device_context& c)
            {
                if (!this->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint socket!");
                }

                auto& emu = win_emu.emu();
                if (c.input_buffer_length < sizeof(AFD_SEND_INFO<Traits>))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                std::vector<EMU_WSABUF<Traits>> loaded_buffers;
                const bool use_captured = this->active_pending_ && !this->active_pending_->stream_buffers.empty();
                AFD_SEND_INFO<Traits> send_info{};
                if (use_captured)
                {
                    send_info.AfdFlags = this->active_pending_->stream_afd_flags;
                }
                else
                {
                    send_info = emu.read_memory<AFD_SEND_INFO<Traits>>(c.input_buffer);
                    if (!this->executing_delayed_ioctl_)
                    {
                        win_emu.afd_diagnostics.record_buffer_count(afd_profile::operation::send, send_info.BufferCount);
                    }
                }
                if (!use_captured)
                {
                    const auto status =
                        load_stream_buffers<Traits>(win_emu.memory, send_info.BufferArray, send_info.BufferCount, loaded_buffers);
                    if (status != STATUS_SUCCESS)
                    {
                        return status;
                    }
                }
                const auto& buffers = use_captured ? this->active_pending_->stream_buffers : loaded_buffers;

                std::vector<std::byte> host_buffer;
                try
                {
                    host_buffer.resize(stream_transfer_size<Traits>(buffers));
                }
                catch (const std::bad_alloc&)
                {
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                size_t copied = 0;
                for (const auto& buffer : buffers)
                {
                    const auto length = std::min<size_t>(buffer.len, host_buffer.size() - copied);
                    if (length && !win_emu.memory.try_read_memory(buffer.buf, host_buffer.data() + copied, length))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    copied += length;
                    if (copied == host_buffer.size())
                    {
                        break;
                    }
                }

                const auto bytes_sent = this->s_->send(host_buffer);
                if (bytes_sent < 0)
                {
                    const auto error = this->s_->get_last_error();
                    if (error == SERR(EWOULDBLOCK))
                    {
                        const auto status = this->pend_or_would_block(c, false, send_info.AfdFlags);
                        if (status == STATUS_PENDING && !this->executing_delayed_ioctl_)
                        {
                            auto& pending = this->pending_requests_.back();
                            pending.stream_buffers = buffers;
                            pending.stream_afd_flags = send_info.AfdFlags;
                        }
                        return status;
                    }
                    if (error == SERR(ECONNRESET))
                    {
                        return STATUS_CONNECTION_RESET;
                    }
                    return STATUS_UNSUCCESSFUL;
                }
                if (static_cast<size_t>(bytes_sent) > host_buffer.size())
                {
                    return STATUS_UNSUCCESSFUL;
                }

                if (c.io_status_block)
                {
                    status_block block{};
                    block.Information = static_cast<uint32_t>(bytes_sent);
                    c.io_status_block.write(block);
                }
                win_emu.afd_diagnostics.record_transfer(afd_profile::operation::send, static_cast<uint64_t>(bytes_sent));
                return STATUS_SUCCESS;
            }

            static std::vector<const afd_endpoint*> resolve_endpoints(windows_emulator& win_emu,
                                                                      const std::span<const AFD_POLL_HANDLE_INFO<Traits>> handles)
            {
                auto& proc = win_emu.process;

                std::vector<const afd_endpoint*> endpoints{};
                endpoints.reserve(handles.size());

                for (const auto& handle : handles)
                {
                    auto* device = proc.devices.get(handle.Handle);
                    if (!device)
                    {
                        throw std::runtime_error("Bad device!");
                    }

                    const auto* endpoint = device->template get_internal_device<afd_endpoint<Traits>>();
                    if (!endpoint || !endpoint->s_)
                    {
                        throw std::runtime_error("Invalid AFD endpoint!");
                    }

                    endpoints.push_back(endpoint);
                }

                return endpoints;
            }

            static NTSTATUS perform_poll(windows_emulator& win_emu, const io_device_context& c,
                                         const std::span<const afd_endpoint* const> endpoints,
                                         const std::span<const AFD_POLL_HANDLE_INFO<Traits>> handles)
            {
                const auto entry_count = std::min(endpoints.size(), handles.size());

                std::vector<network::poll_entry> poll_data{};
                poll_data.resize(entry_count);

                auto endpoint_it = endpoints.begin();
                auto handle_it = handles.begin();

                for (auto& pfd : poll_data)
                {
                    const auto* endpoint = *endpoint_it++;
                    const auto& handle = *handle_it++;

                    pfd.s = endpoint->s_.get();
                    pfd.events = map_afd_request_events_to_socket(handle.PollEvents);
                    pfd.revents = pfd.events;
                }

                const auto count = win_emu.socket_factory().poll_sockets(poll_data);
                if (count <= 0)
                {
                    return STATUS_PENDING;
                }

                constexpr auto info_size = offsetof(AFD_POLL_INFO<Traits>, Handles);
                const emulator_object<AFD_POLL_HANDLE_INFO<Traits>> handle_info_obj{win_emu.emu(), c.input_buffer + info_size};

                size_t current_index = 0;

                for (size_t source_index = 0; source_index < poll_data.size(); ++source_index)
                {
                    const auto& pfd = poll_data.at(source_index);
                    const auto* endpoint = endpoints.subspan(source_index, 1).front();
                    const auto& handle = handles.subspan(source_index, 1).front();

                    if (pfd.revents == 0)
                    {
                        continue;
                    }

                    const bool is_connecting = std::any_of(
                        endpoint->pending_requests_.begin(), endpoint->pending_requests_.end(),
                        [](const pending_request& pending) { return _AFD_REQUEST(pending.context.io_control_code) == AFD_CONNECT; });

                    auto entry = handle;
                    entry.PollEvents =
                        map_socket_response_events_to_afd(pfd.revents, handle.PollEvents, pfd.s->is_listening(), is_connecting);
                    entry.Status = STATUS_SUCCESS;

                    handle_info_obj.write(entry, current_index++);
                }

                assert(current_index == static_cast<size_t>(count));

                const emulator_object<AFD_POLL_INFO<Traits>> info_obj{win_emu.emu(), c.input_buffer};
                info_obj.access([&](AFD_POLL_INFO<Traits>& info) {
                    info.NumberOfHandles = static_cast<ULONG>(current_index); //
                });

                if (c.io_status_block)
                {
                    status_block block{};
                    block.Information = info_size + (sizeof(AFD_POLL_HANDLE_INFO<Traits>) * current_index);
                    c.io_status_block.write(block);
                }

                return STATUS_SUCCESS;
            }

            NTSTATUS ioctl_poll(windows_emulator& win_emu, const io_device_context& c)
            {
                AFD_POLL_INFO<Traits> info{};
                std::vector<AFD_POLL_HANDLE_INFO<Traits>> handles;
                if (this->active_pending_ && this->active_pending_->poll_captured)
                {
                    handles = this->active_pending_->poll_handles;
                }
                else
                {
                    auto captured = get_poll_info<Traits>(win_emu, c);
                    info = captured.first;
                    handles = std::move(captured.second);
                }
                const auto endpoints = resolve_endpoints(win_emu, handles);
                const auto status = perform_poll(win_emu, c, endpoints, handles);
                if (status != STATUS_PENDING)
                {
                    return status;
                }
                if (this->executing_delayed_ioctl_)
                {
                    return STATUS_PENDING;
                }
                if (!info.Timeout.QuadPart)
                {
                    const ULONG count = 0;
                    const auto number_offset = offsetof(AFD_POLL_INFO<Traits>, NumberOfHandles);
                    if (!win_emu.memory.try_write_memory(c.input_buffer + number_offset, &count, sizeof(count)))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    return STATUS_TIMEOUT;
                }
                std::optional<std::chrono::steady_clock::time_point> timeout{};
                if (info.Timeout.QuadPart != std::numeric_limits<int64_t>::max())
                {
                    timeout = utils::convert_delay_interval_to_time_point(win_emu.clock(), info.Timeout,
                                                                          {.QuadPart = std::numeric_limits<int64_t>::max()});
                }
                const auto queued = this->delay_ioctrl(c, {}, timeout);
                if (queued == STATUS_PENDING)
                {
                    auto& pending = this->pending_requests_.back();
                    pending.poll_handles = std::move(handles);
                    pending.poll_captured = true;
                }
                return queued;
            }

            NTSTATUS ioctl_receive_datagram(windows_emulator& win_emu, const io_device_context& c)
            {
                if (!this->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint socket!");
                }
                if (c.input_buffer_length < sizeof(AFD_RECV_DATAGRAM_INFO<Traits>))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                auto& memory = win_emu.memory;
                std::vector<EMU_WSABUF<Traits>> loaded_buffers;
                const bool use_captured = this->active_pending_ && !this->active_pending_->datagram_buffers.empty();
                AFD_RECV_DATAGRAM_INFO<Traits> info{};
                if (use_captured)
                {
                    info.AfdFlags = this->active_pending_->datagram_afd_flags;
                    info.TdiFlags = this->active_pending_->datagram_tdi_flags;
                    info.Address = static_cast<typename Traits::PVOID>(this->active_pending_->datagram_address);
                    info.AddressLength = static_cast<typename Traits::PVOID>(this->active_pending_->datagram_address_length);
                }
                else
                {
                    if (!memory.try_read_memory(c.input_buffer, &info, sizeof(info)))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    if (!this->executing_delayed_ioctl_)
                    {
                        win_emu.afd_diagnostics.record_buffer_count(afd_profile::operation::receive_datagram, info.BufferCount);
                    }
                    const auto status = load_stream_buffers<Traits>(memory, info.BufferArray, info.BufferCount, loaded_buffers);
                    if (status != STATUS_SUCCESS)
                    {
                        return status;
                    }
                }
                if (info.TdiFlags != tdi_receive_normal)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                const auto& buffers = use_captured ? this->active_pending_->datagram_buffers : loaded_buffers;

                // Probe the entire possible output prefix before recvfrom consumes a datagram.
                size_t remaining = max_datagram_bytes;
                for (const auto& buffer : buffers)
                {
                    const auto length = std::min<size_t>(buffer.len, remaining);
                    if (length && !is_writable_guest_range(memory, buffer.buf, length))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    remaining -= length;
                    if (remaining == 0)
                    {
                        break;
                    }
                }
                if (c.io_status_block && !is_writable_guest_range(memory, c.io_status_block.value(), sizeof(status_block)))
                {
                    return STATUS_ACCESS_VIOLATION;
                }
                ULONG address_capacity{};
                if (info.Address && info.AddressLength)
                {
                    if (!is_writable_guest_range(memory, info.AddressLength, sizeof(address_capacity)) ||
                        !memory.try_read_memory(info.AddressLength, &address_capacity, sizeof(address_capacity)))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    const auto address_bytes = this->creation_data && this->creation_data->address_family == 2 ? sizeof(win_sockaddr_in)
                                                                                                               : sizeof(win_sockaddr_in6);
                    if (!is_writable_guest_range(memory, info.Address, std::min<size_t>(address_capacity, address_bytes)))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                }

                // A full-size UDP staging buffer keeps recvfrom from consuming a packet as WSAEMSGSIZE
                // before the guest WSABUF array can receive its ordered prefix.
                std::vector<std::byte> data;
                try
                {
                    data.resize(max_datagram_bytes);
                }
                catch (const std::bad_alloc&)
                {
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                network::address from{};
                const auto received = this->s_->recvfrom(from, data);
                if (received < 0)
                {
                    const auto error = this->s_->get_last_error();
                    if (error == SERR(EWOULDBLOCK))
                    {
                        const auto status = this->pend_or_would_block(c, true, info.AfdFlags);
                        if (status == STATUS_PENDING && !this->executing_delayed_ioctl_)
                        {
                            auto& pending = this->pending_requests_.back();
                            pending.datagram_buffers = std::move(loaded_buffers);
                            pending.datagram_afd_flags = info.AfdFlags;
                            pending.datagram_tdi_flags = info.TdiFlags;
                            pending.datagram_address = info.Address;
                            pending.datagram_address_length = info.AddressLength;
                        }
                        return status;
                    }
                    if (error == SERR(EMSGSIZE))
                    {
                        return STATUS_BUFFER_OVERFLOW;
                    }
                    if (error == SERR(ECONNRESET))
                    {
                        return STATUS_CONNECTION_RESET;
                    }
                    return STATUS_UNSUCCESSFUL;
                }
                if (static_cast<size_t>(received) > data.size())
                {
                    return STATUS_UNSUCCESSFUL;
                }

                const auto payload_size = static_cast<size_t>(received);
                size_t copied = 0;
                for (const auto& buffer : buffers)
                {
                    const auto length = std::min<size_t>(buffer.len, payload_size - copied);
                    if (length && !memory.try_write_memory(buffer.buf, data.data() + copied, length))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    copied += length;
                    if (copied == payload_size)
                    {
                        break;
                    }
                }
                win_emu.afd_diagnostics.record_transfer(afd_profile::operation::receive_datagram, copied);

                if (info.Address && info.AddressLength)
                {
                    const auto win_from = convert_to_win_address(win_emu, from);
                    const auto address_size = std::min<size_t>(win_from.size(), address_capacity);
                    if (address_size && !memory.try_write_memory(info.Address, win_from.data(), address_size))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    const auto written_length = static_cast<ULONG>(address_size);
                    if (!memory.try_write_memory(info.AddressLength, &written_length, sizeof(written_length)))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                }

                if (c.io_status_block)
                {
                    status_block block{};
                    block.Information = copied;
                    c.io_status_block.write(block);
                }
                return copied == payload_size ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
            }

            NTSTATUS ioctl_send_datagram(windows_emulator& win_emu, const io_device_context& c)
            {
                if (!this->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint socket!");
                }
                if (c.input_buffer_length < sizeof(AFD_SEND_DATAGRAM_INFO<Traits>))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                auto& memory = win_emu.memory;
                std::vector<EMU_WSABUF<Traits>> loaded_buffers;
                std::vector<std::byte> loaded_target;
                const bool use_captured = this->active_pending_ && !this->active_pending_->datagram_buffers.empty();
                AFD_SEND_DATAGRAM_INFO<Traits> info{};
                if (use_captured)
                {
                    info.AfdFlags = this->active_pending_->datagram_afd_flags;
                }
                else
                {
                    if (!memory.try_read_memory(c.input_buffer, &info, sizeof(info)))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    if (!this->executing_delayed_ioctl_)
                    {
                        win_emu.afd_diagnostics.record_buffer_count(afd_profile::operation::send_datagram, info.BufferCount);
                    }
                    const auto status = load_stream_buffers<Traits>(memory, info.BufferArray, info.BufferCount, loaded_buffers);
                    if (status != STATUS_SUCCESS)
                    {
                        return status;
                    }
                    const auto length = info.TdiConnInfo.RemoteAddressLength;
                    if (length < static_cast<LONG>(sizeof(win_sockaddr)) || length > static_cast<LONG>(sizeof(win_sockaddr_in6)))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    loaded_target.resize(static_cast<size_t>(length));
                    if (!memory.try_read_memory(info.TdiConnInfo.RemoteAddress, loaded_target.data(), loaded_target.size()))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                }
                const auto& buffers = use_captured ? this->active_pending_->datagram_buffers : loaded_buffers;
                const auto& target_bytes = use_captured ? this->active_pending_->datagram_target : loaded_target;

                size_t payload_size = 0;
                for (const auto& buffer : buffers)
                {
                    if (buffer.len > max_datagram_bytes - payload_size)
                    {
                        return STATUS_INVALID_BUFFER_SIZE;
                    }
                    payload_size += buffer.len;
                }
                std::vector<std::byte> data;
                try
                {
                    data.resize(payload_size);
                }
                catch (const std::bad_alloc&)
                {
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                size_t copied = 0;
                for (const auto& buffer : buffers)
                {
                    if (buffer.len && !memory.try_read_memory(buffer.buf, data.data() + copied, buffer.len))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    copied += buffer.len;
                }

                network::address target{};
                try
                {
                    target = convert_to_host_address(win_emu, target_bytes);
                }
                catch (const std::runtime_error&)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                const auto sent = this->s_->sendto(target, data);
                if (sent < 0)
                {
                    const auto error = this->s_->get_last_error();
                    if (error == SERR(EWOULDBLOCK))
                    {
                        const auto status = this->pend_or_would_block(c, false, info.AfdFlags);
                        if (status == STATUS_PENDING && !this->executing_delayed_ioctl_)
                        {
                            auto& pending = this->pending_requests_.back();
                            pending.datagram_buffers = std::move(loaded_buffers);
                            pending.datagram_target = std::move(loaded_target);
                            pending.datagram_afd_flags = info.AfdFlags;
                        }
                        return status;
                    }
                    if (error == SERR(EMSGSIZE))
                    {
                        return STATUS_INVALID_BUFFER_SIZE;
                    }
                    if (error == SERR(ECONNRESET))
                    {
                        return STATUS_CONNECTION_RESET;
                    }
                    return STATUS_UNSUCCESSFUL;
                }
                if (static_cast<size_t>(sent) != data.size())
                {
                    return STATUS_UNSUCCESSFUL;
                }
                if (c.io_status_block)
                {
                    status_block block{};
                    block.Information = static_cast<uint32_t>(sent);
                    c.io_status_block.write(block);
                }
                win_emu.afd_diagnostics.record_transfer(afd_profile::operation::send_datagram, static_cast<uint64_t>(sent));
                return STATUS_SUCCESS;
            }

            NTSTATUS ioctl_get_address(windows_emulator& win_emu, const io_device_context& c) const
            {
                if (!this->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint socket!");
                }

                const auto local_address = this->s_->get_local_address();
                if (!local_address)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<std::byte> win_addr_bytes = convert_to_win_address(win_emu, *local_address);

                if (c.output_buffer_length < win_addr_bytes.size())
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                win_emu.emu().write_memory(c.output_buffer, win_addr_bytes.data(), win_addr_bytes.size());

                if (c.io_status_block)
                {
                    status_block block{};
                    block.Information = static_cast<ULONG>(win_addr_bytes.size());
                    c.io_status_block.write(block);
                }

                return STATUS_SUCCESS;
            }

            NTSTATUS ioctl_event_select(windows_emulator& win_emu, const io_device_context& c)
            {
                if (!this->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint socket!");
                }

                if (c.input_buffer_length < sizeof(AFD_EVENT_SELECT_INFO))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto select_info = win_emu.emu().read_memory<AFD_EVENT_SELECT_INFO>(c.input_buffer);

                this->event_select_event_ = select_info.Event;
                this->event_select_mask_ = select_info.PollEvents;
                this->triggered_events_ = 0;

                if (auto* event = win_emu.process.events.get(select_info.Event))
                {
                    event->signaled = false;
                }

                return STATUS_SUCCESS;
            }

            NTSTATUS ioctl_enum_network_events(windows_emulator& win_emu, const io_device_context& c)
            {
                if (!this->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint socket!");
                }

                if (c.output_buffer_length < 56)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                if (c.input_buffer)
                {
                    if (c.input_buffer_length == 0)
                    {
                        handle h{};
                        h.bits = c.input_buffer;

                        if (auto* event = win_emu.process.events.get(h))
                        {
                            event->signaled = false;
                        }
                    }
                    else
                    {
                        return STATUS_NOT_SUPPORTED;
                    }
                }

                win_emu.emu().write_memory(c.output_buffer, this->triggered_events_);
                this->triggered_events_ = 0;

                if (c.io_status_block)
                {
                    status_block block{};
                    block.Information = 56;
                    c.io_status_block.write(block);
                }

                return STATUS_SUCCESS;
            }
        };

        template <typename Traits>
        struct afd_async_connect_hlp : stateless_device
        {
            bool may_return_pending() const override
            {
                return true;
            }

            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                if (c.io_control_code != 0x12007)
                {
                    return STATUS_NOT_SUPPORTED;
                }

                if (c.input_buffer_length < sizeof(AFD_CONNECT_JOIN_INFO_TL<Traits>))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                handle target_handle{};
                target_handle.bits = win_emu.emu().read_memory<typename Traits::HANDLE>(
                    c.input_buffer + offsetof(AFD_CONNECT_JOIN_INFO_TL<Traits>, ConnectEndpoint));

                auto* target_device = win_emu.process.devices.get(target_handle);
                if (!target_device)
                {
                    return STATUS_INVALID_HANDLE;
                }

                auto* target_endpoint = target_device->get_internal_device<afd_endpoint<Traits>>();
                if (!target_endpoint)
                {
                    return STATUS_INVALID_HANDLE;
                }

                // The helper's outer execute_ioctl owns the request's IOSB and completion.
                // Calling the endpoint wrapper here would deliver synchronous APC/IOCP twice.
                return target_endpoint->io_control(win_emu, c);
            }
        };
    }

    std::unique_ptr<io_device> create_afd_endpoint(const device_creation_context& context)
    {
        if (context.is_32_bit)
        {
            return std::make_unique<afd_endpoint<EmulatorTraits<Emu32>>>();
        }

        return std::make_unique<afd_endpoint<EmulatorTraits<Emu64>>>();
    }

    std::unique_ptr<io_device> create_afd_async_connect_hlp(const device_creation_context& context)
    {
        if (context.is_32_bit)
        {
            return std::make_unique<afd_async_connect_hlp<EmulatorTraits<Emu32>>>();
        }

        return std::make_unique<afd_async_connect_hlp<EmulatorTraits<Emu64>>>();
    }

} // namespace sogen
