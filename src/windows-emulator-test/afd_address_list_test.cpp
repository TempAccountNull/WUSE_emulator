#include "emulation_test_utils.hpp"
#include <devices/afd_endpoint.hpp>
#include <devices/afd_types.hpp>

#ifdef _WIN32
namespace sogen::test
{
    class AfdAddressListTest : public testing::TestWithParam<bool>
    {
      protected:
        windows_emulator emu{create_empty_emulator({.socket_factory = std::make_unique<network::socket_factory>()})};
        std::unique_ptr<io_device> device;
        static constexpr uint64_t memory = 0x200000;

        void SetUp() override
        {
            ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x4000, memory_permission::read_write));
            const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, AF_INET, SOCK_DGRAM, IPPROTO_UDP, 0};
            emu.memory.write_memory(memory, creation.data(), sizeof(creation));
            device = create_afd_endpoint({.is_32_bit = GetParam()});
            device->create(emu, {.buffer = memory, .length = sizeof(creation)});
        }

        io_device_context context(const uint16_t family = AF_INET, const ULONG output_size = 8192)
        {
            emu.memory.write_memory(memory + 0x100, &family, sizeof(family));
            io_device_context c{emu.memory};
            c.io_status_block = {emu.memory, memory + 0x80};
            c.io_control_code = 0x120b3;
            c.input_buffer = memory + 0x100;
            c.input_buffer_length = 2;
            c.output_buffer = memory + 0x200;
            c.output_buffer_length = output_size;
            return c;
        }

        uint64_t information() const
        {
            return emu.memory.read_memory<uint64_t>(memory + 0x88);
        }
    };

    TEST_P(AfdAddressListTest, MatchesWinsockAddressesAndPreservesUnusedOutput)
    {
        for (const int family : {AF_INET, AF_INET6})
        {
            network::socket reference{family, SOCK_DGRAM, IPPROTO_UDP};
            alignas(SOCKET_ADDRESS_LIST) std::array<std::byte, 8192> native{};
            DWORD returned{};
            ASSERT_EQ(WSAIoctl(reference.get_socket(), SIO_ADDRESS_LIST_QUERY, nullptr, 0, native.data(), static_cast<DWORD>(native.size()),
                               &returned, nullptr, nullptr),
                      0);
            const auto* list = reinterpret_cast<const SOCKET_ADDRESS_LIST*>(native.data());
            std::vector<std::vector<std::byte>> expected;
            for (int index = 0; index < list->iAddressCount; ++index)
            {
                const auto& entry = list->Address[index];
                const auto* bytes = reinterpret_cast<const std::byte*>(entry.lpSockaddr);
                expected.emplace_back(bytes, bytes + entry.iSockaddrLength);
            }
            auto c = context(static_cast<uint16_t>(family));
            emu.memory.set_memory(c.output_buffer, 0xa5, c.output_buffer_length);
            ASSERT_EQ(device->execute_ioctl(emu, c), STATUS_SUCCESS);
            const auto data = emu.memory.read_memory(c.output_buffer, information());
            uint32_t count{};
            memcpy(&count, data.data(), sizeof(count));
            ASSERT_EQ(count, expected.size());
            size_t cursor = 4;
            std::vector<std::vector<std::byte>> actual;
            for (uint32_t index = 0; index < count; ++index)
            {
                ASSERT_GE(data.size() - cursor, 4u);
                uint16_t length{};
                memcpy(&length, data.data() + cursor, sizeof(length));
                ASSERT_LE(cursor + 4u + length, data.size());
                actual.emplace_back(data.data() + cursor + 2, data.data() + cursor + length + 4);
                cursor += length + 4u;
            }
            EXPECT_EQ(cursor, data.size());
            std::ranges::sort(actual);
            std::ranges::sort(expected);
            EXPECT_EQ(actual, expected);
            EXPECT_EQ(emu.memory.read_memory<uint64_t>(c.output_buffer + information()), 0xa5a5a5a5a5a5a5a5ull);
        }
    }

    TEST_P(AfdAddressListTest, ShortBuffersReportFullSizeAndCopyOnlyCompleteRecords)
    {
        for (const uint16_t family : {uint16_t{AF_INET}, uint16_t{AF_INET6}})
        {
            auto c = context(family);
            ASSERT_EQ(device->execute_ioctl(emu, c), STATUS_SUCCESS);
            const auto required = information();
            const auto full = emu.memory.read_memory(c.output_buffer, required);
            std::vector<size_t> boundaries{4};
            for (size_t cursor = 4; cursor < full.size();)
            {
                uint16_t length{};
                memcpy(&length, full.data() + cursor, sizeof(length));
                cursor += length + 4u;
                boundaries.push_back(cursor);
            }
            for (ULONG length = 0; length <= required + 1; ++length)
            {
                c.output_buffer_length = length;
                emu.memory.set_memory(c.output_buffer, 0xa5, required + 16);
                const auto result = device->execute_ioctl(emu, c);
                if (length < 4)
                {
                    EXPECT_EQ(result, STATUS_INVALID_PARAMETER);
                    EXPECT_EQ(emu.memory.read_memory<uint64_t>(c.output_buffer), 0xa5a5a5a5a5a5a5a5ull);
                    continue;
                }
                EXPECT_EQ(result, length < required ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS);
                EXPECT_EQ(information(), required);
                size_t copied = 4;
                for (const auto boundary : boundaries)
                {
                    if (boundary <= length)
                    {
                        copied = boundary;
                    }
                }
                EXPECT_EQ(emu.memory.read_memory(c.output_buffer, copied), (std::vector<std::byte>{full.begin(), full.begin() + copied}));
                EXPECT_EQ(emu.memory.read_memory<uint64_t>(c.output_buffer + copied), 0xa5a5a5a5a5a5a5a5ull);
            }
        }
    }

    TEST_P(AfdAddressListTest, UnknownFamilyReturnsEmptyNativeList)
    {
        auto c = context(0, 4);
        ASSERT_EQ(device->execute_ioctl(emu, c), STATUS_SUCCESS);
        EXPECT_EQ(information(), 4u);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(c.output_buffer), 0u);
    }

    TEST_P(AfdAddressListTest, RejectsShortInputAndInvalidGuestPointers)
    {
        auto c = context();
        for (const ULONG length : {0u, 1u})
        {
            c.input_buffer_length = length;
            EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_INVALID_PARAMETER);
        }
        c.input_buffer_length = 2;
        c.input_buffer = 0x7fffffff0000;
        EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_ACCESS_VIOLATION);
        c = context();
        c.output_buffer = 0x7fffffff0000;
        EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_ACCESS_VIOLATION);
    }

    INSTANTIATE_TEST_SUITE_P(GuestBitness, AfdAddressListTest, testing::Bool());
}
#endif
