#include "emulation_test_utils.hpp"
#include <devices/afd_endpoint.hpp>
#include <devices/afd_types.hpp>
#include <network/socket_factory.hpp>
#include <network/static_socket_factory.hpp>
#include <network/socket_wrapper.hpp>
#include <utils/finally.hpp>
#include <syscall_utils.hpp>

#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <string_view>
#include <vector>

namespace sogen::syscalls
{
    NTSTATUS handle_NtClose(const syscall_context&, handle);
}

namespace sogen::test
{
    namespace
    {
        struct adjustable_clock final : utils::clock
        {
            steady_time_point steady{std::chrono::steady_clock::now()};

            steady_time_point steady_now() override
            {
                return steady;
            }
        };

        struct pending_socket_state
        {
            std::deque<std::byte> incoming;
            std::vector<std::byte> outgoing;
            bool send_blocked{true};
            bool connect_blocked{};
            uint32_t disconnect_mode{};
            int last_error{};
        };

        struct pending_test_socket final : network::i_socket
        {
            explicit pending_test_socket(std::shared_ptr<pending_socket_state> shared)
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

            uint32_t partial_disconnect(const uint32_t mode, int64_t) override
            {
                state->disconnect_mode |= mode;
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
                if (state->connect_blocked)
                {
                    state->last_error = SERR(EWOULDBLOCK);
                    return false;
                }
                state->last_error = 0;
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

            sent_size send(std::span<const std::byte> bytes) override
            {
                if (state->disconnect_mode & 4)
                {
                    state->last_error = SERR(ECONNABORTED);
                    return -1;
                }
                if (state->disconnect_mode & 1)
                {
                    state->last_error = SERR(ESHUTDOWN);
                    return -1;
                }
                if (state->send_blocked)
                {
                    state->last_error = SERR(EWOULDBLOCK);
                    return -1;
                }
                state->last_error = 0;
                state->outgoing.insert(state->outgoing.end(), bytes.begin(), bytes.end());
                return static_cast<sent_size>(bytes.size());
            }

            sent_size sendto(const network::address&, std::span<const std::byte> bytes) override
            {
                return send(bytes);
            }

            sent_size recv(std::span<std::byte> bytes) override
            {
                if (state->incoming.empty())
                {
                    state->last_error = SERR(EWOULDBLOCK);
                    return -1;
                }
                state->last_error = 0;
                const auto count = std::min(bytes.size(), state->incoming.size());
                for (size_t index = 0; index < count; ++index)
                {
                    bytes[index] = state->incoming.front();
                    state->incoming.pop_front();
                }
                return static_cast<sent_size>(count);
            }

            sent_size recvfrom(network::address&, std::span<std::byte> bytes) override
            {
                return recv(bytes);
            }

            std::shared_ptr<pending_socket_state> state;
        };

        struct pending_test_factory final : network::socket_factory
        {
            explicit pending_test_factory(std::shared_ptr<pending_socket_state> shared)
                : state(std::move(shared))
            {
            }

            std::unique_ptr<network::i_socket> create_socket(int, int, int) override
            {
                return std::make_unique<pending_test_socket>(state);
            }

            int poll_sockets(std::span<network::poll_entry> entries) override
            {
                int ready = 0;
                for (auto& entry : entries)
                {
                    entry.revents = 0;
                    if (!state->incoming.empty())
                    {
                        entry.revents |= entry.events & POLLIN;
                    }
                    if (!state->send_blocked)
                    {
                        entry.revents |= entry.events & POLLOUT;
                    }
                    ready += entry.revents != 0;
                }
                return ready;
            }

            std::shared_ptr<pending_socket_state> state;
        };
    }

    class AfdPendingRequestTest : public testing::TestWithParam<bool>
    {
      protected:
        static constexpr uint64_t memory = 0x220000;
        std::shared_ptr<pending_socket_state> socket = std::make_shared<pending_socket_state>();
        adjustable_clock* test_clock{};
        windows_emulator emu{[this] {
            emulator_settings settings{};
            settings.use_relative_time = true;
            settings.load_registry = false;
            emulator_interfaces interfaces{};
            interfaces.socket_factory = std::make_unique<pending_test_factory>(socket);
            auto clock = std::make_unique<adjustable_clock>();
            test_clock = clock.get();
            interfaces.clock = std::move(clock);
            return create_emulator(std::move(settings), {}, std::move(interfaces));
        }()};
        std::unique_ptr<io_device> device;
        handle receive_event{};
        handle send_event{};
        handle second_send_event{};
        handle poll_event{};

        void SetUp() override
        {
            ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x1000, memory_permission::read_write));
            const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
            emu.memory.write_memory(memory, creation.data(), sizeof(creation));
            device = create_afd_endpoint({.is_32_bit = GetParam()});
            device->create(emu, {.buffer = memory, .length = sizeof(creation)});
            event receive_signal{};
            receive_signal.type = SynchronizationEvent;
            receive_event = emu.process.events.store(std::move(receive_signal));
            event send_signal{};
            send_signal.type = SynchronizationEvent;
            send_event = emu.process.events.store(std::move(send_signal));
            event second_send_signal{};
            second_send_signal.type = SynchronizationEvent;
            second_send_event = emu.process.events.store(std::move(second_send_signal));
            event poll_signal{};
            poll_signal.type = SynchronizationEvent;
            poll_event = emu.process.events.store(std::move(poll_signal));
            emu.memory.set_memory(memory + 0x400, 0xa5, 8);
            emu.memory.write_memory(memory + 0x500, "XYZ", 3);
            emu.memory.write_memory(memory + 0x700, "abc", 3);
        }

        template <typename Guest>
        NTSTATUS transfer(bool is_send, ULONG tdi_flags = 0, io_device* endpoint = nullptr, handle file_handle = {},
                          uint32_t issuer_thread_id = 0, uint64_t apc_routine = 0, uint64_t apc_context = 0)
        {
            using Traits = EmulatorTraits<Guest>;
            const uint64_t info = memory + (is_send ? 0x120 : 0x100);
            const uint64_t request = memory + (is_send ? 0x240 : 0x200);
            const uint64_t descriptor = memory + (is_send ? 0x340 : 0x300);
            const uint64_t data = memory + (is_send ? 0x500 : 0x400);
            const EMU_WSABUF<Traits> wsabuf{.len = 3, .buf = static_cast<typename Traits::PVOID>(data)};
            emu.memory.write_memory(descriptor, &wsabuf, sizeof(wsabuf));
            io_device_context context{emu.memory};
            context.io_control_code = is_send ? 0x1201f : 0x12017;
            context.file_handle = file_handle;
            context.issuer_thread_id = issuer_thread_id;
            context.apc_routine = apc_routine;
            context.apc_context = apc_context;
            context.io_status_block = {emu.memory, info};
            context.event = is_send ? send_event : receive_event;
            context.input_buffer = request;
            context.input_buffer_length = is_send ? sizeof(AFD_SEND_INFO<Traits>) : sizeof(AFD_RECV_INFO<Traits>);
            if (is_send)
            {
                const AFD_SEND_INFO<Traits> input{.BufferArray = static_cast<typename Traits::PVOID>(descriptor), .BufferCount = 1};
                emu.memory.write_memory(request, &input, sizeof(input));
            }
            else
            {
                const AFD_RECV_INFO<Traits> input{
                    .BufferArray = static_cast<typename Traits::PVOID>(descriptor), .BufferCount = 1, .TdiFlags = tdi_flags};
                emu.memory.write_memory(request, &input, sizeof(input));
            }
            return (endpoint ? endpoint : device.get())->execute_ioctl(emu, context);
        }

        NTSTATUS transfer(bool is_send, ULONG tdi_flags = 0, io_device* endpoint = nullptr, handle file_handle = {},
                          uint32_t issuer_thread_id = 0, uint64_t apc_routine = 0, uint64_t apc_context = 0)
        {
            return GetParam() ? transfer<Emu32>(is_send, tdi_flags, endpoint, file_handle, issuer_thread_id, apc_routine, apc_context)
                              : transfer<Emu64>(is_send, tdi_flags, endpoint, file_handle, issuer_thread_id, apc_routine, apc_context);
        }

        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> status(bool is_send)
        {
            return emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(memory + (is_send ? 0x120 : 0x100));
        }

        template <typename Guest>
        NTSTATUS second_send()
        {
            using Traits = EmulatorTraits<Guest>;
            constexpr uint64_t request = memory + 0x640;
            constexpr uint64_t descriptor = memory + 0x680;
            const EMU_WSABUF<Traits> wsabuf{.len = 3, .buf = static_cast<typename Traits::PVOID>(memory + 0x700)};
            const AFD_SEND_INFO<Traits> input{.BufferArray = static_cast<typename Traits::PVOID>(descriptor), .BufferCount = 1};
            emu.memory.write_memory(descriptor, &wsabuf, sizeof(wsabuf));
            emu.memory.write_memory(request, &input, sizeof(input));
            io_device_context context{emu.memory};
            context.io_control_code = 0x1201f;
            context.io_status_block = {emu.memory, memory + 0x620};
            context.event = second_send_event;
            context.input_buffer = request;
            context.input_buffer_length = sizeof(input);
            return device->execute_ioctl(emu, context);
        }

        template <typename Guest>
        NTSTATUS poll_with_expired_deadline()
        {
            using Traits = EmulatorTraits<Guest>;
            emu.process.is_wow64_process = GetParam();
            const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
            emu.memory.write_memory(memory, creation.data(), sizeof(creation));
            const auto endpoint_handle =
                emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});

            AFD_POLL_INFO<Traits> input{};
            input.Timeout.QuadPart = -10000; // One millisecond, in NT 100-ns relative ticks.
            input.NumberOfHandles = 1;
            input.Handles[0].Handle = static_cast<typename Traits::HANDLE>(endpoint_handle.bits);
            input.Handles[0].PollEvents = AFD_POLL_RECEIVE;
            emu.memory.write_memory(memory + 0x800, &input, sizeof(input));

            io_device_context context{emu.memory};
            context.io_control_code = 0x12024;
            context.io_status_block = {emu.memory, memory + 0x780};
            context.event = poll_event;
            context.input_buffer = memory + 0x800;
            context.input_buffer_length = sizeof(input);
            context.output_buffer = context.input_buffer;
            context.output_buffer_length = context.input_buffer_length;
            return device->execute_ioctl(emu, context);
        }
    };

    TEST_P(AfdPendingRequestTest, AsyncConnectHelperUsesGuestLayoutAndCompletesApcIocpOnlyOnce)
    {
        emu.process.is_wow64_process = GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        const auto helper_handle = emu.process.devices.store(io_device_container{u"Afd\\AsyncConnectHlp", emu, {}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        auto* helper = emu.process.devices.get(helper_handle);
        ASSERT_NE(endpoint, nullptr);
        ASSERT_NE(helper, nullptr);
        const auto port_handle = emu.process.io_completions.store(io_completion{});
        helper->completion_port = port_handle;
        helper->completion_key = 0x9876;

        emulator_thread issuer{emu.memory};
        issuer.id = 0x1234;
        const auto thread_handle = emu.process.threads.store(std::move(issuer));
        emu.process.thread_handles_by_id.emplace(0x1234, thread_handle);

        const auto submit = [&]<typename Guest>(const uint64_t apc_routine) {
            using Traits = EmulatorTraits<Guest>;
            AFD_CONNECT_JOIN_INFO_TL<Traits> info{};
            info.ConnectEndpoint = static_cast<typename Traits::HANDLE>(endpoint_handle.bits);
            info.RemoteAddress.sa_family = 2; // AF_INET
            emu.memory.write_memory(memory + 0x800, &info, sizeof(info));
            io_device_context context{emu.memory};
            context.file_handle = helper_handle;
            context.issuer_thread_id = 0x1234;
            context.io_control_code = 0x12007;
            context.io_status_block = {emu.memory, memory + 0x100};
            context.event = receive_event;
            context.apc_routine = apc_routine;
            context.apc_context = 0xfeed;
            context.input_buffer = memory + 0x800;
            context.input_buffer_length = sizeof(info);
            return helper->execute_ioctl(emu, context);
        };
        const auto invoke = [&](const uint64_t apc_routine) {
            return GetParam() ? submit.template operator()<Emu32>(apc_routine) : submit.template operator()<Emu64>(apc_routine);
        };

        ASSERT_EQ(invoke(0), STATUS_SUCCESS);
        auto* port = emu.process.io_completions.get(port_handle);
        ASSERT_NE(port, nullptr);
        ASSERT_EQ(port->queue.size(), 1u);
        EXPECT_EQ(port->queue.front().key_context, 0x9876u);
        EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
        port->queue.clear();
        emu.process.events.get(receive_event)->signaled = false;

        ASSERT_EQ(invoke(0x567800), STATUS_SUCCESS);
        EXPECT_TRUE(port->queue.empty());
        EXPECT_EQ(emu.process.threads.get(thread_handle)->pending_apcs.size(), 1u);
        EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
        emu.process.events.get(receive_event)->signaled = false;

        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};
        emu.memory.write_memory(memory + 0x100, &initial, sizeof(initial));
        socket->connect_blocked = true;
        ASSERT_EQ(invoke(0), STATUS_PENDING);
        EXPECT_EQ(status(false).Status, initial.Status);
        EXPECT_EQ(status(false).Information, initial.Information);
        EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
        EXPECT_TRUE(port->queue.empty());

        socket->connect_blocked = false;
        socket->send_blocked = false; // The test poller reports a writable socket for connect completion.
        endpoint->work(emu);
        EXPECT_EQ(status(false).Status, STATUS_SUCCESS);
        EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
        ASSERT_EQ(port->queue.size(), 1u);
        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_SUCCESS);
    }

    TEST_P(AfdPendingRequestTest, QueryHandlesMatchesNativeBufferAndStatusContract)
    {
        constexpr uint64_t input = memory + 0x800;
        constexpr uint64_t output = memory + 0x880;
        constexpr uint64_t iosb = memory + 0x100;
        const auto expected_size = GetParam() ? sizeof(AFD_HANDLE_INFO<EmulatorTraits<Emu32>>)
                                              : sizeof(AFD_HANDLE_INFO<EmulatorTraits<Emu64>>);
        const auto invoke = [&](const uint32_t flags, const uint32_t input_length, const uint32_t output_length) {
            emu.memory.write_memory(input, &flags, sizeof(flags));
            emu.memory.set_memory(output, 0xa5, 24);
            io_device_context context{emu.memory};
            context.io_control_code = 0x12037;
            context.io_status_block = {emu.memory, iosb};
            context.input_buffer = input;
            context.input_buffer_length = input_length;
            context.output_buffer = output;
            context.output_buffer_length = output_length;
            return device->execute_ioctl(emu, context);
        };
        for (const auto flags : {1u, 2u, 3u})
        {
            EXPECT_EQ(invoke(flags, 4, static_cast<uint32_t>(expected_size)), STATUS_SUCCESS);
            const auto block = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
            EXPECT_EQ(block.Status, STATUS_SUCCESS);
            EXPECT_EQ(block.Information, expected_size);
            const auto bytes = emu.memory.read_memory(output, 24);
            EXPECT_TRUE(std::all_of(bytes.begin(), bytes.begin() + expected_size,
                                    [](const std::byte value) { return value == std::byte{0xff}; }));
            EXPECT_TRUE(std::all_of(bytes.begin() + expected_size, bytes.end(),
                                    [](const std::byte value) { return value == std::byte{0xa5}; }));
        }
        for (const auto flags : {0u, 4u})
        {
            EXPECT_EQ(invoke(flags, 4, static_cast<uint32_t>(expected_size)), STATUS_INVALID_PARAMETER);
            const auto block = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
            EXPECT_EQ(block.Status, STATUS_INVALID_PARAMETER);
            EXPECT_EQ(block.Information, 0u);
            const auto bytes = emu.memory.read_memory(output, 24);
            EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(),
                                    [](const std::byte value) { return value == std::byte{0xa5}; }));
        }
        EXPECT_EQ(invoke(3, 3, static_cast<uint32_t>(expected_size)), STATUS_BUFFER_TOO_SMALL);
        EXPECT_EQ(invoke(3, 4, static_cast<uint32_t>(expected_size - 1)), STATUS_BUFFER_TOO_SMALL);
    }

    TEST_P(AfdPendingRequestTest, PartialDisconnectMatchesMeasuredNativeStatusAndSendState)
    {
        constexpr uint64_t input = memory + 0x800;
        constexpr uint64_t iosb = memory + 0x100;
        socket->send_blocked = false;
        const auto invoke = [&](const uint32_t mode, const uint32_t input_length, const int64_t timeout = 0) {
            AFD_PARTIAL_DISCONNECT_INFO request{};
            request.DisconnectMode = mode;
            request.Timeout.QuadPart = timeout;
            emu.memory.write_memory(input, &request, sizeof(request));
            io_device_context context{emu.memory};
            context.io_control_code = 0x1202b;
            context.io_status_block = {emu.memory, iosb};
            context.input_buffer = input;
            context.input_buffer_length = input_length;
            return device->execute_ioctl(emu, context);
        };
        // Native Windows: short input completes with INVALID_PARAMETER, Info=0.
        for (const auto length : {0u, 15u})
        {
            EXPECT_EQ(invoke(0, length), STATUS_INVALID_PARAMETER);
            const auto block = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
            EXPECT_EQ(block.Status, STATUS_INVALID_PARAMETER);
            EXPECT_EQ(block.Information, 0u);
        }
        // Native Windows: these modes and timeout values complete synchronously.
        for (const auto [mode, timeout] : {std::pair<uint32_t, int64_t>{0, 0}, {2, -10000}, {8, 1}})
        {
            EXPECT_EQ(invoke(mode, sizeof(AFD_PARTIAL_DISCONNECT_INFO), timeout), STATUS_SUCCESS);
            const auto block = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
            EXPECT_EQ(block.Status, STATUS_SUCCESS);
            EXPECT_EQ(block.Information, 0u);
        }
        EXPECT_EQ(transfer(true), STATUS_SUCCESS);
        EXPECT_EQ(status(true).Information, 3u);
        emu.process.events.get(send_event)->signaled = false;

        EXPECT_EQ(invoke(1, sizeof(AFD_PARTIAL_DISCONNECT_INFO), -10000000), STATUS_SUCCESS);
        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{
            .Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};
        emu.memory.write_memory(memory + 0x120, &initial, sizeof(initial));
        EXPECT_EQ(transfer(true), STATUS_PIPE_DISCONNECTED);
        EXPECT_EQ(status(true).Status, initial.Status);
        EXPECT_EQ(status(true).Information, initial.Information);
        EXPECT_FALSE(emu.process.events.get(send_event)->signaled);

        EXPECT_EQ(invoke(4, sizeof(AFD_PARTIAL_DISCONNECT_INFO)), STATUS_SUCCESS);
        emu.memory.write_memory(memory + 0x120, &initial, sizeof(initial));
        EXPECT_EQ(transfer(true), STATUS_LOCAL_DISCONNECT);
        EXPECT_EQ(status(true).Status, initial.Status);
        EXPECT_EQ(status(true).Information, initial.Information);
        EXPECT_FALSE(emu.process.events.get(send_event)->signaled);
    }

    TEST_P(AfdPendingRequestTest, EnumNetworkEventsWritesEveryStatusSlot)
    {
        constexpr uint64_t output = memory + 0x880;
        constexpr uint64_t iosb = memory + 0x100;
        const auto invoke = [&](const uint32_t length) {
            emu.memory.set_memory(output, 0xa5, 64);
            io_device_context context{emu.memory};
            context.io_control_code = 0x1208b;
            context.io_status_block = {emu.memory, iosb};
            context.output_buffer = output;
            context.output_buffer_length = length;
            return device->execute_ioctl(emu, context);
        };
        EXPECT_EQ(invoke(55), STATUS_INVALID_PARAMETER);
        auto block = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
        EXPECT_EQ(block.Status, STATUS_INVALID_PARAMETER);
        EXPECT_EQ(block.Information, 0u);
        auto bytes = emu.memory.read_memory(output, 64);
        EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(),
                                [](const std::byte value) { return value == std::byte{0xa5}; }));
        EXPECT_EQ(invoke(64), STATUS_SUCCESS);
        block = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
        EXPECT_EQ(block.Status, STATUS_SUCCESS);
        EXPECT_EQ(block.Information, sizeof(AFD_ENUM_NETWORK_EVENTS_INFO));
        bytes = emu.memory.read_memory(output, 64);
        EXPECT_TRUE(std::all_of(bytes.begin(), bytes.begin() + sizeof(AFD_ENUM_NETWORK_EVENTS_INFO),
                                [](const std::byte value) { return value == std::byte{0}; }));
        EXPECT_TRUE(std::all_of(bytes.begin() + sizeof(AFD_ENUM_NETWORK_EVENTS_INFO), bytes.end(),
                                [](const std::byte value) { return value == std::byte{0xa5}; }));
    }

    TEST_P(AfdPendingRequestTest, PendingIoStatusBlockRetainsCallerFieldsUntilCompletion)
    {
        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};
        emu.memory.write_memory(memory + 0x100, &initial, sizeof(initial));

        ASSERT_EQ(transfer(false), STATUS_PENDING);
        EXPECT_EQ(status(false).Status, initial.Status);
        EXPECT_EQ(status(false).Information, initial.Information);

        socket->incoming.push_back(std::byte{'q'});
        device->work(emu);

        EXPECT_EQ(status(false).Status, STATUS_SUCCESS);
        EXPECT_EQ(status(false).Information, 1u);
        EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
    }

    TEST_P(AfdPendingRequestTest, ImmediateInvalidRequestDoesNotCompleteApcEventOrIocp)
    {
        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};
        emu.memory.write_memory(memory + 0x100, &initial, sizeof(initial));

        emulator_thread issuer{emu.memory};
        issuer.id = 0x1234;
        const auto thread_handle = emu.process.threads.store(std::move(issuer));
        emu.process.thread_handles_by_id.emplace(0x1234, thread_handle);

        io_device_context request{emu.memory};
        request.io_control_code = 0x120b0; // AFD_ADDRESS_LIST_QUERY with missing required input.
        request.io_status_block = {emu.memory, memory + 0x100};
        request.event = receive_event;
        request.issuer_thread_id = 0x1234;
        request.apc_routine = 0x567800;
        request.apc_context = 0xfeed;
        ASSERT_EQ(device->execute_ioctl(emu, request), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(status(false).Status, initial.Status);
        EXPECT_EQ(status(false).Information, initial.Information);
        EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
        EXPECT_TRUE(emu.process.threads.get(thread_handle)->pending_apcs.empty());

        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        const auto port_handle = emu.process.io_completions.store(io_completion{});
        endpoint->completion_port = port_handle;
        endpoint->completion_key = 0x9876;

        request.apc_routine = 0;
        request.file_handle = endpoint_handle;
        ASSERT_EQ(endpoint->execute_ioctl(emu, request), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(status(false).Status, initial.Status);
        EXPECT_EQ(status(false).Information, initial.Information);
        EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
        EXPECT_TRUE(emu.process.io_completions.get(port_handle)->queue.empty());
    }

    TEST_P(AfdPendingRequestTest, ReceiveAndSendCompleteIndependentlyOnSameEndpoint)
    {
        ASSERT_EQ(transfer(false), STATUS_PENDING);
        ASSERT_EQ(transfer(true), STATUS_PENDING);
        EXPECT_EQ(status(false).Status, STATUS_SUCCESS);
        EXPECT_EQ(status(true).Status, STATUS_SUCCESS);
        EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
        EXPECT_FALSE(emu.process.events.get(send_event)->signaled);

        for (const char value : std::string_view{"abc"})
        {
            socket->incoming.push_back(static_cast<std::byte>(value));
        }
        device->work(emu);

        EXPECT_EQ(status(false).Status, STATUS_SUCCESS);
        EXPECT_EQ(status(false).Information, 3u);
        EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
        EXPECT_FALSE(emu.process.events.get(send_event)->signaled);
        const auto received = emu.memory.read_memory(memory + 0x400, 3);
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(received.data()), received.size()), "abc");
        EXPECT_EQ(status(true).Status, STATUS_SUCCESS);

        socket->send_blocked = false;
        device->work(emu);

        EXPECT_EQ(status(true).Status, STATUS_SUCCESS);
        EXPECT_EQ(status(true).Information, 3u);
        EXPECT_TRUE(emu.process.events.get(send_event)->signaled);
        EXPECT_EQ(socket->outgoing, (std::vector<std::byte>{std::byte{'X'}, std::byte{'Y'}, std::byte{'Z'}}));
    }

    TEST_P(AfdPendingRequestTest, PendingSendsPreserveFifoOrder)
    {
        ASSERT_EQ(transfer(true), STATUS_PENDING);
        ASSERT_EQ(GetParam() ? second_send<Emu32>() : second_send<Emu64>(), STATUS_PENDING);
        EXPECT_EQ(status(true).Status, STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(memory + 0x620).Status, STATUS_SUCCESS);

        socket->send_blocked = false;
        device->work(emu);

        EXPECT_EQ(status(true).Status, STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(memory + 0x620).Status, STATUS_SUCCESS);
        EXPECT_TRUE(emu.process.events.get(send_event)->signaled);
        EXPECT_TRUE(emu.process.events.get(second_send_event)->signaled);
        EXPECT_EQ(socket->outgoing,
                  (std::vector<std::byte>{std::byte{'X'}, std::byte{'Y'}, std::byte{'Z'}, std::byte{'a'}, std::byte{'b'}, std::byte{'c'}}));
    }

    TEST_P(AfdPendingRequestTest, PollDeadlineCompletesWithoutSocketReadiness)
    {
        ASSERT_EQ(transfer(false), STATUS_PENDING);
        ASSERT_EQ(GetParam() ? poll_with_expired_deadline<Emu32>() : poll_with_expired_deadline<Emu64>(), STATUS_PENDING);
        EXPECT_FALSE(emu.process.events.get(poll_event)->signaled);

        test_clock->steady += std::chrono::milliseconds{2};
        device->work(emu);

        const auto poll_status = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(memory + 0x780);
        EXPECT_EQ(poll_status.Status, STATUS_TIMEOUT);
        const auto number_offset = GetParam() ? offsetof(AFD_POLL_INFO<EmulatorTraits<Emu32>>, NumberOfHandles)
                                              : offsetof(AFD_POLL_INFO<EmulatorTraits<Emu64>>, NumberOfHandles);
        EXPECT_EQ(emu.memory.read_memory<ULONG>(memory + 0x800 + number_offset), 0u);
        EXPECT_TRUE(emu.process.events.get(poll_event)->signaled);
        EXPECT_EQ(status(false).Status, STATUS_SUCCESS);
        EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
    }

    TEST_P(AfdPendingRequestTest, UnsupportedStreamPeekDoesNotConsumeInput)
    {
        for (const char value : std::string_view{"abc"})
        {
            socket->incoming.push_back(static_cast<std::byte>(value));
        }
        EXPECT_EQ(transfer(false, 0x80), STATUS_NOT_SUPPORTED);
        EXPECT_EQ(socket->incoming.size(), 3u);
        EXPECT_EQ(transfer(false), STATUS_SUCCESS);
        EXPECT_TRUE(socket->incoming.empty());
        const auto received = emu.memory.read_memory(memory + 0x400, 3);
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(received.data()), received.size()), "abc");
    }

    TEST_P(AfdPendingRequestTest, PendingRequestsSurviveDeviceSnapshotRoundTrip)
    {
        ASSERT_EQ(transfer(false), STATUS_PENDING);
        ASSERT_EQ(transfer(true), STATUS_PENDING);

        utils::buffer_serializer saved{};
        device->serialize(saved);
        utils::buffer_deserializer input{saved};
        input.register_factory<memory_manager_wrapper>([this] { return memory_manager_wrapper{emu.memory}; });
        input.register_factory<x64_emulator_wrapper>([this] { return x64_emulator_wrapper{emu.emu()}; });
        input.register_factory<socket_factory_wrapper>([this] { return socket_factory_wrapper{emu.socket_factory()}; });
        auto restored = create_afd_endpoint({.is_32_bit = GetParam()});
        restored->deserialize(input);
        utils::buffer_serializer resaved{};
        restored->serialize(resaved);
        EXPECT_EQ(saved.get_buffer(), resaved.get_buffer());

        for (const char value : std::string_view{"abc"})
        {
            socket->incoming.push_back(static_cast<std::byte>(value));
        }
        socket->send_blocked = false;
        restored->work(emu);
        EXPECT_EQ(status(false).Status, STATUS_SUCCESS);
        EXPECT_EQ(status(true).Status, STATUS_SUCCESS);
        EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
        EXPECT_TRUE(emu.process.events.get(send_event)->signaled);
        const auto received = emu.memory.read_memory(memory + 0x400, 3);
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(received.data()), received.size()), "abc");
        EXPECT_EQ(socket->outgoing, (std::vector<std::byte>{std::byte{'X'}, std::byte{'Y'}, std::byte{'Z'}}));
    }

    TEST_P(AfdPendingRequestTest, PendingReceiveQueuesApcOnIssuingThread)
    {
        emulator_thread issuer{emu.memory};
        issuer.id = 0x1234;
        const auto thread_handle = emu.process.threads.store(std::move(issuer));
        emu.process.thread_handles_by_id.emplace(0x1234, thread_handle);

        ASSERT_EQ(transfer(false, 0, nullptr, {}, 0x1234, 0x567800, 0xfeed), STATUS_PENDING);
        for (const char value : std::string_view{"abc"})
        {
            socket->incoming.push_back(static_cast<std::byte>(value));
        }
        device->work(emu);

        EXPECT_EQ(status(false).Status, STATUS_SUCCESS);
        auto* target = emu.process.threads.get(thread_handle);
        ASSERT_NE(target, nullptr);
        ASSERT_EQ(target->pending_apcs.size(), 1u);
        const auto& apc = target->pending_apcs.front();
        EXPECT_EQ(apc.apc_routine, 0x567800u);
        EXPECT_EQ(apc.apc_argument1, 0xfeedu);
        EXPECT_EQ(apc.apc_argument2, memory + 0x100);
    }

    TEST_P(AfdPendingRequestTest, PendingSendQueuesIocpOnAssociatedEndpoint)
    {
        emu.process.is_wow64_process = GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        const auto port_handle = emu.process.io_completions.store(io_completion{});
        endpoint->completion_port = port_handle;
        endpoint->completion_key = 0x9876;

        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};
        emu.memory.write_memory(memory + 0x120, &initial, sizeof(initial));
        ASSERT_EQ(transfer(true, 0, endpoint, endpoint_handle), STATUS_PENDING);
        EXPECT_EQ(status(true).Status, initial.Status);
        EXPECT_EQ(status(true).Information, initial.Information);
        EXPECT_TRUE(emu.process.io_completions.get(port_handle)->queue.empty());
        socket->send_blocked = false;
        endpoint->work(emu);

        EXPECT_EQ(status(true).Status, STATUS_SUCCESS);
        const auto* port = emu.process.io_completions.get(port_handle);
        ASSERT_NE(port, nullptr);
        ASSERT_EQ(port->queue.size(), 1u);
        EXPECT_EQ(port->queue.front().key_context, 0x9876u);
        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_SUCCESS);
        EXPECT_EQ(port->queue.front().io_status_block.Information, 3u);
    }

    TEST_P(AfdPendingRequestTest, CancelByIosbCompletesOnlyMatchingRequestAndQueuesApc)
    {
        emulator_thread issuer{emu.memory};
        issuer.id = 0x1234;
        const auto thread_handle = emu.process.threads.store(std::move(issuer));
        emu.process.thread_handles_by_id.emplace(0x1234, thread_handle);

        ASSERT_EQ(transfer(false, 0, nullptr, {}, 0x1234, 0x567800, 0xfeed), STATUS_PENDING);
        ASSERT_EQ(transfer(true), STATUS_PENDING);

        EXPECT_EQ(device->cancel_pending_io(emu, memory + 0x100, 0), 1u);
        EXPECT_EQ(device->cancel_pending_io(emu, memory + 0x100, 0), 0u);
        EXPECT_EQ(status(false).Status, STATUS_CANCELLED);
        EXPECT_EQ(status(false).Information, 0u);
        EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
        EXPECT_EQ(status(true).Status, STATUS_SUCCESS);
        EXPECT_FALSE(emu.process.events.get(send_event)->signaled);

        const auto* target = emu.process.threads.get(thread_handle);
        ASSERT_NE(target, nullptr);
        ASSERT_EQ(target->pending_apcs.size(), 1u);
        const auto& apc = target->pending_apcs.front();
        EXPECT_EQ(apc.apc_routine, 0x567800u);
        EXPECT_EQ(apc.apc_argument1, 0xfeedu);
        EXPECT_EQ(apc.apc_argument2, memory + 0x100);
        EXPECT_EQ(apc.io_status, static_cast<int32_t>(STATUS_CANCELLED));
        EXPECT_EQ(apc.io_information, 0u);

        socket->send_blocked = false;
        device->work(emu);
        EXPECT_EQ(status(true).Status, STATUS_SUCCESS);
        EXPECT_TRUE(emu.process.events.get(send_event)->signaled);
    }

    TEST_P(AfdPendingRequestTest, CancelByIssuerOnlyCompletesThatThreadsRequests)
    {
        ASSERT_EQ(transfer(false, 0, nullptr, {}, 0x1234), STATUS_PENDING);
        ASSERT_EQ(transfer(true, 0, nullptr, {}, 0x5678), STATUS_PENDING);
        EXPECT_EQ(device->cancel_pending_io(emu, 0, 0x1234), 1u);
        EXPECT_EQ(status(false).Status, STATUS_CANCELLED);
        EXPECT_EQ(status(true).Status, STATUS_SUCCESS);
        EXPECT_EQ(device->cancel_pending_io(emu, 0, 0x5678), 1u);
        EXPECT_EQ(status(true).Status, STATUS_CANCELLED);
        EXPECT_EQ(status(true).Information, 0u);
        EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
        EXPECT_TRUE(emu.process.events.get(send_event)->signaled);
    }

    TEST_P(AfdPendingRequestTest, FinalEndpointCloseCancelsPendingSendBeforeReleasingIocp)
    {
        emu.process.is_wow64_process = GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        const auto port_handle = emu.process.io_completions.store(io_completion{});
        auto* port = emu.process.io_completions.get(port_handle);
        ASSERT_NE(port, nullptr);
        ++port->ref_count; // Retained reference held by the associated endpoint.
        endpoint->completion_port = port_handle;
        endpoint->completion_key = 0x9876;

        ASSERT_EQ(transfer(true, 0, endpoint, endpoint_handle), STATUS_PENDING);
        auto& vcpu = emu.vcpu(0);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        ASSERT_EQ(syscalls::handle_NtClose(context, endpoint_handle), STATUS_SUCCESS);

        EXPECT_EQ(emu.process.devices.get(endpoint_handle), nullptr);
        EXPECT_EQ(status(true).Status, STATUS_CANCELLED);
        EXPECT_EQ(status(true).Information, 0u);
        EXPECT_TRUE(emu.process.events.get(send_event)->signaled);
        port = emu.process.io_completions.get(port_handle);
        ASSERT_NE(port, nullptr);
        ASSERT_EQ(port->queue.size(), 1u);
        EXPECT_EQ(port->queue.front().key_context, 0x9876u);
        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_CANCELLED);
        EXPECT_EQ(port->queue.front().io_status_block.Information, 0u);
    }

#ifdef _WIN32
    TEST(AfdPartialDisconnectHostTest, WrapperForwardsModesAndTimeoutToNativeAfd)
    {
        network::socket_factory initialize_winsock;
        for (const auto [mode, timeout] : {std::pair<uint32_t, int64_t>{0, 0}, {2, -10000},
                                           {8, 1}, {1, -10000000}, {4, 0}})
        {
            SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            ASSERT_NE(listener, INVALID_SOCKET);
            SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            ASSERT_NE(client, INVALID_SOCKET);
            const auto close_handles = utils::finally([&] {
                ::closesocket(listener);
                ::closesocket(client);
            });
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)), 0);
            ASSERT_EQ(::listen(listener, 1), 0);
            int length = sizeof(local);
            ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&local), &length), 0);
            ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&local), sizeof(local)), 0);
            SOCKET accepted = ::accept(listener, nullptr, nullptr);
            ASSERT_NE(accepted, INVALID_SOCKET);
            network::socket_wrapper wrapper{accepted};
            ASSERT_EQ(wrapper.partial_disconnect(mode, timeout), STATUS_SUCCESS) << "mode=" << mode;
            const std::array payload{std::byte{'x'}};
            const auto sent = wrapper.send(payload);
            if (mode == 1 || mode == 4)
            {
                EXPECT_EQ(sent, -1) << "mode=" << mode;
                EXPECT_EQ(wrapper.get_last_error(), mode == 1 ? WSAESHUTDOWN : WSAECONNABORTED);
            }
            else
            {
                EXPECT_EQ(sent, 1) << "mode=" << mode;
            }
        }
    }
#endif

    TEST(AfdPartialDisconnectStaticTest, AbortWakesLocalAndPeerReceives)
    {
        auto factory = network::create_static_socket_factory();
        auto listener = factory->create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        const network::address local{"127.0.0.1", uint16_t{20001}};
        ASSERT_TRUE(listener->bind(local));
        ASSERT_TRUE(listener->listen(1));
        auto client = factory->create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_TRUE(client->connect(local));
        network::address peer{};
        auto accepted = listener->accept(peer);
        ASSERT_NE(accepted, nullptr);
        ASSERT_EQ(accepted->partial_disconnect(4, 0), STATUS_SUCCESS);
        const std::array payload{std::byte{'x'}};
        std::array<std::byte, 1> received{};
        EXPECT_EQ(accepted->send(payload), -1);
        EXPECT_EQ(accepted->get_last_error(), SERR(ECONNABORTED));
        EXPECT_EQ(accepted->recv(received), -1);
        EXPECT_EQ(accepted->get_last_error(), SERR(ECONNABORTED));
        EXPECT_EQ(client->recv(received), -1);
        EXPECT_EQ(client->get_last_error(), SERR(ECONNRESET));
        network::poll_entry local_poll{accepted.get(), POLLIN, 0};
        network::poll_entry peer_poll{client.get(), POLLIN, 0};
        EXPECT_EQ(factory->poll_sockets(std::span(&local_poll, 1)), 1);
        EXPECT_NE(local_poll.revents & POLLERR, 0);
        EXPECT_EQ(factory->poll_sockets(std::span(&peer_poll, 1)), 1);
        EXPECT_NE(peer_poll.revents & POLLHUP, 0);
    }

#ifdef _WIN32
    TEST(AfdPartialDisconnectHostTest, UdpExplicitSendAndReceiveRemainAvailable)
    {
        network::socket_factory initialize_winsock;
        for (const auto [mode, expected_error] : {std::pair<uint32_t, int>{1, WSAESHUTDOWN},
                                                  {4, WSAECONNABORTED}, {8, WSAENOTCONN}})
        {
            SOCKET local = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            SOCKET peer = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            ASSERT_NE(local, INVALID_SOCKET);
            ASSERT_NE(peer, INVALID_SOCKET);
            const auto close_peer = utils::finally([&] { ::closesocket(peer); });
            network::socket_wrapper wrapper{local};
            sockaddr_in peer_address{};
            peer_address.sin_family = AF_INET;
            peer_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ASSERT_EQ(::bind(peer, reinterpret_cast<sockaddr*>(&peer_address), sizeof(peer_address)), 0);
            int length = sizeof(peer_address);
            ASSERT_EQ(::getsockname(peer, reinterpret_cast<sockaddr*>(&peer_address), &length), 0);
            ASSERT_EQ(::connect(local, reinterpret_cast<sockaddr*>(&peer_address), sizeof(peer_address)), 0);
            ASSERT_EQ(wrapper.partial_disconnect(mode, 0), STATUS_SUCCESS);
            const std::array payload{std::byte{'x'}};
            EXPECT_EQ(wrapper.send(payload), -1);
            EXPECT_EQ(wrapper.get_last_error(), expected_error);
            EXPECT_EQ(wrapper.sendto(network::address{peer_address}, payload), 1);
        }
    }
#endif

    TEST(AfdPartialDisconnectStaticTest, UdpExplicitSendAndReceiveRemainAvailable)
    {
        const network::address peer_address{"127.0.0.1", uint16_t{20002}};
        for (const auto [mode, expected_error] : {std::pair<uint32_t, int>{1, SERR(ESHUTDOWN)},
                                                  {4, SERR(ECONNABORTED)}, {8, SERR(ENOTCONN)}})
        {
            auto factory = network::create_static_socket_factory();
            auto local = factory->create_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            auto peer = factory->create_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            ASSERT_TRUE(peer->bind(peer_address));
            ASSERT_TRUE(local->connect(peer_address));
            ASSERT_EQ(local->partial_disconnect(mode, 0), STATUS_SUCCESS);
            const std::array payload{std::byte{'x'}};
            EXPECT_EQ(local->send(payload), -1);
            EXPECT_EQ(local->get_last_error(), expected_error);
            EXPECT_EQ(local->sendto(peer_address, payload), 1);
            std::array<std::byte, 1> received{};
            network::address source{};
            EXPECT_EQ(peer->recvfrom(source, received), 1);
            EXPECT_EQ(received[0], payload[0]);
        }
    }

    TEST(AfdPartialDisconnectStaticTest, UdpModeTwoBlocksConnectedReceiveButAllowsRecvfrom)
    {
        auto factory = network::create_static_socket_factory();
        auto local = factory->create_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        auto peer = factory->create_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const auto local_address = *local->get_local_address();
        ASSERT_EQ(local->partial_disconnect(2, 0), STATUS_SUCCESS);
        const std::array payload{std::byte{'z'}};
        ASSERT_EQ(peer->sendto(local_address, payload), 1);
        std::array<std::byte, 1> received{};
        EXPECT_EQ(local->recv(received), -1);
        EXPECT_EQ(local->get_last_error(), SERR(ESHUTDOWN));
        network::address source{};
        EXPECT_EQ(local->recvfrom(source, received), 1);
        EXPECT_EQ(received[0], payload[0]);
    }

    INSTANTIATE_TEST_SUITE_P(GuestBitness, AfdPendingRequestTest, testing::Bool());
}
