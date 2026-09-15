#include "socket_wrapper.hpp"
#include <cassert>
#include <utils/nt_handle.hpp>

namespace sogen
{

    namespace network
    {
        socket_wrapper::socket_wrapper(SOCKET s)
            : socket_(s)
        {
        }

        socket_wrapper::socket_wrapper(const int af, const int type, const int protocol)
            : socket_(af, type, protocol)
        {
        }

        void socket_wrapper::set_blocking(const bool blocking)
        {
            this->socket_.set_blocking(blocking);
        }

        int socket_wrapper::get_last_error()
        {
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

        bool socket_wrapper::is_ready(const bool in_poll)
        {
            return this->socket_.is_ready(in_poll);
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
            sockaddr addr{};
            socklen_t addrlen = sizeof(sockaddr);
            const auto s = ::accept(this->socket_.get_socket(), &addr, &addrlen);

            if (s == INVALID_SOCKET)
            {
                return nullptr;
            }

            address.set_address(&addr, addrlen);

            return std::make_unique<socket_wrapper>(s);
        }

        sent_size socket_wrapper::send(const std::span<const std::byte> data)
        {
            return ::send(this->socket_.get_socket(), reinterpret_cast<const char*>(data.data()), static_cast<send_size>(data.size()), 0);
        }

        sent_size socket_wrapper::sendto(const address& destination, const std::span<const std::byte> data)
        {
            return ::sendto(this->socket_.get_socket(), reinterpret_cast<const char*>(data.data()), static_cast<send_size>(data.size()), 0,
                            &destination.get_addr(), destination.get_size());
        }

        sent_size socket_wrapper::recv(std::span<std::byte> data)
        {
            return ::recv(this->socket_.get_socket(), reinterpret_cast<char*>(data.data()), static_cast<send_size>(data.size()), 0);
        }

        sent_size socket_wrapper::recvfrom(address& source, std::span<std::byte> data)
        {
            auto source_length = source.get_max_size();
            const auto res = ::recvfrom(this->socket_.get_socket(), reinterpret_cast<char*>(data.data()),
                                        static_cast<send_size>(data.size()), 0, &source.get_addr(), &source_length);

            assert(res < 0 || source.get_size() == source_length);

            return res;
        }
    }

} // namespace sogen
