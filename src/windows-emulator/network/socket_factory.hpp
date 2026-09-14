#pragma once

#include "i_socket.hpp"
#include "network_store.hpp"

#include <memory>

namespace sogen
{

    namespace network
    {
        struct poll_entry
        {
            i_socket* s{};
            int16_t events{};
            int16_t revents{};
        };

        struct socket_factory
        {
            socket_factory();
            virtual ~socket_factory() = default;

            virtual std::unique_ptr<i_socket> create_socket(int af, int type, int protocol);
            virtual int poll_sockets(std::span<poll_entry> entries);
            virtual uint32_t query_network_store(network_store_query& query);
        };
    }

} // namespace sogen
