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

    INSTANTIATE_TEST_SUITE_P(PortHeaders, CoreMessagingRegistrarTest, testing::Bool());
}
