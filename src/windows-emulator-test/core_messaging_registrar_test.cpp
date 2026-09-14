#include "emulation_test_utils.hpp"
#include <port.hpp>

namespace sogen::test
{
    class CoreMessagingRegistrarTest : public testing::TestWithParam<bool>
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
    };

    TEST_P(CoreMessagingRegistrarTest, BootstrapMatchesRegisterThreadReceiveContract)
    {
        emu.setup_process_if_necessary();
        emu.version.set_windows_build_number(19045);
        const auto buffer = emu.memory.allocate_memory(0x2000, memory_permission::read_write);
        auto service = create_port(u"\\BaseNamedObjects\\CoreMessagingRegistrar");
        lpc_port_message header{};
        header.is_wow64 = GetParam();
        header.native.u1.s1.DataLength = 0x30;
        header.native.u1.s1.TotalLength = static_cast<CSHORT>(header.wire_size() + 0x30);
        header.native.MessageId = 17;
        const emulator_object<PORT_MESSAGE64> send{emu.emu(), buffer};
        header.write(send);
        constexpr std::array<uint32_t, 12> request{0, 0, 0, 0, 2, 7, 0x10000, 0, 8, 0, 2, 0x10001};
        emu.memory.write_memory(buffer + header.wire_size(), request.data(), sizeof(request));
        lpc_message_context context{emu.emu()};
        context.send_message = send;
        context.receive_message = emulator_object<PORT_MESSAGE64>{emu.emu(), buffer + 0x1000};
        context.receive_buffer_length = 0x1000;
        const auto result = service->handle_message(emu, context);
        ASSERT_EQ(result.status, STATUS_SUCCESS);
        EXPECT_EQ(result.message.is_wow64, GetParam());
        EXPECT_EQ(result.message.native.MessageId, 17);
        EXPECT_EQ(result.message.header_size(), header.wire_size());
        EXPECT_EQ(result.message.native.u2.s2.Type, LPC_REPLY);
        const auto& reply = result.payload;
        ASSERT_GE(reply.size(), 0x30);
        const auto read_u32 = [&](const size_t offset) {
            uint32_t value{};
            std::memcpy(&value, reply.data() + offset, sizeof(value));
            return value;
        };
        EXPECT_EQ(read_u32(0x14), 7);
        EXPECT_EQ(read_u32(0x20), reply.size() - 0x28);
        EXPECT_EQ(read_u32(0x2C), 0x40000);
        size_t offset = 0x30;
        // CoreMessaging's RegisterThread receiver declares result, GUID, partition ID, then group ID.
        constexpr std::array<uint32_t, 4> parameter_sizes{4, 16, 8, 4};
        for (const auto expected_size : parameter_sizes)
        {
            ASSERT_LE(offset + sizeof(uint32_t), reply.size());
            const auto size = read_u32(offset);
            ASSERT_EQ(size, expected_size);
            offset += sizeof(uint32_t);
            ASSERT_LE(offset + size, reply.size());
            offset += size;
        }
        EXPECT_EQ(offset, reply.size());
        EXPECT_EQ(read_u32(0x34), 0);
        EXPECT_NE(read_u32(offset - sizeof(uint32_t)), 0);
    }

    TEST_P(CoreMessagingRegistrarTest, Windows10ReplyMethodsMatchReceiverParameters)
    {
        emu.setup_process_if_necessary();
        emu.version.set_windows_build_number(19045);
        const auto buffer = emu.memory.allocate_memory(0x2000, memory_permission::read_write);
        auto service = create_port(u"\\BaseNamedObjects\\CoreMessagingRegistrar");

        struct contract
        {
            uint32_t request_method;
            uint32_t reply_method;
            std::vector<uint32_t> parameter_sizes;
        };

        const std::array contracts{
            contract{.request_method = 0xD0001, .reply_method = 0x100000, .parameter_sizes = {}},
            contract{.request_method = 0xE0001, .reply_method = 0x110000, .parameter_sizes = {}},
            contract{.request_method = 0xF0001, .reply_method = 0x120000, .parameter_sizes = {16}},
            contract{.request_method = 0x160001, .reply_method = 0x1B0000, .parameter_sizes = {4, 4, 8, 16, 56}},
            contract{.request_method = 0x170001, .reply_method = 0x1C0000, .parameter_sizes = {4}},
            contract{.request_method = 0xA0001, .reply_method = 0xD0000, .parameter_sizes = {4}},
            contract{.request_method = 0x30001, .reply_method = 0x60000, .parameter_sizes = {4, 0, 16, 56}},
        };
        for (const auto& item : contracts)
        {
            SCOPED_TRACE(item.request_method);
            std::array<uint32_t, 64> request{};
            request[4] = 2;
            request[5] = 31;
            request[6] = 0x10000;
            request[8] = sizeof(request) - 0x28;
            request[10] = request[8] / sizeof(uint32_t);
            request[11] = item.request_method;
            if (item.request_method == 0x160001)
            {
                constexpr auto name = std::to_array(u"System\\InputDelivery");
                request[12] = sizeof(name);
                std::memcpy(reinterpret_cast<uint8_t*>(request.data()) + 0x34, name.data(), sizeof(name));
            }
            lpc_port_message header{};
            header.is_wow64 = GetParam();
            header.native.u1.s1.DataLength = sizeof(request);
            header.native.u1.s1.TotalLength = static_cast<CSHORT>(header.wire_size() + sizeof(request));
            const emulator_object<PORT_MESSAGE64> send{emu.emu(), buffer};
            header.write(send);
            emu.memory.write_memory(buffer + header.wire_size(), request.data(), sizeof(request));
            lpc_message_context context{emu.emu()};
            context.send_message = send;
            context.receive_message = emulator_object<PORT_MESSAGE64>{emu.emu(), buffer + 0x1000};
            context.receive_buffer_length = 0x1000;
            const auto result = service->handle_message(emu, context);
            ASSERT_EQ(result.status, STATUS_SUCCESS);
            const auto& reply = result.payload;
            ASSERT_GE(reply.size(), 0x30);
            const auto read_u32 = [&](const size_t offset) {
                uint32_t value{};
                std::memcpy(&value, reply.data() + offset, sizeof(value));
                return value;
            };
            EXPECT_EQ(read_u32(0x14), 31);
            EXPECT_EQ(read_u32(0x20), reply.size() - 0x28);
            EXPECT_EQ(read_u32(0x28) * sizeof(uint32_t), reply.size() - 0x28);
            EXPECT_EQ(read_u32(0x2C), item.reply_method);
            size_t offset = 0x30;
            for (const auto expected_size : item.parameter_sizes)
            {
                ASSERT_LE(offset + sizeof(uint32_t), reply.size());
                const auto size = read_u32(offset);
                offset += sizeof(uint32_t);
                ASSERT_LE(offset + size, reply.size());
                if (expected_size)
                {
                    EXPECT_EQ(size, expected_size);
                }
                else
                {
                    ASSERT_GE(size, sizeof(char16_t));
                    EXPECT_EQ(size % sizeof(char16_t), 0);
                    EXPECT_EQ(reply[offset + size - 1], 0);
                    EXPECT_EQ(reply[offset + size - 2], 0);
                }
                offset += (size + 3) & ~3U;
            }
            EXPECT_EQ(offset, reply.size());
        }
    }

    TEST_P(CoreMessagingRegistrarTest, AdvertisesOnePartitionForClientAndInputService)
    {
        emu.setup_process_if_necessary();
        emu.version.set_windows_build_number(19045);
        const auto buffer = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        auto service = create_port(u"\\BaseNamedObjects\\CoreMessagingRegistrar");
        const auto call = [&](const uint32_t method, const uint32_t length) {
            std::vector<uint32_t> request(length / sizeof(uint32_t), 0);
            request[4] = 2;
            request[6] = 0x10000;
            request[8] = length - 0x28;
            request[10] = request[8] / sizeof(uint32_t);
            request[11] = method;
            if (method == 0x160001)
            {
                const std::u16string name = length >= 0xFC ? u"System\\InputSystemConversation_BAMO" : u"System\\InputDelivery";
                request[12] = static_cast<uint32_t>((name.size() + 1) * sizeof(char16_t));
                std::memcpy(reinterpret_cast<uint8_t*>(request.data()) + 0x34, name.c_str(), request[12]);
            }
            emu.memory.write_memory(buffer, request.data(), length);
            return service->handle_request(emu, {.send_buffer = buffer, .send_buffer_length = length});
        };
        const auto bootstrap = call(0x10001, 0x30);
        ASSERT_EQ(bootstrap.status, STATUS_SUCCESS);
        ASSERT_TRUE(bootstrap.payload.has_value());
        ASSERT_EQ(bootstrap.payload->size(), 0x60);
        uint64_t partition{};
        std::memcpy(&partition, bootstrap.payload->data() + 0x50, sizeof(partition));
        for (const auto length : {0x88U, 0xFCU})
        {
            const auto resolved = call(0x160001, length);
            ASSERT_EQ(resolved.status, STATUS_SUCCESS);
            ASSERT_TRUE(resolved.payload.has_value());
            ASSERT_GE(resolved.payload->size(), 0x7C);
            uint64_t server_partition{};
            std::memcpy(&server_partition, resolved.payload->data() + 0x74, sizeof(server_partition));
            EXPECT_EQ(server_partition, partition);
        }
        const auto prepared = call(0x30001, 0x6C);
        ASSERT_EQ(prepared.status, STATUS_SUCCESS);
        ASSERT_TRUE(prepared.payload.has_value());
        const auto& reply = *prepared.payload;
        size_t offset = 0x30;
        for (size_t parameter = 0; parameter < 4; ++parameter)
        {
            ASSERT_LE(offset + sizeof(uint32_t), reply.size());
            uint32_t length{};
            std::memcpy(&length, reply.data() + offset, sizeof(length));
            offset += sizeof(length);
            ASSERT_LE(offset + length, reply.size());
            if (parameter == 3)
            {
                ASSERT_EQ(length, 56);
                uint64_t reverse_partition{};
                std::memcpy(&reverse_partition, reply.data() + offset + 16, sizeof(reverse_partition));
                EXPECT_EQ(reverse_partition, partition);
            }
            offset += (length + 3) & ~3U;
        }
    }

    TEST_P(CoreMessagingRegistrarTest, ResolvesServiceNamesAndPreservesUniqueIdsAcrossRestore)
    {
        emu.setup_process_if_necessary();
        emu.version.set_windows_build_number(19045);
        const auto buffer = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        auto service = create_port(u"\\BaseNamedObjects\\CoreMessagingRegistrar");
        const auto call = [&](const std::u16string& name) {
            std::array<uint32_t, 64> request{};
            request[4] = 2;
            request[6] = 0x10000;
            request[8] = sizeof(request) - 0x28;
            request[10] = request[8] / sizeof(uint32_t);
            request[11] = 0x160001;
            request[12] = static_cast<uint32_t>((name.size() + 1) * sizeof(char16_t));
            std::memcpy(reinterpret_cast<uint8_t*>(request.data()) + 0x34, name.c_str(), request[12]);
            emu.memory.write_memory(buffer, request.data(), sizeof(request));
            return service->handle_request(emu, {.send_buffer = buffer, .send_buffer_length = sizeof(request)});
        };
        const auto first = call(u"System\\InputDelivery");
        const auto second = call(u"System\\InputSystemConversation_BAMO");
        const auto repeated = call(u"System\\InputDelivery");
        for (const auto* result : {&first, &second, &repeated})
        {
            ASSERT_EQ(result->status, STATUS_SUCCESS);
            ASSERT_TRUE(result->payload.has_value());
            ASSERT_EQ(result->payload->size(), 0x9C);
        }
        const auto id = [](const lpc_request_result& result) {
            uint64_t value{};
            std::memcpy(&value, result.payload->data() + 0x44, sizeof(value));
            return value;
        };
        EXPECT_NE(id(first), id(second));
        EXPECT_NE(id(first), id(repeated));
        EXPECT_NE(id(second), id(repeated));
        EXPECT_NE(std::memcmp(first.payload->data() + 0x7C, second.payload->data() + 0x7C, 16), 0);
        EXPECT_EQ(std::memcmp(first.payload->data() + 0x7C, repeated.payload->data() + 0x7C, 16), 0);
        EXPECT_EQ(call(u"System\\UnknownInputService").status, STATUS_NOT_SUPPORTED);
        emu.emu().write_memory<uint32_t>(buffer + 0x30, 0xFFFFFFFF);
        EXPECT_EQ(service->handle_request(emu, {.send_buffer = buffer, .send_buffer_length = 0x100}).status, STATUS_INVALID_PARAMETER);
        utils::buffer_serializer serialized;
        service->serialize_object(serialized);
        service = create_port(u"\\BaseNamedObjects\\CoreMessagingRegistrar");
        utils::buffer_deserializer restore{serialized};
        service->deserialize_object(restore);
        const auto restored = call(u"System\\InputDelivery");
        ASSERT_EQ(restored.status, STATUS_SUCCESS);
        ASSERT_TRUE(restored.payload.has_value());
        EXPECT_EQ(id(restored), id(repeated) + 1);
    }

    INSTANTIATE_TEST_SUITE_P(PortHeaders, CoreMessagingRegistrarTest, testing::Bool());
}
