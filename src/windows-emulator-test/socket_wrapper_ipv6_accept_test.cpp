#include "emulation_test_utils.hpp"
#include <network/socket_factory.hpp>
#include <network/socket_wrapper.hpp>

using namespace std::string_view_literals;

namespace sogen::test
{
    TEST(SocketWrapperHostTest, AcceptCapturesIpv6PeerAndConnectedState)
    {
        network::socket_factory initialize_winsock;
        network::socket_wrapper listener{AF_INET6, SOCK_STREAM, IPPROTO_TCP};
        network::socket_wrapper client{AF_INET6, SOCK_STREAM, IPPROTO_TCP};
        ASSERT_TRUE(listener.bind(network::address{"::1"sv, AF_INET6}));
        ASSERT_TRUE(listener.listen(1));
        ASSERT_FALSE(listener.is_connected());

        const auto local = listener.get_local_address();
        ASSERT_TRUE(local.has_value());
        ASSERT_TRUE(client.connect(*local));
        EXPECT_TRUE(client.is_connected());

        network::address remote{};
        auto accepted = listener.accept(remote);
        ASSERT_NE(accepted, nullptr);
        EXPECT_EQ(remote.get_family(), AF_INET6);
        EXPECT_EQ(remote.get_size(), sizeof(sockaddr_in6));
        EXPECT_NE(remote.get_port(), 0);
        EXPECT_TRUE(accepted->is_connected());
        EXPECT_FALSE(listener.is_connected());

        const auto& ipv6 = remote.get_in6_addr().sin6_addr;
        for (int i = 0; i < 15; ++i)
        {
            EXPECT_EQ(ipv6.s6_addr[i], 0);
        }
        EXPECT_EQ(ipv6.s6_addr[15], 1);
    }
} // namespace sogen::test
