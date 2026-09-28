#include "socket_wrapper.hpp"
#include <array>
#include <cassert>
#include <utils/nt_handle.hpp>

namespace sogen
{

    namespace network
    {
        socket_wrapper::socket_wrapper(SOCKET s)
            : socket_(s)
        {
            socklen_t length = sizeof(this->socket_type_);
            getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&this->socket_type_), &length);
        }

        socket_wrapper::socket_wrapper(const int af, const int type, const int protocol)
            : socket_(af, type, protocol), socket_type_(type)
        {
        }

        void socket_wrapper::set_blocking(const bool blocking)
        {
            this->socket_.set_blocking(blocking);
        }

        int socket_wrapper::get_last_error()
        {
#ifndef _WIN32
            if (this->synthetic_error_)
            {
                return this->synthetic_error_;
            }
#endif
            return GET_SOCKET_ERROR();
        }

        namespace
        {
            uint32_t socket_control(const SOCKET socket_handle, const uint32_t code, const std::span<const std::byte> input,
                                    const std::span<std::byte> output, uint64_t& information)
            {
#ifdef _WIN32
                struct io_status
                {
                    uintptr_t status;
                    uintptr_t information;
                };

                using ioctl_function = LONG(WINAPI*)(HANDLE, HANDLE, void*, void*, io_status*, ULONG, const void*, ULONG, void*, ULONG);
                static const auto ioctl =
                    reinterpret_cast<ioctl_function>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDeviceIoControlFile"));
                if (!ioctl)
                {
                    return 0xc00000bb;
                }
                const utils::nt::handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
                if (!event)
                {
                    return 0xc000009a;
                }
                io_status completion{};
                auto status = static_cast<uint32_t>(ioctl(reinterpret_cast<HANDLE>(socket_handle), event, nullptr, nullptr, &completion,
                                                          code, input.data(), static_cast<ULONG>(input.size()), output.data(),
                                                          static_cast<ULONG>(output.size())));
                if (status == 0x103)
                {
                    WaitForSingleObject(event, INFINITE);
                    status = static_cast<uint32_t>(completion.status);
                }
                information = completion.information;
                return status;
#else
                (void)socket_handle;
                (void)code;
                (void)input;
                (void)output;
                (void)information;
                return 0xc00000bb;
#endif
            }

            uint32_t socket_information(const SOCKET socket_handle, const bool set, const uint32_t information_class, uint64_t& value,
                                        const std::span<const std::byte> parameters)
            {
                struct information_request
                {
                    uint32_t information_class;
                    uint32_t reserved;
                    uint64_t value;
                };

                information_request request{.information_class = information_class, .reserved = 0, .value = value};
                std::vector<std::byte> input(sizeof(request) + parameters.size());
                memcpy(input.data(), &request, sizeof(request));
                if (!parameters.empty())
                {
                    memcpy(input.data() + sizeof(request), parameters.data(), parameters.size());
                }
                const auto output = set ? std::span<std::byte>{} : std::as_writable_bytes(std::span(&request, 1));
                uint64_t information{};
                const auto status = socket_control(socket_handle, set ? 0x1203b : 0x1207b, input, output, information);
                if (status == 0 && !set)
                {
                    value = request.value;
                }
                return status;
            }
        }

        uint32_t socket_wrapper::partial_disconnect(const uint32_t mode, const int64_t timeout)
        {
#ifdef _WIN32
            struct alignas(8) request
            {
                uint32_t disconnect_mode;
                int64_t timeout;
            };
            static_assert(sizeof(request) == 16);
            const request input{mode, timeout};
            uint64_t information{};
            return socket_control(this->socket_.get_socket(), 0x1202B, std::as_bytes(std::span(&input, 1)), {}, information);
#else
            (void)timeout;
            const auto native = this->socket_.get_socket();
            if (mode & 4)
            {
                if (this->socket_type_ != SOCK_DGRAM)
                {
                    linger abortive{1, 0};
                    setsockopt(native, SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive));
                    this->socket_.close();
                }
                this->disconnect_mode_ |= 4;
                return 0;
            }
            if ((mode & 1) && !(this->disconnect_mode_ & 1))
            {
                if (this->socket_type_ != SOCK_DGRAM && ::shutdown(native, SHUT_WR) != 0)
                {
                    return 0xC0000001;
                }
                this->disconnect_mode_ |= 1;
            }
            if (this->socket_type_ == SOCK_DGRAM)
            {
                this->disconnect_mode_ |= mode & (2 | 8);
            }
            // TCP modes 2/8 leave receive state intact; UDP sendto/recvfrom
            // remain usable after modes 1/4/8 in the native localhost probe.
            return 0;
#endif
        }

        uint32_t socket_wrapper::query_information(const uint32_t information_class, uint64_t& value,
                                                   const std::span<const std::byte> parameters)
        {
            return socket_information(this->socket_.get_socket(), false, information_class, value, parameters);
        }

        uint32_t socket_wrapper::set_information(const uint32_t information_class, uint64_t value)
        {
            return socket_information(this->socket_.get_socket(), true, information_class, value, {});
        }

        uint32_t socket_wrapper::query_address_list(const uint16_t family, std::vector<std::byte>& data)
        {
            data.resize(sizeof(uint32_t));
            for (unsigned int attempt = 0; attempt < 8; ++attempt)
            {
                uint64_t information{};
                const auto status =
                    socket_control(this->socket_.get_socket(), 0x120b3, std::as_bytes(std::span(&family, 1)), data, information);
                if (status == 0)
                {
                    if (information < sizeof(uint32_t) || information > data.size())
                    {
                        return 0xc000000d;
                    }
                    data.resize(static_cast<size_t>(information));
                    return 0;
                }
                if (status != 0x80000005 || information <= data.size())
                {
                    return status;
                }
                if (information > UINT32_MAX)
                {
                    return 0xc000009a;
                }
                data.resize(static_cast<size_t>(information));
            }
            return 0xc000022d;
        }

        uint32_t socket_wrapper::sort_address_list(std::vector<address_sort_entry>& addresses)
        {
#ifdef _WIN32
            if (addresses.empty() || addresses.size() > 1024)
            {
                return 0xc000000d;
            }

            const size_t prefix = offsetof(SOCKET_ADDRESS_LIST, Address);
            const size_t length = prefix + addresses.size() * sizeof(SOCKET_ADDRESS);
            std::vector<std::byte> input(length);
            std::vector<std::byte> output(length, std::byte{0xa5});
            const auto count = static_cast<int>(addresses.size());
            memcpy(input.data(), &count, sizeof(count));
            for (size_t index = 0; index < addresses.size(); ++index)
            {
                const SOCKET_ADDRESS entry{reinterpret_cast<LPSOCKADDR>(&addresses[index].address), sizeof(sockaddr_in6)};
                memcpy(input.data() + prefix + index * sizeof(entry), &entry, sizeof(entry));
            }

            struct transport_request
            {
                uint32_t type{3};
                uint32_t reserved{};
                uint32_t control_code{SIO_ADDRESS_LIST_SORT};
                uint8_t overlapped{1};
                uint8_t padding[3]{};
                const void* nested_input{};
                uintptr_t nested_length{};
            };
            static_assert(sizeof(transport_request) == 32);
            const transport_request request{.nested_input = input.data(), .nested_length = length};
            uint64_t information{};
            const auto status = socket_control(this->socket_.get_socket(), 0x120bf,
                                               std::as_bytes(std::span(&request, 1)), output, information);
            if (status != 0)
            {
                return status;
            }
            if (information < prefix || information > output.size())
            {
                return 0xc000000d;
            }
            int returned_count{};
            memcpy(&returned_count, output.data(), sizeof(returned_count));
            if (returned_count < 0 || static_cast<size_t>(returned_count) > addresses.size() ||
                information != prefix + static_cast<size_t>(returned_count) * sizeof(SOCKET_ADDRESS))
            {
                return 0xc000000d;
            }
            std::vector<address_sort_entry> sorted;
            sorted.reserve(returned_count);
            std::vector<bool> used(addresses.size());
            for (int position = 0; position < returned_count; ++position)
            {
                SOCKET_ADDRESS entry{};
                memcpy(&entry, output.data() + prefix + static_cast<size_t>(position) * sizeof(entry), sizeof(entry));
                if (entry.iSockaddrLength != sizeof(sockaddr_in6))
                {
                    return 0xc000000d;
                }
                bool found = false;
                for (size_t index = 0; index < addresses.size(); ++index)
                {
                    if (!used[index] && entry.lpSockaddr == reinterpret_cast<LPSOCKADDR>(&addresses[index].address))
                    {
                        sorted.push_back(addresses[index]);
                        used[index] = true;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    return 0xc000000d;
                }
            }
            addresses = std::move(sorted);
            return 0;
#else
            (void)addresses;
            return 0xc00000bb;
#endif
        }

        uint32_t socket_wrapper::set_transport_option(const uint32_t level, const uint32_t option, const std::span<const std::byte> value)
        {
#ifdef _WIN32
            struct transport_request
            {
                uint32_t type{1};
                uint32_t level{};
                uint32_t option{};
                uint8_t is_set{1};
                std::array<uint8_t, 3> padding{};
                const void* nested_input{};
                uintptr_t nested_length{};
            };

            static_assert(sizeof(transport_request) == 32);
            const transport_request request{.level = level, .option = option, .nested_input = value.data(), .nested_length = value.size()};
            uint64_t information{};
            const auto status = socket_control(this->socket_.get_socket(), 0x120bf, std::as_bytes(std::span(&request, 1)), {}, information);
            if (status != 0)
            {
                return status;
            }
            if (setsockopt(this->socket_.get_socket(), static_cast<int>(level), static_cast<int>(option),
                           reinterpret_cast<const char*>(value.data()), static_cast<int>(value.size())) != 0)
            {
                const auto error = WSAGetLastError();
                if (error == WSAEINVAL)
                {
                    return 0xc000000d;
                }
                if (error == WSAENOPROTOOPT || error == WSAEOPNOTSUPP)
                {
                    return 0xc00000bb;
                }
                return 0xc0000001;
            }
            return 0;
#else
            (void)level;
            (void)option;
            (void)value;
            return 0xc00000bb;
#endif
        }

        bool socket_wrapper::is_ready(const bool in_poll)
        {
            return this->is_aborted() || this->socket_.is_ready(in_poll);
        }

        bool socket_wrapper::is_listening()
        {
            if (!this->socket_.is_valid())
            {
                return false;
            }

            int val{};
            socklen_t len = sizeof(val);
            const auto res = getsockopt(this->socket_.get_socket(), SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char*>(&val), &len);

            return res != SOCKET_ERROR && val == 1;
        }

        bool socket_wrapper::is_connected()
        {
            if (!this->socket_.is_valid())
            {
                return false;
            }

            sockaddr_storage peer{};
            socklen_t length = sizeof(peer);
            return ::getpeername(this->socket_.get_socket(), reinterpret_cast<sockaddr*>(&peer), &length) == 0;
        }

        std::optional<address> socket_wrapper::get_local_address()
        {
            sockaddr_storage addr{};
            socklen_t addrlen = sizeof(sockaddr_storage);
            const auto res = ::getsockname(this->socket_.get_socket(), reinterpret_cast<sockaddr*>(&addr), &addrlen);

            if (res != 0)
            {
                return {};
            }

            address address{};
            address.set_address(reinterpret_cast<sockaddr*>(&addr), addrlen);
            return address;
        }

        bool socket_wrapper::bind(const address& addr)
        {
            return this->socket_.bind(addr);
        }

        bool socket_wrapper::connect(const address& addr)
        {
            return ::connect(this->socket_.get_socket(), &addr.get_addr(), addr.get_size()) == 0;
        }

        bool socket_wrapper::listen(int backlog)
        {
            return ::listen(this->socket_.get_socket(), backlog) == 0;
        }

        std::unique_ptr<i_socket> socket_wrapper::accept(address& address)
        {
            sockaddr_storage addr{};
            socklen_t addrlen = sizeof(addr);
            const auto s = ::accept(this->socket_.get_socket(), reinterpret_cast<sockaddr*>(&addr), &addrlen);

            if (s == INVALID_SOCKET)
            {
                return nullptr;
            }

            address.set_address(reinterpret_cast<sockaddr*>(&addr), addrlen);

            return std::make_unique<socket_wrapper>(s);
        }

        sent_size socket_wrapper::send(const std::span<const std::byte> data)
        {
#ifndef _WIN32
            if (this->disconnect_mode_ & 4)
            {
                this->synthetic_error_ = SERR(ECONNABORTED);
                return -1;
            }
            if (this->disconnect_mode_ & 1)
            {
                this->synthetic_error_ = SERR(ESHUTDOWN);
                return -1;
            }
            if (this->socket_type_ == SOCK_DGRAM && (this->disconnect_mode_ & 8))
            {
                this->synthetic_error_ = SERR(ENOTCONN);
                return -1;
            }
            this->synthetic_error_ = 0;
#endif
            return ::send(this->socket_.get_socket(), reinterpret_cast<const char*>(data.data()), static_cast<send_size>(data.size()), 0);
        }

        sent_size socket_wrapper::sendto(const address& destination, const std::span<const std::byte> data)
        {
#ifndef _WIN32
            if (this->socket_type_ != SOCK_DGRAM && (this->disconnect_mode_ & 4))
            {
                this->synthetic_error_ = SERR(ECONNABORTED);
                return -1;
            }
            if (this->socket_type_ != SOCK_DGRAM && (this->disconnect_mode_ & 1))
            {
                this->synthetic_error_ = SERR(ESHUTDOWN);
                return -1;
            }
            this->synthetic_error_ = 0;
#endif
            return ::sendto(this->socket_.get_socket(), reinterpret_cast<const char*>(data.data()), static_cast<send_size>(data.size()), 0,
                            &destination.get_addr(), destination.get_size());
        }

        sent_size socket_wrapper::recv(std::span<std::byte> data)
        {
#ifndef _WIN32
            if (this->disconnect_mode_ & 4)
            {
                this->synthetic_error_ = SERR(ECONNABORTED);
                return -1;
            }
            if (this->socket_type_ == SOCK_DGRAM && (this->disconnect_mode_ & 2))
            {
                this->synthetic_error_ = SERR(ESHUTDOWN);
                return -1;
            }
            this->synthetic_error_ = 0;
#endif
            return ::recv(this->socket_.get_socket(), reinterpret_cast<char*>(data.data()), static_cast<send_size>(data.size()), 0);
        }

        sent_size socket_wrapper::recvfrom(address& source, std::span<std::byte> data)
        {
#ifndef _WIN32
            if (this->socket_type_ != SOCK_DGRAM && (this->disconnect_mode_ & 4))
            {
                this->synthetic_error_ = SERR(ECONNABORTED);
                return -1;
            }
            this->synthetic_error_ = 0;
#endif
            auto source_length = source.get_max_size();
            const auto res = ::recvfrom(this->socket_.get_socket(), reinterpret_cast<char*>(data.data()),
                                        static_cast<send_size>(data.size()), 0, &source.get_addr(), &source_length);

            assert(res < 0 || source.get_size() == source_length);

            return res;
        }
    }

} // namespace sogen
