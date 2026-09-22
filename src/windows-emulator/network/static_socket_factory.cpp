#include "static_socket_factory.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include <network/socket.hpp>

#ifndef POLLHUP
#define POLLHUP 0x0010
#endif
#ifndef POLLRDNORM
#define POLLRDNORM 0x0040
#endif
#ifndef POLLRDBAND
#define POLLRDBAND 0x0080
#endif
#ifndef POLLWRNORM
#define POLLWRNORM 0x0100
#endif

namespace sogen
{

    namespace network
    {
        namespace
        {
            struct pipe_state
            {
                std::deque<std::byte> client_to_server;
                std::deque<std::byte> server_to_client;
                bool client_closed{false};
                bool server_closed{false};
            };

            struct pending_connection
            {
                address client_addr;
                std::shared_ptr<pipe_state> p;
            };

            struct shared_state
            {
                using packet_data = std::vector<std::byte>;
                using packet = std::pair<address, packet_data>;
                using packet_queue = std::queue<packet>;
                using packet_mapping = std::unordered_map<address, packet_queue>;
                using listen_mapping = std::unordered_map<address, std::deque<pending_connection>>;

                packet_mapping packets;
                listen_mapping listen_queues;
            };

            struct static_socket_factory_impl : socket_factory
            {
                uint32_t query_network_store(network_store_query& query) override
                {
                    // Deterministic offline network view: report exactly one adapter - the same
                    // hardcoded interface row the NSI device itself returned before the rework
                    // that routed NSI queries through the socket backend. The win backend answers
                    // with real host nsi.dll data; the static backend stays isolated from host
                    // queries, so it must synthesize the row or iphlpapi's
                    // GetAdaptersAddresses fails and dnsapi aborts resolution with
                    // DNS_ERROR_NO_DNS_SERVERS (9852) - measured on the sample probe.
                    constexpr uint64_t adapter_index = 7;
                    static constexpr std::array<uint8_t, 0xB8> adapter_parameter = {
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x30, 0x75, 0x00, 0x00, 0xE8, 0x03, 0x00, 0x00, 0xC0, 0x27,
                        0x09, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0x19, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00,
                        0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                        0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
                        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xDC, 0x05, 0x00, 0x00, 0x40, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x07, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x35, 0x97, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00,
                    };

                    const auto fill_row = [&](network_store_buffer& buffer) {
                        const auto bytes_to_write = std::min<size_t>(buffer.bytes.size(), adapter_parameter.size());
                        buffer.bytes.assign(adapter_parameter.begin(), adapter_parameter.begin() + static_cast<std::ptrdiff_t>(bytes_to_write));
                    };
                    const auto fill_zero = [](network_store_buffer& buffer) {
                        std::fill(buffer.bytes.begin(), buffer.bytes.end(), uint8_t{0});
                    };
                    const auto fill_key = [&](network_store_buffer& buffer) {
                        if (buffer.bytes.size() < sizeof(uint64_t))
                        {
                            return;
                        }
                        uint64_t index = adapter_index;
                        std::memcpy(buffer.bytes.data(), &index, sizeof(index));
                    };

                    switch (query.operation)
                    {
                    case network_store_operation::parameter:
                        // NsiGetParameter: 4-byte outputs read a count/state (1), 8-byte outputs
                        // read the adapter index, larger outputs read the interface row.
                        if (query.buffers[1].bytes.size() == sizeof(uint32_t))
                        {
                            uint32_t value = 1;
                            std::memcpy(query.buffers[1].bytes.data(), &value, sizeof(value));
                        }
                        else if (query.buffers[1].bytes.size() == sizeof(uint64_t))
                        {
                            uint64_t value = adapter_index;
                            std::memcpy(query.buffers[1].bytes.data(), &value, sizeof(value));
                        }
                        else
                        {
                            fill_row(query.buffers[1]);
                        }
                        return 0;

                    case network_store_operation::all_parameters:
                        fill_row(query.buffers[1]);
                        fill_zero(query.buffers[2]);
                        fill_zero(query.buffers[3]);
                        return 0;

                    case network_store_operation::enumerate:
                        query.count = 1;
                        fill_key(query.buffers[0]);
                        fill_row(query.buffers[1]);
                        fill_zero(query.buffers[2]);
                        fill_zero(query.buffers[3]);
                        return 0;
                    }

                    return 50;
                }

                std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
                uint16_t port{0};

                struct static_socket : i_socket
                {
                    static_socket_factory_impl* factory{};
                    int error{0};
                    address a{};
                    address peer_addr{};
                    std::shared_ptr<pipe_state> pipe{};
                    bool listening{false};
                    bool is_server_side{false};

                    explicit static_socket(static_socket_factory_impl& f)
                        : factory(&f)
                    {
                    }

                    static_socket(static_socket_factory_impl& f, const int af)
                        : factory(&f)
                    {
                        if (af == AF_INET)
                        {
                            a.set_ipv4(0);
                        }
                        else if (af == AF_INET6)
                        {
                            a.set_ipv6({});
                        }
                        else
                        {
                            throw std::runtime_error("Invalid address family");
                        }

                        a.set_port(++f.port);
                    }

                    ~static_socket() override
                    {
                        if (this->pipe)
                        {
                            if (this->is_server_side)
                            {
                                this->pipe->server_closed = true;
                            }
                            else
                            {
                                this->pipe->client_closed = true;
                            }
                        }
                    }

                    static_socket(const static_socket&) = delete;
                    static_socket& operator=(const static_socket&) = delete;
                    static_socket(static_socket&&) = delete;
                    static_socket& operator=(static_socket&&) = delete;

                    void set_blocking(const bool blocking) override
                    {
                        if (blocking)
                        {
                            throw std::runtime_error("Blocking sockets not supported yet!");
                        }
                    }

                    uint32_t query_information(const uint32_t information_class, uint64_t& value, std::span<const std::byte>) override
                    {
                        // AFD information classes the static (non-networked) socket must still
                        // answer: 4 = poll counter handled below; 6 = AFD_MAX_PACKET_SIZE;
                        // 7 = AFD_SEND_SIZE / bytes-in-send. Real Windows reports per-socket
                        // defaults for these; failing them (0xC00000BB) makes the sample's
                        // Winsock self-test bail out with exit code 1 (measured on the probe).
                        switch (information_class)
                        {
                        case 4:
                            value = 0;
                            return 0;
                        case 6: // AFD_MAX_PACKET_SIZE (datagram MTU-ish; loopback-safe default)
                            value = 0xFFFF;
                            return 0;
                        case 7: // AFD_SEND_SIZE (outstanding send bytes - nothing queued here)
                            value = 0;
                            return 0;
                        case 3: // AFD_INBOUND_BYTES
                            value = 0;
                            return 0;
                        case 8: // AFD_BYTES_PENDING - nothing pending on a static endpoint
                            value = 0;
                            return 0;
                        default:
                            return 0xc00000bb;
                        }
                    }

                    int get_last_error() override
                    {
                        return this->error;
                    }

                    bool is_ready(const bool in_poll) override
                    {
                        if (this->listening)
                        {
                            auto it = this->factory->state->listen_queues.find(this->a);
                            return it != this->factory->state->listen_queues.end() && !it->second.empty();
                        }
                        if (this->pipe && in_poll)
                        {
                            auto& q = this->is_server_side ? this->pipe->client_to_server : this->pipe->server_to_client;
                            bool peer_closed = this->is_server_side ? this->pipe->client_closed : this->pipe->server_closed;
                            return !q.empty() || peer_closed;
                        }
                        return true;
                    }

                    bool is_listening() override
                    {
                        return this->listening;
                    }

                    std::optional<address> get_local_address() override
                    {
                        return this->a;
                    }

                    bool bind(const address& addr) override
                    {
                        this->a = addr;
                        return true;
                    }

                    bool connect(const address& addr) override
                    {
                        this->peer_addr = addr;

                        auto& queues = this->factory->state->listen_queues;
                        auto it = queues.find(addr);
                        if (it == queues.end())
                        {
                            this->error = SERR(ECONNREFUSED);
                            return false;
                        }

                        this->pipe = std::make_shared<pipe_state>();
                        this->is_server_side = false;
                        it->second.emplace_back(pending_connection{.client_addr = this->a, .p = this->pipe});
                        this->error = 0;
                        return true;
                    }

                    bool listen(int /*backlog*/) override
                    {
                        this->listening = true;
                        this->factory->state->listen_queues.try_emplace(this->a);
                        this->error = 0;
                        return true;
                    }

                    std::unique_ptr<i_socket> accept(address& out_addr) override
                    {
                        auto& queues = this->factory->state->listen_queues;
                        auto it = queues.find(this->a);
                        if (it == queues.end() || it->second.empty())
                        {
                            this->error = SERR(EWOULDBLOCK);
                            return nullptr;
                        }

                        auto pending = std::move(it->second.front());
                        it->second.pop_front();

                        auto sock = std::make_unique<static_socket>(*this->factory);
                        sock->a = this->a;
                        sock->peer_addr = pending.client_addr;
                        sock->pipe = pending.p;
                        sock->is_server_side = true;

                        out_addr = pending.client_addr;
                        this->error = 0;
                        return sock;
                    }

                    sent_size send(std::span<const std::byte> data) override
                    {
                        if (!this->pipe)
                        {
                            this->error = SERR(ENOTCONN);
                            return -1;
                        }
                        auto& q = this->is_server_side ? this->pipe->server_to_client : this->pipe->client_to_server;
                        q.insert(q.end(), data.begin(), data.end());
                        this->error = 0;
                        return static_cast<sent_size>(data.size());
                    }

                    sent_size sendto(const address& destination, std::span<const std::byte> data) override
                    {
                        this->error = 0;
                        this->factory->state->packets[destination].emplace(this->a, shared_state::packet_data{data.begin(), data.end()});
                        return static_cast<sent_size>(data.size());
                    }

                    sent_size recv(std::span<std::byte> data) override
                    {
                        if (!this->pipe)
                        {
                            this->error = SERR(ENOTCONN);
                            return -1;
                        }
                        auto& q = this->is_server_side ? this->pipe->client_to_server : this->pipe->server_to_client;
                        bool peer_closed = this->is_server_side ? this->pipe->client_closed : this->pipe->server_closed;

                        if (q.empty())
                        {
                            if (peer_closed)
                            {
                                this->error = 0;
                                return 0;
                            }
                            this->error = SERR(EWOULDBLOCK);
                            return -1;
                        }

                        const size_t n = std::min(data.size(), q.size());
                        for (size_t i = 0; i < n; ++i)
                        {
                            data[i] = q.front();
                            q.pop_front();
                        }
                        this->error = 0;
                        return static_cast<sent_size>(n);
                    }

                    sent_size recvfrom(address& source, std::span<std::byte> data) override
                    {
                        this->error = 0;

                        auto& q = this->factory->state->packets[this->a];

                        if (q.empty())
                        {
                            this->error = SERR(EWOULDBLOCK);
                            return -1;
                        }

                        const auto p = std::move(q.front());
                        q.pop();

                        memcpy(data.data(), p.second.data(), std::min(data.size(), p.second.size()));

                        source = p.first;
                        return static_cast<sent_size>(p.second.size());
                    }
                };

                std::unique_ptr<i_socket> create_socket(const int af, const int /*type*/, const int /*protocol*/) override
                {
                    return std::make_unique<static_socket>(*this, af);
                }

                int poll_sockets(std::span<poll_entry> entries) override
                {
                    int ready_count = 0;
                    for (auto& entry : entries)
                    {
                        entry.revents = 0;
                        if (!entry.s)
                        {
                            continue;
                        }

                        auto* s = dynamic_cast<static_socket*>(entry.s);
                        if (!s)
                        {
                            continue;
                        }

                        constexpr auto read_mask = static_cast<int16_t>(POLLRDNORM | POLLRDBAND);
                        constexpr auto write_mask = static_cast<int16_t>(POLLWRNORM);

                        int16_t revents = 0;
                        bool readable = false;
                        bool peer_closed = false;

                        if (s->listening)
                        {
                            auto it = this->state->listen_queues.find(s->a);
                            readable = it != this->state->listen_queues.end() && !it->second.empty();
                        }
                        else if (s->pipe)
                        {
                            auto& q = s->is_server_side ? s->pipe->client_to_server : s->pipe->server_to_client;
                            peer_closed = s->is_server_side ? s->pipe->client_closed : s->pipe->server_closed;
                            readable = !q.empty();
                        }
                        else
                        {
                            auto packet_it = this->state->packets.find(s->a);
                            readable = packet_it != this->state->packets.end() && !packet_it->second.empty();
                        }

                        if (readable && (entry.events & read_mask))
                        {
                            revents = static_cast<int16_t>(revents | (entry.events & read_mask));
                        }

                        if (peer_closed)
                        {
                            revents = static_cast<int16_t>(revents | POLLHUP);
                        }

                        if (entry.events & write_mask)
                        {
                            revents = static_cast<int16_t>(revents | (entry.events & write_mask));
                        }

                        entry.revents = revents;
                        if (revents != 0)
                        {
                            ++ready_count;
                        }
                    }

                    return ready_count;
                }
            };
        }

        std::unique_ptr<socket_factory> create_static_socket_factory()
        {
            return std::make_unique<static_socket_factory_impl>();
        }
    }

} // namespace sogen
