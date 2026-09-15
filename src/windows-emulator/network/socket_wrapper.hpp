#pragma once

#include "i_socket.hpp"

namespace sogen
{

    namespace network
    {
        class socket_wrapper : public i_socket
        {
          public:
            socket_wrapper(SOCKET s);
            socket_wrapper(int af, int type, int protocol);
            ~socket_wrapper() override = default;

            void set_blocking(bool blocking) override;

            int get_last_error() override;
            uint32_t query_information(uint32_t information_class, uint64_t& value, std::span<const std::byte> parameters = {}) override;
            uint32_t set_information(uint32_t information_class, uint64_t value) override;

            uint32_t query_address_list(uint16_t family, std::vector<std::byte>& data) override;

            bool is_ready(bool in_poll) override;
            bool is_listening() override;

            std::optional<address> get_local_address() override;

            bool bind(const address& addr) override;
            bool connect(const address& addr) override;
            bool listen(int backlog) override;
            std::unique_ptr<i_socket> accept(address& address) override;

            sent_size send(std::span<const std::byte> data) override;
            sent_size sendto(const address& destination, std::span<const std::byte> data) override;

            sent_size recv(std::span<std::byte> data) override;
            sent_size recvfrom(address& source, std::span<std::byte> data) override;

            const socket& get() const
            {
                return this->socket_;
            }

          private:
            socket socket_{};
        };
    }

} // namespace sogen
