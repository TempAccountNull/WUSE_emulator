#include "emulation_test_utils.hpp"
#include <devices/afd_endpoint.hpp>
#include <devices/afd_types.hpp>

namespace sogen::test
{
    class AfdBlockingModeTest : public testing::TestWithParam<bool>
    {
      protected:
        windows_emulator emu{create_empty_emulator()};
        std::unique_ptr<io_device> device;
        static constexpr uint64_t memory = 0x200000;
        handle completion{};

        void SetUp() override
        {
            ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x1000, memory_permission::read_write));
            const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 17, 0};
            emu.memory.write_memory(memory, creation.data(), sizeof(creation));
            device = create_afd_endpoint({.is_32_bit = GetParam()});
            device->create(emu, {.buffer = memory, .length = sizeof(creation)});
            event signal{};
            signal.type = SynchronizationEvent;
            completion = emu.process.events.store(std::move(signal));
        }

        io_device_context context(const ULONG code, const ULONG length)
        {
            io_device_context c{emu.memory};
            c.event = completion;
            c.io_status_block = {emu.memory, memory + 0x80};
            c.io_control_code = code;
            c.input_buffer = memory + 0x100;
            c.input_buffer_length = length;
            return c;
        }

        NTSTATUS set_mode(const uint8_t value, const ULONG length = 16, const ULONG information_class = 2)
        {
            const std::array<uint64_t, 2> request{information_class, 0xa5a5a5a5a5a5a500ull | value};
            emu.memory.write_memory(memory + 0x100, request.data(), sizeof(request));
            return device->execute_ioctl(emu, context(0x1203b, length));
        }

        NTSTATUS receive(const ULONG flags = 0)
        {
            ULONG length{};
            if (GetParam())
            {
                const EMU_WSABUF<EmulatorTraits<Emu32>> buffer{.len = 32, .buf = memory + 0x300};
                const AFD_RECV_DATAGRAM_INFO<EmulatorTraits<Emu32>> request{
                    .BufferArray = memory + 0x200, .BufferCount = 1, .AfdFlags = flags, .TdiFlags = 0x20};
                emu.memory.write_memory(memory + 0x200, &buffer, sizeof(buffer));
                emu.memory.write_memory(memory + 0x100, &request, sizeof(request));
                length = sizeof(request);
            }
            else
            {
                const EMU_WSABUF<EmulatorTraits<Emu64>> buffer{.len = 32, .buf = memory + 0x300};
                const AFD_RECV_DATAGRAM_INFO<EmulatorTraits<Emu64>> request{
                    .BufferArray = memory + 0x200, .BufferCount = 1, .AfdFlags = flags, .TdiFlags = 0x20};
                emu.memory.write_memory(memory + 0x200, &buffer, sizeof(buffer));
                emu.memory.write_memory(memory + 0x100, &request, sizeof(request));
                length = sizeof(request);
            }
            emu.memory.set_memory(memory + 0x300, 0xa5, 32);
            emu.process.events.get(completion)->signaled = false;
            return device->execute_ioctl(emu, context(0x1201b, length));
        }

        void send_packet()
        {
            auto sender = emu.socket_factory().create_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            const std::array<std::byte, 3> data{std::byte{1}, std::byte{2}, std::byte{3}};
            EXPECT_EQ(sender->sendto(network::address{"0.0.0.0", uint16_t{1}}, data), 3);
            device->work(emu);
        }
    };

    TEST_P(AfdBlockingModeTest, NonblockingReceiveCompletesWithoutWaiting)
    {
        ASSERT_EQ(set_mode(1), STATUS_SUCCESS);
        ASSERT_EQ(receive(), STATUS_DEVICE_NOT_READY);
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        emu.process.events.get(completion)->signaled = false;
        send_packet();
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x300), 0xa5);
        EXPECT_FALSE(emu.process.events.get(completion)->signaled);
    }

    TEST_P(AfdBlockingModeTest, BlockingModeCanBeRestoredAndReceiveCompletes)
    {
        ASSERT_EQ(set_mode(1), STATUS_SUCCESS);
        ASSERT_EQ(set_mode(0), STATUS_SUCCESS);
        ASSERT_EQ(receive(), STATUS_PENDING);
        send_packet();
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x300), 1);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(memory + 0x88), 3);
    }

    TEST_P(AfdBlockingModeTest, OverlappedReceiveStillPendsOnNonblockingSocket)
    {
        ASSERT_EQ(set_mode(1), STATUS_SUCCESS);
        ASSERT_EQ(receive(2), STATUS_PENDING);
        send_packet();
        EXPECT_TRUE(emu.process.events.get(completion)->signaled);
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x300), 1);
    }

    TEST_P(AfdBlockingModeTest, ImmediateReceiveDoesNotWaitOnBlockingSocket)
    {
        ASSERT_EQ(receive(4), STATUS_DEVICE_NOT_READY);
    }

    TEST_P(AfdBlockingModeTest, ShortRequestDoesNotChangeMode)
    {
        ASSERT_EQ(set_mode(1), STATUS_SUCCESS);
        EXPECT_EQ(set_mode(0, 15), STATUS_BUFFER_TOO_SMALL);
        EXPECT_EQ(receive(), STATUS_DEVICE_NOT_READY);
    }

    TEST_P(AfdBlockingModeTest, UnknownInformationClassDoesNotReportSuccess)
    {
        EXPECT_EQ(set_mode(1, 16, 0xffffffff), STATUS_INVALID_PARAMETER);
    }

    INSTANTIATE_TEST_SUITE_P(GuestBitness, AfdBlockingModeTest, testing::Bool());
#ifdef _WIN32
    TEST(AfdInformationTest, HostInformationMatchesNativeSocketDefaults)
    {
        network::socket reference{AF_INET, SOCK_DGRAM, IPPROTO_UDP};
        auto emu = create_empty_emulator({.socket_factory = std::make_unique<network::socket_factory>()});
        constexpr uint64_t address = 0x200000;
        ASSERT_TRUE(emu.memory.allocate_memory(address, 0x1000, memory_permission::read_write));
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 17, 0};
        emu.memory.write_memory(address, creation.data(), sizeof(creation));
        for (const bool is_32_bit : {false, true})
        {
            auto device = create_afd_endpoint({.is_32_bit = is_32_bit});
            device->create(emu, {.buffer = address, .length = sizeof(creation)});
            io_device_context c{emu.memory};
            c.io_status_block = {emu.memory, address + 0x80};
            c.input_buffer = address + 0x100;
            c.input_buffer_length = sizeof(AFD_INFO);
            for (const ULONG information_class : {6u, 7u})
            {
                AFD_INFO request{.InformationClass = information_class, .Reserved = 0, .Information = 131072};
                emu.memory.write_memory(c.input_buffer, &request, sizeof(request));
                c.io_control_code = 0x1203b;
                ASSERT_EQ(device->execute_ioctl(emu, c), STATUS_SUCCESS);
                request.Information = 0xa5a5a5a5a5a5a5a5ull;
                emu.memory.write_memory(c.input_buffer, &request, sizeof(request));
                c.io_control_code = 0x1207b;
                c.output_buffer = address + 0x200;
                c.output_buffer_length = sizeof(AFD_INFO);
                emu.memory.set_memory(c.output_buffer, 0xa5, 32);
                ASSERT_EQ(device->execute_ioctl(emu, c), STATUS_SUCCESS);
                const auto result = emu.memory.read_memory<AFD_INFO>(c.output_buffer);
                EXPECT_EQ(result.InformationClass, information_class);
                int expected{};
                socklen_t expected_length = sizeof(expected);
                ASSERT_EQ(getsockopt(reference.get_socket(), SOL_SOCKET, information_class == 6 ? SO_RCVBUF : SO_SNDBUF,
                                     reinterpret_cast<char*>(&expected), &expected_length),
                          0);
                EXPECT_EQ(result.Information, static_cast<uint64_t>(expected));
                EXPECT_EQ(emu.memory.read_memory<uint64_t>(c.output_buffer + 16), 0xa5a5a5a5a5a5a5a5ull);
                EXPECT_EQ(emu.memory.read_memory<uint64_t>(address + 0x88), 16);
                c.output_buffer_length = 15;
                EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_BUFFER_TOO_SMALL);
            }
        }
    }
#endif

}
