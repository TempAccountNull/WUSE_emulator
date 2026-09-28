#include "emulation_test_utils.hpp"
#include <devices/afd_endpoint.hpp>
#include <devices/afd_types.hpp>
#include <network/socket_factory.hpp>
#include <network/socket_wrapper.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace sogen::test
{
    namespace
    {
        struct remote_socket_state
        {
            bool listening{};
            bool connected{};
            std::optional<network::address> accepted_remote{};
        };

        struct remote_test_socket final : network::i_socket
        {
            explicit remote_test_socket(std::shared_ptr<remote_socket_state> value) : state(std::move(value)) {}

            void set_blocking(bool) override {}
            int get_last_error() override { return SERR(EWOULDBLOCK); }
            uint32_t partial_disconnect(uint32_t, int64_t) override { return 0; }
            bool is_ready(bool) override { return true; }
            bool is_listening() override { return state->listening; }
            bool is_connected() override { return state->connected; }
            std::optional<network::address> get_local_address() override { return {}; }
            bool bind(const network::address&) override { return true; }
            bool connect(const network::address&) override
            {
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
                if (!state->accepted_remote) return {};
                remote = *state->accepted_remote;
                state->accepted_remote.reset();
                auto accepted = std::make_shared<remote_socket_state>();
                accepted->connected = true;
                return std::make_unique<remote_test_socket>(std::move(accepted));
            }
            sent_size send(std::span<const std::byte>) override { return -1; }
            sent_size sendto(const network::address&, std::span<const std::byte>) override { return -1; }
            sent_size recv(std::span<std::byte>) override { return -1; }
            sent_size recvfrom(network::address&, std::span<std::byte>) override { return -1; }

            std::shared_ptr<remote_socket_state> state;
        };

        struct remote_test_factory final : network::socket_factory
        {
            std::vector<std::shared_ptr<remote_socket_state>> created;

            std::unique_ptr<network::i_socket> create_socket(int, int, int) override
            {
                auto state = std::make_shared<remote_socket_state>();
                created.push_back(state);
                return std::make_unique<remote_test_socket>(std::move(state));
            }
            int poll_sockets(std::span<network::poll_entry>) override { return 0; }
        };
    }

    class AfdRemoteAddressTest : public testing::TestWithParam<bool>
    {
      protected:
        static constexpr uint64_t memory = 0x260000;
        static constexpr uint64_t iosb = memory + 0x100;
        static constexpr uint64_t input = memory + 0x400;
        static constexpr uint64_t output = memory + 0x800;
        remote_test_factory* test_factory{};
        windows_emulator emu{[this] {
            emulator_settings settings{};
            settings.load_registry = false;
            emulator_interfaces interfaces{};
            auto factory = std::make_unique<remote_test_factory>();
            test_factory = factory.get();
            interfaces.socket_factory = std::move(factory);
            return create_emulator(std::move(settings), {}, std::move(interfaces));
        }()};
        std::unique_ptr<io_device> device;

        void SetUp() override
        {
            ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x2000, memory_permission::read_write));
            device = create_endpoint(2);
        }

        std::unique_ptr<io_device> create_endpoint(uint32_t family)
        {
            const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, family, 1, 6, 0};
            emu.memory.write_memory(memory, creation.data(), sizeof(creation));
            auto endpoint = create_afd_endpoint({.is_32_bit = GetParam()});
            endpoint->create(emu, {.buffer = memory, .length = sizeof(creation)});
            return endpoint;
        }

        NTSTATUS invoke(io_device& endpoint, uint32_t code, uint64_t in, uint32_t in_length,
                        uint64_t out, uint32_t out_length)
        {
            io_device_context request{emu.memory};
            request.io_control_code = code;
            request.io_status_block = {emu.memory, iosb};
            request.input_buffer = in;
            request.input_buffer_length = in_length;
            request.output_buffer = out;
            request.output_buffer_length = out_length;
            return endpoint.execute_ioctl(emu, request);
        }

        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> status() const
        {
            // Endpoint bitness changes AFD request layout, not IOSB storage.
            return emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(iosb);
        }

        std::vector<std::byte> standard_context(uint32_t family) const
        {
            const auto address_size = family == 23 ? 28u : 16u;
            const auto size = (family == 23 ? 196u : 164u) + (GetParam() ? 0u : 4u);
            std::vector<std::byte> bytes(size);
            const auto put = [&](size_t offset, uint32_t value) { memcpy(bytes.data() + offset, &value, sizeof(value)); };
            put(4, family);
            put(8, 1);
            put(12, 6);
            put(16, address_size);
            put(20, address_size);
            return bytes;
        }

        void set_context(io_device& endpoint, const std::vector<std::byte>& bytes,
                         uint64_t alias = 0, uint32_t alias_length = 0)
        {
            if (!bytes.empty()) emu.memory.write_memory(input, bytes.data(), bytes.size());
            ASSERT_EQ(invoke(endpoint, 0x12047, bytes.empty() ? 0 : input,
                             static_cast<uint32_t>(bytes.size()), alias, alias_length), STATUS_SUCCESS);
        }

        std::vector<std::byte> remote_sockaddr(uint32_t family) const
        {
            std::vector<std::byte> result(family == 23 ? 28u : 16u);
            const auto family_value = static_cast<uint16_t>(family);
            memcpy(result.data(), &family_value, sizeof(family_value));
            result[2] = std::byte{0x1f};
            result[3] = std::byte{0x90};
            if (family == 2)
            {
                result[4] = std::byte{127};
                result[7] = std::byte{1};
            }
            else result[23] = std::byte{1};
            return result;
        }

        template <typename Guest>
        NTSTATUS connect(io_device& endpoint, const std::vector<std::byte>& remote)
        {
            using traits = EmulatorTraits<Guest>;
            constexpr auto offset = offsetof(AFD_CONNECT_JOIN_INFO_TL<traits>, RemoteAddress);
            std::vector<std::byte> request(offset + remote.size());
            memcpy(request.data() + offset, remote.data(), remote.size());
            emu.memory.write_memory(input, request.data(), request.size());
            return invoke(endpoint, 0x12004, input, static_cast<uint32_t>(request.size()), 0, 0);
        }

        std::unique_ptr<io_device> snapshot(io_device& endpoint)
        {
            utils::buffer_serializer saved{};
            endpoint.serialize(saved);
            utils::buffer_deserializer restored_buffer{saved};
            restored_buffer.register_factory<memory_manager_wrapper>([this] { return memory_manager_wrapper{emu.memory}; });
            restored_buffer.register_factory<x64_emulator_wrapper>([this] { return x64_emulator_wrapper{emu.emu()}; });
            restored_buffer.register_factory<socket_factory_wrapper>([this] { return socket_factory_wrapper{emu.socket_factory()}; });
            // An accepted target is stored as an io_device_container; its
            // serialized wrapper must be restored before the inner endpoint.
            std::unique_ptr<io_device> restored = dynamic_cast<io_device_container*>(&endpoint)
                                                      ? std::make_unique<io_device_container>()
                                                      : create_afd_endpoint({.is_32_bit = GetParam()});
            restored->deserialize(restored_buffer);
            return restored;
        }
    };

    TEST_P(AfdRemoteAddressTest, OutgoingIpv4Ipv6NativeStatusBytesContextAndSnapshot)
    {
        for (const uint32_t family : {2u, 23u})
        {
            device = create_endpoint(family);
            const auto remote = remote_sockaddr(family);
            const auto context = standard_context(family);
            const auto context_size = context.size();
            const auto address_offset = family == 23 ? 160u : 144u;
            emu.memory.set_memory(output, 0xa5, 64);
            EXPECT_EQ(invoke(*device, 0x1203f, 0, 0, output, 64), STATUS_INVALID_CONNECTION);
            EXPECT_EQ(status().Information, 0u);
            set_context(*device, context);
            EXPECT_EQ(invoke(*device, 0x1203f, 0, 0, output, 64), STATUS_INVALID_CONNECTION);
            EXPECT_EQ(status().Information, 0u);
            ASSERT_EQ(GetParam() ? connect<Emu32>(*device, remote) : connect<Emu64>(*device, remote), STATUS_SUCCESS);

            for (uint32_t capacity : {0u, static_cast<uint32_t>(remote.size() - 1)})
            {
                emu.memory.set_memory(output, 0xa5, 64);
                EXPECT_EQ(invoke(*device, 0x1203f, 0, 0, capacity ? output : 0, capacity), STATUS_BUFFER_TOO_SMALL);
                EXPECT_EQ(status().Status, STATUS_BUFFER_TOO_SMALL);
                EXPECT_EQ(status().Information, 0u);
                const auto bytes = emu.memory.read_memory(output, 64);
                EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](auto byte) { return byte == std::byte{0xa5}; }));
            }
            emu.memory.set_memory(output, 0xa5, 64);
            ASSERT_EQ(invoke(*device, 0x1203f, 0, 0, output, 64), STATUS_SUCCESS);
            EXPECT_EQ(status().Status, STATUS_SUCCESS);
            EXPECT_EQ(status().Information, context_size);
            const auto result = emu.memory.read_memory(output, 64);
            EXPECT_TRUE(std::equal(remote.begin(), remote.end(), result.begin()));
            EXPECT_EQ(result[remote.size()], std::byte{0xa5});

            auto modified = context;
            std::fill_n(modified.begin() + address_offset, remote.size(), std::byte{0xee});
            set_context(*device, modified);
            emu.memory.set_memory(output, 0xa5, 64);
            ASSERT_EQ(invoke(*device, 0x1203f, 0, 0, output, 64), STATUS_SUCCESS);
            auto changed = emu.memory.read_memory(output, remote.size());
            EXPECT_TRUE(std::all_of(changed.begin(), changed.end(), [](auto byte) { return byte == std::byte{0xee}; }));
            auto restored = snapshot(*device);
            ASSERT_EQ(invoke(*restored, 0x1203f, 0, 0, output, 64), STATUS_SUCCESS);
            changed = emu.memory.read_memory(output, remote.size());
            EXPECT_TRUE(std::all_of(changed.begin(), changed.end(), [](auto byte) { return byte == std::byte{0xee}; }));

            set_context(*device, std::vector<std::byte>(4));
            emu.memory.set_memory(output, 0xa5, 64);
            EXPECT_EQ(invoke(*device, 0x1203f, 0, 0, output, 64), STATUS_INVALID_CONNECTION);
            EXPECT_EQ(status().Information, 0u);
        }
    }

    TEST_P(AfdRemoteAddressTest, AcceptedSocketCopiesNoRemoteAndReportsContextLength)
    {
        emu.process.is_wow64_process = GetParam();
        const auto context = standard_context(2);
        auto listener = create_endpoint(2);
        auto listener_state = test_factory->created.back();
        listener_state->listening = true;
        network::address remote{};
        remote.set_ipv4(0x7f000001);
        remote.set_port(3456);
        listener_state->accepted_remote = remote;

        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory, creation.data(), sizeof(creation));
        const auto target_handle = emu.process.devices.store(
            io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory, .length = sizeof(creation)}});
        auto* target = emu.process.devices.get(target_handle);
        ASSERT_NE(target, nullptr);
        set_context(*target, context);
        ASSERT_EQ(invoke(*listener, 0x1200c, 0, 0, output, 20), STATUS_SUCCESS);
        const auto sequence = emu.memory.read_memory<LONG>(output);
        const AFD_ACCEPT_INFO accept{.Sequence = sequence, .AcceptHandle = target_handle};
        emu.memory.write_memory(input, &accept, sizeof(accept));
        ASSERT_EQ(invoke(*listener, 0x12010, input, sizeof(accept), 0, 0), STATUS_SUCCESS);

        for (uint32_t capacity : {0u, 15u, 64u})
        {
            emu.memory.set_memory(output, 0xa5, 64);
            EXPECT_EQ(invoke(*target, 0x1203f, 0, 0, capacity ? output : 0, capacity), STATUS_SUCCESS);
            EXPECT_EQ(status().Information, context.size());
            const auto bytes = emu.memory.read_memory(output, 64);
            EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](auto byte) { return byte == std::byte{0xa5}; }));
        }

        auto restored = snapshot(*target);
        emu.memory.set_memory(output, 0xa5, 64);
        EXPECT_EQ(invoke(*restored, 0x1203f, 0, 0, output, 64), STATUS_SUCCESS);
        EXPECT_EQ(status().Information, context.size());
        const auto saved_bytes = emu.memory.read_memory(output, 64);
        EXPECT_TRUE(std::all_of(saved_bytes.begin(), saved_bytes.end(), [](auto byte) { return byte == std::byte{0xa5}; }));

        set_context(*target, {});
        EXPECT_EQ(invoke(*target, 0x1203f, 0, 0, 0, 0), STATUS_SUCCESS);
        EXPECT_EQ(status().Information, 0u);
    }

    TEST_P(AfdRemoteAddressTest, ModernMswsockIgnoresAliasAndUnknownProviderIsRejected)
    {
        const auto remote = remote_sockaddr(2);
        std::vector<std::byte> provider_blob(64);
        // Even an accepted SET_CONTEXT alias does not establish the modern
        // mswsock remote slot for an unrecognized provider blob.
        set_context(*device, provider_blob, input + 24, static_cast<uint32_t>(remote.size()));
        ASSERT_EQ(GetParam() ? connect<Emu32>(*device, remote) : connect<Emu64>(*device, remote), STATUS_SUCCESS);
        emu.memory.set_memory(output, 0xa5, 64);
        EXPECT_EQ(invoke(*device, 0x1203f, 0, 0, output, 64), STATUS_INVALID_CONNECTION);
        EXPECT_EQ(status().Information, 0u);

        device = create_endpoint(2);
        auto context = standard_context(2);
        set_context(*device, context, input + 64, static_cast<uint32_t>(remote.size()));
        ASSERT_EQ(GetParam() ? connect<Emu32>(*device, remote) : connect<Emu64>(*device, remote), STATUS_SUCCESS);
        emu.memory.set_memory(output, 0xa5, 64);
        ASSERT_EQ(invoke(*device, 0x1203f, 0, 0, output, 64), STATUS_SUCCESS);
        EXPECT_EQ(status().Information, context.size());
        const auto result = emu.memory.read_memory(output, remote.size());
        EXPECT_EQ(result, remote);

        std::vector<std::byte> after(context.size());
        emu.memory.set_memory(output, 0xa5, 256);
        ASSERT_EQ(invoke(*device, 0x12043, 0, 0, output, static_cast<uint32_t>(after.size())), STATUS_SUCCESS);
        after = emu.memory.read_memory(output, after.size());
        EXPECT_TRUE(std::equal(remote.begin(), remote.end(), after.begin() + 144));
        EXPECT_TRUE(std::all_of(after.begin() + 64, after.begin() + 64 + remote.size(),
                                [](auto byte) { return byte == std::byte{0}; }));
    }

    INSTANTIATE_TEST_SUITE_P(GuestBitness, AfdRemoteAddressTest, testing::Values(false, true));
} // namespace sogen::test
