#include "emulation_test_utils.hpp"
#include <port.hpp>
#include <registry/registry_utils.hpp>

namespace sogen::test
{
    class SspiServiceTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        std::unique_ptr<port> service;
        uint64_t buffer{};

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            buffer = emu.memory.allocate_memory(0x2000, memory_permission::read_write);
            service = create_port(u"\\RPC Control\\lsasspirpc");
            ASSERT_NE(service, nullptr);
            std::array<uint8_t, 64> bind{};
            bind[0] = 1;
            bind[32] = 3;
            constexpr std::array<uint8_t, 16> interface = {0xc8, 0xad, 0x32, 0x4f, 0x52, 0x60, 0x04, 0x4a,
                                                           0x87, 0x01, 0x29, 0x3c, 0xcf, 0x20, 0x96, 0xf0};
            std::ranges::copy(interface, bind.begin() + 12);
            emu.memory.write_memory(buffer, bind.data(), bind.size());
            const auto result = service->handle_request(
                emu, {.send_buffer = buffer, .send_buffer_length = 64, .recv_buffer = buffer + 0x1000, .recv_buffer_length = 0x1000});
            ASSERT_EQ(result.status, STATUS_SUCCESS);
        }

        lpc_request_result call(const uint32_t opnum, const std::span<const uint8_t> body)
        {
            std::vector<uint8_t> request(64 + body.size(), 0);
            std::memcpy(request.data() + 20, &opnum, sizeof(opnum));
            std::ranges::copy(body, request.begin() + 64);
            emu.memory.write_memory(buffer, request.data(), request.size());
            return service->handle_request(emu, {.send_buffer = buffer,
                                                 .send_buffer_length = static_cast<ULONG>(request.size()),
                                                 .recv_buffer = buffer + 0x1000,
                                                 .recv_buffer_length = 0x1000});
        }

        std::vector<uint8_t> connect()
        {
            std::array<uint8_t, 12> request{};
            request[8] = 2;
            auto result = call(0, request);
            EXPECT_EQ(result.status, STATUS_SUCCESS);
            if (!result.payload || result.payload->size() < 56)
            {
                ADD_FAILURE() << "Missing SSPI connection reply";
                return {};
            }
            return {result.payload->begin() + 32, result.payload->begin() + 52};
        }
    };

    TEST_F(SspiServiceTest, DisconnectInvalidatesOnlyItsContext)
    {
        const auto first = connect();
        const auto second = connect();
        ASSERT_EQ(first.size(), 20);
        ASSERT_EQ(second.size(), 20);
        EXPECT_NE(first, second);
        EXPECT_EQ(call(1, first).status, STATUS_SUCCESS);
        EXPECT_EQ(call(1, first).status, STATUS_INVALID_HANDLE);
        EXPECT_EQ(call(1, second).status, STATUS_SUCCESS);
    }

    TEST_F(SspiServiceTest, RestoredConnectionReturnsGuestSamName)
    {
        const auto context = connect();
        ASSERT_EQ(context.size(), 20);
        utils::buffer_serializer serialized;
        service->serialize_object(serialized);
        service = create_port(u"\\RPC Control\\lsasspirpc");
        utils::buffer_deserializer restore{serialized};
        service->deserialize_object(restore);
        std::array<uint8_t, 44> request{};
        std::ranges::copy(context, request.begin());
        request[24] = process_context::process_id;
        request[32] = 8;
        request[40] = 2;
        const auto result = call(14, request);
        ASSERT_EQ(result.status, STATUS_SUCCESS);
        ASSERT_TRUE(result.payload.has_value());
        const auto name = registry_utils::get_account_domain(emu.registry) + u"\\" + registry_utils::get_user_name(emu.registry);
        ASSERT_GE(result.payload->size(), 64 + name.size() * sizeof(char16_t) + 8);
        EXPECT_EQ(std::memcmp(result.payload->data() + 64, name.data(), name.size() * sizeof(char16_t)), 0);
        request[40] = 0;
        EXPECT_EQ(call(14, request).status, STATUS_NOT_SUPPORTED);
    }

    TEST_F(SspiServiceTest, RejectsMalformedAndUnsupportedRequests)
    {
        EXPECT_EQ(call(0, {}).status, STATUS_INVALID_PARAMETER);
        EXPECT_EQ(call(1, {}).status, STATUS_INVALID_HANDLE);
        EXPECT_EQ(call(14, {}).status, STATUS_INVALID_PARAMETER);
        EXPECT_EQ(call(99, {}).status, STATUS_NOT_SUPPORTED);
    }
}
