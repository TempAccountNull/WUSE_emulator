#include "emulation_test_utils.hpp"
#include <devices/afd_endpoint.hpp>
#include <devices/afd_types.hpp>

#include <algorithm>
#include <deque>
#include <stdexcept>

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

            uint32_t partial_disconnect(uint32_t, int64_t) override
            {
                return 0;
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

            sent_size recv(std::span<std::byte>, bool = false) override
            {
                return -1;
            }

            sent_size recvfrom(network::address& source, std::span<std::byte> data, const bool peek = false) override
            {
                if (state->incoming.empty())
                {
                    state->last_error = SERR(EWOULDBLOCK);
                    return -1;
                }
                const auto packet = state->incoming.front();
                if (!peek)
                {
                    state->incoming.pop_front();
                }
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
                          uint64_t source_address = memory + 0x500, ULONG afd_flags = 0, ULONG source_capacity = sizeof(test_sockaddr_in),
                          io_device* target = nullptr, handle file_handle = {}, uint64_t apc_context = 0)
        {
            using Traits = EmulatorTraits<Guest>;
            io_device_context context{emu.memory};
            context.io_control_code = is_send ? 0x12023 : 0x1201b;
            context.io_status_block = {emu.memory, memory + 0x80};
            context.event = completion;
            context.file_handle = file_handle;
            context.apc_context = apc_context;
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
                                                             .AfdFlags = afd_flags,
                                                             .TdiFlags = tdi_flags,
                                                             .Address = static_cast<typename Traits::PVOID>(source_address),
                                                             .AddressLength = static_cast<typename Traits::PVOID>(memory + 0x520)};
                emu.memory.write_memory(memory + 0x520, &source_capacity, sizeof(source_capacity));
                emu.memory.write_memory(context.input_buffer, &request, sizeof(request));
            }
            return (target ? target : device.get())->execute_ioctl(emu, context);
        }

        NTSTATUS send(ULONG count, uint64_t array = memory + 0x200)
        {
            return GetParam() ? transfer<Emu32>(true, count, array) : transfer<Emu64>(true, count, array);
        }

        NTSTATUS receive(ULONG count, uint64_t array = memory + 0x200, ULONG tdi_flags = 0x20, ULONG afd_flags = 0)
        {
            return GetParam() ? transfer<Emu32>(false, count, array, tdi_flags, memory + 0x500, afd_flags)
                              : transfer<Emu64>(false, count, array, tdi_flags, memory + 0x500, afd_flags);
        }

        template <typename Guest>
        NTSTATUS transfer_common_receive(ULONG count, ULONG tdi_flags, ULONG afd_flags)
        {
            using Traits = EmulatorTraits<Guest>;
            io_device_context context{emu.memory};
            context.io_control_code = 0x12017;
            context.io_status_block = {emu.memory, memory + 0x80};
            context.event = completion;
            context.input_buffer = memory + 0x100;
            context.input_buffer_length = sizeof(AFD_RECV_INFO<Traits>);
            emu.memory.set_memory(memory + 0x80, 0xa5, 16);
            const AFD_RECV_INFO<Traits> request{.BufferArray = static_cast<typename Traits::PVOID>(memory + 0x200),
                                                .BufferCount = count,
                                                .AfdFlags = afd_flags,
                                                .TdiFlags = tdi_flags};
            emu.memory.write_memory(context.input_buffer, &request, sizeof(request));
            return device->execute_ioctl(emu, context);
        }

        NTSTATUS common_receive(ULONG count, ULONG tdi_flags = 0x20, ULONG afd_flags = 0)
        {
            return GetParam() ? transfer_common_receive<Emu32>(count, tdi_flags, afd_flags)
                              : transfer_common_receive<Emu64>(count, tdi_flags, afd_flags);
        }

        NTSTATUS receive_on(io_device* target, handle file_handle, ULONG count, ULONG afd_flags, uint64_t apc_context = 0,
                            ULONG tdi_flags = 0xa0)
        {
            return GetParam() ? transfer<Emu32>(false, count, memory + 0x200, tdi_flags, memory + 0x500, afd_flags,
                                                sizeof(test_sockaddr_in), target, file_handle, apc_context)
                              : transfer<Emu64>(false, count, memory + 0x200, tdi_flags, memory + 0x500, afd_flags,
                                                sizeof(test_sockaddr_in), target, file_handle, apc_context);
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

        void check_pending_datagram_priority(const bool peek_first, const bool cancel_peek = false, const bool ordinary_fallback = false,
                                             const bool peek_fallback = false)
        {
            event peek_signal{};
            peek_signal.type = SynchronizationEvent;
            const auto peek_event = emu.process.events.store(std::move(peek_signal));
            const std::array<uint64_t, 2> bases{memory + 0x800, memory + 0xc00};
            const std::array<handle, 2> events{completion, peek_event};
            const auto run = [&]<typename Guest>() {
                using Traits = EmulatorTraits<Guest>;
                const auto submit = [&](const size_t operation) {
                    const auto base = bases[operation];
                    emu.memory.set_memory(base, 0xa5, 16);
                    emu.memory.set_memory(base + 0xc0, 0xa5, 8);
                    emu.memory.set_memory(base + 0x100, 0xa5, 16);
                    const EMU_WSABUF<Traits> buffer{.len = 3, .buf = static_cast<typename Traits::PVOID>(base + 0xc0)};
                    const AFD_RECV_DATAGRAM_INFO<Traits> input{.BufferArray = static_cast<typename Traits::PVOID>(base + 0x80),
                                                               .BufferCount = 1,
                                                               .AfdFlags = 2,
                                                               .TdiFlags = operation == 0 ? 0x20u : 0xa0u,
                                                               .Address = static_cast<typename Traits::PVOID>(base + 0x100),
                                                               .AddressLength = static_cast<typename Traits::PVOID>(base + 0x120)};
                    emu.memory.write_memory(base + 0x40, &input, sizeof(input));
                    emu.memory.write_memory(base + 0x80, &buffer, sizeof(buffer));
                    const ULONG capacity = sizeof(test_sockaddr_in);
                    emu.memory.write_memory(base + 0x120, &capacity, sizeof(capacity));
                    io_device_context context{emu.memory};
                    const bool fallback = operation == 0 ? ordinary_fallback : peek_fallback;
                    context.io_control_code = fallback ? 0x12017 : 0x1201b;
                    context.issuer_thread_id = operation == 0 ? 0x5678 : 0x1234;
                    context.io_status_block = {emu.memory, base};
                    context.event = events[operation];
                    context.input_buffer = base + 0x40;
                    context.input_buffer_length = fallback ? sizeof(AFD_RECV_INFO<Traits>) : sizeof(input);
                    return device->execute_ioctl(emu, context);
                };
                ASSERT_EQ(submit(peek_first ? 1 : 0), STATUS_PENDING);
                ASSERT_EQ(submit(peek_first ? 0 : 1), STATUS_PENDING);
                const auto ordinary_pending = emu.memory.read_memory(bases[0], 16);
                const auto peek_pending = emu.memory.read_memory(bases[1], 16);
                const auto peek_payload = emu.memory.read_memory(bases[1] + 0xc0, 8);
                const auto peek_address = emu.memory.read_memory(bases[1] + 0x100, 16);
                for (const auto base : bases)
                {
                    emu.memory.set_memory(base + 0x40, 0, 64);
                    emu.memory.set_memory(base + 0x80, 0, 32);
                }
                device->work(emu);
                EXPECT_EQ(emu.memory.read_memory(bases[0], 16), ordinary_pending);
                EXPECT_EQ(emu.memory.read_memory(bases[1], 16), peek_pending);
                EXPECT_FALSE(emu.process.events.get(completion)->signaled);
                EXPECT_FALSE(emu.process.events.get(peek_event)->signaled);
                EXPECT_EQ(emu.process.events.get(completion)->ref_count, 2u);
                EXPECT_EQ(emu.process.events.get(peek_event)->ref_count, 2u);
                queue("abc");
                device->work(emu);
                EXPECT_EQ(emu.memory.read_memory<uint32_t>(bases[0]), STATUS_SUCCESS);
                EXPECT_EQ(emu.memory.read_memory<uint64_t>(bases[0] + 8), 3u);
                EXPECT_EQ(read(bases[0] + 0xc0, 3), "abc");
                EXPECT_EQ(emu.memory.read_memory<uint8_t>(bases[0] + 0xc3), 0xa5);
                if (ordinary_fallback)
                {
                    EXPECT_EQ(emu.memory.read_memory<uint8_t>(bases[0] + 0x100), 0xa5);
                }
                else
                {
                    EXPECT_EQ(emu.memory.read_memory<test_sockaddr_in>(bases[0] + 0x100).port, htons(7777));
                }
                EXPECT_EQ(emu.memory.read_memory<uint32_t>(bases[0] + 0x120), 16u);
                EXPECT_TRUE(emu.process.events.get(completion)->signaled);
                EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
                EXPECT_TRUE(packets->incoming.empty());
                const auto ordinary_completed = emu.memory.read_memory(bases[0], 16);
                EXPECT_EQ(emu.memory.read_memory(bases[1], 16), peek_pending);
                EXPECT_EQ(emu.memory.read_memory(bases[1] + 0xc0, 8), peek_payload);
                EXPECT_EQ(emu.memory.read_memory(bases[1] + 0x100, 16), peek_address);
                EXPECT_FALSE(emu.process.events.get(peek_event)->signaled);
                EXPECT_EQ(emu.process.events.get(peek_event)->ref_count, 2u);
                if (cancel_peek)
                {
                    EXPECT_EQ(device->cancel_pending_io(emu, bases[1], 0x5678), 0u);
                    EXPECT_EQ(device->cancel_pending_io(emu, bases[1], 0x1234), 1u);
                    EXPECT_EQ(emu.memory.read_memory<uint32_t>(bases[1]), static_cast<uint32_t>(STATUS_CANCELLED));
                    EXPECT_EQ(emu.memory.read_memory<uint64_t>(bases[1] + 8), 0u);
                    EXPECT_TRUE(emu.process.events.get(peek_event)->signaled);
                    EXPECT_EQ(emu.process.events.get(peek_event)->ref_count, 1u);
                    queue("def");
                    device->work(emu);
                    EXPECT_EQ(emu.memory.read_memory(bases[1] + 0xc0, 8), peek_payload);
                    EXPECT_EQ(emu.memory.read_memory(bases[1] + 0x100, 16), peek_address);
                }
                else
                {
                    queue("def");
                    device->work(emu);
                    EXPECT_EQ(emu.memory.read_memory<uint32_t>(bases[1]), STATUS_SUCCESS);
                    EXPECT_EQ(emu.memory.read_memory<uint64_t>(bases[1] + 8), 3u);
                    EXPECT_EQ(read(bases[1] + 0xc0, 3), "def");
                    EXPECT_EQ(emu.memory.read_memory<uint8_t>(bases[1] + 0xc3), 0xa5);
                    if (peek_fallback)
                    {
                        EXPECT_EQ(emu.memory.read_memory<uint8_t>(bases[1] + 0x100), 0xa5);
                    }
                    else
                    {
                        EXPECT_EQ(emu.memory.read_memory<test_sockaddr_in>(bases[1] + 0x100).port, htons(7777));
                    }
                    EXPECT_EQ(emu.memory.read_memory<uint32_t>(bases[1] + 0x120), 16u);
                    EXPECT_TRUE(emu.process.events.get(peek_event)->signaled);
                    EXPECT_EQ(emu.process.events.get(peek_event)->ref_count, 1u);
                    device->work(emu);
                }
                ASSERT_EQ(packets->incoming.size(), 1u);
                EXPECT_EQ(emu.memory.read_memory(bases[0], 16), ordinary_completed);
                EXPECT_EQ(read(bases[0] + 0xc0, 3), "abc");
                buffers({{3, memory + 0x1300}});
                ASSERT_EQ(receive(1), STATUS_SUCCESS);
                EXPECT_EQ(read(memory + 0x1300, 3), "def");
                EXPECT_TRUE(packets->incoming.empty());
            };
            if (GetParam())
            {
                run.template operator()<Emu32>();
            }
            else
            {
                run.template operator()<Emu64>();
            }
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

    TEST_P(AfdDatagramScatterGatherTest, CommonReceiveDatagramPeeksScatterAndRetainTheFullTruncatedPacket)
    {
        queue("abcdef");
        const auto original = packets->incoming.front().bytes;
        emu.memory.set_memory(memory + 0x500, 0xa5, 32);
        const auto no_source_output = emu.memory.read_memory(memory + 0x500, 32);
        buffers({{2, memory + 0x300}, {0, 0}, {1, memory + 0x320}});
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            emu.memory.set_memory(memory + 0x300, 0xa5, 8);
            emu.memory.set_memory(memory + 0x320, 0xa5, 8);
            ASSERT_EQ(common_receive(3, 0xa0), STATUS_BUFFER_OVERFLOW);
            EXPECT_EQ(read(memory + 0x300, 2), "ab");
            EXPECT_EQ(read(memory + 0x320, 1), "c");
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x321), 0xa5);
            EXPECT_EQ(information(), 3u);
            ASSERT_EQ(packets->incoming.size(), 1u);
            EXPECT_EQ(packets->incoming.front().bytes, original);
            EXPECT_EQ(emu.memory.read_memory(memory + 0x500, 32), no_source_output);
        }
        buffers({{6, memory + 0x340}});
        ASSERT_EQ(common_receive(1), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x340, 6), "abcdef");
        EXPECT_TRUE(packets->incoming.empty());
        EXPECT_EQ(emu.memory.read_memory(memory + 0x500, 32), no_source_output);
    }

    TEST_P(AfdDatagramScatterGatherTest, CommonReceiveDatagramRejectsSelectorsBeforeUnsupportedPolicies)
    {
        queue("abcdef");
        buffers({{6, memory + 0x300}});
        emu.memory.set_memory(memory + 0x300, 0xa5, 8);
        for (const ULONG flags : {0u, 0x60u, 0x80u, 0xe0u})
        {
            EXPECT_EQ(common_receive(1, flags), STATUS_INVALID_PARAMETER);
            EXPECT_EQ(packets->incoming.size(), 1u);
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x300), 0xa5);
        }
        for (const ULONG flags : {0x40u, 0x4020u, 0xb0u})
        {
            EXPECT_EQ(common_receive(1, flags), STATUS_NOT_SUPPORTED);
            EXPECT_EQ(packets->incoming.size(), 1u);
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x300), 0xa5);
        }
        ASSERT_EQ(common_receive(1), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x300, 6), "abcdef");
        EXPECT_TRUE(packets->incoming.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, CommonReceiveDatagramOrdinaryTruncationConsumesOnePacket)
    {
        queue("abcdef");
        buffers({{3, memory + 0x300}});
        ASSERT_EQ(common_receive(1), STATUS_BUFFER_OVERFLOW);
        EXPECT_EQ(read(memory + 0x300, 3), "abc");
        EXPECT_EQ(information(), 3u);
        EXPECT_TRUE(packets->incoming.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, CommonReceiveDatagramPendingSnapshotKeepsDescriptorsAndCompletionPolicy)
    {
        buffers({{2, memory + 0x300}, {3, memory + 0x320}});
        emu.memory.set_memory(memory + 0x380, 0xa5, 8);
        ASSERT_EQ(common_receive(2, 0xa0, 3), STATUS_PENDING);
        const auto pending = emu.memory.read_memory(memory + 0x80, 16);
        buffers({{5, memory + 0x380}, {0, 0}});
        emu.memory.set_memory(memory + 0x100, 0, 64);
        utils::buffer_serializer saved{};
        device->serialize(saved);
        utils::buffer_deserializer input{saved};
        input.register_factory<memory_manager_wrapper>([this] { return memory_manager_wrapper{emu.memory}; });
        input.register_factory<x64_emulator_wrapper>([this] { return x64_emulator_wrapper{emu.emu()}; });
        input.register_factory<windows_emulator_wrapper>([this] { return windows_emulator_wrapper{emu}; });
        input.register_factory<socket_factory_wrapper>([this] { return socket_factory_wrapper{emu.socket_factory()}; });
        auto restored = create_afd_endpoint({.is_32_bit = GetParam()});
        restored->deserialize(input);
        utils::buffer_serializer resaved{};
        restored->serialize(resaved);
        EXPECT_EQ(saved.get_buffer(), resaved.get_buffer());
        device = std::move(restored);
        device->work(emu);
        EXPECT_EQ(emu.memory.read_memory(memory + 0x80, 16), pending);
        queue("abcde");
        device->work(emu);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x80), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x84), GetParam() ? 0u : 0xa5a5a5a5u);
        EXPECT_EQ(read(memory + 0x300, 2), "ab");
        EXPECT_EQ(read(memory + 0x320, 3), "cde");
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x380), 0xa5);
        EXPECT_EQ(information(), 5u);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        ASSERT_EQ(packets->incoming.size(), 1u);
        buffers({{5, memory + 0x380}});
        ASSERT_EQ(common_receive(1), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x380, 5), "abcde");
        EXPECT_TRUE(packets->incoming.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, PendingCommonDatagramTruncationPreservesQualifiedInformation)
    {
        buffers({{2, memory + 0x300}, {1, memory + 0x320}});
        ASSERT_EQ(common_receive(2, 0xa0, 3), STATUS_PENDING);
        buffers({{6, memory + 0x380}, {0, 0}});
        emu.memory.set_memory(memory + 0x100, 0, 64);
        queue("abcdef");
        device->work(emu);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x80), static_cast<uint32_t>(STATUS_BUFFER_OVERFLOW));
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x84), GetParam() ? 0u : 0xa5a5a5a5u);
        EXPECT_EQ(information(), 3u);
        EXPECT_EQ(read(memory + 0x300, 2), "ab");
        EXPECT_EQ(read(memory + 0x320, 1), "c");
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        ASSERT_EQ(packets->incoming.size(), 1u);
        buffers({{6, memory + 0x380}});
        ASSERT_EQ(common_receive(1), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x380, 6), "abcdef");
        EXPECT_TRUE(packets->incoming.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, MixedCommonAndAddressDatagramWaitersKeepOrdinaryPriority)
    {
        check_pending_datagram_priority(true, false, true, false);
    }

    TEST_P(AfdDatagramScatterGatherTest, MixedAddressAndCommonDatagramWaitersKeepOrdinaryPriority)
    {
        check_pending_datagram_priority(true, false, false, true);
    }

    TEST_P(AfdDatagramScatterGatherTest, ShortDatagramSourceCapacityKeepsAddressAndLengthAfterPayloadCopy)
    {
        for (const ULONG capacity : {0u, 4u, 15u})
        {
            for (const bool peek : {false, true})
            {
                for (const ULONG payload_capacity : {3u, 6u})
                {
                    queue("abcdef");
                    buffers({{payload_capacity, memory + 0x300}});
                    emu.memory.set_memory(memory + 0x300, 0xa5, 8);
                    emu.memory.set_memory(memory + 0x500, 0xa5, 16);
                    const auto address_before = emu.memory.read_memory(memory + 0x500, 16);
                    const auto flags = peek ? 0xa0u : 0x20u;
                    const auto status = GetParam() ? transfer<Emu32>(false, 1, memory + 0x200, flags, memory + 0x500, 0, capacity)
                                                   : transfer<Emu64>(false, 1, memory + 0x200, flags, memory + 0x500, 0, capacity);
                    ASSERT_EQ(status, STATUS_BUFFER_TOO_SMALL);
                    EXPECT_EQ(read(memory + 0x300, payload_capacity), std::string("abcdef").substr(0, payload_capacity));
                    EXPECT_EQ(information(), payload_capacity);
                    EXPECT_EQ(emu.memory.read_memory(memory + 0x500, 16), address_before);
                    EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), capacity);
                    EXPECT_EQ(packets->incoming.size(), peek ? 1u : 0u);
                    if (peek)
                    {
                        buffers({{6, memory + 0x340}});
                        ASSERT_EQ(receive(1), STATUS_SUCCESS);
                        EXPECT_EQ(read(memory + 0x340, 6), "abcdef");
                        EXPECT_TRUE(packets->incoming.empty());
                    }
                }
            }
        }
    }

    TEST_P(AfdDatagramScatterGatherTest, PendingDatagramSourceCapacityIgnoresCallerLengthReduction)
    {
        for (const ULONG flags : {0x20u, 0xa0u})
        {
            buffers({{6, memory + 0x300}});
            emu.memory.set_memory(memory + 0x300, 0xa5, 8);
            emu.memory.set_memory(memory + 0x500, 0xa5, 24);
            emu.process.events.get(completion)->signaled = false;
            const auto submitted = GetParam() ? transfer<Emu32>(false, 1, memory + 0x200, flags, memory + 0x500, 2, 16)
                                              : transfer<Emu64>(false, 1, memory + 0x200, flags, memory + 0x500, 2, 16);
            ASSERT_EQ(submitted, STATUS_PENDING);
            const auto original_iosb = emu.memory.read_memory(memory + 0x80, 16);
            EXPECT_EQ(emu.process.events.get(completion)->ref_count, 2u);
            emulator_object<ULONG>{emu.memory, memory + 0x520}.write(0);
            emu.memory.set_memory(memory + 0x100, 0, 64);
            emu.memory.set_memory(memory + 0x200, 0, 32);
            device->work(emu);
            EXPECT_EQ(emu.memory.read_memory(memory + 0x80, 16), original_iosb);
            EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 0u);
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x500), 0xa5);
            EXPECT_FALSE(emu.process.events.get(completion)->signaled);
            queue("abcdef");
            device->work(emu);
            EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x80), STATUS_SUCCESS);
            EXPECT_EQ(information(), 6u);
            EXPECT_EQ(read(memory + 0x300, 6), "abcdef");
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x306), 0xa5);
            EXPECT_EQ(emu.memory.read_memory<test_sockaddr_in>(memory + 0x500).port, htons(7777));
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x510), 0xa5);
            EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 16u);
            EXPECT_TRUE(emu.process.events.get(completion)->signaled);
            EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
            const auto terminal_iosb = emu.memory.read_memory(memory + 0x80, 16);
            emu.process.events.get(completion)->signaled = false;
            device->work(emu);
            EXPECT_EQ(emu.memory.read_memory(memory + 0x80, 16), terminal_iosb);
            EXPECT_FALSE(emu.process.events.get(completion)->signaled);
            EXPECT_EQ(packets->incoming.size(), flags == 0xa0 ? 1u : 0u);
            if (flags == 0xa0)
            {
                buffers({{6, memory + 0x340}});
                ASSERT_EQ(receive(1), STATUS_SUCCESS);
                EXPECT_EQ(read(memory + 0x340, 6), "abcdef");
                EXPECT_TRUE(packets->incoming.empty());
            }
        }
    }

    TEST_P(AfdDatagramScatterGatherTest, PendingDatagramSourceCapacityIgnoresCallerLengthExpansion)
    {
        for (const ULONG flags : {0x20u, 0xa0u})
        {
            buffers({{3, memory + 0x300}});
            emu.memory.set_memory(memory + 0x300, 0xa5, 8);
            emu.memory.set_memory(memory + 0x500, 0xa5, 24);
            const auto original_address = emu.memory.read_memory(memory + 0x500, 24);
            emu.process.events.get(completion)->signaled = false;
            const auto submitted = GetParam() ? transfer<Emu32>(false, 1, memory + 0x200, flags, memory + 0x500, 2, 8)
                                              : transfer<Emu64>(false, 1, memory + 0x200, flags, memory + 0x500, 2, 8);
            ASSERT_EQ(submitted, STATUS_PENDING);
            const auto original_iosb = emu.memory.read_memory(memory + 0x80, 16);
            EXPECT_EQ(emu.process.events.get(completion)->ref_count, 2u);
            emulator_object<ULONG>{emu.memory, memory + 0x520}.write(16);
            emu.memory.set_memory(memory + 0x100, 0, 64);
            emu.memory.set_memory(memory + 0x200, 0, 32);
            device->work(emu);
            EXPECT_EQ(emu.memory.read_memory(memory + 0x80, 16), original_iosb);
            EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 16u);
            EXPECT_EQ(emu.memory.read_memory(memory + 0x500, 24), original_address);
            EXPECT_FALSE(emu.process.events.get(completion)->signaled);
            queue("abcdef");
            device->work(emu);
            EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x80), STATUS_BUFFER_TOO_SMALL);
            EXPECT_EQ(information(), 3u);
            EXPECT_EQ(read(memory + 0x300, 3), "abc");
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x303), 0xa5);
            EXPECT_EQ(emu.memory.read_memory(memory + 0x500, 24), original_address);
            EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 16u);
            EXPECT_TRUE(emu.process.events.get(completion)->signaled);
            EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
            const auto terminal_iosb = emu.memory.read_memory(memory + 0x80, 16);
            emu.process.events.get(completion)->signaled = false;
            device->work(emu);
            EXPECT_EQ(emu.memory.read_memory(memory + 0x80, 16), terminal_iosb);
            EXPECT_FALSE(emu.process.events.get(completion)->signaled);
            EXPECT_EQ(packets->incoming.size(), flags == 0xa0 ? 1u : 0u);
            if (flags == 0xa0)
            {
                buffers({{6, memory + 0x340}});
                ASSERT_EQ(receive(1), STATUS_SUCCESS);
                EXPECT_EQ(read(memory + 0x340, 6), "abcdef");
                EXPECT_TRUE(packets->incoming.empty());
            }
        }
    }

    TEST_P(AfdDatagramScatterGatherTest, PendingDatagramFullSourceCapacitySurvivesSnapshotAndCallerMutation)
    {
        buffers({{3, memory + 0x300}});
        emu.memory.set_memory(memory + 0x300, 0xa5, 8);
        emu.memory.set_memory(memory + 0x500, 0xa5, 24);
        ASSERT_EQ(receive(1, memory + 0x200, 0xa0, 2), STATUS_PENDING);
        const auto original_iosb = emu.memory.read_memory(memory + 0x80, 16);
        emulator_object<ULONG>{emu.memory, memory + 0x520}.write(0);
        emu.memory.set_memory(memory + 0x100, 0, 64);
        emu.memory.set_memory(memory + 0x200, 0, 32);
        utils::buffer_serializer saved{};
        device->serialize(saved);
        emulator_object<ULONG>{emu.memory, memory + 0x520}.write(8);
        utils::buffer_deserializer input{saved};
        input.register_factory<memory_manager_wrapper>([this] { return memory_manager_wrapper{emu.memory}; });
        input.register_factory<x64_emulator_wrapper>([this] { return x64_emulator_wrapper{emu.emu()}; });
        input.register_factory<windows_emulator_wrapper>([this] { return windows_emulator_wrapper{emu}; });
        input.register_factory<socket_factory_wrapper>([this] { return socket_factory_wrapper{emu.socket_factory()}; });
        auto restored = create_afd_endpoint({.is_32_bit = GetParam()});
        restored->deserialize(input);
        utils::buffer_serializer resaved{};
        restored->serialize(resaved);
        EXPECT_EQ(saved.get_buffer(), resaved.get_buffer());
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 3u);
        device = std::move(restored);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 2u);
        device->work(emu);
        EXPECT_EQ(emu.memory.read_memory(memory + 0x80, 16), original_iosb);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 8u);
        EXPECT_FALSE(emu.process.events.get(completion)->signaled);
        queue("abcdef");
        device->work(emu);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x80), STATUS_BUFFER_OVERFLOW);
        EXPECT_EQ(information(), 3u);
        EXPECT_EQ(read(memory + 0x300, 3), "abc");
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x303), 0xa5);
        EXPECT_EQ(emu.memory.read_memory<test_sockaddr_in>(memory + 0x500).port, htons(7777));
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x510), 0xa5);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 16u);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
        ASSERT_EQ(packets->incoming.size(), 1u);
        buffers({{6, memory + 0x340}});
        ASSERT_EQ(receive(1), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x340, 6), "abcdef");
        EXPECT_TRUE(packets->incoming.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, PendingDatagramShortSourceCapacitySurvivesSnapshotAndCallerMutation)
    {
        buffers({{3, memory + 0x300}});
        emu.memory.set_memory(memory + 0x300, 0xa5, 8);
        emu.memory.set_memory(memory + 0x500, 0xa5, 24);
        const auto original_address = emu.memory.read_memory(memory + 0x500, 24);
        const auto submitted = GetParam() ? transfer<Emu32>(false, 1, memory + 0x200, 0xa0, memory + 0x500, 2, 8)
                                          : transfer<Emu64>(false, 1, memory + 0x200, 0xa0, memory + 0x500, 2, 8);
        ASSERT_EQ(submitted, STATUS_PENDING);
        emulator_object<ULONG>{emu.memory, memory + 0x520}.write(16);
        emu.memory.set_memory(memory + 0x100, 0, 64);
        emu.memory.set_memory(memory + 0x200, 0, 32);
        utils::buffer_serializer saved{};
        device->serialize(saved);
        utils::buffer_deserializer input{saved};
        input.register_factory<memory_manager_wrapper>([this] { return memory_manager_wrapper{emu.memory}; });
        input.register_factory<x64_emulator_wrapper>([this] { return x64_emulator_wrapper{emu.emu()}; });
        input.register_factory<windows_emulator_wrapper>([this] { return windows_emulator_wrapper{emu}; });
        input.register_factory<socket_factory_wrapper>([this] { return socket_factory_wrapper{emu.socket_factory()}; });
        auto restored = create_afd_endpoint({.is_32_bit = GetParam()});
        restored->deserialize(input);
        utils::buffer_serializer resaved{};
        restored->serialize(resaved);
        EXPECT_EQ(saved.get_buffer(), resaved.get_buffer());
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 3u);
        device = std::move(restored);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 2u);
        queue("abcdef");
        device->work(emu);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x80), STATUS_BUFFER_TOO_SMALL);
        EXPECT_EQ(information(), 3u);
        EXPECT_EQ(read(memory + 0x300, 3), "abc");
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x303), 0xa5);
        EXPECT_EQ(emu.memory.read_memory(memory + 0x500, 24), original_address);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 16u);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
        ASSERT_EQ(packets->incoming.size(), 1u);
        buffers({{6, memory + 0x340}});
        ASSERT_EQ(receive(1), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x340, 6), "abcdef");
        EXPECT_TRUE(packets->incoming.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, CapturedDatagramCapacityEncodingRefusesUncapturedFeaturePrefix)
    {
        buffers({{3, memory + 0x300}});
        ASSERT_EQ(receive(1, memory + 0x200, 0xa0, 2), STATUS_PENDING);
        utils::buffer_serializer saved{};
        device->serialize(saved);
        utils::buffer_serializer current_marker{};
        current_marker.write<uint64_t>(0x324b454550444641ull);
        utils::buffer_serializer previous_marker{};
        previous_marker.write<uint64_t>(0x314b454550444641ull);
        auto previous_prefix = saved.get_buffer();
        const auto position = std::search(previous_prefix.begin(), previous_prefix.end(), current_marker.get_buffer().begin(),
                                          current_marker.get_buffer().end());
        ASSERT_NE(position, previous_prefix.end());
        ASSERT_EQ(std::search(position + current_marker.get_buffer().size(), previous_prefix.end(), current_marker.get_buffer().begin(),
                              current_marker.get_buffer().end()),
                  previous_prefix.end());
        std::copy(previous_marker.get_buffer().begin(), previous_marker.get_buffer().end(), position);
        const auto references_before = emu.process.events.get(completion)->ref_count;
        utils::buffer_deserializer input{previous_prefix};
        input.register_factory<memory_manager_wrapper>([this] { return memory_manager_wrapper{emu.memory}; });
        input.register_factory<x64_emulator_wrapper>([this] { return x64_emulator_wrapper{emu.emu()}; });
        input.register_factory<windows_emulator_wrapper>([this] { return windows_emulator_wrapper{emu}; });
        input.register_factory<socket_factory_wrapper>([this] { return socket_factory_wrapper{emu.socket_factory()}; });
        auto restored = create_afd_endpoint({.is_32_bit = GetParam()});
        EXPECT_THROW(restored->deserialize(input), std::runtime_error);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, references_before);
        EXPECT_FALSE(emu.process.events.get(completion)->signaled);
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

    TEST_P(AfdDatagramScatterGatherTest, OrdinaryDatagramWaiterPrecedesEarlierPeekOnArrival)
    {
        check_pending_datagram_priority(true);
    }

    TEST_P(AfdDatagramScatterGatherTest, OrdinaryDatagramWaiterPrecedesLaterPeekOnArrival)
    {
        check_pending_datagram_priority(false);
    }

    TEST_P(AfdDatagramScatterGatherTest, DeferredDatagramPeekStillCancelsByItsCapturedIssuer)
    {
        check_pending_datagram_priority(true, true);
    }

    TEST_P(AfdDatagramScatterGatherTest, DatagramPeekScattersRepeatedlyAndRetainsPacketAndSender)
    {
        queue("abcde");
        const auto original_packet = packets->incoming.front().bytes;
        const auto original_source = packets->incoming.front().source;
        buffers({{2, memory + 0x300}, {0, 0}, {3, memory + 0x320}});
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            emu.memory.set_memory(memory + 0x300, 0xa5, 16);
            emu.memory.set_memory(memory + 0x320, 0xa5, 16);
            emu.process.events.get(completion)->signaled = false;
            ASSERT_EQ(receive(3, memory + 0x200, 0xa0), STATUS_SUCCESS);
            EXPECT_EQ(read(memory + 0x300, 2), "ab");
            EXPECT_EQ(read(memory + 0x320, 3), "cde");
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x323), 0xa5);
            EXPECT_EQ(information(), 5u);
            ASSERT_EQ(packets->incoming.size(), 1u);
            EXPECT_EQ(packets->incoming.front().bytes, original_packet);
            EXPECT_EQ(packets->incoming.front().source, original_source);
            EXPECT_EQ(emu.memory.read_memory<test_sockaddr_in>(memory + 0x500).port, htons(7777));
            EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 16u);
            EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        }
        ASSERT_EQ(receive(3), STATUS_SUCCESS);
        EXPECT_TRUE(packets->incoming.empty());
        EXPECT_EQ(read(memory + 0x300, 2), "ab");
        EXPECT_EQ(read(memory + 0x320, 3), "cde");
        EXPECT_EQ(information(), 5u);
    }

    TEST_P(AfdDatagramScatterGatherTest, TruncatedDatagramPeeksRetainTheFullPacketForOrdinaryReceive)
    {
        queue("abcdef");
        const auto original_packet = packets->incoming.front().bytes;
        const auto original_source = packets->incoming.front().source;
        buffers({{2, memory + 0x300}, {1, memory + 0x320}});
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            emu.memory.set_memory(memory + 0x300, 0xa5, 16);
            emu.memory.set_memory(memory + 0x320, 0xa5, 16);
            ASSERT_EQ(receive(2, memory + 0x200, 0xa0), STATUS_BUFFER_OVERFLOW);
            EXPECT_EQ(read(memory + 0x300, 2), "ab");
            EXPECT_EQ(read(memory + 0x320, 1), "c");
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x321), 0xa5);
            EXPECT_EQ(information(), 3u);
            ASSERT_EQ(packets->incoming.size(), 1u);
            EXPECT_EQ(packets->incoming.front().bytes, original_packet);
            EXPECT_EQ(packets->incoming.front().source, original_source);
            EXPECT_EQ(emu.memory.read_memory<test_sockaddr_in>(memory + 0x500).port, htons(7777));
        }
        buffers({{6, memory + 0x340}});
        ASSERT_EQ(receive(1), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x340, 6), "abcdef");
        EXPECT_EQ(information(), 6u);
        EXPECT_TRUE(packets->incoming.empty());
        EXPECT_EQ(emu.memory.read_memory<test_sockaddr_in>(memory + 0x500).port, htons(7777));
    }

    TEST_P(AfdDatagramScatterGatherTest, ReadyTruncatedOverlappedPeekReturnsPendingWithOneTerminalWarning)
    {
        emu.process.is_wow64_process = GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 17, 0};
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        const auto port_handle = emu.process.io_completions.store(io_completion{});
        auto* port = emu.process.io_completions.get(port_handle);
        ASSERT_NE(port, nullptr);
        endpoint->completion_port = port_handle;
        endpoint->completion_key = 0x1234;
        endpoint->completion_notification_flags = 1;
        queue("abcdef");
        const auto original_packet = packets->incoming.front();
        buffers({{2, memory + 0x300}, {1, memory + 0x320}});
        for (const ULONG flags : {0u, 2u})
        {
            for (const uint64_t context : {0ull, 0x5678ull})
            {
                emu.memory.set_memory(memory + 0x300, 0xa5, 16);
                emu.memory.set_memory(memory + 0x320, 0xa5, 16);
                emu.process.events.get(completion)->signaled = false;
                ASSERT_EQ(receive_on(endpoint, endpoint_handle, 2, flags, context), flags == 2 ? STATUS_PENDING : STATUS_BUFFER_OVERFLOW);
                EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x80), STATUS_BUFFER_OVERFLOW);
                EXPECT_EQ(information(), 3u);
                EXPECT_EQ(read(memory + 0x300, 2), "ab");
                EXPECT_EQ(read(memory + 0x320, 1), "c");
                EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x321), 0xa5);
                EXPECT_EQ(emu.memory.read_memory<test_sockaddr_in>(memory + 0x500).port, htons(7777));
                EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 16u);
                EXPECT_TRUE(emu.process.events.get(completion)->signaled);
                EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
                ASSERT_EQ(packets->incoming.size(), 1u);
                EXPECT_EQ(packets->incoming.front().bytes, original_packet.bytes);
                EXPECT_EQ(packets->incoming.front().source, original_packet.source);
                const bool packet_expected = flags == 2 && context != 0;
                ASSERT_EQ(port->queue.size(), packet_expected ? 1u : 0u);
                if (packet_expected)
                {
                    EXPECT_EQ(port->queue.front().key_context, 0x1234u);
                    EXPECT_EQ(port->queue.front().apc_context, context);
                    EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_BUFFER_OVERFLOW);
                    EXPECT_EQ(port->queue.front().io_status_block.Information, 3u);
                }
                port->queue.clear();
                const auto terminal_iosb = emu.memory.read_memory(memory + 0x80, 16);
                endpoint->work(emu);
                EXPECT_TRUE(port->queue.empty());
                EXPECT_EQ(emu.memory.read_memory(memory + 0x80, 16), terminal_iosb);
                EXPECT_EQ(endpoint->cancel_pending_io(emu, memory + 0x80, 0), 0u);
            }
        }
        buffers({{6, memory + 0x340}});
        ASSERT_EQ(receive_on(endpoint, endpoint_handle, 1, 0, 0, 0x20), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x340, 6), "abcdef");
        EXPECT_TRUE(packets->incoming.empty());
        EXPECT_TRUE(port->queue.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, ReadyOverlappedPeekThatFitsKeepsImmediateSuccess)
    {
        queue("abc");
        buffers({{3, memory + 0x300}});
        ASSERT_EQ(receive(1, memory + 0x200, 0xa0, 2), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x80), STATUS_SUCCESS);
        EXPECT_EQ(information(), 3u);
        EXPECT_EQ(read(memory + 0x300, 3), "abc");
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
        ASSERT_EQ(packets->incoming.size(), 1u);
        const auto terminal_iosb = emu.memory.read_memory(memory + 0x80, 16);
        device->work(emu);
        EXPECT_EQ(emu.memory.read_memory(memory + 0x80, 16), terminal_iosb);
        EXPECT_EQ(device->cancel_pending_io(emu, memory + 0x80, 0), 0u);
    }

    TEST_P(AfdDatagramScatterGatherTest, InitiallyEmptyTruncatedPeekPublishesWarningOnlyAfterArrival)
    {
        emu.process.is_wow64_process = GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 17, 0};
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        const auto port_handle = emu.process.io_completions.store(io_completion{});
        auto* port = emu.process.io_completions.get(port_handle);
        ASSERT_NE(port, nullptr);
        endpoint->completion_port = port_handle;
        endpoint->completion_key = 0x1234;
        buffers({{2, memory + 0x300}, {1, memory + 0x320}});
        emu.memory.set_memory(memory + 0x300, 0xa5, 16);
        emu.memory.set_memory(memory + 0x320, 0xa5, 16);
        ASSERT_EQ(receive_on(endpoint, endpoint_handle, 2, 2, 0x5678), STATUS_PENDING);
        const auto pending_iosb = emu.memory.read_memory(memory + 0x80, 16);
        EXPECT_EQ(pending_iosb, std::vector<std::byte>(16, std::byte{0xa5}));
        EXPECT_FALSE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 2u);
        EXPECT_TRUE(port->queue.empty());
        emu.memory.set_memory(memory + 0x100, 0, 64);
        buffers({{6, memory + 0x380}});
        endpoint->work(emu);
        EXPECT_EQ(emu.memory.read_memory(memory + 0x80, 16), pending_iosb);
        EXPECT_TRUE(port->queue.empty());
        queue("abcdef");
        endpoint->work(emu);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x80), STATUS_BUFFER_OVERFLOW);
        EXPECT_EQ(information(), 3u);
        EXPECT_EQ(read(memory + 0x300, 2), "ab");
        EXPECT_EQ(read(memory + 0x320, 1), "c");
        EXPECT_EQ(emu.memory.read_memory<test_sockaddr_in>(memory + 0x500).port, htons(7777));
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
        ASSERT_EQ(port->queue.size(), 1u);
        EXPECT_EQ(port->queue.front().apc_context, 0x5678u);
        EXPECT_EQ(port->queue.front().key_context, 0x1234u);
        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_BUFFER_OVERFLOW);
        EXPECT_EQ(port->queue.front().io_status_block.Information, 3u);
        ASSERT_EQ(packets->incoming.size(), 1u);
        EXPECT_EQ(packets->incoming.front().bytes.size(), 6u);
        port->queue.clear();
        const auto terminal_iosb = emu.memory.read_memory(memory + 0x80, 16);
        endpoint->work(emu);
        EXPECT_TRUE(port->queue.empty());
        EXPECT_EQ(emu.memory.read_memory(memory + 0x80, 16), terminal_iosb);
        EXPECT_EQ(endpoint->cancel_pending_io(emu, memory + 0x80, 0), 0u);
        buffers({{6, memory + 0x380}});
        ASSERT_EQ(receive_on(endpoint, endpoint_handle, 1, 0, 0, 0x20), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x380, 6), "abcdef");
        EXPECT_TRUE(packets->incoming.empty());
        EXPECT_TRUE(port->queue.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, EmptyNonblockingDatagramPeekLeavesPayloadAndAddressUntouched)
    {
        buffers({{3, memory + 0x300}});
        emu.memory.set_memory(memory + 0x300, 0xa5, 8);
        emu.memory.set_memory(memory + 0x500, 0xa5, 16);
        ASSERT_EQ(receive(1, memory + 0x200, 0xa0, 4), STATUS_DEVICE_NOT_READY);
        EXPECT_EQ(information(), 0u);
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x300), 0xa5);
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x500), 0xa5);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 16u);
        EXPECT_TRUE(packets->incoming.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, PendingDatagramPeekKeepsOriginalPolicyDescriptorsAndAddress)
    {
        buffers({{2, memory + 0x300}, {3, memory + 0x320}});
        emu.memory.set_memory(memory + 0x300, 0xa5, 16);
        emu.memory.set_memory(memory + 0x320, 0xa5, 16);
        emu.memory.set_memory(memory + 0x380, 0xa5, 16);
        emu.memory.set_memory(memory + 0x500, 0xa5, 16);
        ASSERT_EQ(receive(2, memory + 0x200, 0xa0, 2), STATUS_PENDING);
        const auto original_iosb = emu.memory.read_memory(memory + 0x80, 16);
        EXPECT_FALSE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 2u);
        buffers({{5, memory + 0x380}, {0, 0}});
        emu.memory.set_memory(memory + 0x100, 0, 64);
        device->work(emu);
        EXPECT_EQ(emu.memory.read_memory(memory + 0x80, 16), original_iosb);
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x300), 0xa5);
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x500), 0xa5);
        EXPECT_FALSE(emu.process.events.get(completion)->signaled);
        queue("abcde");
        device->work(emu);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x80), STATUS_SUCCESS);
        EXPECT_EQ(information(), 5u);
        EXPECT_EQ(read(memory + 0x300, 2), "ab");
        EXPECT_EQ(read(memory + 0x320, 3), "cde");
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x380), 0xa5);
        EXPECT_EQ(emu.memory.read_memory<test_sockaddr_in>(memory + 0x500).port, htons(7777));
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x520), 16u);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
        ASSERT_EQ(packets->incoming.size(), 1u);
        EXPECT_EQ(packets->incoming.front().bytes.size(), 5u);
        device->work(emu);
        EXPECT_EQ(packets->incoming.size(), 1u);
        buffers({{5, memory + 0x380}});
        ASSERT_EQ(receive(1), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x380, 5), "abcde");
        EXPECT_TRUE(packets->incoming.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, PendingDatagramPeekSnapshotRetainsPacketPolicy)
    {
        buffers({{3, memory + 0x300}});
        ASSERT_EQ(receive(1, memory + 0x200, 0xa0, 2), STATUS_PENDING);
        emu.memory.set_memory(memory + 0x100, 0, 64);
        emu.memory.set_memory(memory + 0x200, 0, 32);
        utils::buffer_serializer saved{};
        device->serialize(saved);
        utils::buffer_deserializer input{saved};
        input.register_factory<memory_manager_wrapper>([this] { return memory_manager_wrapper{emu.memory}; });
        input.register_factory<x64_emulator_wrapper>([this] { return x64_emulator_wrapper{emu.emu()}; });
        input.register_factory<windows_emulator_wrapper>([this] { return windows_emulator_wrapper{emu}; });
        input.register_factory<socket_factory_wrapper>([this] { return socket_factory_wrapper{emu.socket_factory()}; });
        auto restored = create_afd_endpoint({.is_32_bit = GetParam()});
        restored->deserialize(input);
        utils::buffer_serializer resaved{};
        restored->serialize(resaved);
        EXPECT_EQ(saved.get_buffer(), resaved.get_buffer());
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 3u);
        device = std::move(restored);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 2u);
        queue("abc");
        device->work(emu);
        EXPECT_EQ(read(memory + 0x300, 3), "abc");
        EXPECT_EQ(information(), 3u);
        EXPECT_EQ(packets->incoming.size(), 1u);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
        buffers({{3, memory + 0x300}});
        ASSERT_EQ(receive(1), STATUS_SUCCESS);
        EXPECT_TRUE(packets->incoming.empty());
    }

    TEST_P(AfdDatagramScatterGatherTest, DatagramPeekRejectsUnimplementedFlagsAndInvalidDestinationsWithoutConsumption)
    {
        queue("abc");
        buffers({{3, memory + 0x300}});
        for (const auto flags : {0x80u, 0xc0u, 0xe0u, 0xb0u})
        {
            EXPECT_EQ(receive(1, memory + 0x200, flags), STATUS_NOT_SUPPORTED);
            EXPECT_EQ(packets->incoming.size(), 1u);
        }
        buffers({{3, memory + 0x3000}});
        EXPECT_EQ(receive(1, memory + 0x200, 0xa0), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(packets->incoming.size(), 1u);
        buffers({{3, memory + 0x300}});
        const auto invalid_address = memory + 0x3000;
        EXPECT_EQ(GetParam() ? transfer<Emu32>(false, 1, memory + 0x200, 0xa0, invalid_address)
                             : transfer<Emu64>(false, 1, memory + 0x200, 0xa0, invalid_address),
                  STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(packets->incoming.size(), 1u);
        ASSERT_EQ(receive(1, memory + 0x200, 0xa0), STATUS_SUCCESS);
        EXPECT_EQ(packets->incoming.size(), 1u);
    }

    TEST_P(AfdDatagramScatterGatherTest, ZeroLengthDatagramPeekRetainsThePacketAndSource)
    {
        queue("");
        buffers({{3, memory + 0x300}});
        emu.memory.set_memory(memory + 0x300, 0xa5, 8);
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            ASSERT_EQ(receive(1, memory + 0x200, 0xa0), STATUS_SUCCESS);
            EXPECT_EQ(information(), 0u);
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x300), 0xa5);
            EXPECT_EQ(emu.memory.read_memory<test_sockaddr_in>(memory + 0x500).port, htons(7777));
            EXPECT_EQ(packets->incoming.size(), 1u);
        }
        ASSERT_EQ(receive(1), STATUS_SUCCESS);
        EXPECT_EQ(information(), 0u);
        EXPECT_TRUE(packets->incoming.empty());
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

    class AfdDatagramIrpPublicationTest : public testing::Test
    {
      protected:
        using Traits = EmulatorTraits<Emu64>;
        static constexpr uint64_t memory = 0x200000;
        static constexpr uint64_t iosb = memory + 0x80;
        static constexpr std::array<uint8_t, 16> initial_iosb{0x4d, 0x4c, 0x4b, 0x4a, 0xdd, 0xcc, 0xbb, 0xaa,
                                                              0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
        static constexpr std::array<uint8_t, 16> success_iosb{0, 0, 0, 0, 0xdd, 0xcc, 0xbb, 0xaa, 7, 0, 0, 0, 0, 0, 0, 0};
        static constexpr std::array<uint8_t, 16> short_source_iosb{0x23, 0, 0, 0xc0, 0xdd, 0xcc, 0xbb, 0xaa, 7, 0, 0, 0, 0, 0, 0, 0};
        std::shared_ptr<datagram_state> packets = std::make_shared<datagram_state>();
        windows_emulator emu{[this] {
            emulator_settings settings{};
            settings.use_relative_time = true;
            settings.load_registry = false;
            emulator_interfaces interfaces{};
            interfaces.socket_factory = std::make_unique<test_datagram_factory>(packets);
            return create_emulator(std::move(settings), {}, std::move(interfaces));
        }()};
        handle endpoint_handle{};
        io_device_container* endpoint{};
        handle completion{};
        handle port_handle{};
        io_completion* port{};

        void SetUp() override
        {
            ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x2000, memory_permission::read_write));
            const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 17, 0};
            emu.memory.write_memory(memory, creation.data(), sizeof(creation));
            endpoint_handle =
                emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
            endpoint = emu.process.devices.get(endpoint_handle);
            ASSERT_NE(endpoint, nullptr);
            event signal{};
            signal.type = SynchronizationEvent;
            completion = emu.process.events.store(std::move(signal));
            port_handle = emu.process.io_completions.store(io_completion{});
            port = emu.process.io_completions.get(port_handle);
            ASSERT_NE(port, nullptr);
        }

        io_device_context prepare(const ULONG source_capacity, const ULONG payload_capacity = 64)
        {
            emu.memory.set_memory(iosb - 8, 0x5a, 32);
            emu.memory.write_memory(iosb, initial_iosb.data(), initial_iosb.size());
            emu.memory.set_memory(memory + 0x300, 0xcc, 72);
            emu.memory.set_memory(memory + 0x380, 0x6b, 72);
            emu.memory.set_memory(memory + 0x500, 0xcc, 64);
            emulator_object<ULONG>{emu.memory, memory + 0x520}.write(source_capacity);
            emulator_object<ULONG>{emu.memory, memory + 0x524}.write(0xddeeff00);
            const EMU_WSABUF<Traits> buffer{.len = payload_capacity, .buf = memory + 0x300};
            emu.memory.write_memory(memory + 0x200, &buffer, sizeof(buffer));
            const AFD_RECV_DATAGRAM_INFO<Traits> info{.BufferArray = memory + 0x200,
                                                      .BufferCount = 1,
                                                      .AfdFlags = 2,
                                                      .TdiFlags = 0xa0,
                                                      .Address = memory + 0x500,
                                                      .AddressLength = memory + 0x520};
            emu.memory.write_memory(memory + 0x100, &info, sizeof(info));
            io_device_context context{emu.memory};
            context.io_control_code = 0x1201b;
            context.io_status_block = {emu.memory, iosb};
            context.event = completion;
            context.file_handle = endpoint_handle;
            context.input_buffer = memory + 0x100;
            context.input_buffer_length = sizeof(info);
            return context;
        }

        void queue()
        {
            datagram_packet packet{};
            for (const char c : std::string_view{"abcdefg"})
            {
                packet.bytes.push_back(static_cast<std::byte>(c));
            }
            packets->incoming.push_back(std::move(packet));
        }

        void expect_iosb(const std::array<uint8_t, 16>& expected)
        {
            const auto raw = emu.memory.read_memory(iosb, expected.size());
            for (size_t i = 0; i < expected.size(); ++i)
            {
                EXPECT_EQ(std::to_integer<uint8_t>(raw[i]), expected[i]) << i;
            }
            EXPECT_EQ(emu.memory.read_memory(iosb - 8, 8), std::vector<std::byte>(8, std::byte{0x5a}));
            EXPECT_EQ(emu.memory.read_memory(iosb + 16, 8), std::vector<std::byte>(8, std::byte{0x5a}));
        }

        void check_raw_control(const ULONG source_capacity, const bool mutate)
        {
            auto context = prepare(source_capacity);
            ASSERT_EQ(endpoint->execute_ioctl(emu, context), STATUS_PENDING);
            expect_iosb(initial_iosb);
            EXPECT_FALSE(emu.process.events.get(completion)->signaled);
            EXPECT_EQ(emu.process.events.get(completion)->ref_count, 2u);
            if (mutate)
            {
                emulator_object<ULONG>{emu.memory, memory + 0x520}.write(source_capacity == 16 ? 0u : 16u);
                const EMU_WSABUF<Traits> replacement{.len = 64, .buf = memory + 0x380};
                emu.memory.write_memory(memory + 0x200, &replacement, sizeof(replacement));
                emu.memory.set_memory(memory + 0x100, 0, sizeof(AFD_RECV_DATAGRAM_INFO<Traits>));
            }
            endpoint->work(emu);
            expect_iosb(initial_iosb);
            EXPECT_FALSE(emu.process.events.get(completion)->signaled);
            queue();
            endpoint->work(emu);
            expect_iosb(source_capacity == 16 ? success_iosb : short_source_iosb);
            const auto payload = emu.memory.read_memory(memory + 0x300, 7);
            EXPECT_EQ(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()), "abcdefg");
            EXPECT_EQ(emu.memory.read_memory(memory + 0x307, 65), std::vector<std::byte>(65, std::byte{0xcc}));
            EXPECT_EQ(emu.memory.read_memory(memory + 0x380, 72), std::vector<std::byte>(72, std::byte{0x6b}));
            EXPECT_EQ(emu.memory.read_memory<ULONG>(memory + 0x524), 0xddeeff00u);
            if (source_capacity == 16)
            {
                EXPECT_EQ(emu.memory.read_memory<test_sockaddr_in>(memory + 0x500).port, htons(7777));
                EXPECT_EQ(emu.memory.read_memory<ULONG>(memory + 0x520), 16u);
            }
            else
            {
                EXPECT_EQ(emu.memory.read_memory(memory + 0x500, 16), std::vector<std::byte>(16, std::byte{0xcc}));
                EXPECT_EQ(emu.memory.read_memory<ULONG>(memory + 0x520), mutate ? 16u : 8u);
            }
            EXPECT_TRUE(emu.process.events.get(completion)->signaled);
            EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
            EXPECT_EQ(packets->incoming.size(), 1u);
            emu.process.events.get(completion)->signaled = false;
            endpoint->work(emu);
            expect_iosb(source_capacity == 16 ? success_iosb : short_source_iosb);
            EXPECT_FALSE(emu.process.events.get(completion)->signaled);
            EXPECT_EQ(endpoint->cancel_pending_io(emu, iosb, 0), 0u);
        }

        void check_cancel_control(const bool iocp, const bool mutate)
        {
            auto context = prepare(16);
            if (iocp)
            {
                endpoint->completion_port = port_handle;
                endpoint->completion_key = 0x43616e63656c;
                context.apc_context = iosb;
            }
            ASSERT_EQ(endpoint->execute_ioctl(emu, context), STATUS_PENDING);
            expect_iosb(initial_iosb);
            EXPECT_TRUE(port->queue.empty());
            EXPECT_FALSE(emu.process.events.get(completion)->signaled);
            EXPECT_EQ(emu.process.events.get(completion)->ref_count, 2u);
            if (mutate)
            {
                emulator_object<ULONG>{emu.memory, memory + 0x520}.write(0);
                const EMU_WSABUF<Traits> replacement{.len = 64, .buf = memory + 0x380};
                emu.memory.write_memory(memory + 0x200, &replacement, sizeof(replacement));
                emu.memory.set_memory(memory + 0x100, 0, sizeof(AFD_RECV_DATAGRAM_INFO<Traits>));
            }
            ASSERT_EQ(endpoint->cancel_pending_io(emu, iosb, 0), 1u);
            const std::array<uint8_t, 16> canceled{0x20, 1, 0, 0xc0, 0xdd, 0xcc, 0xbb, 0xaa, 0, 0, 0, 0, 0, 0, 0, 0};
            expect_iosb(canceled);
            EXPECT_EQ(emu.memory.read_memory(memory + 0x300, 72), std::vector<std::byte>(72, std::byte{0xcc}));
            EXPECT_EQ(emu.memory.read_memory(memory + 0x380, 72), std::vector<std::byte>(72, std::byte{0x6b}));
            EXPECT_EQ(emu.memory.read_memory(memory + 0x500, 16), std::vector<std::byte>(16, std::byte{0xcc}));
            EXPECT_EQ(emu.memory.read_memory<ULONG>(memory + 0x520), mutate ? 0u : 16u);
            EXPECT_EQ(emu.memory.read_memory<ULONG>(memory + 0x524), 0xddeeff00u);
            EXPECT_TRUE(emu.process.events.get(completion)->signaled);
            EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
            ASSERT_EQ(port->queue.size(), iocp ? 1u : 0u);
            if (iocp)
            {
                EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_CANCELLED);
                EXPECT_EQ(port->queue.front().io_status_block.Information, 0u);
                EXPECT_EQ(port->queue.front().key_context, 0x43616e63656cull);
                EXPECT_EQ(port->queue.front().apc_context, iosb);
            }
            port->queue.clear();
            emu.process.events.get(completion)->signaled = false;
            EXPECT_EQ(endpoint->cancel_pending_io(emu, iosb, 0), 0u);
            queue();
            endpoint->work(emu);
            expect_iosb(canceled);
            EXPECT_TRUE(port->queue.empty());
            EXPECT_FALSE(emu.process.events.get(completion)->signaled);
            EXPECT_EQ(packets->incoming.size(), 1u);
            const EMU_WSABUF<Traits> fresh_buffer{.len = 64, .buf = memory + 0x380};
            emu.memory.write_memory(memory + 0x220, &fresh_buffer, sizeof(fresh_buffer));
            const AFD_RECV_DATAGRAM_INFO<Traits> fresh_info{.BufferArray = memory + 0x220, .BufferCount = 1, .TdiFlags = 0x20};
            emu.memory.write_memory(memory + 0x180, &fresh_info, sizeof(fresh_info));
            io_device_context fresh{emu.memory};
            fresh.io_control_code = 0x1201b;
            fresh.io_status_block = {emu.memory, memory + 0xc0};
            fresh.file_handle = endpoint_handle;
            fresh.input_buffer = memory + 0x180;
            fresh.input_buffer_length = sizeof(fresh_info);
            ASSERT_EQ(endpoint->execute_ioctl(emu, fresh), STATUS_SUCCESS);
            EXPECT_EQ(emu.memory.read_memory<uint64_t>(memory + 0xc8), 7u);
            const auto payload = emu.memory.read_memory(memory + 0x380, 7);
            EXPECT_EQ(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()), "abcdefg");
            EXPECT_TRUE(packets->incoming.empty());
            expect_iosb(canceled);
            EXPECT_TRUE(port->queue.empty());
            EXPECT_FALSE(emu.process.events.get(completion)->signaled);
        }
    };

    TEST_F(AfdDatagramIrpPublicationTest, Raw19PendingFullSourceKeepsUpperStatusAndSevenBytes)
    {
        check_raw_control(16, false);
    }

    TEST_F(AfdDatagramIrpPublicationTest, Raw20PendingFullSourceKeepsCapturedFieldsAfterMutation)
    {
        check_raw_control(16, true);
    }

    TEST_F(AfdDatagramIrpPublicationTest, Raw21PendingShortSourceKeepsUpperStatusAndSevenBytes)
    {
        check_raw_control(8, false);
    }

    TEST_F(AfdDatagramIrpPublicationTest, Raw22PendingShortSourceKeepsCapturedFieldsAfterMutation)
    {
        check_raw_control(8, true);
    }

    TEST_F(AfdDatagramIrpPublicationTest, ReadyCopiedWarningKeepsUpperStatusAndOnePendingStyleNotification)
    {
        auto context = prepare(16, 3);
        endpoint->completion_port = port_handle;
        endpoint->completion_key = 0x1234;
        endpoint->completion_notification_flags = 1;
        context.apc_context = 0x5678;
        queue();
        ASSERT_EQ(endpoint->execute_ioctl(emu, context), STATUS_PENDING);
        const std::array<uint8_t, 16> expected{5, 0, 0, 0x80, 0xdd, 0xcc, 0xbb, 0xaa, 3, 0, 0, 0, 0, 0, 0, 0};
        expect_iosb(expected);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
        ASSERT_EQ(port->queue.size(), 1u);
        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_BUFFER_OVERFLOW);
        EXPECT_EQ(port->queue.front().io_status_block.Information, 3u);
        EXPECT_EQ(port->queue.front().key_context, 0x1234u);
        EXPECT_EQ(port->queue.front().apc_context, 0x5678u);
        port->queue.clear();
        endpoint->work(emu);
        expect_iosb(expected);
        EXPECT_TRUE(port->queue.empty());
        EXPECT_EQ(endpoint->cancel_pending_io(emu, iosb, 0), 0u);
    }

    TEST_F(AfdDatagramIrpPublicationTest, PendingCopiedWarningKeepsUpperStatusAndOneTerminalNotification)
    {
        auto context = prepare(16, 3);
        endpoint->completion_port = port_handle;
        endpoint->completion_notification_flags = 1;
        context.apc_context = 0x5678;
        ASSERT_EQ(endpoint->execute_ioctl(emu, context), STATUS_PENDING);
        expect_iosb(initial_iosb);
        EXPECT_TRUE(port->queue.empty());
        queue();
        endpoint->work(emu);
        const std::array<uint8_t, 16> expected{5, 0, 0, 0x80, 0xdd, 0xcc, 0xbb, 0xaa, 3, 0, 0, 0, 0, 0, 0, 0};
        expect_iosb(expected);
        ASSERT_EQ(port->queue.size(), 1u);
        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_BUFFER_OVERFLOW);
        EXPECT_EQ(port->queue.front().io_status_block.Information, 3u);
        port->queue.clear();
        endpoint->work(emu);
        EXPECT_TRUE(port->queue.empty());
        expect_iosb(expected);
    }

    TEST_F(AfdDatagramIrpPublicationTest, PendingShortSourceErrorKeepsCopiedInformationInCompletionMessage)
    {
        auto context = prepare(8);
        endpoint->completion_port = port_handle;
        endpoint->completion_notification_flags = 1;
        context.apc_context = 0x5678;
        ASSERT_EQ(endpoint->execute_ioctl(emu, context), STATUS_PENDING);
        expect_iosb(initial_iosb);
        queue();
        endpoint->work(emu);
        expect_iosb(short_source_iosb);
        ASSERT_EQ(port->queue.size(), 1u);
        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_BUFFER_TOO_SMALL);
        EXPECT_EQ(port->queue.front().io_status_block.Information, 7u);
    }

    TEST_F(AfdDatagramIrpPublicationTest, ImmediateSuccessKeepsDefaultSynchronousApcPolicy)
    {
        auto context = prepare(16);
        emulator_thread issuer{emu.memory};
        issuer.id = 0x1234;
        const auto thread_handle = emu.process.threads.store(std::move(issuer));
        emu.process.thread_handles_by_id.emplace(0x1234, thread_handle);
        context.issuer_thread_id = 0x1234;
        context.apc_routine = 0x567800;
        context.apc_context = 0x5678;
        endpoint->completion_port = port_handle;
        queue();
        ASSERT_EQ(endpoint->execute_ioctl(emu, context), STATUS_SUCCESS);
        expect_iosb(success_iosb);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_TRUE(emu.process.threads.get(thread_handle)->pending_apcs.empty());
        EXPECT_TRUE(port->queue.empty());
    }

    TEST_F(AfdDatagramIrpPublicationTest, PendingSuccessQueuesIssuerApcAndExcludesIocp)
    {
        auto context = prepare(16);
        emulator_thread issuer{emu.memory};
        issuer.id = 0x1234;
        const auto thread_handle = emu.process.threads.store(std::move(issuer));
        emu.process.thread_handles_by_id.emplace(0x1234, thread_handle);
        context.issuer_thread_id = 0x1234;
        context.apc_routine = 0x567800;
        context.apc_context = 0x5678;
        endpoint->completion_port = port_handle;
        ASSERT_EQ(endpoint->execute_ioctl(emu, context), STATUS_PENDING);
        expect_iosb(initial_iosb);
        EXPECT_TRUE(emu.process.threads.get(thread_handle)->pending_apcs.empty());
        queue();
        endpoint->work(emu);
        expect_iosb(success_iosb);
        const auto* thread = emu.process.threads.get(thread_handle);
        ASSERT_EQ(thread->pending_apcs.size(), 1u);
        EXPECT_EQ(thread->pending_apcs.front().apc_routine, 0x567800u);
        EXPECT_EQ(thread->pending_apcs.front().apc_argument1, 0x5678u);
        EXPECT_EQ(thread->pending_apcs.front().apc_argument2, iosb);
        EXPECT_EQ(thread->pending_apcs.front().io_status, static_cast<int32_t>(STATUS_SUCCESS));
        EXPECT_EQ(thread->pending_apcs.front().io_information, 7u);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_TRUE(port->queue.empty());
        endpoint->work(emu);
        EXPECT_EQ(thread->pending_apcs.size(), 1u);
    }

    TEST_F(AfdDatagramIrpPublicationTest, UncopiedValidationFailureDoesNotRetainSentinelInformation)
    {
        auto context = prepare(16);
        const EMU_WSABUF<Traits> invalid{.len = 64, .buf = 0x900000};
        emu.memory.write_memory(memory + 0x200, &invalid, sizeof(invalid));
        queue();
        ASSERT_EQ(endpoint->execute_ioctl(emu, context), STATUS_ACCESS_VIOLATION);
        const std::array<uint8_t, 16> expected{5, 0, 0, 0xc0, 0xdd, 0xcc, 0xbb, 0xaa, 0, 0, 0, 0, 0, 0, 0, 0};
        expect_iosb(expected);
        EXPECT_EQ(packets->incoming.size(), 1u);
        EXPECT_EQ(emu.memory.read_memory(memory + 0x300, 72), std::vector<std::byte>(72, std::byte{0xcc}));
    }

    TEST_F(AfdDatagramIrpPublicationTest, RawCancelEventPublishesOnlyStatusAndZeroInformation)
    {
        check_cancel_control(false, false);
    }

    TEST_F(AfdDatagramIrpPublicationTest, RawCancelIocpPublishesOneCanceledPacketWithExactContext)
    {
        check_cancel_control(true, false);
    }

    TEST_F(AfdDatagramIrpPublicationTest, RawCancelEventUsesCapturedFieldsAfterCallerMutation)
    {
        check_cancel_control(false, true);
    }

    TEST_F(AfdDatagramIrpPublicationTest, RawCancelIocpUsesCapturedFieldsAfterCallerMutation)
    {
        check_cancel_control(true, true);
    }

    TEST_F(AfdDatagramIrpPublicationTest, DatagramMethodVariantsRetainDefaultCancellationPublication)
    {
        for (const ULONG code : {0x12018u, 0x12019u, 0x1201au})
        {
            auto context = prepare(16);
            context.io_control_code = code;
            emu.process.events.get(completion)->signaled = false;
            EXPECT_EQ(endpoint->capture_completion_mode(emu, context), device_ioctl_completion_mode::default_completion);
            ASSERT_EQ(endpoint->execute_ioctl(emu, context), STATUS_PENDING);
            expect_iosb(initial_iosb);
            ASSERT_EQ(endpoint->cancel_pending_io(emu, iosb, 0), 1u);
            const std::array<uint8_t, 16> canceled{0x20, 1, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            expect_iosb(canceled);
            EXPECT_EQ(emu.process.events.get(completion)->ref_count, 1u);
        }
    }

    TEST_F(AfdDatagramIrpPublicationTest, DeserializedCancellationUsesCapturedFieldsWithoutTransientMode)
    {
        auto context = prepare(16);
        ASSERT_EQ(endpoint->execute_ioctl(emu, context), STATUS_PENDING);
        expect_iosb(initial_iosb);
        emulator_object<ULONG>{emu.memory, memory + 0x520}.write(0);
        emu.memory.set_memory(memory + 0x100, 0, sizeof(AFD_RECV_DATAGRAM_INFO<Traits>));
        emu.memory.set_memory(memory + 0x200, 0, sizeof(EMU_WSABUF<Traits>));
        utils::buffer_serializer saved{};
        endpoint->serialize(saved);
        utils::buffer_deserializer input{saved};
        input.register_factory<memory_manager_wrapper>([this] { return memory_manager_wrapper{emu.memory}; });
        input.register_factory<x64_emulator_wrapper>([this] { return x64_emulator_wrapper{emu.emu()}; });
        input.register_factory<windows_emulator_wrapper>([this] { return windows_emulator_wrapper{emu}; });
        input.register_factory<socket_factory_wrapper>([this] { return socket_factory_wrapper{emu.socket_factory()}; });
        io_device_container restored;
        restored.deserialize(input);
        utils::buffer_serializer resaved{};
        restored.serialize(resaved);
        EXPECT_EQ(saved.get_buffer(), resaved.get_buffer());
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 3u);
        ASSERT_EQ(restored.cancel_pending_io(emu, iosb, 0), 1u);
        const std::array<uint8_t, 16> canceled{0x20, 1, 0, 0xc0, 0xdd, 0xcc, 0xbb, 0xaa, 0, 0, 0, 0, 0, 0, 0, 0};
        expect_iosb(canceled);
        EXPECT_EQ(emu.process.events.get(completion)->ref_count, 2u);
        EXPECT_EQ(restored.cancel_pending_io(emu, iosb, 0), 0u);
        EXPECT_EQ(emu.memory.read_memory(memory + 0x300, 72), std::vector<std::byte>(72, std::byte{0xcc}));
        EXPECT_EQ(emu.memory.read_memory(memory + 0x500, 16), std::vector<std::byte>(16, std::byte{0xcc}));
        EXPECT_EQ(emu.memory.read_memory<ULONG>(memory + 0x520), 0u);
        EXPECT_EQ(endpoint->cancel_pending_io(emu, iosb, 0), 1u);
    }

    INSTANTIATE_TEST_SUITE_P(GuestBitness, AfdDatagramScatterGatherTest, testing::Bool());
}
