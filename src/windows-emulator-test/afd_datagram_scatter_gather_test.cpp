#include "emulation_test_utils.hpp"
#include <devices/afd_endpoint.hpp>
#include <devices/afd_types.hpp>

#include <algorithm>
#include <deque>

namespace sogen::test
{
    namespace
    {
        struct datagram_packet
        {
            std::vector<std::byte> bytes;
            network::address source{"127.0.0.1", uint16_t{7777}};
        };

        struct datagram_state
        {
            std::deque<datagram_packet> incoming;
            std::vector<datagram_packet> outgoing;
            bool block_next_send{};
            int next_send_error{};
            int last_error{};
        };

        struct test_datagram_socket final : network::i_socket
        {
            explicit test_datagram_socket(std::shared_ptr<datagram_state> shared)
                : state(std::move(shared))
            {
            }

            void set_blocking(bool) override
            {
            }

            int get_last_error() override
            {
                return state->last_error;
            }

            bool is_ready(bool in_poll) override
            {
                return !in_poll || !state->incoming.empty();
            }

            bool is_listening() override
            {
                return false;
            }

            std::optional<network::address> get_local_address() override
            {
                return {};
            }

            bool bind(const network::address&) override
            {
                return true;
            }

            bool connect(const network::address&) override
            {
                return true;
            }

            bool listen(int) override
            {
                return true;
            }

            std::unique_ptr<network::i_socket> accept(network::address&) override
            {
                return {};
            }

            sent_size send(std::span<const std::byte>) override
            {
                return -1;
            }

            sent_size sendto(const network::address& destination, std::span<const std::byte> data) override
            {
                if (state->block_next_send)
                {
                    state->block_next_send = false;
                    state->last_error = SERR(EWOULDBLOCK);
                    return -1;
                }
                if (state->next_send_error)
                {
                    state->last_error = state->next_send_error;
                    return -1;
                }
                state->last_error = 0;
                state->outgoing.push_back({.bytes = {data.begin(), data.end()}, .source = destination});
                return static_cast<sent_size>(data.size());
            }

            sent_size recv(std::span<std::byte>) override
            {
                return -1;
            }

            sent_size recvfrom(network::address& source, std::span<std::byte> data) override
            {
                if (state->incoming.empty())
                {
                    state->last_error = SERR(EWOULDBLOCK);
                    return -1;
                }
                auto packet = std::move(state->incoming.front());
                state->incoming.pop_front();
                if (packet.bytes.size() > data.size())
                {
                    state->last_error = SERR(EMSGSIZE);
                    return -1;
                }
                state->last_error = 0;
                source = packet.source;
                std::copy(packet.bytes.begin(), packet.bytes.end(), data.begin());
                return static_cast<sent_size>(packet.bytes.size());
            }

            std::shared_ptr<datagram_state> state;
        };

        struct test_datagram_factory final : network::socket_factory
        {
            explicit test_datagram_factory(std::shared_ptr<datagram_state> shared)
                : state(std::move(shared))
            {
            }

            std::unique_ptr<network::i_socket> create_socket(int, int, int) override
            {
                return std::make_unique<test_datagram_socket>(state);
            }

            int poll_sockets(std::span<network::poll_entry> entries) override
            {
                int ready = 0;
                for (auto& entry : entries)
                {
                    entry.revents = (entry.events & POLLIN) && state->incoming.empty() ? 0 : entry.events;
                    ready += entry.revents != 0;
                }
                return ready;
            }

            std::shared_ptr<datagram_state> state;
        };

        struct test_sockaddr_in
        {
            uint16_t family{2};
            uint16_t port{htons(7777)};
            in_addr address{};
            std::array<uint8_t, 8> padding{};
        };

        static_assert(sizeof(test_sockaddr_in) == 16);
        static_assert(sizeof(AFD_RECV_DATAGRAM_INFO<EmulatorTraits<Emu32>>) == 24);
        static_assert(sizeof(AFD_RECV_DATAGRAM_INFO<EmulatorTraits<Emu64>>) == 40);
        static_assert(sizeof(AFD_SEND_DATAGRAM_INFO<EmulatorTraits<Emu32>>) == 56);
        static_assert(sizeof(AFD_SEND_DATAGRAM_INFO<EmulatorTraits<Emu64>>) == 104);
    }

    class AfdDatagramScatterGatherTest : public testing::TestWithParam<bool>
    {
      protected:
        static constexpr uint64_t memory = 0x200000;
        std::shared_ptr<datagram_state> packets = std::make_shared<datagram_state>();
        windows_emulator emu{[this] {
            emulator_settings settings{};
            settings.use_relative_time = true;
            settings.load_registry = false;
            emulator_interfaces interfaces{};
            interfaces.socket_factory = std::make_unique<test_datagram_factory>(packets);
            return create_emulator(std::move(settings), {}, std::move(interfaces));
        }()};
        std::unique_ptr<io_device> device;
        handle completion{};

        void SetUp() override
        {
            ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x2000, memory_permission::read_write));
            const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 17, 0};
            emu.memory.write_memory(memory, creation.data(), sizeof(creation));
            device = create_afd_endpoint({.is_32_bit = GetParam()});
            device->create(emu, {.buffer = memory, .length = sizeof(creation)});
            event signal{};
            signal.type = SynchronizationEvent;
            completion = emu.process.events.store(std::move(signal));
            test_sockaddr_in target{};
            target.address.s_addr = htonl(INADDR_LOOPBACK);
            emu.memory.write_memory(memory + 0x400, &target, sizeof(target));
        }

        template <typename Guest>
        void write_buffers(std::initializer_list<std::pair<ULONG, uint64_t>> entries)
        {
            using Traits = EmulatorTraits<Guest>;
            std::vector<EMU_WSABUF<Traits>> descriptors;
            for (const auto [length, address] : entries)
            {
                descriptors.push_back({.len = length, .buf = static_cast<typename Traits::PVOID>(address)});
            }
            emu.memory.write_memory(memory + 0x200, descriptors.data(), descriptors.size() * sizeof(descriptors[0]));
        }

        void buffers(std::initializer_list<std::pair<ULONG, uint64_t>> entries)
        {
            if (GetParam())
            {
                write_buffers<Emu32>(entries);
            }
            else
            {
                write_buffers<Emu64>(entries);
            }
        }

        template <typename Guest>
        NTSTATUS transfer(bool is_send, ULONG count, uint64_t array = memory + 0x200, ULONG tdi_flags = 0x20,
                          uint64_t source_address = memory + 0x500)
        {
            using Traits = EmulatorTraits<Guest>;
            io_device_context context{emu.memory};
            context.io_control_code = is_send ? 0x12023 : 0x1201b;
            context.io_status_block = {emu.memory, memory + 0x80};
            context.event = completion;
            context.input_buffer = memory + 0x100;
            context.input_buffer_length = is_send ? sizeof(AFD_SEND_DATAGRAM_INFO<Traits>) : sizeof(AFD_RECV_DATAGRAM_INFO<Traits>);
            emu.memory.set_memory(memory + 0x80, 0xa5, 16);
            if (is_send)
            {
                const AFD_SEND_DATAGRAM_INFO<Traits> request{
                    .BufferArray = static_cast<typename Traits::PVOID>(array),
                    .BufferCount = count,
                    .TdiConnInfo = {.RemoteAddressLength = sizeof(test_sockaddr_in),
                                    .RemoteAddress = static_cast<typename Traits::PVOID>(memory + 0x400)}};
                emu.memory.write_memory(context.input_buffer, &request, sizeof(request));
            }
            else
            {
                const AFD_RECV_DATAGRAM_INFO<Traits> request{.BufferArray = static_cast<typename Traits::PVOID>(array),
                                                             .BufferCount = count,
                                                             .TdiFlags = tdi_flags,
                                                             .Address = static_cast<typename Traits::PVOID>(source_address),
                                                             .AddressLength = static_cast<typename Traits::PVOID>(memory + 0x520)};
                const ULONG capacity = sizeof(test_sockaddr_in);
                emu.memory.write_memory(memory + 0x520, &capacity, sizeof(capacity));
                emu.memory.write_memory(context.input_buffer, &request, sizeof(request));
            }
            return device->execute_ioctl(emu, context);
        }

        NTSTATUS send(ULONG count, uint64_t array = memory + 0x200)
        {
            return GetParam() ? transfer<Emu32>(true, count, array) : transfer<Emu64>(true, count, array);
        }

        NTSTATUS receive(ULONG count, uint64_t array = memory + 0x200)
        {
            return GetParam() ? transfer<Emu32>(false, count, array) : transfer<Emu64>(false, count, array);
        }

        NTSTATUS receive_with_tdi_flags(ULONG flags)
        {
            return GetParam() ? transfer<Emu32>(false, 1, memory + 0x200, flags) : transfer<Emu64>(false, 1, memory + 0x200, flags);
        }

        void write(uint64_t address, std::string_view data)
        {
            emu.memory.write_memory(address, data.data(), data.size());
        }

        std::string read(uint64_t address, size_t count)
        {
            const auto data = emu.memory.read_memory(address, count);
            return {reinterpret_cast<const char*>(data.data()), data.size()};
        }

        void queue(std::string_view data)
        {
            datagram_packet packet{};
            for (const char c : data)
            {
                packet.bytes.push_back(static_cast<std::byte>(c));
            }
            packets->incoming.push_back(std::move(packet));
        }

        uint64_t information()
        {
            return emu.memory.read_memory<uint64_t>(memory + 0x88);
        }
    };

    TEST_P(AfdDatagramScatterGatherTest, SendGathersOnePacketAcrossEmptyMember)
    {
        write(memory + 0x300, "abc");
        write(memory + 0x320, "de");
        write(memory + 0x340, "fghi");
        buffers({{3, memory + 0x300}, {0, 0}, {2, memory + 0x320}, {4, memory + 0x340}});
        ASSERT_EQ(send(4), STATUS_SUCCESS);
        ASSERT_EQ(packets->outgoing.size(), 1u);
        const auto& bytes = packets->outgoing.front().bytes;
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "abcdefghi");
        EXPECT_EQ(information(), 9u);
        EXPECT_EQ(packets->outgoing.front().source.get_port(), 7777u);
    }

    TEST_P(AfdDatagramScatterGatherTest, ReceiveScattersOnePacketAcrossEmptyMember)
    {
        queue("abcdefghi");
        emu.memory.set_memory(memory + 0x300, 0xa5, 16);
        emu.memory.set_memory(memory + 0x320, 0xa5, 16);
        emu.memory.set_memory(memory + 0x340, 0xa5, 16);
        buffers({{2, memory + 0x300}, {0, 0}, {3, memory + 0x320}, {5, memory + 0x340}});
        ASSERT_EQ(receive(4), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x300, 2), "ab");
        EXPECT_EQ(read(memory + 0x320, 3), "cde");
        EXPECT_EQ(read(memory + 0x340, 4), "fghi");
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x344), 0xa5);
        EXPECT_EQ(information(), 9u);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 16u);
        EXPECT_TRUE(packets->incoming.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, ShortReceiveReportsTruncationAndConsumesPacket)
    {
        queue("abcdef");
        emu.memory.set_memory(memory + 0x300, 0xa5, 16);
        emu.memory.set_memory(memory + 0x320, 0xa5, 16);
        buffers({{2, memory + 0x300}, {1, memory + 0x320}});
        ASSERT_EQ(receive(2), STATUS_BUFFER_OVERFLOW);
        EXPECT_EQ(read(memory + 0x300, 2), "ab");
        EXPECT_EQ(read(memory + 0x320, 1), "c");
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x321), 0xa5);
        EXPECT_EQ(information(), 3u);
        EXPECT_TRUE(packets->incoming.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, NormalReceiveFlagIsRequired)
    {
        queue("abcdef");
        buffers({{6, memory + 0x300}});
        EXPECT_EQ(receive_with_tdi_flags(0), STATUS_NOT_SUPPORTED);
        EXPECT_EQ(packets->incoming.size(), 1u);
        ASSERT_EQ(receive_with_tdi_flags(0x20), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x300, 6), "abcdef");
    }

    TEST_P(AfdDatagramScatterGatherTest, UnsupportedPeekDoesNotConsumePacket)
    {
        queue("abcdef");
        buffers({{6, memory + 0x300}});
        EXPECT_EQ(receive_with_tdi_flags(0x20 | 0x80), STATUS_NOT_SUPPORTED);
        EXPECT_EQ(packets->incoming.size(), 1u);
        ASSERT_EQ(receive_with_tdi_flags(0x20), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x300, 6), "abcdef");
    }

    TEST_P(AfdDatagramScatterGatherTest, InvalidBuffersRejectBeforeHostTransfer)
    {
        write(memory + 0x300, "abc");
        buffers({{2, memory + 0x300}, {1, 0}});
        EXPECT_EQ(send(2), STATUS_ACCESS_VIOLATION);
        EXPECT_TRUE(packets->outgoing.empty());
        EXPECT_EQ(receive(2), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(send(0), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(receive(0), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(send(2, 0), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(receive(2, 0), STATUS_INVALID_PARAMETER);
        buffers({{65536, memory + 0x300}, {1, memory + 0x320}});
        EXPECT_EQ(send(2), STATUS_INVALID_BUFFER_SIZE);
        EXPECT_TRUE(packets->outgoing.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, InvalidReceiveDestinationsDoNotConsumePacket)
    {
        queue("abc");
        buffers({{3, memory + 0x3000}}); // Non-null, outside the mapped guest allocation.
        EXPECT_EQ(receive(1), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(packets->incoming.size(), 1u);

        buffers({{3, memory + 0x300}});
        const auto invalid_address = memory + 0x3000;
        EXPECT_EQ(GetParam() ? transfer<Emu32>(false, 1, memory + 0x200, 0x20, invalid_address)
                             : transfer<Emu64>(false, 1, memory + 0x200, 0x20, invalid_address),
                  STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(packets->incoming.size(), 1u);

        ASSERT_EQ(receive(1), STATUS_SUCCESS);
        EXPECT_TRUE(packets->incoming.empty());
        EXPECT_EQ(read(memory + 0x300, 3), "abc");
    }

    TEST_P(AfdDatagramScatterGatherTest, PendingReceiveKeepsOriginalDescriptorsAndAddress)
    {
        emu.memory.set_memory(memory + 0x300, 0xa5, 16);
        emu.memory.set_memory(memory + 0x320, 0xa5, 16);
        emu.memory.set_memory(memory + 0x380, 0xa5, 16);
        buffers({{2, memory + 0x300}, {3, memory + 0x320}});
        ASSERT_EQ(receive(2), STATUS_PENDING);
        EXPECT_FALSE(emu.process.events.get(completion)->signaled);
        buffers({{5, memory + 0x380}, {0, 0}});
        emu.memory.set_memory(memory + 0x100, 0xa5, 64);
        queue("abcde");
        device->work(emu);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(read(memory + 0x300, 2), "ab");
        EXPECT_EQ(read(memory + 0x320, 3), "cde");
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x380), 0xa5);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 16u);
        EXPECT_EQ(information(), 5u);
    }

    TEST_P(AfdDatagramScatterGatherTest, PendingSendKeepsOriginalDescriptorsAndDestination)
    {
        packets->block_next_send = true;
        write(memory + 0x300, "abc");
        write(memory + 0x320, "de");
        write(memory + 0x380, "wrong");
        buffers({{3, memory + 0x300}, {2, memory + 0x320}});
        ASSERT_EQ(send(2), STATUS_PENDING);
        EXPECT_FALSE(emu.process.events.get(completion)->signaled);
        buffers({{5, memory + 0x380}, {0, 0}});
        emu.memory.set_memory(memory + 0x100, 0xa5, 128);
        emu.memory.set_memory(memory + 0x400, 0xa5, 16);
        device->work(emu);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        ASSERT_EQ(packets->outgoing.size(), 1u);
        const auto& bytes = packets->outgoing.front().bytes;
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "abcde");
        EXPECT_EQ(packets->outgoing.front().source.get_port(), 7777u);
        EXPECT_EQ(information(), 5u);
    }

    TEST_P(AfdDatagramScatterGatherTest, HostRejectsOversizedDatagramWithoutSuccess)
    {
        packets->next_send_error = SERR(EMSGSIZE);
        write(memory + 0x300, "abc");
        buffers({{3, memory + 0x300}});
        EXPECT_EQ(send(1), STATUS_INVALID_BUFFER_SIZE);
        EXPECT_TRUE(packets->outgoing.empty());
    }

    INSTANTIATE_TEST_SUITE_P(GuestBitness, AfdDatagramScatterGatherTest, testing::Bool());
}
