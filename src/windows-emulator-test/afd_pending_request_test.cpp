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
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace sogen::syscalls
{
    NTSTATUS handle_NtClose(const syscall_context&, handle);
    NTSTATUS handle_NtDeviceIoControlFile(const syscall_context&, handle, handle, emulator_pointer, emulator_pointer,
                                          emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>, ULONG, emulator_pointer, ULONG,
                                          emulator_pointer, ULONG);
}

namespace sogen::test
{
    namespace
    {
        class observed_iosb_memory final : public memory_interface
        {
          public:
            explicit observed_iosb_memory(memory_interface& backing)
                : backing_(backing)
            {
            }

            std::vector<std::pair<uint64_t, size_t>> stores;
            std::function<void()> after_byte_store;

            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                this->backing_.read_memory(address, data, size);
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                return this->backing_.try_read_memory(address, data, size);
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                this->stores.emplace_back(address, size);
                const auto* bytes = static_cast<const std::byte*>(data);
                for (size_t index = 0; index < size; ++index)
                {
                    this->backing_.write_memory(address + index, bytes + index, 1);
                    if (this->after_byte_store)
                    {
                        this->after_byte_store();
                    }
                }
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                this->stores.emplace_back(address, size);
                const auto* bytes = static_cast<const std::byte*>(data);
                for (size_t index = 0; index < size; ++index)
                {
                    if (!this->backing_.try_write_memory(address + index, bytes + index, 1))
                    {
                        return false;
                    }
                    if (this->after_byte_store)
                    {
                        this->after_byte_store();
                    }
                }
                return true;
            }

          private:
            memory_interface& backing_;

            void map_mmio(uint64_t, size_t, mmio_read_callback, mmio_write_callback) override
            {
            }

            void map_memory(uint64_t, size_t, memory_permission) override
            {
            }

            void unmap_memory(uint64_t, size_t) override
            {
            }

            void apply_memory_protection(uint64_t, size_t, memory_permission) override
            {
            }
        };

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
            bool connect_refused{};
            bool listening{};
            bool connected{};
            std::optional<network::address> next_accepted_remote{};
            uint32_t disconnect_mode{};
            uint32_t transport_level{};
            uint32_t transport_option{};
            std::vector<std::byte> transport_value{};
            uint32_t transport_status{};
            size_t transport_calls{};
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

            uint32_t sort_address_list(std::vector<network::address_sort_entry>& addresses) override
            {
                // The real host parity test below checks Windows policy. This
                // deterministic test backend exercises guest pointer translation.
                if (addresses.size() == 2)
                {
                    std::swap(addresses[0], addresses[1]);
                }
                return STATUS_SUCCESS;
            }

            uint32_t set_transport_option(const uint32_t level, const uint32_t option, const std::span<const std::byte> value) override
            {
                state->transport_level = level;
                state->transport_option = option;
                state->transport_value.assign(value.begin(), value.end());
                ++state->transport_calls;
                return state->transport_status;
            }

            bool is_ready(bool) override
            {
                return true;
            }

            bool is_listening() override
            {
                return state->listening;
            }

            bool is_connected() override
            {
                return state->connected;
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
                if (state->connect_refused)
                {
                    state->last_error = SERR(ECONNREFUSED);
                    return false;
                }
                state->last_error = 0;
                state->connected = true;
                return true;
            }

            bool listen(int) override
            {
                state->listening = true;
                return true;
            }

            std::unique_ptr<network::i_socket> accept(network::address& remote) override
            {
                if (!state->next_accepted_remote)
                {
                    state->last_error = SERR(EWOULDBLOCK);
                    return {};
                }
                remote = *state->next_accepted_remote;
                state->next_accepted_remote.reset();
                state->last_error = 0;
                return std::make_unique<pending_test_socket>(state);
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
                          uint32_t issuer_thread_id = 0, uint64_t apc_routine = 0, uint64_t apc_context = 0, ULONG afd_flags = 0,
                          memory_interface* status_memory = nullptr)
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
            context.io_status_block = {status_memory ? *status_memory : static_cast<memory_interface&>(emu.memory), info};
            context.event = is_send ? send_event : receive_event;
            context.input_buffer = request;
            context.input_buffer_length = is_send ? sizeof(AFD_SEND_INFO<Traits>) : sizeof(AFD_RECV_INFO<Traits>);
            if (is_send)
            {
                const AFD_SEND_INFO<Traits> input{
                    .BufferArray = static_cast<typename Traits::PVOID>(descriptor), .BufferCount = 1, .AfdFlags = afd_flags};
                emu.memory.write_memory(request, &input, sizeof(input));
            }
            else
            {
                const AFD_RECV_INFO<Traits> input{.BufferArray = static_cast<typename Traits::PVOID>(descriptor),
                                                  .BufferCount = 1,
                                                  .AfdFlags = afd_flags,
                                                  .TdiFlags = tdi_flags};
                emu.memory.write_memory(request, &input, sizeof(input));
            }
            return (endpoint ? endpoint : device.get())->execute_ioctl(emu, context);
        }

        NTSTATUS transfer(bool is_send, ULONG tdi_flags = 0, io_device* endpoint = nullptr, handle file_handle = {},
                          uint32_t issuer_thread_id = 0, uint64_t apc_routine = 0, uint64_t apc_context = 0, ULONG afd_flags = 0,
                          memory_interface* status_memory = nullptr)
        {
            return GetParam() ? transfer<Emu32>(is_send, tdi_flags, endpoint, file_handle, issuer_thread_id, apc_routine, apc_context,
                                                afd_flags, status_memory)
                              : transfer<Emu64>(is_send, tdi_flags, endpoint, file_handle, issuer_thread_id, apc_routine, apc_context,
                                                afd_flags, status_memory);
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

        ASSERT_EQ(invoke(0), STATUS_PENDING);
        auto* port = emu.process.io_completions.get(port_handle);
        ASSERT_NE(port, nullptr);
        EXPECT_TRUE(port->queue.empty());
        EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
        endpoint->work(emu);
        ASSERT_NE(port, nullptr);
        ASSERT_EQ(port->queue.size(), 1u);
        EXPECT_EQ(port->queue.front().key_context, 0x9876u);
        EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
        port->queue.clear();
        emu.process.events.get(receive_event)->signaled = false;

        ASSERT_EQ(invoke(0x567800), STATUS_PENDING);
        EXPECT_TRUE(port->queue.empty());
        endpoint->work(emu);
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
        port->queue.clear();
        emu.process.events.get(receive_event)->signaled = false;

        emu.memory.write_memory(memory + 0x100, &initial, sizeof(initial));
        socket->connect_blocked = true;
        ASSERT_EQ(invoke(0), STATUS_PENDING);
        socket->connect_blocked = false;
        socket->connect_refused = true;
        endpoint->work(emu);
        EXPECT_EQ(status(false).Status, STATUS_CONNECTION_REFUSED);
        EXPECT_EQ(status(false).Information, 0u);
        EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
        ASSERT_EQ(port->queue.size(), 1u);
        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_CONNECTION_REFUSED);
    }

    TEST_P(AfdPendingRequestTest, AsyncConnectHelperRejectsShortInputAndBadTargetWithoutCompletion)
    {
        emu.process.is_wow64_process = GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        const auto helper_handle = emu.process.devices.store(io_device_container{u"Afd\\AsyncConnectHlp", emu, {}});
        auto* helper = emu.process.devices.get(helper_handle);
        ASSERT_NE(helper, nullptr);
        const auto port_handle = emu.process.io_completions.store(io_completion{});
        helper->completion_port = port_handle;
        auto* completion_port = emu.process.io_completions.get(port_handle);
        ASSERT_NE(completion_port, nullptr);

        const auto check = [this, helper, helper_handle, endpoint_handle, completion_port]<typename Guest>() {
            using traits = EmulatorTraits<Guest>;
            using connect_info = AFD_CONNECT_JOIN_INFO_TL<traits>;
            connect_info info{};
            info.ConnectEndpoint = static_cast<typename traits::HANDLE>(endpoint_handle.bits);
            info.RemoteAddress.sa_family = 2;
            const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};
            constexpr auto family_end = offsetof(connect_info, RemoteAddress) + sizeof(USHORT);
            for (const auto length : {size_t{0}, family_end - 1, family_end, sizeof(connect_info) - 1})
            {
                emu.memory.write_memory(memory + 0x800, &info, sizeof(info));
                emu.memory.write_memory(memory + 0x100, &initial, sizeof(initial));
                emu.process.events.get(receive_event)->signaled = false;
                io_device_context context{emu.memory};
                context.file_handle = helper_handle;
                context.io_control_code = 0x12007;
                context.io_status_block = {emu.memory, memory + 0x100};
                context.event = receive_event;
                context.input_buffer = memory + 0x800;
                context.input_buffer_length = static_cast<ULONG>(length);
                const auto expected = length < family_end ? STATUS_INVALID_PARAMETER : STATUS_INVALID_ADDRESS;
                EXPECT_EQ(helper->execute_ioctl(emu, context), expected);
                EXPECT_EQ(context.io_status_block.read().Status, initial.Status);
                EXPECT_EQ(context.io_status_block.read().Information, initial.Information);
                EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
                EXPECT_TRUE(completion_port->queue.empty());
            }

            info.ConnectEndpoint = static_cast<typename traits::HANDLE>(0xdeadbeef);
            emu.memory.write_memory(memory + 0x800, &info, sizeof(info));
            emu.memory.write_memory(memory + 0x100, &initial, sizeof(initial));
            io_device_context context{emu.memory};
            context.file_handle = helper_handle;
            context.io_control_code = 0x12007;
            context.io_status_block = {emu.memory, memory + 0x100};
            context.event = receive_event;
            context.input_buffer = memory + 0x800;
            context.input_buffer_length = sizeof(info);
            EXPECT_EQ(helper->execute_ioctl(emu, context), STATUS_INVALID_HANDLE);
            EXPECT_EQ(context.io_status_block.read().Status, initial.Status);
            EXPECT_EQ(context.io_status_block.read().Information, initial.Information);
            EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
            EXPECT_TRUE(completion_port->queue.empty());
        };
        if (GetParam())
        {
            check.template operator()<Emu32>();
        }
        else
        {
            check.template operator()<Emu64>();
        }
    }

    TEST_P(AfdPendingRequestTest, ContextStoresGuestBytesAndReturnsNativePrefixContract)
    {
        constexpr uint64_t input = memory + 0x800;
        constexpr uint64_t output = memory + 0xa00;
        constexpr uint64_t iosb = memory + 0x100;
        // The endpoint request layout varies with GetParam(); io_device_context
        // stores IOSB as Emu64 for both endpoint bitnesses.
        const auto set = [&](const uint32_t size, const uint64_t alias = 0, const uint32_t alias_size = 0) {
            io_device_context request{emu.memory};
            request.io_control_code = 0x12047;
            request.io_status_block = {emu.memory, iosb};
            request.input_buffer = size ? input : 0;
            request.input_buffer_length = size;
            request.output_buffer = alias;
            request.output_buffer_length = alias_size;
            return device->execute_ioctl(emu, request);
        };
        const auto get = [&](io_device& endpoint, const uint32_t capacity) {
            io_device_context request{emu.memory};
            request.io_control_code = 0x12043;
            request.io_status_block = {emu.memory, iosb};
            request.output_buffer = capacity ? output : 0;
            request.output_buffer_length = capacity;
            return endpoint.execute_ioctl(emu, request);
        };
        // A direct endpoint with no context follows the XP AFD null-context
        // path. Modern Winsock sockets already set context during creation.
        EXPECT_EQ(get(*device, 256), STATUS_INVALID_PARAMETER);

        const auto native_default_size = GetParam() ? 164u : 168u;
        for (const uint32_t size : {0u, 1u, 16u, native_default_size})
        {
            std::vector<std::byte> payload(size);
            for (size_t index = 0; index < payload.size(); ++index)
            {
                payload[index] = static_cast<std::byte>((index * 17 + 3) & 0xff);
            }
            if (size)
            {
                emu.memory.write_memory(input, payload.data(), payload.size());
            }
            ASSERT_EQ(set(size), STATUS_SUCCESS);
            EXPECT_EQ(emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb).Information, 0u);

            emu.memory.set_memory(output, 0xa5, 256);
            const auto short_size = size ? size - 1 : 0;
            EXPECT_EQ(get(*device, short_size), size ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS);
            const auto short_iosb = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
            EXPECT_EQ(short_iosb.Information, size);
            const auto short_bytes = emu.memory.read_memory(output, size + 1);
            EXPECT_TRUE(std::equal(payload.begin(), payload.begin() + short_size, short_bytes.begin()));
            EXPECT_EQ(short_bytes[short_size], std::byte{0xa5});

            emu.memory.set_memory(output, 0xa5, 256);
            EXPECT_EQ(get(*device, size + 8), STATUS_SUCCESS);
            const auto full_iosb = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
            EXPECT_EQ(full_iosb.Information, size);
            const auto full_bytes = emu.memory.read_memory(output, size + 8);
            EXPECT_TRUE(std::equal(payload.begin(), payload.end(), full_bytes.begin()));
            EXPECT_TRUE(std::all_of(full_bytes.begin() + size, full_bytes.end(),
                                    [](const std::byte byte) { return byte == std::byte{0xa5}; }));
        }

        // The native USHORT alias bounds are enforced before replacing context.
        std::array<std::byte, 32> replacement{};
        replacement.fill(std::byte{0x77});
        emu.memory.write_memory(input, replacement.data(), replacement.size());
        EXPECT_EQ(set(32, input + 30, 4), STATUS_INVALID_PARAMETER);
        emu.memory.set_memory(output, 0xa5, 256);
        EXPECT_EQ(get(*device, native_default_size), STATUS_SUCCESS);
        const auto preserved = emu.memory.read_memory(output, native_default_size);
        EXPECT_NE(preserved.front(), replacement.front());
        EXPECT_EQ(set(32, input + 4, 8), STATUS_SUCCESS);
        EXPECT_EQ(get(*device, 32), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory(output, 32),
                  (std::vector<std::byte>(replacement.begin(), replacement.end())));

        utils::buffer_serializer saved{};
        device->serialize(saved);
        utils::buffer_deserializer saved_input{saved};
        saved_input.register_factory<memory_manager_wrapper>([this] { return memory_manager_wrapper{emu.memory}; });
        saved_input.register_factory<x64_emulator_wrapper>([this] { return x64_emulator_wrapper{emu.emu()}; });
        saved_input.register_factory<socket_factory_wrapper>([this] { return socket_factory_wrapper{emu.socket_factory()}; });
        auto restored = create_afd_endpoint({.is_32_bit = GetParam()});
        restored->deserialize(saved_input);
        EXPECT_EQ(get(*restored, 32), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory(output, 32),
                  (std::vector<std::byte>(replacement.begin(), replacement.end())));
    }

    TEST_P(AfdPendingRequestTest, TransportType1OptionsMatchNativeStatusAndCompletion)
    {
        constexpr uint64_t creation_ptr = memory + 0x600;
        constexpr uint64_t wrapper_ptr = memory + 0x800;
        constexpr uint64_t nested_ptr = memory + 0x900;
        constexpr uint64_t iosb_ptr = memory + 0x100;
        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};

        const auto make_endpoint = [&](const uint32_t family, const uint32_t type, const uint32_t protocol) {
            std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, family, type, protocol, 0};
            emu.memory.write_memory(creation_ptr, creation.data(), sizeof(creation));
            auto endpoint = create_afd_endpoint({.is_32_bit = GetParam()});
            endpoint->create(emu, {.buffer = creation_ptr, .length = sizeof(creation)});
            return endpoint;
        };

        const auto run = [this, &initial, &make_endpoint]<typename Guest>() {
            using traits = EmulatorTraits<Guest>;
            using wrapper = AFD_WINSOCK_TRANSPORT_IOCTL<traits>;
            const auto invoke = [&](io_device& endpoint, const uint32_t level, const uint32_t option,
                                    const std::span<const std::byte> value) {
                emu.memory.write_memory(nested_ptr, value.data(), value.size());
                const wrapper outer{.Type = 1,
                                    .Reserved = level,
                                    .ControlCode = option,
                                    .Overlapped = 1,
                                    .Padding = {},
                                    .InputBuffer = static_cast<typename traits::PVOID>(nested_ptr),
                                    .InputBufferLength = static_cast<typename traits::SIZE_T>(value.size())};
                emu.memory.write_memory(wrapper_ptr, &outer, sizeof(outer));
                emu.memory.write_memory(iosb_ptr, &initial, sizeof(initial));
                emu.process.events.get(receive_event)->signaled = false;
                io_device_context request{emu.memory};
                request.io_control_code = 0x120bf;
                request.io_status_block = {emu.memory, iosb_ptr};
                request.event = receive_event;
                request.input_buffer = wrapper_ptr;
                request.input_buffer_length = sizeof(outer);
                return endpoint.execute_ioctl(emu, request);
            };
            const std::array<std::byte, 4> zero{};
            const std::array<std::byte, 4> one{std::byte{1}};
            const std::array<std::byte, 1> byte_one{std::byte{1}};
            const auto check_success = [&] {
                const auto result = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb_ptr);
                EXPECT_EQ(result.Status, STATUS_SUCCESS);
                EXPECT_EQ(result.Information, 0u);
                EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
            };

            auto ipv6_tcp = make_endpoint(23, 1, 6);
            EXPECT_EQ(invoke(*ipv6_tcp, 0x29, 0x1b, zero), STATUS_SUCCESS);
            check_success();
            EXPECT_EQ(socket->transport_level, 0x29u);
            EXPECT_EQ(socket->transport_option, 0x1bu);
            EXPECT_EQ(socket->transport_value, (std::vector<std::byte>(zero.begin(), zero.end())));

            auto ipv4_udp = make_endpoint(2, 2, 17);
            EXPECT_EQ(invoke(*ipv4_udp, 0xffff, 0x20, one), STATUS_SUCCESS);
            check_success();
            EXPECT_EQ(socket->transport_option, 0x20u);
            EXPECT_EQ(socket->transport_value, (std::vector<std::byte>(one.begin(), one.end())));

            auto ipv4_tcp = make_endpoint(2, 1, 6);
            EXPECT_EQ(invoke(*ipv4_tcp, IPPROTO_TCP, TCP_NODELAY, one), STATUS_SUCCESS);
            check_success();
            EXPECT_EQ(socket->transport_level, static_cast<uint32_t>(IPPROTO_TCP));
            EXPECT_EQ(socket->transport_option, static_cast<uint32_t>(TCP_NODELAY));
            EXPECT_EQ(socket->transport_value, (std::vector<std::byte>(one.begin(), one.end())));
            EXPECT_EQ(invoke(*ipv4_tcp, IPPROTO_TCP, TCP_NODELAY, byte_one), STATUS_SUCCESS);
            check_success();
            const auto nodelay_calls = socket->transport_calls;
            EXPECT_EQ(invoke(*ipv4_tcp, IPPROTO_TCP, TCP_NODELAY, {}), STATUS_BUFFER_TOO_SMALL);
            const auto short_value = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb_ptr);
            EXPECT_EQ(short_value.Status, initial.Status);
            EXPECT_EQ(short_value.Information, initial.Information);
            EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
            EXPECT_EQ(socket->transport_calls, nodelay_calls);

            EXPECT_EQ(invoke(*ipv4_tcp, 0xffff, 0x3007, byte_one), STATUS_SUCCESS);
            check_success();
            EXPECT_EQ(socket->transport_option, 0x3007u);
            EXPECT_EQ(socket->transport_value, (std::vector<std::byte>(byte_one.begin(), byte_one.end())));
            const auto calls = socket->transport_calls;

            EXPECT_EQ(invoke(*ipv4_tcp, 0xffff, 0x20, one), STATUS_NOT_SUPPORTED);
            const auto rejected = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb_ptr);
            EXPECT_EQ(rejected.Status, initial.Status);
            EXPECT_EQ(rejected.Information, initial.Information);
            EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
            EXPECT_EQ(socket->transport_calls, calls);

            EXPECT_EQ(invoke(*ipv4_udp, IPPROTO_TCP, TCP_NODELAY, one), STATUS_PENDING);
            ipv4_udp->work(emu);
            const auto udp_nodelay = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb_ptr);
            EXPECT_EQ(udp_nodelay.Status, STATUS_INVALID_PARAMETER);
            EXPECT_EQ(udp_nodelay.Information, 0u);
            EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
            EXPECT_EQ(socket->transport_calls, calls);

            EXPECT_EQ(invoke(*ipv4_udp, 0xffff, 0x3007, byte_one), STATUS_PENDING);
            const auto pending = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb_ptr);
            EXPECT_EQ(pending.Status, initial.Status);
            EXPECT_EQ(pending.Information, initial.Information);
            EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
            ipv4_udp->work(emu);
            const auto completed = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb_ptr);
            EXPECT_EQ(completed.Status, STATUS_INVALID_PARAMETER);
            EXPECT_EQ(completed.Information, 0u);
            EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
            EXPECT_EQ(socket->transport_calls, calls);
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

#ifdef _WIN32
    TEST(AfdType1HostTest, NativeBackendAppliesBroadcastAndPreservesUnsupportedStatus)
    {
        network::initialize_wsa();
        network::socket_wrapper udp{AF_INET, SOCK_DGRAM, IPPROTO_UDP};
        network::socket_wrapper tcp{AF_INET, SOCK_STREAM, IPPROTO_TCP};
        network::socket_wrapper ipv6_tcp{AF_INET6, SOCK_STREAM, IPPROTO_TCP};
        const std::array<std::byte, 4> zero{};
        const std::array<std::byte, 4> one{std::byte{1}};
        const std::array<std::byte, 1> byte_one{std::byte{1}};
        ASSERT_EQ(ipv6_tcp.set_transport_option(IPPROTO_IPV6, IPV6_V6ONLY, zero), STATUS_SUCCESS);
        ASSERT_EQ(tcp.set_transport_option(SOL_SOCKET, 0x3007, byte_one), STATUS_SUCCESS);
        ASSERT_EQ(tcp.set_transport_option(IPPROTO_TCP, TCP_NODELAY, one), STATUS_SUCCESS);
        int nodelay{};
        int nodelay_length = sizeof(nodelay);
        ASSERT_EQ(getsockopt(tcp.get().get_socket(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&nodelay),
                             &nodelay_length), 0);
        EXPECT_EQ(nodelay, 1);
        ASSERT_EQ(udp.set_transport_option(SOL_SOCKET, SO_BROADCAST, one), STATUS_SUCCESS);
        int broadcast{};
        int length = sizeof(broadcast);
        ASSERT_EQ(getsockopt(udp.get().get_socket(), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&broadcast), &length), 0);
        EXPECT_EQ(broadcast, 1);
        EXPECT_EQ(tcp.set_transport_option(SOL_SOCKET, SO_BROADCAST, one), STATUS_NOT_SUPPORTED);
    }
#endif

    TEST_P(AfdPendingRequestTest, TransportAddressSortMatchesNativePendingAndOutputContract)
    {
        struct guest_sockaddr_in6
        {
            uint16_t family;
            uint16_t port;
            uint32_t flowinfo;
            std::array<uint8_t, 16> address;
            uint32_t scope_id;
        };

        static_assert(sizeof(guest_sockaddr_in6) == 28);
        constexpr uint64_t wrapper_ptr = memory + 0x800;
        constexpr uint64_t nested_ptr = memory + 0x900;
        constexpr uint64_t addr0_ptr = memory + 0xa00;
        constexpr uint64_t addr1_ptr = memory + 0xa40;
        constexpr uint64_t output_ptr = memory + 0xb00;
        constexpr uint64_t iosb_ptr = memory + 0x100;
        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};

        const auto run = [this, &initial]<typename Guest>() {
            using traits = EmulatorTraits<Guest>;
            using wrapper = AFD_WINSOCK_TRANSPORT_IOCTL<traits>;
            using list = AFD_SORT_ADDRESS_LIST<traits>;
            using entry = AFD_SORT_SOCKET_ADDRESS<traits>;
            constexpr uint32_t list_size = offsetof(list, Address) + 2 * sizeof(entry);
            static_assert(sizeof(wrapper) == (std::is_same_v<Guest, Emu32> ? 24 : 32));
            static_assert(offsetof(wrapper, InputBuffer) == 16);

            guest_sockaddr_in6 mapped{.family = 23};
            mapped.address[10] = 0xff;
            mapped.address[11] = 0xff;
            mapped.address[12] = 127;
            mapped.address[15] = 1;
            guest_sockaddr_in6 v6{.family = 23};
            v6.address[15] = 1;
            emu.memory.write_memory(addr0_ptr, &mapped, sizeof(mapped));
            emu.memory.write_memory(addr1_ptr, &v6, sizeof(v6));
            const int32_t count = 2;
            emu.memory.write_memory(nested_ptr, &count, sizeof(count));
            const std::array<entry, 2> guest_entries{{
                {.Sockaddr = static_cast<typename traits::PVOID>(addr0_ptr), .SockaddrLength = 28},
                {.Sockaddr = static_cast<typename traits::PVOID>(addr1_ptr), .SockaddrLength = 28},
            }};
            emu.memory.write_memory(nested_ptr + offsetof(list, Address), guest_entries.data(), sizeof(guest_entries));
            const wrapper outer{.Type = 3,
                                .ControlCode = 0xc8000019,
                                .Overlapped = 1,
                                .InputBuffer = static_cast<typename traits::PVOID>(nested_ptr),
                                .InputBufferLength = list_size};
            emu.memory.write_memory(wrapper_ptr, &outer, sizeof(outer));

            const auto invoke = [&](const uint32_t output_length) {
                emu.memory.write_memory(iosb_ptr, &initial, sizeof(initial));
                emu.memory.set_memory(output_ptr, 0xa5, 64);
                emu.process.events.get(receive_event)->signaled = false;
                io_device_context request{emu.memory};
                request.io_control_code = 0x120bf;
                request.io_status_block = {emu.memory, iosb_ptr};
                request.event = receive_event;
                request.input_buffer = wrapper_ptr;
                request.input_buffer_length = sizeof(outer);
                request.output_buffer = output_ptr;
                request.output_buffer_length = output_length;
                return device->execute_ioctl(emu, request);
            };

            // Native: initial syscall returns PENDING; output and IOSB are untouched.
            ASSERT_EQ(invoke(list_size), STATUS_PENDING);
            EXPECT_EQ(emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb_ptr).Status, initial.Status);
            EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(output_ptr), 0xa5);
            // A pending request owns a snapshot even if the guest reuses input memory.
            emu.memory.set_memory(wrapper_ptr, 0, sizeof(outer));
            emu.memory.set_memory(nested_ptr, 0, list_size);
            emu.memory.set_memory(addr0_ptr, 0, sizeof(mapped));
            emu.memory.set_memory(addr1_ptr, 0, sizeof(v6));
            device->work(emu);
            const auto final_status = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb_ptr);
            EXPECT_EQ(final_status.Status, STATUS_SUCCESS);
            EXPECT_EQ(final_status.Information, list_size);
            EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
            EXPECT_EQ(emu.memory.read_memory<int32_t>(output_ptr), 2);
            EXPECT_EQ(emu.memory.read_memory<entry>(output_ptr + offsetof(list, Address)).Sockaddr, addr1_ptr);
            EXPECT_EQ(emu.memory.read_memory<entry>(output_ptr + offsetof(list, Address) + sizeof(entry)).Sockaddr, addr0_ptr);
            EXPECT_EQ(emu.memory.read_memory<guest_sockaddr_in6>(addr0_ptr).address[12], 127);
            EXPECT_EQ(emu.memory.read_memory<guest_sockaddr_in6>(addr1_ptr).address[15], 1);
            const auto beyond = emu.memory.read_memory(output_ptr + list_size, 64 - list_size);
            EXPECT_TRUE(std::all_of(beyond.begin(), beyond.end(), [](std::byte value) { return value == std::byte{0xa5}; }));

            // Restore the request, then verify native short-output completion.
            emu.memory.write_memory(wrapper_ptr, &outer, sizeof(outer));
            emu.memory.write_memory(nested_ptr, &count, sizeof(count));
            emu.memory.write_memory(nested_ptr + offsetof(list, Address), guest_entries.data(), sizeof(guest_entries));
            ASSERT_EQ(invoke(list_size - 1), STATUS_PENDING);
            EXPECT_EQ(emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb_ptr).Status, initial.Status);
            device->work(emu);
            const auto short_status = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb_ptr);
            EXPECT_EQ(short_status.Status, STATUS_INVALID_PARAMETER);
            EXPECT_EQ(short_status.Information, 0u);
            EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
            const auto short_output = emu.memory.read_memory(output_ptr, 64);
            EXPECT_TRUE(std::all_of(short_output.begin(), short_output.end(), [](std::byte value) { return value == std::byte{0xa5}; }));

            // Unknown modern codes cannot silently claim success.
            wrapper unsupported = outer;
            unsupported.ControlCode = 0xc800001a;
            emu.memory.write_memory(wrapper_ptr, &unsupported, sizeof(unsupported));
            EXPECT_EQ(invoke(list_size), STATUS_NOT_SUPPORTED);
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

    TEST_P(AfdPendingRequestTest, QueryHandlesMatchesNativeBufferAndStatusContract)
    {
        constexpr uint64_t input = memory + 0x800;
        constexpr uint64_t output = memory + 0x880;
        constexpr uint64_t iosb = memory + 0x100;
        const auto expected_size =
            GetParam() ? sizeof(AFD_HANDLE_INFO<EmulatorTraits<Emu32>>) : sizeof(AFD_HANDLE_INFO<EmulatorTraits<Emu64>>);
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
            EXPECT_TRUE(
                std::all_of(bytes.begin(), bytes.begin() + expected_size, [](const std::byte value) { return value == std::byte{0xff}; }));
            EXPECT_TRUE(
                std::all_of(bytes.begin() + expected_size, bytes.end(), [](const std::byte value) { return value == std::byte{0xa5}; }));
        }
        for (const auto flags : {0u, 4u})
        {
            EXPECT_EQ(invoke(flags, 4, static_cast<uint32_t>(expected_size)), STATUS_INVALID_PARAMETER);
            const auto block = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
            EXPECT_EQ(block.Status, STATUS_INVALID_PARAMETER);
            EXPECT_EQ(block.Information, 0u);
            const auto bytes = emu.memory.read_memory(output, 24);
            EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) { return value == std::byte{0xa5}; }));
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
        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};
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
        EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) { return value == std::byte{0xa5}; }));
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

    TEST_P(AfdPendingRequestTest, AcceptShortBuffersMatchNativeStatusAndLeaveIosbUntouched)
    {
        constexpr uint64_t input = memory + 0x800;
        constexpr uint64_t output = memory + 0x880;
        constexpr uint64_t iosb = memory + 0x100;
        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};
        const auto invoke = [&](const ULONG code, const uint32_t input_length, const uint32_t output_length) {
            emu.memory.write_memory(iosb, &initial, sizeof(initial));
            emu.memory.set_memory(input, 0, sizeof(AFD_ACCEPT_INFO));
            emu.memory.set_memory(output, 0xa5, 32);
            io_device_context context{emu.memory};
            context.io_control_code = code;
            context.io_status_block = {emu.memory, iosb};
            context.input_buffer = input;
            context.input_buffer_length = input_length;
            context.output_buffer = output;
            context.output_buffer_length = output_length;
            const auto result = device->execute_ioctl(emu, context);
            const auto block = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
            EXPECT_EQ(block.Status, initial.Status);
            EXPECT_EQ(block.Information, initial.Information);
            const auto bytes = emu.memory.read_memory(output, 32);
            EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) { return value == std::byte{0xa5}; }));
            return result;
        };

        // Windows x64: 0/4/8 bytes -> INVALID_PARAMETER; 12/16 -> BUFFER_TOO_SMALL.
        for (const auto length : {0u, 4u, 8u})
        {
            EXPECT_EQ(invoke(0x1200c, 0, length), STATUS_INVALID_PARAMETER);
        }
        for (const auto length : {12u, 16u})
        {
            EXPECT_EQ(invoke(0x1200c, 0, length), STATUS_BUFFER_TOO_SMALL);
        }
        // With enough output, a non-listening socket is an invalid request.
        EXPECT_EQ(invoke(0x1200c, 0, 20), STATUS_INVALID_PARAMETER);

        // Native AFD_ACCEPT rejects every input size below its 16-byte structure.
        for (const auto length : {0u, 1u, 4u, 8u, 12u, 15u})
        {
            EXPECT_EQ(invoke(0x12010, length, 0), STATUS_INVALID_PARAMETER);
        }
        EXPECT_EQ(invoke(0x12010, sizeof(AFD_ACCEPT_INFO), 0), STATUS_INVALID_PARAMETER);
    }

    TEST_P(AfdPendingRequestTest, WaitForListenIpv6MatchesNativeResponseAndPendingContract)
    {
        const std::array<uint32_t, 12> ipv6_creation{0, 0, 0, 0, 0, 0, 0, 0, 23, 1, 6, 0};
        emu.memory.write_memory(memory, ipv6_creation.data(), sizeof(ipv6_creation));
        device = create_afd_endpoint({.is_32_bit = GetParam()});
        device->create(emu, {.buffer = memory, .length = sizeof(ipv6_creation)});
        socket->listening = true;
        network::address remote{};
        remote.set_ipv6({});
        remote.set_port(4567);
        socket->next_accepted_remote = remote;
        constexpr uint64_t output = memory + 0x880;
        constexpr uint64_t iosb = memory + 0x100;
        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};
        const auto invoke = [&](const uint32_t length) {
            emu.memory.write_memory(iosb, &initial, sizeof(initial));
            emu.memory.set_memory(output, 0xa5, 40);
            io_device_context context{emu.memory};
            context.io_control_code = 0x1200c;
            context.io_status_block = {emu.memory, iosb};
            context.output_buffer = output;
            context.output_buffer_length = length;
            return device->execute_ioctl(emu, context);
        };
        EXPECT_EQ(invoke(31), STATUS_BUFFER_TOO_SMALL);
        EXPECT_EQ(emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb).Status, initial.Status);
        EXPECT_TRUE(socket->next_accepted_remote.has_value());
        EXPECT_EQ(invoke(32), STATUS_SUCCESS);
        const auto completed = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
        EXPECT_EQ(completed.Status, STATUS_SUCCESS);
        EXPECT_EQ(completed.Information, 32u);
        EXPECT_EQ(emu.memory.read_memory<LONG>(output), 1);
        EXPECT_EQ(emu.memory.read_memory<uint16_t>(output + sizeof(LONG)), 23u);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(output + 32), 0xa5a5a5a5a5a5a5a5ull);
        EXPECT_EQ(invoke(32), STATUS_PENDING);
        const auto pending = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
        EXPECT_EQ(pending.Status, initial.Status);
        EXPECT_EQ(pending.Information, initial.Information);
    }

    TEST_P(AfdPendingRequestTest, AcceptValidatesTargetBeforeSequenceAndPreservesIosbOnFailure)
    {
        emu.process.is_wow64_process = GetParam();
        socket->listening = true;
        network::address remote{};
        remote.set_ipv4(0x7f000001);
        remote.set_port(4567);
        socket->next_accepted_remote = remote;
        constexpr uint64_t input = memory + 0x800;
        constexpr uint64_t output = memory + 0x880;
        constexpr uint64_t iosb = memory + 0x100;
        io_device_context wait{emu.memory};
        wait.io_control_code = 0x1200c;
        wait.io_status_block = {emu.memory, iosb};
        wait.output_buffer = output;
        wait.output_buffer_length = 20;
        ASSERT_EQ(device->execute_ioctl(emu, wait), STATUS_SUCCESS);
        const auto sequence = emu.memory.read_memory<LONG>(output);
        ASSERT_EQ(sequence, 1);
        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};
        const auto invoke = [&](const LONG value, const handle target) {
            const AFD_ACCEPT_INFO request{.Sequence = value, .AcceptHandle = target};
            emu.memory.write_memory(input, &request, sizeof(request));
            emu.memory.write_memory(iosb, &initial, sizeof(initial));
            io_device_context context{emu.memory};
            context.io_control_code = 0x12010;
            context.io_status_block = {emu.memory, iosb};
            context.input_buffer = input;
            context.input_buffer_length = sizeof(request);
            return device->execute_ioctl(emu, context);
        };
        EXPECT_EQ(invoke(sequence + 1, handle{.bits = 0xffff}), STATUS_INVALID_HANDLE);
        EXPECT_EQ(emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb).Status, initial.Status);

        const auto store_target = [&](const uint32_t type, const uint32_t protocol) {
            const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, type, protocol, 0};
            emu.memory.write_memory(memory + 0x900, creation.data(), sizeof(creation));
            return emu.process.devices.store(
                io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory + 0x900, .length = sizeof(creation)}});
        };
        const auto udp = store_target(2, 17);
        ASSERT_NE(emu.process.devices.get(udp), nullptr);
        EXPECT_EQ(invoke(sequence, udp), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb).Status, initial.Status);
        const auto tcp = store_target(1, 6);
        ASSERT_NE(emu.process.devices.get(tcp), nullptr);
        socket->connected = true;
        EXPECT_EQ(invoke(sequence, tcp), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb).Status, initial.Status);
        socket->connected = false;
        EXPECT_EQ(invoke(sequence, tcp), STATUS_SUCCESS);
        const auto completed = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
        EXPECT_EQ(completed.Status, STATUS_SUCCESS);
        EXPECT_EQ(completed.Information, 0u);
        EXPECT_EQ(invoke(sequence, tcp), STATUS_INVALID_PARAMETER);
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

    TEST_P(AfdPendingRequestTest, SendPollIgnoresWritablePendingConnectAndCompactsQualifiedHandles)
    {
        emu.process.is_wow64_process = GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto connecting_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        const auto ready_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* connecting = emu.process.devices.get(connecting_handle);
        ASSERT_NE(connecting, nullptr);
        ASSERT_NE(emu.process.devices.get(ready_handle), nullptr);
        socket->connect_blocked = true;
        socket->send_blocked = false;

        const auto run = [&]<typename Guest>() {
            using Traits = EmulatorTraits<Guest>;
            using poll_info = AFD_POLL_INFO<Traits>;
            using poll_handle = AFD_POLL_HANDLE_INFO<Traits>;
            constexpr auto prefix = offsetof(poll_info, Handles);
            constexpr uint64_t poll_buffer = memory + 0x800;
            constexpr uint64_t mixed_buffer = memory + 0xa00;
            constexpr uint64_t poll_iosb = memory + 0x780;
            constexpr uint64_t mixed_iosb = memory + 0x620;
            const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Pointer = 0xaabbccdd4a4b4c4dull, .Information = 0x1122334455667788ull};

            AFD_CONNECT_JOIN_INFO_TL<Traits> connect_input{};
            connect_input.RemoteAddress.sa_family = 2;
            emu.memory.write_memory(memory + 0x900, &connect_input, sizeof(connect_input));
            emu.memory.write_memory(memory + 0x100, &initial, sizeof(initial));
            io_device_context connect_request{emu.memory};
            connect_request.file_handle = connecting_handle;
            connect_request.io_control_code = 0x12007;
            connect_request.io_status_block = {emu.memory, memory + 0x100};
            connect_request.event = receive_event;
            connect_request.input_buffer = memory + 0x900;
            connect_request.input_buffer_length = sizeof(connect_input);
            ASSERT_EQ(connecting->execute_ioctl(emu, connect_request), STATUS_PENDING);
            EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
            EXPECT_FALSE(socket->connected);

            poll_info input{};
            input.Timeout.QuadPart = -10'000'000;
            input.NumberOfHandles = 1;
            input.Handles[0].Handle = static_cast<typename Traits::HANDLE>(connecting_handle.bits);
            input.Handles[0].PollEvents = AFD_POLL_SEND;
            input.Handles[0].Status = static_cast<NTSTATUS>(0x4a4b4c4d);
            emu.memory.write_memory(poll_buffer, &input, sizeof(input));
            emu.memory.write_memory(poll_iosb, &initial, sizeof(initial));
            const auto original_input = emu.memory.read_memory(poll_buffer, sizeof(input));
            const auto original_iosb = emu.memory.read_memory(poll_iosb, sizeof(initial));
            io_device_context request{emu.memory};
            request.io_control_code = 0x12024;
            request.io_status_block = {emu.memory, poll_iosb};
            request.event = poll_event;
            request.input_buffer = poll_buffer;
            request.input_buffer_length = sizeof(input);
            request.output_buffer = poll_buffer;
            request.output_buffer_length = sizeof(input);
            ASSERT_EQ(device->execute_ioctl(emu, request), STATUS_PENDING);
            EXPECT_EQ(emu.memory.read_memory(poll_buffer, sizeof(input)), original_input);
            EXPECT_EQ(emu.memory.read_memory(poll_iosb, sizeof(initial)), original_iosb);
            EXPECT_FALSE(emu.process.events.get(poll_event)->signaled);

            connecting->work(emu);
            device->work(emu);
            EXPECT_EQ(emu.memory.read_memory(poll_buffer, sizeof(input)), original_input);
            EXPECT_EQ(emu.memory.read_memory(poll_iosb, sizeof(initial)), original_iosb);
            EXPECT_FALSE(emu.process.events.get(poll_event)->signaled);
            EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
            EXPECT_FALSE(socket->connected);

            const auto mixed_size = prefix + 2 * sizeof(poll_handle);
            poll_info mixed_input = input;
            mixed_input.NumberOfHandles = 2;
            const poll_handle ready{.Handle = static_cast<typename Traits::HANDLE>(ready_handle.bits),
                                    .PollEvents = AFD_POLL_SEND,
                                    .Status = static_cast<NTSTATUS>(0x51525354)};
            emu.memory.set_memory(mixed_buffer, 0xcc, mixed_size);
            emu.memory.write_memory(mixed_buffer, &mixed_input, sizeof(mixed_input));
            emu.memory.write_memory(mixed_buffer + prefix + sizeof(poll_handle), &ready, sizeof(ready));
            const auto trailing_row = emu.memory.read_memory(mixed_buffer + prefix + sizeof(poll_handle), sizeof(poll_handle));
            emu.memory.write_memory(mixed_iosb, &initial, sizeof(initial));
            io_device_context mixed_request{emu.memory};
            mixed_request.io_control_code = 0x12024;
            mixed_request.io_status_block = {emu.memory, mixed_iosb};
            mixed_request.event = second_send_event;
            mixed_request.input_buffer = mixed_buffer;
            mixed_request.input_buffer_length = static_cast<ULONG>(mixed_size);
            mixed_request.output_buffer = mixed_buffer;
            mixed_request.output_buffer_length = static_cast<ULONG>(mixed_size);
            ASSERT_EQ(device->execute_ioctl(emu, mixed_request), STATUS_SUCCESS);
            const auto mixed_result = emu.memory.read_memory<poll_info>(mixed_buffer);
            EXPECT_EQ(mixed_result.NumberOfHandles, 1u);
            EXPECT_EQ(mixed_result.Handles[0].Handle, static_cast<typename Traits::HANDLE>(ready_handle.bits));
            EXPECT_EQ(mixed_result.Handles[0].PollEvents, static_cast<ULONG>(AFD_POLL_SEND));
            EXPECT_EQ(mixed_result.Handles[0].Status, STATUS_SUCCESS);
            EXPECT_EQ(mixed_request.io_status_block.read().Status, STATUS_SUCCESS);
            EXPECT_EQ(mixed_request.io_status_block.read().Information, prefix + sizeof(poll_handle));
            EXPECT_TRUE(emu.process.events.get(second_send_event)->signaled);
            EXPECT_EQ(emu.memory.read_memory(mixed_buffer + prefix + sizeof(poll_handle), sizeof(poll_handle)), trailing_row);
            EXPECT_EQ(emu.memory.read_memory(poll_buffer, sizeof(input)), original_input);
            EXPECT_EQ(emu.memory.read_memory(poll_iosb, sizeof(initial)), original_iosb);
            EXPECT_FALSE(emu.process.events.get(poll_event)->signaled);

            socket->connect_blocked = false;
            connecting->work(emu);
            EXPECT_TRUE(socket->connected);
            EXPECT_EQ(connect_request.io_status_block.read().Status, STATUS_SUCCESS);
            EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
            device->work(emu);
            const auto result = emu.memory.read_memory<poll_info>(poll_buffer);
            EXPECT_EQ(result.NumberOfHandles, 1u);
            EXPECT_EQ(result.Handles[0].Handle, static_cast<typename Traits::HANDLE>(connecting_handle.bits));
            EXPECT_EQ(result.Handles[0].PollEvents, static_cast<ULONG>(AFD_POLL_SEND));
            EXPECT_EQ(result.Handles[0].Status, STATUS_SUCCESS);
            EXPECT_EQ(request.io_status_block.read().Status, STATUS_SUCCESS);
            EXPECT_EQ(request.io_status_block.read().Information, prefix + sizeof(poll_handle));
            EXPECT_TRUE(emu.process.events.get(poll_event)->signaled);
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

    TEST_P(AfdPendingRequestTest, PendingTaggedEventSurvivesWholeProcessSnapshotRestore)
    {
        emu.process.is_wow64_process = GetParam();
        socket->connected = true;
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        const auto port_handle = emu.process.io_completions.store(io_completion{});
        endpoint->completion_port = port_handle;
        endpoint->completion_key = 0x1234;
        const handle tagged_event{.bits = receive_event.bits | 1u};
        ASSERT_EQ(receive_event.bits & 3u, 0u);
        ASSERT_EQ(tagged_event.bits & 3u, 1u);
        ASSERT_EQ(emu.process.events.get(tagged_event), emu.process.events.get(receive_event));
        ASSERT_NE(emu.process.events.get(receive_event), emu.process.events.get(send_event));
        emu.process.events.get(receive_event)->signaled = false;
        emu.process.events.get(send_event)->signaled = false;
        emu.memory.set_memory(memory + 0x400, 0xcc, 16);
        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Pointer = 0xaabbccdd4a4b4c4dull, .Information = 0x1122334455667788ull};
        emu.memory.write_memory(memory + 0x100, &initial, sizeof(initial));
        const auto original_iosb = emu.memory.read_memory(memory + 0x100, sizeof(initial));
        io_device_context request{emu.memory};
        request.file_handle = endpoint_handle;
        request.event = tagged_event;
        request.apc_context = 0x5678;
        request.io_status_block = {emu.memory, memory + 0x100};
        request.io_control_code = 0x12017;
        request.input_buffer = memory + 0x200;
        const auto submit = [&]<typename Guest>() {
            using Traits = EmulatorTraits<Guest>;
            const EMU_WSABUF<Traits> buffer{.len = 3, .buf = static_cast<typename Traits::PVOID>(memory + 0x400)};
            const AFD_RECV_INFO<Traits> input{
                .BufferArray = static_cast<typename Traits::PVOID>(memory + 0x300), .BufferCount = 1, .AfdFlags = 2, .TdiFlags = 0x20};
            emu.memory.write_memory(memory + 0x300, &buffer, sizeof(buffer));
            emu.memory.write_memory(memory + 0x200, &input, sizeof(input));
            request.input_buffer_length = sizeof(input);
            EXPECT_EQ(endpoint->capture_completion_mode(emu, request), device_ioctl_completion_mode::default_completion);
            return endpoint->execute_ioctl(emu, request);
        };
        ASSERT_EQ(GetParam() ? submit.template operator()<Emu32>() : submit.template operator()<Emu64>(), STATUS_PENDING);
        ASSERT_FALSE(request.event_reference_captured);
        ASSERT_EQ(emu.process.events.get(receive_event)->ref_count, 2u);
        EXPECT_EQ(emu.memory.read_memory(memory + 0x100, sizeof(initial)), original_iosb);
        const auto original_input = emu.memory.read_memory(memory + 0x200, request.input_buffer_length);
        const auto descriptor_size = GetParam() ? sizeof(EMU_WSABUF<EmulatorTraits<Emu32>>) : sizeof(EMU_WSABUF<EmulatorTraits<Emu64>>);
        const auto original_descriptor = emu.memory.read_memory(memory + 0x300, descriptor_size);
        EXPECT_FALSE(emu.process.events.get(tagged_event)->signaled);
        EXPECT_FALSE(emu.process.events.get(send_event)->signaled);
        EXPECT_TRUE(emu.process.io_completions.get(port_handle)->queue.empty());

        // The saved count represents the user handle and the actual pending device request.
        utils::buffer_serializer saved_device{};
        endpoint->serialize(saved_device);
        utils::buffer_serializer saved_process{};
        auto& active_thread = emu.vcpu(0).active_thread;
        const auto active_thread_handle = emu.process.threads.find_handle(active_thread);
        const auto saved_epoch = emu.process.io_event_reference_epoch;
        emu.process.serialize(saved_process, active_thread);

        // This old-epoch hold is acquired after saving and is absent from the saved count.
        io_device_context stale_context{emu.memory};
        stale_context.event = tagged_event;
        stale_context.capture_event_reference(emu.process);
        ASSERT_NE(stale_context.event_reference, nullptr);
        EXPECT_EQ(stale_context.event_reference->canonical_handle(), receive_event);
        ASSERT_EQ(emu.process.events.get(receive_event)->ref_count, 3u);

        utils::buffer_deserializer input{saved_process};
        input.register_factory<memory_manager_wrapper>([this] { return memory_manager_wrapper{emu.memory}; });
        input.register_factory<module_manager_wrapper>([this] { return module_manager_wrapper{emu.mod_manager}; });
        input.register_factory<x64_emulator_wrapper>([this] { return x64_emulator_wrapper{emu.emu()}; });
        input.register_factory<windows_emulator_wrapper>([this] { return windows_emulator_wrapper{emu}; });
        input.register_factory<clock_wrapper>([this] { return clock_wrapper{emu.clock()}; });
        input.register_factory<socket_factory_wrapper>([this] { return socket_factory_wrapper{emu.socket_factory()}; });
        input.register_factory<window>([this] { return window{emu.emu()}; });
        emu.process.deserialize(input, active_thread);
        EXPECT_EQ(input.get_remaining_size(), 0u);
        EXPECT_EQ(emu.process.threads.find_handle(active_thread), active_thread_handle);
        EXPECT_EQ(emu.process.io_event_reference_epoch, saved_epoch + 1);
        EXPECT_FALSE(emu.process.adopting_restored_io_event_references);
        ASSERT_NE(emu.process.events.get(receive_event), nullptr);
        ASSERT_EQ(emu.process.events.get(tagged_event), emu.process.events.get(receive_event));
        EXPECT_EQ(emu.process.events.find_handle(emu.process.events.get(tagged_event)), receive_event);
        ASSERT_EQ(emu.process.events.get(receive_event)->ref_count, 2u);
        EXPECT_EQ(stale_context.completion_event(emu.process), nullptr);
        stale_context.event_reference.reset();
        EXPECT_EQ(emu.process.events.get(receive_event)->ref_count, 2u);

        endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        EXPECT_EQ(endpoint->completion_port, port_handle);
        EXPECT_EQ(endpoint->completion_key, 0x1234u);
        utils::buffer_serializer restored_device{};
        endpoint->serialize(restored_device);
        // Exact pending device bytes include the original tagged event, captured context and canonical hold.
        EXPECT_EQ(restored_device.get_buffer(), saved_device.get_buffer());
        EXPECT_EQ(emu.memory.read_memory(memory + 0x200, request.input_buffer_length), original_input);
        EXPECT_EQ(emu.memory.read_memory(memory + 0x300, descriptor_size), original_descriptor);
        EXPECT_EQ(emu.memory.read_memory(memory + 0x100, sizeof(initial)), original_iosb);
        EXPECT_FALSE(emu.process.events.get(tagged_event)->signaled);
        EXPECT_FALSE(emu.process.events.get(send_event)->signaled);
        auto* const port = emu.process.io_completions.get(port_handle);
        ASSERT_NE(port, nullptr);
        EXPECT_TRUE(port->queue.empty());
        endpoint->work(emu);
        EXPECT_EQ(emu.memory.read_memory(memory + 0x100, sizeof(initial)), original_iosb);
        EXPECT_FALSE(emu.process.events.get(tagged_event)->signaled);
        EXPECT_TRUE(port->queue.empty());
        EXPECT_EQ(emu.process.events.get(receive_event)->ref_count, 2u);

        for (const char byte : std::string_view{"abc"})
        {
            socket->incoming.push_back(static_cast<std::byte>(byte));
        }
        endpoint->work(emu);
        EXPECT_EQ(status(false).Status, STATUS_SUCCESS);
        EXPECT_EQ(status(false).Information, 3u);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x104), 0u);
        EXPECT_EQ((emu.memory.read_memory<std::array<char, 3>>(memory + 0x400)), (std::array<char, 3>{'a', 'b', 'c'}));
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x403), 0xccu);
        EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
        EXPECT_TRUE(emu.process.events.get(tagged_event)->signaled);
        EXPECT_FALSE(emu.process.events.get(send_event)->signaled);
        EXPECT_EQ(emu.process.events.get(receive_event)->ref_count, 1u);
        EXPECT_EQ(emu.process.events.get(send_event)->ref_count, 1u);
        ASSERT_EQ(port->queue.size(), 1u);
        EXPECT_EQ(port->queue.front().key_context, 0x1234u);
        EXPECT_EQ(port->queue.front().apc_context, 0x5678u);
        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_SUCCESS);
        EXPECT_EQ(port->queue.front().io_status_block.Information, 3u);
        endpoint->work(emu);
        EXPECT_EQ(port->queue.size(), 1u);
        EXPECT_EQ(emu.process.events.get(receive_event)->ref_count, 1u);
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
        input.register_factory<windows_emulator_wrapper>([this] { return windows_emulator_wrapper{emu}; });
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
        ASSERT_EQ(transfer(true, 0, endpoint, endpoint_handle, 0, 0, 0xfeed), STATUS_PENDING);
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
        EXPECT_EQ(port->queue.front().apc_context, 0xfeedu);
        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_SUCCESS);
        EXPECT_EQ(port->queue.front().io_status_block.Information, 3u);
    }

    TEST_P(AfdPendingRequestTest, DefaultPendingReceiveUsesCapturedCompletionContext)
    {
        emu.process.is_wow64_process = GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        socket->connected = true;
        const auto port_handle = emu.process.io_completions.store(io_completion{});
        endpoint->completion_port = port_handle;
        endpoint->completion_key = 0x1234;
        auto* port = emu.process.io_completions.get(port_handle);
        ASSERT_NE(port, nullptr);
        const auto check = [&]<typename Guest>() {
            using Traits = EmulatorTraits<Guest>;
            for (const uint64_t captured_context : {0ull, 0x5678ull})
            {
                SCOPED_TRACE(testing::Message() << "captured_context=" << captured_context);
                port->queue.clear();
                socket->incoming.clear();
                emu.process.events.get(receive_event)->signaled = false;
                emu.process.events.get(send_event)->signaled = false;
                emu.memory.set_memory(memory + 0x400, 0xcc, 16);
                const EMU_WSABUF<Traits> buffer{.len = 3, .buf = static_cast<typename Traits::PVOID>(memory + 0x400)};
                const AFD_RECV_INFO<Traits> input{
                    .BufferArray = static_cast<typename Traits::PVOID>(memory + 0x300), .BufferCount = 1, .AfdFlags = 2, .TdiFlags = 0x20};
                emu.memory.write_memory(memory + 0x300, &buffer, sizeof(buffer));
                emu.memory.write_memory(memory + 0x200, &input, sizeof(input));
                const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> sentinel{.Pointer = 0xaabbccdd4a4b4c4dull,
                                                                      .Information = 0x1122334455667788ull};
                emu.memory.write_memory(memory + 0x100, &sentinel, sizeof(sentinel));
                emu.memory.write_memory(memory + 0x180, &sentinel, sizeof(sentinel));
                const auto original = emu.memory.read_memory(memory + 0x100, sizeof(sentinel));
                io_device_context request{emu.memory};
                request.file_handle = endpoint_handle;
                request.event = receive_event;
                request.apc_context = captured_context;
                request.io_status_block = {emu.memory, memory + 0x100};
                request.io_control_code = 0x12017;
                request.input_buffer = memory + 0x200;
                request.input_buffer_length = sizeof(input);
                ASSERT_EQ(endpoint->capture_completion_mode(emu, request), device_ioctl_completion_mode::default_completion);
                ASSERT_EQ(endpoint->execute_ioctl(emu, request), STATUS_PENDING);
                EXPECT_EQ(emu.memory.read_memory(memory + 0x100, sizeof(sentinel)), original);
                EXPECT_TRUE(port->queue.empty());
                EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
                request.apc_context = captured_context ? 0 : 0x9999;
                request.event = send_event;
                request.file_handle = {};
                request.io_status_block = {emu.memory, memory + 0x180};
                for (const char byte : std::string_view{"abc"})
                {
                    socket->incoming.push_back(static_cast<std::byte>(byte));
                }
                endpoint->work(emu);
                EXPECT_EQ(status(false).Status, STATUS_SUCCESS);
                EXPECT_EQ(status(false).Information, 3u);
                EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x104), 0u);
                EXPECT_EQ((emu.memory.read_memory<std::array<char, 3>>(memory + 0x400)), (std::array<char, 3>{'a', 'b', 'c'}));
                EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x403), 0xccu);
                EXPECT_EQ(emu.memory.read_memory(memory + 0x180, sizeof(sentinel)), original);
                EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
                EXPECT_FALSE(emu.process.events.get(send_event)->signaled);
                if (captured_context == 0)
                {
                    EXPECT_TRUE(port->queue.empty());
                }
                else
                {
                    ASSERT_EQ(port->queue.size(), 1u);
                    EXPECT_EQ(port->queue.front().key_context, 0x1234u);
                    EXPECT_EQ(port->queue.front().apc_context, captured_context);
                    EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_SUCCESS);
                    EXPECT_EQ(port->queue.front().io_status_block.Information, 3u);
                }
                const auto completed_count = port->queue.size();
                endpoint->work(emu);
                EXPECT_EQ(port->queue.size(), completed_count);
            }
        };
        if (GetParam())
        {
            check.template operator()<Emu32>();
        }
        else
        {
            check.template operator()<Emu64>();
        }
    }

    TEST_P(AfdPendingRequestTest, PollCompletionUsesCapturedContextAndPreservesReadyOutputAndEvent)
    {
        emu.process.is_wow64_process = GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        socket->connected = true;
        const auto port_handle = emu.process.io_completions.store(io_completion{});
        endpoint->completion_port = port_handle;
        endpoint->completion_key = 0x1234;
        auto* port = emu.process.io_completions.get(port_handle);
        ASSERT_NE(port, nullptr);
        const auto check = [&]<typename Guest>() {
            using Traits = EmulatorTraits<Guest>;
            constexpr ULONG requested_mask = AFD_POLL_RECEIVE | AFD_POLL_DISCONNECT | AFD_POLL_ABORT | AFD_POLL_ACCEPT;
            static_assert(requested_mask == 0x99);
            constexpr auto completed_bytes = offsetof(AFD_POLL_INFO<Traits>, Handles) + sizeof(AFD_POLL_HANDLE_INFO<Traits>);
            for (const bool initially_ready : {false, true})
            {
                for (const uint64_t captured_context : {0ull, 0x5678ull})
                {
                    SCOPED_TRACE(testing::Message() << "ready=" << initially_ready << " captured_context=" << captured_context);
                    port->queue.clear();
                    socket->incoming.clear();
                    emu.process.events.get(poll_event)->signaled = false;
                    AFD_POLL_INFO<Traits> input{};
                    input.Timeout.QuadPart = -10000000; // One second, never the separate zero-timeout path.
                    input.NumberOfHandles = 1;
                    input.Handles[0].Handle = static_cast<typename Traits::HANDLE>(endpoint_handle.bits);
                    input.Handles[0].PollEvents = requested_mask;
                    emu.memory.write_memory(memory + 0x800, &input, sizeof(input));
                    const auto original_input = emu.memory.read_memory(memory + 0x800, sizeof(input));
                    const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> sentinel{.Pointer = 0xaabbccdd4a4b4c4dull,
                                                                          .Information = 0x1122334455667788ull};
                    emu.memory.write_memory(memory + 0x780, &sentinel, sizeof(sentinel));
                    const auto original_status = emu.memory.read_memory(memory + 0x780, sizeof(sentinel));
                    io_device_context request{emu.memory};
                    request.file_handle = endpoint_handle;
                    request.event = poll_event;
                    request.apc_context = captured_context;
                    request.io_control_code = 0x12024;
                    request.io_status_block = {emu.memory, memory + 0x780};
                    request.input_buffer = memory + 0x800;
                    request.input_buffer_length = sizeof(input);
                    request.output_buffer = request.input_buffer;
                    request.output_buffer_length = request.input_buffer_length;
                    ASSERT_EQ(endpoint->capture_completion_mode(emu, request), device_ioctl_completion_mode::default_completion);
                    if (initially_ready)
                    {
                        socket->incoming.push_back(std::byte{'a'});
                    }
                    ASSERT_EQ(endpoint->execute_ioctl(emu, request), initially_ready ? STATUS_SUCCESS : STATUS_PENDING);
                    if (!initially_ready)
                    {
                        EXPECT_EQ(emu.memory.read_memory(memory + 0x800, sizeof(input)), original_input);
                        EXPECT_EQ(emu.memory.read_memory(memory + 0x780, sizeof(sentinel)), original_status);
                        EXPECT_FALSE(emu.process.events.get(poll_event)->signaled);
                        EXPECT_TRUE(port->queue.empty());
                        request.apc_context = captured_context ? 0 : 0x9999;
                        socket->incoming.push_back(std::byte{'a'});
                        endpoint->work(emu);
                    }
                    const auto output = emu.memory.read_memory<AFD_POLL_INFO<Traits>>(memory + 0x800);
                    EXPECT_EQ(output.NumberOfHandles, 1u);
                    EXPECT_EQ(output.Handles[0].Handle, static_cast<typename Traits::HANDLE>(endpoint_handle.bits));
                    EXPECT_EQ(output.Handles[0].PollEvents, AFD_POLL_RECEIVE);
                    EXPECT_EQ(output.Handles[0].Status, STATUS_SUCCESS);
                    const auto final_status = emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(memory + 0x780);
                    EXPECT_EQ(final_status.Status, STATUS_SUCCESS);
                    EXPECT_EQ(final_status.Information, completed_bytes);
                    EXPECT_TRUE(emu.process.events.get(poll_event)->signaled);
                    if (captured_context == 0)
                    {
                        EXPECT_TRUE(port->queue.empty());
                    }
                    else
                    {
                        ASSERT_EQ(port->queue.size(), 1u);
                        EXPECT_EQ(port->queue.front().key_context, 0x1234u);
                        EXPECT_EQ(port->queue.front().apc_context, captured_context);
                        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_SUCCESS);
                        EXPECT_EQ(port->queue.front().io_status_block.Information, completed_bytes);
                    }
                    const auto completed_count = port->queue.size();
                    endpoint->work(emu);
                    EXPECT_EQ(port->queue.size(), completed_count);
                }
            }
        };
        if (GetParam())
        {
            check.template operator()<Emu32>();
        }
        else
        {
            check.template operator()<Emu64>();
        }
    }

    TEST_P(AfdPendingRequestTest, SynchronousSendSkipsApcButPendingSendQueuesIt)
    {
        emu.process.is_wow64_process = GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);

        emulator_thread issuer{emu.memory};
        issuer.id = 0x1234;
        const auto thread_handle = emu.process.threads.store(std::move(issuer));
        emu.process.thread_handles_by_id.emplace(0x1234, thread_handle);
        auto* thread = emu.process.threads.get(thread_handle);
        ASSERT_NE(thread, nullptr);

        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Status = static_cast<NTSTATUS>(0x4A4B4C4D), .Information = 0x11223344};
        emu.memory.write_memory(memory + 0x120, &initial, sizeof(initial));
        socket->send_blocked = false;
        ASSERT_EQ(transfer(true, 0, endpoint, endpoint_handle, 0x1234, 0x567800, 0xfeed), STATUS_SUCCESS);
        EXPECT_EQ(status(true).Status, STATUS_SUCCESS);
        EXPECT_EQ(status(true).Information, 3u);
        EXPECT_TRUE(emu.process.events.get(send_event)->signaled);
        EXPECT_TRUE(thread->pending_apcs.empty());
        EXPECT_EQ(socket->outgoing, (std::vector<std::byte>{std::byte{'X'}, std::byte{'Y'}, std::byte{'Z'}}));

        emu.process.events.get(send_event)->signaled = false;
        emu.memory.write_memory(memory + 0x120, &initial, sizeof(initial));
        socket->send_blocked = true;
        ASSERT_EQ(transfer(true, 0, endpoint, endpoint_handle, 0x1234, 0x567800, 0xfeed), STATUS_PENDING);
        EXPECT_EQ(status(true).Status, initial.Status);
        EXPECT_EQ(status(true).Information, initial.Information);
        EXPECT_FALSE(emu.process.events.get(send_event)->signaled);
        EXPECT_TRUE(thread->pending_apcs.empty());

        socket->send_blocked = false;
        endpoint->work(emu);
        EXPECT_EQ(status(true).Status, STATUS_SUCCESS);
        EXPECT_EQ(status(true).Information, 3u);
        EXPECT_TRUE(emu.process.events.get(send_event)->signaled);
        ASSERT_EQ(thread->pending_apcs.size(), 1u);
        EXPECT_EQ(thread->pending_apcs.front().apc_routine, 0x567800u);
        EXPECT_EQ(thread->pending_apcs.front().apc_argument1, 0xfeedu);
        EXPECT_EQ(thread->pending_apcs.front().apc_argument2, memory + 0x120);
    }

    TEST_P(AfdPendingRequestTest, StreamCallbackModeQueuesSynchronousApcAndRetainsStatusUnion)
    {
        static_assert(sizeof(AFD_SEND_INFO<EmulatorTraits<Emu32>>) == 16);
        static_assert(sizeof(AFD_RECV_INFO<EmulatorTraits<Emu32>>) == 16);
        static_assert(offsetof(AFD_SEND_INFO<EmulatorTraits<Emu32>>, AfdFlags) == 8);
        static_assert(offsetof(AFD_RECV_INFO<EmulatorTraits<Emu32>>, AfdFlags) == 8);
        static_assert(sizeof(AFD_SEND_INFO<EmulatorTraits<Emu64>>) == 24);
        static_assert(sizeof(AFD_RECV_INFO<EmulatorTraits<Emu64>>) == 24);
        static_assert(offsetof(AFD_SEND_INFO<EmulatorTraits<Emu64>>, AfdFlags) == 12);
        static_assert(offsetof(AFD_RECV_INFO<EmulatorTraits<Emu64>>, AfdFlags) == 12);
        static_assert(sizeof(IO_STATUS_BLOCK<EmulatorTraits<Emu32>>) == 8);
        static_assert(offsetof(IO_STATUS_BLOCK<EmulatorTraits<Emu32>>, Information) == 4);
        static_assert(sizeof(IO_STATUS_BLOCK<EmulatorTraits<Emu64>>) == 16);
        static_assert(offsetof(IO_STATUS_BLOCK<EmulatorTraits<Emu64>>, Information) == 8);

        emu.process.is_wow64_process = GetParam();
        const bool qualified = !GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        emulator_thread issuer{emu.memory};
        issuer.id = 0x1234;
        const auto thread_handle = emu.process.threads.store(std::move(issuer));
        emu.process.thread_handles_by_id.emplace(0x1234, thread_handle);
        auto* thread = emu.process.threads.get(thread_handle);
        ASSERT_NE(thread, nullptr);
        socket->send_blocked = false;

        for (const bool is_send : {false, true})
        {
            for (const ULONG flags : {0u, 3u})
            {
                SCOPED_TRACE(testing::Message() << "send=" << is_send << " flags=" << flags);
                const bool callback_mode = qualified && flags == 3;
                thread->pending_apcs.clear();
                const auto iosb = memory + (is_send ? 0x120 : 0x100);
                const auto signal = is_send ? send_event : receive_event;
                const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Pointer = 0xaabbccdd4a4b4c4dull,
                                                                     .Information = 0x1122334455667788ull};
                emu.memory.write_memory(iosb, &initial, sizeof(initial));
                emu.process.events.get(signal)->signaled = false;
                observed_iosb_memory observed{emu.memory};
                size_t terminal_observations = 0;
                observed.after_byte_store = [&] {
                    if (!qualified)
                    {
                        return;
                    }
                    EXPECT_EQ(emu.memory.read_memory<uint32_t>(iosb + 4), 0xaabbccddu);
                    if (status(is_send).Status == STATUS_SUCCESS)
                    {
                        ++terminal_observations;
                        EXPECT_EQ(status(is_send).Information, 3u);
                        if (!is_send)
                        {
                            EXPECT_EQ((emu.memory.read_memory<std::array<char, 3>>(memory + 0x400)), (std::array<char, 3>{'a', 'b', 'c'}));
                        }
                        else
                        {
                            EXPECT_GE(socket->outgoing.size(), 3u);
                        }
                    }
                };
                if (!is_send)
                {
                    for (const char value : std::string_view{"abc"})
                    {
                        socket->incoming.push_back(static_cast<std::byte>(value));
                    }
                }
                ASSERT_EQ(transfer(is_send, is_send ? 0 : 0x20, endpoint, endpoint_handle, 0x1234, 0x567800, 0xfeed, flags,
                                   flags == 3 ? &observed : nullptr),
                          STATUS_SUCCESS);
                io_device_context mode_request{emu.memory};
                mode_request.io_control_code = is_send ? 0x1201f : 0x12017;
                mode_request.input_buffer = memory + (is_send ? 0x240 : 0x200);
                mode_request.input_buffer_length =
                    GetParam() ? sizeof(AFD_SEND_INFO<EmulatorTraits<Emu32>>) : sizeof(AFD_SEND_INFO<EmulatorTraits<Emu64>>);
                EXPECT_EQ(endpoint->capture_completion_mode(emu, mode_request), callback_mode
                                                                                    ? device_ioctl_completion_mode::afd_stream_callback
                                                                                    : device_ioctl_completion_mode::default_completion);
                if (callback_mode)
                {
                    EXPECT_GT(terminal_observations, 0u);
                    EXPECT_EQ(observed.stores, (std::vector<std::pair<uint64_t, size_t>>{{iosb + 8, 8}, {iosb, 4}}));
                }
                else if (flags == 3)
                {
                    EXPECT_EQ(observed.stores, (std::vector<std::pair<uint64_t, size_t>>{
                                                   {iosb, sizeof(initial)}, {iosb, sizeof(initial)}, {iosb, sizeof(initial)}}));
                }
                if (is_send)
                {
                    ASSERT_EQ(socket->outgoing.size(), flags == 3 ? 6u : 3u);
                    EXPECT_EQ((std::vector<std::byte>(socket->outgoing.end() - 3, socket->outgoing.end())),
                              (std::vector<std::byte>{std::byte{'X'}, std::byte{'Y'}, std::byte{'Z'}}));
                }
                else
                {
                    EXPECT_EQ((emu.memory.read_memory<std::array<char, 3>>(memory + 0x400)), (std::array<char, 3>{'a', 'b', 'c'}));
                }
                EXPECT_EQ(status(is_send).Status, STATUS_SUCCESS);
                EXPECT_EQ(status(is_send).Information, 3u);
                EXPECT_EQ(emu.memory.read_memory<uint32_t>(iosb + 4), callback_mode ? 0xaabbccddu : 0u);
                EXPECT_TRUE(emu.process.events.get(signal)->signaled);
                ASSERT_EQ(thread->pending_apcs.size(), callback_mode ? 1u : 0u);
                if (callback_mode)
                {
                    const auto& apc = thread->pending_apcs.front();
                    EXPECT_EQ(apc.apc_routine, 0x567800u);
                    EXPECT_EQ(apc.apc_argument1, 0xfeedu);
                    EXPECT_EQ(apc.apc_argument2, iosb);
                    EXPECT_EQ(apc.io_status, static_cast<int32_t>(STATUS_SUCCESS));
                    EXPECT_EQ(apc.io_information, 3u);
                }
                endpoint->work(emu);
                EXPECT_EQ(thread->pending_apcs.size(), callback_mode ? 1u : 0u);
            }
        }
    }

    TEST_P(AfdPendingRequestTest, NoFastStreamWouldBlockPreservesIosbWithoutCompletionSignal)
    {
        emu.process.is_wow64_process = GetParam();
        const bool qualified = !GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        emulator_thread issuer{emu.memory};
        issuer.id = 0x1234;
        const auto thread_handle = emu.process.threads.store(std::move(issuer));
        emu.process.thread_handles_by_id.emplace(0x1234, thread_handle);
        auto* thread = emu.process.threads.get(thread_handle);
        ASSERT_NE(thread, nullptr);

        const AFD_INFO nonblocking{.InformationClass = 2, .Information = 1};
        emu.memory.write_memory(memory + 0x900, &nonblocking, sizeof(nonblocking));
        io_device_context mode_request{emu.memory};
        mode_request.io_control_code = 0x1203b;
        mode_request.input_buffer = memory + 0x900;
        mode_request.input_buffer_length = sizeof(nonblocking);
        mode_request.io_status_block = {emu.memory, memory + 0x80};
        ASSERT_EQ(endpoint->execute_ioctl(emu, mode_request), STATUS_SUCCESS);

        auto& vcpu = emu.vcpu(0);
        auto* const original_thread = vcpu.active_thread;
        vcpu.active_thread = thread;
        const auto restore_thread = utils::finally([&] { vcpu.active_thread = original_thread; });
        const syscall_context syscall{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        observed_iosb_memory observed{emu.memory};
        const auto submit = [&]<typename Guest>(const ULONG flags) {
            using Traits = EmulatorTraits<Guest>;
            const EMU_WSABUF<Traits> buffer{.len = 3, .buf = static_cast<typename Traits::PVOID>(memory + 0x400)};
            const AFD_RECV_INFO<Traits> request{
                .BufferArray = static_cast<typename Traits::PVOID>(memory + 0x300), .BufferCount = 1, .AfdFlags = flags, .TdiFlags = 0x20};
            emu.memory.write_memory(memory + 0x300, &buffer, sizeof(buffer));
            emu.memory.write_memory(memory + 0x200, &request, sizeof(request));
            io_device_context capture_request{emu.memory};
            capture_request.io_control_code = 0x12017;
            capture_request.input_buffer = memory + 0x200;
            capture_request.input_buffer_length = sizeof(request);
            EXPECT_EQ(endpoint->capture_completion_mode(emu, capture_request), qualified && flags == 1
                                                                                   ? device_ioctl_completion_mode::afd_stream_no_fast_io
                                                                                   : device_ioctl_completion_mode::default_completion);
            return syscalls::handle_NtDeviceIoControlFile(syscall, endpoint_handle, receive_event, 0x567800, 0xfeed,
                                                          {observed, memory + 0x100}, 0x12017, memory + 0x200, sizeof(request), 0, 0);
        };

        for (const ULONG flags : {0u, 1u})
        {
            for (const bool initially_signaled : {false, true})
            {
                SCOPED_TRACE(testing::Message() << "flags=" << flags << " initial_event=" << initially_signaled);
                const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Pointer = 0xaabbccdd4a4b4c4dull,
                                                                     .Information = 0x1122334455667788ull};
                emu.memory.write_memory(memory + 0x100, &initial, sizeof(initial));
                const auto original_bytes = emu.memory.read_memory(memory + 0x100, sizeof(initial));
                emu.process.events.get(receive_event)->signaled = initially_signaled;
                observed.stores.clear();
                ASSERT_EQ(GetParam() ? submit.template operator()<Emu32>(flags) : submit.template operator()<Emu64>(flags),
                          STATUS_DEVICE_NOT_READY);
                if (qualified && flags == 1)
                {
                    EXPECT_EQ(emu.memory.read_memory(memory + 0x100, sizeof(initial)), original_bytes);
                    EXPECT_FALSE(emu.process.events.get(receive_event)->signaled);
                    EXPECT_TRUE(observed.stores.empty());
                }
                else
                {
                    EXPECT_EQ(status(false).Status, STATUS_DEVICE_NOT_READY);
                    EXPECT_EQ(status(false).Information, 0u);
                    EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x104), 0u);
                    EXPECT_TRUE(emu.process.events.get(receive_event)->signaled);
                    EXPECT_EQ(observed.stores, (std::vector<std::pair<uint64_t, size_t>>{{memory + 0x100, sizeof(initial)},
                                                                                         {memory + 0x100, sizeof(initial)}}));
                }
                EXPECT_TRUE(thread->pending_apcs.empty());
                endpoint->work(emu);
                EXPECT_TRUE(thread->pending_apcs.empty());
            }
        }

        // Flags1 send A3 is not independently measured; keep its existing
        // completion fallback instead of extending the receive-only omission policy.
        const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Pointer = 0xaabbccdd4a4b4c4dull, .Information = 0x1122334455667788ull};
        emu.memory.write_memory(memory + 0x120, &initial, sizeof(initial));
        socket->send_blocked = true;
        emu.process.events.get(send_event)->signaled = false;
        ASSERT_EQ(transfer(true, 0, endpoint, endpoint_handle, 0x1234, 0x567800, 0xfeed, 1), STATUS_DEVICE_NOT_READY);
        io_device_context capture_request{emu.memory};
        capture_request.io_control_code = 0x1201f;
        capture_request.input_buffer = memory + 0x240;
        capture_request.input_buffer_length =
            GetParam() ? sizeof(AFD_SEND_INFO<EmulatorTraits<Emu32>>) : sizeof(AFD_SEND_INFO<EmulatorTraits<Emu64>>);
        EXPECT_EQ(endpoint->capture_completion_mode(emu, capture_request), device_ioctl_completion_mode::default_completion);
        EXPECT_EQ(status(true).Status, STATUS_DEVICE_NOT_READY);
        EXPECT_EQ(status(true).Information, 0u);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x124), 0u);
        EXPECT_TRUE(emu.process.events.get(send_event)->signaled);
        EXPECT_TRUE(thread->pending_apcs.empty());
    }

    TEST_P(AfdPendingRequestTest, CallbackPendingStreamUsesCapturedModeAndRetainsStatusUnion)
    {
        emu.process.is_wow64_process = GetParam();
        const bool qualified = !GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        emulator_thread issuer{emu.memory};
        issuer.id = 0x1234;
        const auto thread_handle = emu.process.threads.store(std::move(issuer));
        emu.process.thread_handles_by_id.emplace(0x1234, thread_handle);
        auto* thread = emu.process.threads.get(thread_handle);
        ASSERT_NE(thread, nullptr);
        for (const bool is_send : {false, true})
        {
            SCOPED_TRACE(testing::Message() << "send=" << is_send);
            thread->pending_apcs.clear();
            socket->send_blocked = true;
            const auto iosb = memory + (is_send ? 0x120 : 0x100);
            const auto input = memory + (is_send ? 0x240 : 0x200);
            const auto flags_offset = GetParam() ? 8u : 12u;
            const auto signal = is_send ? send_event : receive_event;
            const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Pointer = 0xaabbccdd4a4b4c4dull, .Information = 0x1122334455667788ull};
            emu.memory.write_memory(iosb, &initial, sizeof(initial));
            const auto original_bytes = emu.memory.read_memory(iosb, sizeof(initial));
            emu.process.events.get(signal)->signaled = false;
            observed_iosb_memory observed{emu.memory};
            size_t terminal_observations = 0;
            observed.after_byte_store = [&] {
                if (!qualified)
                {
                    return;
                }
                EXPECT_EQ(emu.memory.read_memory<uint32_t>(iosb + 4), 0xaabbccddu);
                if (status(is_send).Status == STATUS_SUCCESS)
                {
                    ++terminal_observations;
                    EXPECT_EQ(status(is_send).Information, 3u);
                    if (!is_send)
                    {
                        EXPECT_EQ((emu.memory.read_memory<std::array<char, 3>>(memory + 0x400)), (std::array<char, 3>{'a', 'b', 'c'}));
                    }
                    else
                    {
                        EXPECT_EQ(socket->outgoing.size(), 3u);
                    }
                }
            };
            ASSERT_EQ(transfer(is_send, is_send ? 0 : 0x20, endpoint, endpoint_handle, 0x1234, 0x567800, 0xfeed, 3, &observed),
                      STATUS_PENDING);
            io_device_context mode_request{emu.memory};
            mode_request.io_control_code = is_send ? 0x1201f : 0x12017;
            mode_request.input_buffer = input;
            mode_request.input_buffer_length =
                GetParam() ? sizeof(AFD_SEND_INFO<EmulatorTraits<Emu32>>) : sizeof(AFD_SEND_INFO<EmulatorTraits<Emu64>>);
            EXPECT_EQ(endpoint->capture_completion_mode(emu, mode_request),
                      qualified ? device_ioctl_completion_mode::afd_stream_callback : device_ioctl_completion_mode::default_completion);
            EXPECT_EQ(emu.memory.read_memory(iosb, sizeof(initial)), original_bytes);
            EXPECT_FALSE(emu.process.events.get(signal)->signaled);
            EXPECT_TRUE(thread->pending_apcs.empty());
            if (qualified)
            {
                EXPECT_TRUE(observed.stores.empty());
            }
            else
            {
                EXPECT_EQ(observed.stores, (std::vector<std::pair<uint64_t, size_t>>{{iosb, sizeof(initial)}, {iosb, sizeof(initial)}}));
            }
            observed.stores.clear();
            // Completion must use the captured original mode, not caller-mutated input.
            emulator_object<uint32_t>{emu.memory, input + flags_offset}.write(0u);
            for (const char value : std::string_view{"abc"})
            {
                if (!is_send)
                {
                    socket->incoming.push_back(static_cast<std::byte>(value));
                }
            }
            socket->send_blocked = false;
            endpoint->work(emu);
            if (qualified)
            {
                EXPECT_GT(terminal_observations, 0u);
                EXPECT_EQ(observed.stores, (std::vector<std::pair<uint64_t, size_t>>{{iosb + 8, 8}, {iosb, 4}}));
            }
            else
            {
                EXPECT_EQ(observed.stores, (std::vector<std::pair<uint64_t, size_t>>{
                                               {iosb, sizeof(initial)}, {iosb, sizeof(initial)}, {iosb, sizeof(initial)}}));
            }
            if (is_send)
            {
                EXPECT_EQ(socket->outgoing, (std::vector<std::byte>{std::byte{'X'}, std::byte{'Y'}, std::byte{'Z'}}));
            }
            else
            {
                EXPECT_EQ((emu.memory.read_memory<std::array<char, 3>>(memory + 0x400)), (std::array<char, 3>{'a', 'b', 'c'}));
            }
            EXPECT_EQ(status(is_send).Status, STATUS_SUCCESS);
            EXPECT_EQ(status(is_send).Information, 3u);
            EXPECT_EQ(emu.memory.read_memory<uint32_t>(iosb + 4), qualified ? 0xaabbccddu : 0u);
            EXPECT_TRUE(emu.process.events.get(signal)->signaled);
            ASSERT_EQ(thread->pending_apcs.size(), 1u);
            EXPECT_EQ(thread->pending_apcs.front().apc_routine, 0x567800u);
            EXPECT_EQ(thread->pending_apcs.front().apc_argument1, 0xfeedu);
            EXPECT_EQ(thread->pending_apcs.front().apc_argument2, iosb);
            EXPECT_EQ(thread->pending_apcs.front().io_status, static_cast<int32_t>(STATUS_SUCCESS));
            EXPECT_EQ(thread->pending_apcs.front().io_information, 3u);
            endpoint->work(emu);
            EXPECT_EQ(thread->pending_apcs.size(), 1u);
        }
    }

    TEST_P(AfdPendingRequestTest, InvalidStreamHeadersDoNotEnableCallbackCompletionMode)
    {
        emu.process.is_wow64_process = GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        emulator_thread issuer{emu.memory};
        issuer.id = 0x1234;
        const auto thread_handle = emu.process.threads.store(std::move(issuer));
        emu.process.thread_handles_by_id.emplace(0x1234, thread_handle);
        auto* thread = emu.process.threads.get(thread_handle);
        ASSERT_NE(thread, nullptr);
        const auto submit = [&]<typename Guest>(const bool wrong_base) {
            using Traits = EmulatorTraits<Guest>;
            const AFD_RECV_INFO<Traits> request{.BufferCount = 1, .AfdFlags = 3, .TdiFlags = 0x20};
            emu.memory.write_memory(memory + 0x200, &request, sizeof(request));
            io_device_context context{emu.memory};
            context.file_handle = endpoint_handle;
            context.issuer_thread_id = 0x1234;
            context.event = receive_event;
            context.apc_routine = 0x567800;
            context.apc_context = 0xfeed;
            context.io_status_block = {emu.memory, memory + 0x100};
            context.io_control_code = wrong_base ? 0x13017 : 0x12017;
            context.input_buffer = memory + 0x200;
            context.input_buffer_length = wrong_base ? sizeof(request) : sizeof(request) - 1;
            return endpoint->execute_ioctl(emu, context);
        };
        for (const bool wrong_base : {false, true})
        {
            SCOPED_TRACE(testing::Message() << "wrong_base=" << wrong_base);
            const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Pointer = 0xaabbccdd4a4b4c4dull, .Information = 0x1122334455667788ull};
            emu.memory.write_memory(memory + 0x100, &initial, sizeof(initial));
            const auto expected = wrong_base ? STATUS_NOT_SUPPORTED : STATUS_BUFFER_TOO_SMALL;
            EXPECT_EQ(GetParam() ? submit.template operator()<Emu32>(wrong_base) : submit.template operator()<Emu64>(wrong_base), expected);
            EXPECT_EQ(status(false).Status, expected);
            EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x104), 0u);
            EXPECT_TRUE(thread->pending_apcs.empty());
        }

        observed_iosb_memory observed{emu.memory};
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> wrapping{observed, std::numeric_limits<uint64_t>::max() - 3};
        EXPECT_THROW(write_irp_io_information(wrapping, 3), std::out_of_range);
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> wrapping_status{observed, std::numeric_limits<uint64_t>::max() - 1};
        EXPECT_THROW(write_irp_io_status(wrapping_status, STATUS_SUCCESS), std::out_of_range);
        EXPECT_TRUE(observed.stores.empty());
    }

    TEST_P(AfdPendingRequestTest, ReceiveIocpRequiresContextForBaseAndTaggedEventAliases)
    {
        emu.process.is_wow64_process = GetParam();
        const bool qualified = !GetParam();
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto endpoint_handle =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* endpoint = emu.process.devices.get(endpoint_handle);
        ASSERT_NE(endpoint, nullptr);
        const AFD_INFO nonblocking{.InformationClass = 2, .Information = 1};
        emu.memory.write_memory(memory + 0x900, &nonblocking, sizeof(nonblocking));
        io_device_context mode_request{emu.memory};
        mode_request.io_control_code = 0x1203b;
        mode_request.input_buffer = memory + 0x900;
        mode_request.input_buffer_length = sizeof(nonblocking);
        mode_request.io_status_block = {emu.memory, memory + 0x80};
        ASSERT_EQ(endpoint->execute_ioctl(emu, mode_request), STATUS_SUCCESS);
        socket->connected = true;
        const auto port_handle = emu.process.io_completions.store(io_completion{});
        endpoint->completion_port = port_handle;
        endpoint->completion_key = 0x1234;
        auto* port = emu.process.io_completions.get(port_handle);
        ASSERT_NE(port, nullptr);
        const auto base_event = receive_event;
        const handle tagged_event{.bits = base_event.bits | 1u};
        const auto decoy_event = send_event;
        const std::array<handle, 2> signals{base_event, tagged_event};
        ASSERT_EQ(base_event.bits & 3u, 0u);
        ASSERT_EQ(decoy_event.bits & 3u, 0u);
        ASSERT_EQ(tagged_event.bits & 3u, 1u);
        ASSERT_NE(base_event.bits, tagged_event.bits);
        ASSERT_NE(base_event.bits, decoy_event.bits);
        ASSERT_NE(emu.process.events.get(base_event), nullptr);
        ASSERT_NE(emu.process.events.get(tagged_event), nullptr);
        ASSERT_NE(emu.process.events.get(decoy_event), nullptr);
        ASSERT_EQ(emu.process.events.get(base_event), emu.process.events.get(tagged_event));
        ASSERT_NE(emu.process.events.get(base_event), emu.process.events.get(decoy_event));
        const auto submit = [&]<typename Guest>(const handle signal, const uint64_t completion_context) {
            using Traits = EmulatorTraits<Guest>;
            const EMU_WSABUF<Traits> buffer{.len = 16, .buf = static_cast<typename Traits::PVOID>(memory + 0x400)};
            const AFD_RECV_INFO<Traits> input{
                .BufferArray = static_cast<typename Traits::PVOID>(memory + 0x300), .BufferCount = 1, .AfdFlags = 3, .TdiFlags = 0x20};
            emu.memory.write_memory(memory + 0x300, &buffer, sizeof(buffer));
            emu.memory.write_memory(memory + 0x200, &input, sizeof(input));
            io_device_context request{emu.memory};
            request.file_handle = endpoint_handle;
            request.event = signal;
            request.apc_context = completion_context;
            request.io_status_block = {emu.memory, memory + 0x100};
            request.io_control_code = 0x12017;
            request.input_buffer = memory + 0x200;
            request.input_buffer_length = sizeof(input);
            EXPECT_EQ(endpoint->capture_completion_mode(emu, request),
                      qualified ? device_ioctl_completion_mode::afd_stream_callback : device_ioctl_completion_mode::default_completion);
            return endpoint->execute_ioctl(emu, request);
        };
        for (const bool initially_ready : {false, true})
        {
            for (const auto signal : signals)
            {
                for (const uint64_t completion_context : {0ull, 0x5678ull})
                {
                    SCOPED_TRACE(testing::Message() << "ready=" << initially_ready << " event=" << signal.bits
                                                    << " raw_low_bit=" << (signal.bits & 1u) << " context=" << completion_context);
                    port->queue.clear();
                    socket->incoming.clear();
                    emu.process.events.get(base_event)->signaled = false;
                    emu.process.events.get(decoy_event)->signaled = false;
                    emu.memory.set_memory(memory + 0x400, 0xcc, 16);
                    const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{.Pointer = 0xaabbccdd4a4b4c4dull,
                                                                         .Information = 0x1122334455667788ull};
                    emu.memory.write_memory(memory + 0x100, &initial, sizeof(initial));
                    const auto original = emu.memory.read_memory(memory + 0x100, sizeof(initial));
                    const auto feed = [&] {
                        for (const char byte : std::string_view{"abc"})
                        {
                            socket->incoming.push_back(static_cast<std::byte>(byte));
                        }
                    };
                    if (initially_ready)
                    {
                        feed();
                    }
                    const auto result = GetParam() ? submit.template operator()<Emu32>(signal, completion_context)
                                                   : submit.template operator()<Emu64>(signal, completion_context);
                    ASSERT_EQ(result, initially_ready ? STATUS_SUCCESS : STATUS_PENDING);
                    if (!initially_ready)
                    {
                        EXPECT_EQ(emu.memory.read_memory(memory + 0x100, sizeof(initial)), original);
                        EXPECT_FALSE(emu.process.events.get(base_event)->signaled);
                        EXPECT_FALSE(emu.process.events.get(tagged_event)->signaled);
                        EXPECT_FALSE(emu.process.events.get(decoy_event)->signaled);
                        EXPECT_TRUE(port->queue.empty());
                        const auto flags_offset = GetParam() ? 8u : 12u;
                        emulator_object<uint32_t>{emu.memory, memory + 0x200 + flags_offset}.write(0u);
                        feed();
                        endpoint->work(emu);
                    }
                    EXPECT_EQ(status(false).Status, STATUS_SUCCESS);
                    EXPECT_EQ(status(false).Information, 3u);
                    EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x104), qualified ? 0xaabbccddu : 0u);
                    EXPECT_EQ((emu.memory.read_memory<std::array<char, 3>>(memory + 0x400)), (std::array<char, 3>{'a', 'b', 'c'}));
                    EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x403), 0xccu);
                    EXPECT_TRUE(emu.process.events.get(base_event)->signaled);
                    EXPECT_TRUE(emu.process.events.get(tagged_event)->signaled);
                    EXPECT_FALSE(emu.process.events.get(decoy_event)->signaled);
                    if (completion_context == 0)
                    {
                        EXPECT_TRUE(port->queue.empty());
                    }
                    else
                    {
                        ASSERT_EQ(port->queue.size(), 1u);
                        EXPECT_EQ(port->queue.front().key_context, 0x1234u);
                        EXPECT_EQ(port->queue.front().apc_context, completion_context);
                        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_SUCCESS);
                        EXPECT_EQ(port->queue.front().io_status_block.Information, 3u);
                    }
                    const auto completed_count = port->queue.size();
                    endpoint->work(emu);
                    EXPECT_EQ(port->queue.size(), completed_count);
                }
            }
        }
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

        ASSERT_EQ(transfer(true, 0, endpoint, endpoint_handle, 0, 0, 0xfeed), STATUS_PENDING);
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
        EXPECT_EQ(port->queue.front().apc_context, 0xfeedu);
        EXPECT_EQ(port->queue.front().io_status_block.Status, STATUS_CANCELLED);
        EXPECT_EQ(port->queue.front().io_status_block.Information, 0u);
    }

#ifdef _WIN32
    TEST(AfdTransportSortHostTest, WrapperUsesNativeWindowsAddressPolicy)
    {
        network::socket_factory initialize_winsock;
        network::socket_wrapper wrapper{AF_INET6, SOCK_STREAM, IPPROTO_TCP};
        std::vector<network::address_sort_entry> addresses(2);
        addresses[0].address.sin6_family = AF_INET6;
        addresses[0].source_index = 0;
        auto* mapped = reinterpret_cast<uint8_t*>(&addresses[0].address.sin6_addr);
        mapped[10] = 0xff;
        mapped[11] = 0xff;
        mapped[12] = 127;
        mapped[15] = 1;
        addresses[1].address.sin6_family = AF_INET6;
        addresses[1].source_index = 1;
        auto* loopback = reinterpret_cast<uint8_t*>(&addresses[1].address.sin6_addr);
        loopback[15] = 1;

        ASSERT_EQ(wrapper.sort_address_list(addresses), STATUS_SUCCESS);
        ASSERT_EQ(addresses.size(), 2u);
        EXPECT_EQ(addresses[0].source_index, 1u);
        EXPECT_EQ(addresses[1].source_index, 0u);
    }

    TEST(AfdPartialDisconnectHostTest, WrapperForwardsModesAndTimeoutToNativeAfd)
    {
        network::socket_factory initialize_winsock;
        for (const auto [mode, timeout] : {std::pair<uint32_t, int64_t>{0, 0}, {2, -10000}, {8, 1}, {1, -10000000}, {4, 0}})
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
        for (const auto [mode, expected_error] : {std::pair<uint32_t, int>{1, WSAESHUTDOWN}, {4, WSAECONNABORTED}, {8, WSAENOTCONN}})
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
        for (const auto [mode, expected_error] :
             {std::pair<uint32_t, int>{1, SERR(ESHUTDOWN)}, {4, SERR(ECONNABORTED)}, {8, SERR(ENOTCONN)}})
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
