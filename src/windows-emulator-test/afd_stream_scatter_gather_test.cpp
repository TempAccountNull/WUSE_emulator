#include "emulation_test_utils.hpp"
#include <devices/afd_endpoint.hpp>
#include <devices/afd_types.hpp>

#include <algorithm>
#include <deque>
#include <limits>

namespace sogen::test
{
    namespace
    {
        struct stream_state
        {
            std::deque<std::byte> incoming;
            std::vector<std::byte> outgoing;
            size_t send_limit{std::numeric_limits<size_t>::max()};
            size_t receive_limit{std::numeric_limits<size_t>::max()};
            int last_error{};
            uint32_t query_status{};
            uint32_t expedited_bytes{};
            size_t query_calls{};
        };

        struct test_stream_socket final : network::i_socket
        {
            explicit test_stream_socket(std::shared_ptr<stream_state> shared)
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

            uint32_t query_receive_information(network::receive_information& value) override
            {
                ++state->query_calls;
                if (state->query_status != 0)
                {
                    return state->query_status;
                }
                value.normal_bytes = static_cast<uint32_t>(state->incoming.size());
                value.expedited_bytes = state->expedited_bytes;
                return 0;
            }

            uint32_t partial_disconnect(uint32_t, int64_t) override
            {
                return 0;
            }

            bool is_ready(bool) override
            {
                return true;
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

            sent_size send(std::span<const std::byte> data) override
            {
                const auto count = std::min(data.size(), state->send_limit);
                state->outgoing.insert(state->outgoing.end(), data.begin(), data.begin() + count);
                return static_cast<sent_size>(count);
            }

            sent_size sendto(const network::address&, std::span<const std::byte> data) override
            {
                return send(data);
            }

            sent_size recv(std::span<std::byte> data, const bool peek = false) override
            {
                if (state->incoming.empty())
                {
                    state->last_error = SERR(EWOULDBLOCK);
                    return -1;
                }
                state->last_error = 0;
                const auto count = std::min({data.size(), state->receive_limit, state->incoming.size()});
                for (size_t index = 0; index < count; ++index)
                {
                    data[index] = peek ? state->incoming[index] : state->incoming.front();
                    if (!peek)
                    {
                        state->incoming.pop_front();
                    }
                }
                return static_cast<sent_size>(count);
            }

            sent_size recvfrom(network::address&, std::span<std::byte> data, const bool peek = false) override
            {
                return recv(data, peek);
            }

            std::shared_ptr<stream_state> state;
        };

        struct test_stream_factory final : network::socket_factory
        {
            explicit test_stream_factory(std::shared_ptr<stream_state> shared)
                : state(std::move(shared))
            {
            }

            std::unique_ptr<network::i_socket> create_socket(int, int, int) override
            {
                return std::make_unique<test_stream_socket>(state);
            }

            int poll_sockets(std::span<network::poll_entry> entries) override
            {
                int ready = 0;
                for (auto& entry : entries)
                {
                    entry.revents = state->incoming.empty() ? 0 : entry.events;
                    ready += entry.revents != 0;
                }
                return ready;
            }

            std::shared_ptr<stream_state> state;
        };
    }

    class AfdStreamScatterGatherTest : public testing::TestWithParam<bool>
    {
      protected:
        static constexpr uint64_t memory = 0x200000;
        std::shared_ptr<stream_state> stream = std::make_shared<stream_state>();
        windows_emulator emu{[this] {
            emulator_settings settings{};
            settings.use_relative_time = true;
            settings.load_registry = false;
            emulator_interfaces interfaces{};
            interfaces.socket_factory = std::make_unique<test_stream_factory>(stream);
            return create_emulator(std::move(settings), {}, std::move(interfaces));
        }()};
        std::unique_ptr<io_device> device;
        handle completion{};

        void SetUp() override
        {
            ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x1000, memory_permission::read_write));
            const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
            emu.memory.write_memory(memory, creation.data(), sizeof(creation));
            device = create_afd_endpoint({.is_32_bit = GetParam()});
            device->create(emu, {.buffer = memory, .length = sizeof(creation)});
            event signal{};
            signal.type = SynchronizationEvent;
            completion = emu.process.events.store(std::move(signal));
        }

        template <typename Guest>
        void write_buffers(std::initializer_list<std::pair<ULONG, uint64_t>> entries)
        {
            using Traits = EmulatorTraits<Guest>;
            std::vector<EMU_WSABUF<Traits>> descriptors;
            descriptors.reserve(entries.size());
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
        NTSTATUS transfer(bool is_send, ULONG count, uint64_t array, ULONG tdi_flags = 0)
        {
            using Traits = EmulatorTraits<Guest>;
            io_device_context context{emu.memory};
            context.io_control_code = is_send ? 0x1201f : 0x12017;
            context.io_status_block = {emu.memory, memory + 0x80};
            context.event = completion;
            context.input_buffer = memory + 0x100;
            context.input_buffer_length = is_send ? sizeof(AFD_SEND_INFO<Traits>) : sizeof(AFD_RECV_INFO<Traits>);
            emu.memory.set_memory(memory + 0x80, 0xa5, 16);
            if (is_send)
            {
                const AFD_SEND_INFO<Traits> request{.BufferArray = static_cast<typename Traits::PVOID>(array), .BufferCount = count};
                emu.memory.write_memory(context.input_buffer, &request, sizeof(request));
            }
            else
            {
                const AFD_RECV_INFO<Traits> request{
                    .BufferArray = static_cast<typename Traits::PVOID>(array), .BufferCount = count, .TdiFlags = tdi_flags};
                emu.memory.write_memory(context.input_buffer, &request, sizeof(request));
            }
            return device->execute_ioctl(emu, context);
        }

        NTSTATUS query_receive_information(uint64_t output = memory + 0x500, uint32_t length = 8, uint64_t input = 0,
                                           uint32_t input_length = 0)
        {
            io_device_context context{emu.memory};
            context.io_control_code = 0x12033;
            context.io_status_block = {emu.memory, memory + 0x80};
            context.event = completion;
            context.input_buffer = input;
            context.input_buffer_length = input_length;
            context.output_buffer = output;
            context.output_buffer_length = length;
            emu.memory.set_memory(memory + 0x80, 0xa5, 16);
            emu.process.events.get(completion)->signaled = false;
            return device->execute_ioctl(emu, context);
        }

        NTSTATUS send(ULONG count, uint64_t array = memory + 0x200)
        {
            return GetParam() ? transfer<Emu32>(true, count, array) : transfer<Emu64>(true, count, array);
        }

        NTSTATUS receive(ULONG count, uint64_t array = memory + 0x200, ULONG tdi_flags = 0)
        {
            return GetParam() ? transfer<Emu32>(false, count, array, tdi_flags) : transfer<Emu64>(false, count, array, tdi_flags);
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

        uint64_t information()
        {
            return emu.memory.read_memory<uint64_t>(memory + 0x88);
        }
    };

    TEST_P(AfdStreamScatterGatherTest, StreamPeekScattersOnlyItsPrefixAndRetainsAllQueuedBytes)
    {
        for (const char value : std::string_view{"abcdefg"})
        {
            stream->incoming.push_back(static_cast<std::byte>(value));
        }
        buffers({{2, memory + 0x300}, {0, 0}, {3, memory + 0x320}});
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            emu.memory.set_memory(memory + 0x300, 0xa5, 16);
            emu.memory.set_memory(memory + 0x320, 0xa5, 16);
            ASSERT_EQ(receive(3, memory + 0x200, 0xa0), STATUS_SUCCESS);
            EXPECT_EQ(read(memory + 0x300, 2), "ab");
            EXPECT_EQ(read(memory + 0x320, 3), "cde");
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x323), 0xa5);
            EXPECT_EQ(information(), 5u);
            EXPECT_EQ(stream->incoming.size(), 7u);
        }
        ASSERT_EQ(receive(3), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x300, 2), "ab");
        EXPECT_EQ(read(memory + 0x320, 3), "cde");
        EXPECT_EQ(information(), 5u);
        EXPECT_EQ(stream->incoming.size(), 2u);
        ASSERT_EQ(receive(3), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x300, 2), "fg");
        EXPECT_EQ(information(), 2u);
        EXPECT_TRUE(stream->incoming.empty());
    }

    TEST_P(AfdStreamScatterGatherTest, SendGathersInOrderAcrossEmptyMember)
    {
        write(memory + 0x300, "abc");
        write(memory + 0x320, "de");
        write(memory + 0x340, "fghi");
        buffers({{3, memory + 0x300}, {0, 0}, {2, memory + 0x320}, {4, memory + 0x340}});
        ASSERT_EQ(send(4), STATUS_SUCCESS);
        EXPECT_EQ(information(), 9u);
        const std::string actual{reinterpret_cast<const char*>(stream->outgoing.data()), stream->outgoing.size()};
        EXPECT_EQ(actual, "abcdefghi");
    }

    TEST_P(AfdStreamScatterGatherTest, PartialSendReportsOnlyBytesAccepted)
    {
        stream->send_limit = 5;
        write(memory + 0x300, "abc");
        write(memory + 0x320, "defg");
        write(memory + 0x340, "hi");
        buffers({{3, memory + 0x300}, {4, memory + 0x320}, {2, memory + 0x340}});
        ASSERT_EQ(send(3), STATUS_SUCCESS);
        EXPECT_EQ(information(), 5u);
        const std::string actual{reinterpret_cast<const char*>(stream->outgoing.data()), stream->outgoing.size()};
        EXPECT_EQ(actual, "abcde");
    }

    TEST_P(AfdStreamScatterGatherTest, ReceiveScattersInOrderAcrossEmptyMember)
    {
        for (const char c : std::string_view{"abcdefghi"})
        {
            stream->incoming.push_back(static_cast<std::byte>(c));
        }
        emu.memory.set_memory(memory + 0x300, 0xa5, 16);
        emu.memory.set_memory(memory + 0x320, 0xa5, 16);
        emu.memory.set_memory(memory + 0x340, 0xa5, 16);
        buffers({{2, memory + 0x300}, {0, 0}, {3, memory + 0x320}, {5, memory + 0x340}});
        ASSERT_EQ(receive(4), STATUS_SUCCESS);
        EXPECT_EQ(information(), 9u);
        EXPECT_EQ(read(memory + 0x300, 2), "ab");
        EXPECT_EQ(read(memory + 0x320, 3), "cde");
        EXPECT_EQ(read(memory + 0x340, 4), "fghi");
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x344), 0xa5);
        EXPECT_TRUE(stream->incoming.empty());
    }

    TEST_P(AfdStreamScatterGatherTest, PartialReceiveLeavesLaterBuffersUntouched)
    {
        for (const char c : std::string_view{"abcdef"})
        {
            stream->incoming.push_back(static_cast<std::byte>(c));
        }
        stream->receive_limit = 3;
        emu.memory.set_memory(memory + 0x300, 0xa5, 16);
        emu.memory.set_memory(memory + 0x320, 0xa5, 16);
        emu.memory.set_memory(memory + 0x340, 0xa5, 16);
        buffers({{2, memory + 0x300}, {3, memory + 0x320}, {3, memory + 0x340}});
        ASSERT_EQ(receive(3), STATUS_SUCCESS);
        EXPECT_EQ(information(), 3u);
        EXPECT_EQ(read(memory + 0x300, 2), "ab");
        EXPECT_EQ(read(memory + 0x320, 1), "c");
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x321), 0xa5);
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x340), 0xa5);
        EXPECT_EQ(stream->incoming.size(), 3u);
    }

    TEST_P(AfdStreamScatterGatherTest, NonblockingEmptyReceiveCanRetryAfterResponseArrives)
    {
        const std::array<uint64_t, 2> set_nonblocking{2, 1};
        emu.memory.write_memory(memory + 0x100, set_nonblocking.data(), sizeof(set_nonblocking));
        io_device_context mode_context{emu.memory};
        mode_context.io_control_code = 0x1203b;
        mode_context.io_status_block = {emu.memory, memory + 0x80};
        mode_context.input_buffer = memory + 0x100;
        mode_context.input_buffer_length = sizeof(set_nonblocking);
        ASSERT_EQ(device->execute_ioctl(emu, mode_context), STATUS_SUCCESS);

        emu.memory.set_memory(memory + 0x300, 0xa5, 6);
        buffers({{6, memory + 0x300}});
        ASSERT_EQ(receive(1), STATUS_DEVICE_NOT_READY);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x80), STATUS_DEVICE_NOT_READY);
        EXPECT_EQ(information(), 0u);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);

        emu.process.events.get(completion)->signaled = false;
        for (const char value : std::string_view{"BAP251"})
        {
            stream->incoming.push_back(static_cast<std::byte>(value));
        }
        device->work(emu);
        EXPECT_FALSE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x300), 0xa5);

        ASSERT_EQ(receive(1), STATUS_SUCCESS);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(information(), 6u);
        EXPECT_EQ(read(memory + 0x300, 6), "BAP251");
        EXPECT_TRUE(stream->incoming.empty());
    }

    TEST_P(AfdStreamScatterGatherTest, InvalidDescriptorRejectsBeforeSocketTransfer)
    {
        buffers({{2, memory + 0x300}, {1, 0}});
        EXPECT_EQ(send(2), STATUS_ACCESS_VIOLATION);
        EXPECT_TRUE(stream->outgoing.empty());
        EXPECT_EQ(receive(2), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(send(0), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(receive(0), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(send(2, 0), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(receive(2, 0), STATUS_INVALID_PARAMETER);
    }

    TEST_P(AfdStreamScatterGatherTest, PendingReceiveKeepsOriginalDescriptors)
    {
        emu.memory.set_memory(memory + 0x300, 0xa5, 16);
        emu.memory.set_memory(memory + 0x320, 0xa5, 16);
        emu.memory.set_memory(memory + 0x380, 0xa5, 16);
        buffers({{2, memory + 0x300}, {3, memory + 0x320}});

        ASSERT_EQ(receive(2), STATUS_PENDING);
        EXPECT_FALSE(emu.process.events.get(completion)->signaled);

        // A pending receive owns a copy of the WSABUF descriptors.
        buffers({{5, memory + 0x380}, {0, 0}});
        for (const char c : std::string_view{"abcde"})
        {
            stream->incoming.push_back(static_cast<std::byte>(c));
        }
        device->work(emu);

        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(information(), 5u);
        EXPECT_EQ(read(memory + 0x300, 2), "ab");
        EXPECT_EQ(read(memory + 0x320, 3), "cde");
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x380), 0xa5);
        EXPECT_TRUE(stream->incoming.empty());
    }

    TEST_P(AfdStreamScatterGatherTest, ReceiveInformationWritesTwoCountersAndExactlyEightBytes)
    {
        emu.memory.set_memory(memory + 0x500, 0xcc, 24);
        ASSERT_EQ(query_receive_information(), STATUS_SUCCESS);
        const auto empty = emu.memory.read_memory<network::receive_information>(memory + 0x500);
        EXPECT_EQ(empty.normal_bytes, 0u);
        EXPECT_EQ(empty.expedited_bytes, 0u);
        EXPECT_EQ(information(), 8u);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        for (const char value : std::string_view{"abcdefg"})
        {
            stream->incoming.push_back(static_cast<std::byte>(value));
        }
        stream->expedited_bytes = 3;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            emu.memory.set_memory(memory + 0x500, 0xcc, 24);
            ASSERT_EQ(query_receive_information(memory + 0x501, 16), STATUS_SUCCESS);
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x500), 0xcc);
            const auto counts = emu.memory.read_memory<network::receive_information>(memory + 0x501);
            EXPECT_EQ(counts.normal_bytes, 7u);
            EXPECT_EQ(counts.expedited_bytes, 3u);
            EXPECT_EQ(information(), 8u);
            EXPECT_EQ(stream->incoming.size(), 7u);
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x509), 0xcc);
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x517), 0xcc);
        }
        EXPECT_EQ(stream->query_calls, 3u);
    }

    TEST_P(AfdStreamScatterGatherTest, ReceiveInformationTracksPeekThenPartialAndFullConsume)
    {
        for (const char value : std::string_view{"abcdefg"})
        {
            stream->incoming.push_back(static_cast<std::byte>(value));
        }
        buffers({{3, memory + 0x300}});
        ASSERT_EQ(query_receive_information(), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x500), 7u);
        ASSERT_EQ(receive(1, memory + 0x200, 0xa0), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x300, 3), "abc");
        ASSERT_EQ(query_receive_information(), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x500), 7u);
        ASSERT_EQ(receive(1), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x300, 3), "abc");
        ASSERT_EQ(query_receive_information(), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x500), 4u);
        buffers({{4, memory + 0x300}});
        ASSERT_EQ(receive(1), STATUS_SUCCESS);
        EXPECT_EQ(read(memory + 0x300, 4), "defg");
        ASSERT_EQ(query_receive_information(), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x500), 0u);
        EXPECT_TRUE(stream->incoming.empty());
    }

    TEST_P(AfdStreamScatterGatherTest, ReceiveInformationRejectsShortAndInvalidOutputsWithoutConsuming)
    {
        for (const char value : std::string_view{"abc"})
        {
            stream->incoming.push_back(static_cast<std::byte>(value));
        }
        for (uint32_t length = 0; length < 8; ++length)
        {
            emu.memory.set_memory(memory + 0x500, 0xcc, 16);
            EXPECT_EQ(query_receive_information(memory + 0x500, length), STATUS_BUFFER_TOO_SMALL);
            EXPECT_EQ(stream->query_calls, 0u);
            EXPECT_EQ(emu.memory.read_memory<uint64_t>(memory + 0x500), 0xccccccccccccccccULL);
            EXPECT_EQ(information(), 0u);
            EXPECT_EQ(stream->incoming.size(), 3u);
        }
        EXPECT_EQ(query_receive_information(0, 8), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(information(), 0u);
        EXPECT_EQ(stream->incoming.size(), 3u);
        EXPECT_EQ(query_receive_information(memory + 0x2000, 8), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(information(), 0u);
        EXPECT_EQ(stream->incoming.size(), 3u);
        ASSERT_EQ(query_receive_information(), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x500), 3u);
    }

    TEST_P(AfdStreamScatterGatherTest, ReceiveInformationPropagatesBackendFailureWithoutOutputOrQueueMutation)
    {
        stream->incoming.push_back(std::byte{'a'});
        for (const auto status : std::array{static_cast<NTSTATUS>(STATUS_NOT_SUPPORTED), static_cast<NTSTATUS>(STATUS_INVALID_PARAMETER)})
        {
            stream->query_status = static_cast<uint32_t>(status);
            emu.memory.set_memory(memory + 0x500, 0xcc, 16);
            EXPECT_EQ(query_receive_information(), status);
            EXPECT_EQ(emu.memory.read_memory<uint64_t>(memory + 0x500), 0xccccccccccccccccULL);
            EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x80), static_cast<uint32_t>(status));
            EXPECT_EQ(information(), 0u);
            EXPECT_TRUE(emu.process.events.get(completion)->signaled);
            EXPECT_EQ(stream->incoming.size(), 1u);
        }
        EXPECT_EQ(stream->query_calls, 2u);
        stream->query_status = 0;
        ASSERT_EQ(query_receive_information(), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x500), 1u);
    }

    TEST_P(AfdStreamScatterGatherTest, ReceiveInformationDoesNotReadOrValidateCallerInput)
    {
        stream->incoming.push_back(std::byte{'z'});
        // This calls the endpoint handler; syscall-layer input probing is a separate boundary.
        ASSERT_EQ(query_receive_information(memory + 0x500, 8, 0xdeadbeef, 0xffffffff), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x500), 1u);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x504), 0u);
        EXPECT_EQ(information(), 8u);
        EXPECT_EQ(stream->incoming.size(), 1u);
    }

    TEST_P(AfdStreamScatterGatherTest, EmptyReceiveInformationCompletesWithoutRetainingPendingWork)
    {
        ASSERT_EQ(query_receive_information(), STATUS_SUCCESS);
        EXPECT_EQ(information(), 8u);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(memory + 0x500), 0u);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        emu.process.events.get(completion)->signaled = false;
        const auto calls = stream->query_calls;
        device->work(emu);
        EXPECT_EQ(stream->query_calls, calls);
        EXPECT_FALSE(emu.process.events.get(completion)->signaled);
        EXPECT_TRUE(stream->incoming.empty());
    }

    INSTANTIATE_TEST_SUITE_P(GuestBitness, AfdStreamScatterGatherTest, testing::Bool());
}
