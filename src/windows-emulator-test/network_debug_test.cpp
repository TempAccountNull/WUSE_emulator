#include "emulation_test_utils.hpp"
#include <network_debug.hpp>
#include <io_device.hpp>
#include <syscall_utils.hpp>
#include <devices/afd_types.hpp>
#include <devices/afd_endpoint.hpp>
#include <network/socket_factory.hpp>
#include <network/socket_wrapper.hpp>

#include <chrono>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>

namespace sogen::test
{
    TEST(NetworkDebug, TransportIoctlCapturesBoundedNestedInputForBothPointerWidths)
    {
        for (const bool wow64 : {false, true})
        {
            emulator_settings settings{};
            settings.load_registry = false;
            auto emu = create_emulator(std::move(settings));
            emu.process.is_wow64_process = wow64;
            constexpr uint64_t memory = 0x240000;
            ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x1000, memory_permission::read_write));
            std::array<std::byte, 80> nested{};
            nested.fill(std::byte{0x77});
            emu.memory.write_memory(memory + 0x300, nested.data(), nested.size());

            const auto path = std::filesystem::temp_directory_path() /
                              ("sogen-network-transport-" + std::to_string(wow64) + "-" +
                               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
            auto& vcpu = emu.vcpu(0);
            vcpu.cpu.reg(x86_register::rip, 0x18009d812);
            vcpu.cpu.reg(x86_register::rsp, memory + 0x100);
            io_device_context request{emu.memory};
            request.io_control_code = 0x120bf;
            request.issuer_thread_id = 41;
            request.input_buffer = memory + 0x200;
            const syscall_context issuer{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};

            {
                network_debug_logger logger;
                logger.open(path);
                ASSERT_TRUE(logger.enabled());
                auto exercise = [&](auto wrapper) {
                    wrapper.Type = 3;
                    wrapper.ControlCode = 0xc8000019;
                    wrapper.InputBuffer = static_cast<decltype(wrapper.InputBuffer)>(memory + 0x300);
                    wrapper.InputBufferLength = 80;
                    emu.memory.write_memory(memory + 0x200, &wrapper, sizeof(wrapper));
                    request.input_buffer_length = sizeof(wrapper);
                    logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");

                    wrapper.InputBuffer = static_cast<decltype(wrapper.InputBuffer)>(0xdead0000);
                    emu.memory.write_memory(memory + 0x200, &wrapper, sizeof(wrapper));
                    logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");

                    request.input_buffer_length = sizeof(wrapper) - 1;
                    logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");

                    request.input_buffer_length = sizeof(wrapper);
                    wrapper.InputBuffer = 0;
                    emu.memory.write_memory(memory + 0x200, &wrapper, sizeof(wrapper));
                    logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
                };
                if (wow64)
                {
                    exercise(AFD_WINSOCK_TRANSPORT_IOCTL<EmulatorTraits<Emu32>>{});
                }
                else
                {
                    exercise(AFD_WINSOCK_TRANSPORT_IOCTL<EmulatorTraits<Emu64>>{});
                }
            }

            std::ifstream input(path, std::ios::binary);
            ASSERT_TRUE(input.good());
            const std::string journal{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
            EXPECT_NE(journal.find("\"transport_type\":3"), std::string::npos);
            EXPECT_NE(journal.find("\"transport_control_code\":\"0xc8000019\""), std::string::npos);
            EXPECT_NE(journal.find("\"transport_input_length\":80"), std::string::npos);
            EXPECT_NE(journal.find("\"transport_input_preview_hex\":\"" + std::string(128, '7') + "\""), std::string::npos);
            EXPECT_EQ(journal.find("\"transport_input_preview_hex\":\"" + std::string(160, '7') + "\""), std::string::npos);
            EXPECT_NE(journal.find("\"transport_input_preview_truncated\":true"), std::string::npos);
            EXPECT_NE(journal.find("\"transport_input_preview_unavailable\":\"nested_input_unreadable\""), std::string::npos);
            EXPECT_NE(journal.find("\"transport_wrapper_unavailable\":\"input_too_short\""), std::string::npos);
            EXPECT_NE(journal.find("\"transport_input_preview_unavailable\":\"null_input_buffer\""), std::string::npos);
            EXPECT_EQ(std::count(journal.begin(), journal.end(), '\n'), 4);
            input.close();
            std::filesystem::remove(path);
        }
    }

    TEST(NetworkDebug, DisabledByDefaultAndEmitsBoundedCorrelatedRecordsWhenEnabled)
    {
        emulator_settings settings{};
        settings.load_registry = false;
        auto emu = create_emulator(std::move(settings));
        EXPECT_FALSE(emu.network_debug.enabled());
        constexpr uint64_t memory = 0x230000;
        ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x1000, memory_permission::read_write));

        const auto path = std::filesystem::temp_directory_path() /
                          ("sogen-network-debug-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
        auto& vcpu = emu.vcpu(0);
        vcpu.cpu.reg(x86_register::rip, 0x18009d812);
        vcpu.cpu.reg(x86_register::rsp, memory + 0x100);
        const uint64_t return_address = 0x140012345;
        emu.memory.write_memory(memory + 0x100, &return_address, sizeof(return_address));
        emu.memory.write_memory(memory + 0x500, "abc", 3);

        using Traits = EmulatorTraits<Emu64>;
        const EMU_WSABUF<Traits> wsabuf{.len = 3, .buf = memory + 0x500};
        const AFD_SEND_INFO<Traits> send{.BufferArray = memory + 0x300, .BufferCount = 1};
        emu.memory.write_memory(memory + 0x300, &wsabuf, sizeof(wsabuf));
        emu.memory.write_memory(memory + 0x200, &send, sizeof(send));

        io_device_context request{emu.memory};
        request.io_control_code = 0x1201f;
        request.issuer_thread_id = 41;
        request.io_status_block = {emu.memory, memory + 0x600};
        request.input_buffer = memory + 0x200;
        request.input_buffer_length = sizeof(send);
        const syscall_context issuer{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};

        uint64_t request_id{};
        {
            network_debug_logger logger;
            logger.open(path);
            ASSERT_TRUE(logger.enabled());
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            request_id = request.network_request_id;
            ASSERT_NE(request_id, 0u);
            logger.afd_result(emu, request, STATUS_PENDING);
            request.io_status_block.write({.Status = STATUS_SUCCESS, .Information = 3});
            logger.afd_completion(emu, request, STATUS_SUCCESS, true);
            logger.apc_queue(request_id, 41, STATUS_SUCCESS, 3);
            logger.apc_dispatch(request_id, 41);
            logger.iocp_queue(request_id, 7, 9, STATUS_SUCCESS, 3);
            io_completion_message message{};
            message.network_request_id = request_id;
            message.key_context = 9;
            message.io_status_block = {.Status = STATUS_SUCCESS, .Information = 3};
            logger.iocp_dequeue(message, 7);

            emu.memory.write_memory(memory + 0x200, "ABCD", 4);
            request.input_buffer_length = 4;
            for (const auto ioctl : {0x12037u, 0x1207bu, 0x120bfu})
            {
                request.io_control_code = ioctl;
                logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
                logger.afd_result(emu, request, STATUS_INVALID_PARAMETER);
            }
            std::array<std::byte, 80> long_control{};
            long_control.fill(std::byte{0xaa});
            emu.memory.write_memory(memory + 0x400, long_control.data(), long_control.size());
            request.input_buffer = memory + 0x400;
            request.input_buffer_length = static_cast<ULONG>(long_control.size());
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            logger.afd_result(emu, request, STATUS_INVALID_PARAMETER);

            request.input_buffer = 0xdead0000;
            request.input_buffer_length = 4;
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            logger.afd_result(emu, request, STATUS_INVALID_PARAMETER);
        }

        std::ifstream input(path, std::ios::binary);
        ASSERT_TRUE(input.good());
        const std::string journal{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        EXPECT_NE(journal.find("\"type\":\"network_io\""), std::string::npos);
        EXPECT_NE(journal.find("\"channel\":\"afd\",\"phase\":\"request\""), std::string::npos);
        EXPECT_NE(journal.find("\"channel\":\"afd\",\"phase\":\"completion\""), std::string::npos);
        EXPECT_NE(journal.find("\"channel\":\"apc\",\"phase\":\"queue\""), std::string::npos);
        EXPECT_NE(journal.find("\"channel\":\"iocp\",\"phase\":\"dequeue\""), std::string::npos);
        EXPECT_NE(journal.find("\"preview_hex\":\"616263\""), std::string::npos);
        EXPECT_NE(journal.find("\"payload_fnv1a64\":\"0xe71fa2190541574b\",\"payload_hashed_bytes\":3"), std::string::npos);
        EXPECT_NE(journal.find("\"operation\":\"query_handles\""), std::string::npos);
        EXPECT_NE(journal.find("\"operation\":\"get_information\""), std::string::npos);
        EXPECT_NE(journal.find("\"operation\":\"transport_ioctl\""), std::string::npos);
        EXPECT_NE(journal.find("\"control_input_preview_hex\":\"41424344\""), std::string::npos);
        EXPECT_NE(journal.find("\"control_input_preview_hex\":\"" + std::string(128, 'a') + "\""), std::string::npos);
        EXPECT_NE(journal.find("\"control_input_preview_truncated\":true"), std::string::npos);
        EXPECT_NE(journal.find("\"control_input_preview_unavailable\":\"input_buffer_unreadable\""), std::string::npos);
        EXPECT_NE(journal.find("\"syscall\":\"NtDeviceIoControlFile\""), std::string::npos);
        EXPECT_NE(journal.find("\"stack_kind\":\"raw_qwords_not_unwound\""), std::string::npos);
        EXPECT_NE(journal.find("\"request_id\":" + std::to_string(request_id)), std::string::npos);
        EXPECT_EQ(std::count(journal.begin(), journal.end(), '\n'), 17);

        utils::buffer_serializer saved{};
        request.serialize(saved);
        utils::buffer_deserializer deserializer{saved};
        deserializer.register_factory<x64_emulator_wrapper>([&] { return x64_emulator_wrapper{emu.emu()}; });
        io_device_context restored{deserializer};
        restored.deserialize(deserializer);
        EXPECT_EQ(restored.network_request_id, request.network_request_id);
        input.close();
        std::filesystem::remove(path);
    }

    TEST(NetworkDebug, CorrelatesEndpointAndCompletedRequestAndResponseBytes)
    {
        emulator_settings settings{};
        settings.load_registry = false;
        auto emu = create_emulator(std::move(settings));
        constexpr uint64_t memory = 0x250000;
        ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x2000, memory_permission::read_write));
        auto& vcpu = emu.vcpu(0);
        vcpu.cpu.reg(x86_register::rip, 0x18009d812);
        vcpu.cpu.reg(x86_register::rsp, memory + 0x100);
        const syscall_context issuer{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const auto path =
            std::filesystem::temp_directory_path() /
            ("sogen-network-endpoint-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");

        using Traits = EmulatorTraits<Emu64>;
        const std::array<std::byte, 16> remote{std::byte{2},   std::byte{0}, std::byte{0x22}, std::byte{0xb8},
                                               std::byte{127}, std::byte{0}, std::byte{0},    std::byte{1}};
        AFD_CONNECT_JOIN_INFO_TL<Traits> connect{};
        std::memcpy(&connect.RemoteAddress, remote.data(), remote.size());
        emu.memory.write_memory(memory + 0x200, &connect, sizeof(connect));

        io_device_context request{emu.memory};
        request.file_handle.bits = 0x44;
        request.issuer_thread_id = 41;
        request.io_status_block = {emu.memory, memory + 0x700};
        request.io_control_code = 0x12007;
        request.input_buffer = memory + 0x200;
        request.input_buffer_length = sizeof(connect);

        {
            network_debug_logger logger;
            logger.open(path);
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            request.io_status_block.write({.Status = STATUS_SUCCESS, .Information = 0});
            logger.afd_completion(emu, request, STATUS_SUCCESS, false);

            const EMU_WSABUF<Traits> descriptor{.len = 3, .buf = memory + 0x500};
            const AFD_SEND_INFO<Traits> send{.BufferArray = memory + 0x300, .BufferCount = 1};
            emu.memory.write_memory(memory + 0x300, &descriptor, sizeof(descriptor));
            emu.memory.write_memory(memory + 0x200, &send, sizeof(send));
            emu.memory.write_memory(memory + 0x500, "abc", 3);
            request.io_control_code = 0x1201f;
            request.input_buffer_length = sizeof(send);
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            request.io_status_block.write({.Status = STATUS_SUCCESS, .Information = 3});
            logger.afd_completion(emu, request, STATUS_SUCCESS, false);

            std::array<std::byte, 80> output{};
            output.fill(std::byte{0x55});
            emu.memory.write_memory(memory + 0x900, output.data(), output.size());
            request.io_control_code = 0x1207b;
            request.input_buffer_length = 0;
            request.output_buffer = memory + 0x900;
            request.output_buffer_length = static_cast<ULONG>(output.size());
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            request.io_status_block.write({.Status = STATUS_SUCCESS, .Information = output.size()});
            logger.afd_completion(emu, request, STATUS_SUCCESS, false);

            const AFD_RECV_DATAGRAM_INFO<Traits> receive{
                .BufferArray = memory + 0x300, .BufferCount = 1, .Address = memory + 0xa00, .AddressLength = memory + 0xb00};
            emu.memory.write_memory(memory + 0x200, &receive, sizeof(receive));
            request.io_control_code = 0x1201b;
            request.input_buffer_length = sizeof(receive);
            request.output_buffer_length = 0;
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            emu.memory.write_memory(memory + 0xa00, remote.data(), remote.size());
            const ULONG address_length = static_cast<ULONG>(remote.size());
            emu.memory.write_memory(memory + 0xb00, &address_length, sizeof(address_length));
            emu.memory.write_memory(memory + 0x500, "xyz", 3);
            request.io_status_block.write({.Status = STATUS_SUCCESS, .Information = 3});
            logger.afd_completion(emu, request, STATUS_SUCCESS, true);
        }

        std::ifstream input(path, std::ios::binary);
        ASSERT_TRUE(input.good());
        const std::string journal{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        EXPECT_NE(journal.find("\"role\":\"remote\",\"source\":\"guest_ioctl_input_at_request\",\"family\":\"ipv4\",\"address\":\"127.0.0."
                               "1\",\"port\":8888"),
                  std::string::npos);
        EXPECT_NE(journal.find("\"preview_hex\":\"616263\""), std::string::npos);
        EXPECT_NE(journal.find("\"completed_bytes\":3,\"syscall\":\"NtDeviceIoControlFile\""), std::string::npos);
        EXPECT_NE(journal.find("\"completed_exceeds_requested\":false"), std::string::npos);
        EXPECT_NE(journal.find("\"control_output_preview_hex\":\"" + std::string(128, '5') + "\""), std::string::npos);
        EXPECT_NE(journal.find("\"control_output_preview_truncated\":true"), std::string::npos);
        EXPECT_NE(journal.find("\"source\":\"guest_datagram_source_at_completion\""), std::string::npos);
        EXPECT_NE(journal.find("\"preview_hex\":\"78797a\""), std::string::npos);
        EXPECT_NE(journal.find("\"payload_fnv1a64\":\"0xbff4aa198026f420\",\"payload_hashed_bytes\":3"), std::string::npos);
        EXPECT_NE(journal.find("\"elapsed_us\":"), std::string::npos);
        input.close();
        std::filesystem::remove(path);
    }

    TEST(NetworkDebug, CarriesListenerAndPeerEndpointsToAcceptedStream)
    {
        emulator_settings settings{};
        settings.load_registry = false;
        auto emu = create_emulator(std::move(settings));
        constexpr uint64_t memory = 0x270000;
        ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x1000, memory_permission::read_write));
        auto& vcpu = emu.vcpu(0);
        vcpu.cpu.reg(x86_register::rip, 0x18009d812);
        vcpu.cpu.reg(x86_register::rsp, memory + 0x100);
        const syscall_context issuer{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const auto path =
            std::filesystem::temp_directory_path() /
            ("sogen-network-accept-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");

        const std::array<std::byte, 16> local{std::byte{2},   std::byte{0}, std::byte{0x78}, std::byte{0xfe},
                                              std::byte{127}, std::byte{0}, std::byte{0},    std::byte{1}};
        const std::array<std::byte, 16> peer{std::byte{2},   std::byte{0}, std::byte{0x23}, std::byte{0x28},
                                             std::byte{127}, std::byte{0}, std::byte{0},    std::byte{2}};
        io_device_context request{emu.memory};
        request.file_handle.bits = 0x55;
        request.io_status_block = {emu.memory, memory + 0x700};
        request.input_buffer = memory + 0x200;
        request.output_buffer = memory + 0x400;
        std::array<std::byte, 20> bind{};
        std::copy(local.begin(), local.end(), bind.begin() + 4);

        {
            network_debug_logger logger;
            logger.open(path);
            emu.memory.write_memory(memory + 0x200, bind.data(), bind.size());
            request.io_control_code = 0x12003;
            request.input_buffer_length = static_cast<ULONG>(bind.size());
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            request.io_status_block.write({.Status = STATUS_SUCCESS, .Information = 0});
            logger.afd_completion(emu, request, STATUS_SUCCESS, false);

            std::array<std::byte, 20> listen_response{};
            const int32_t sequence = 7;
            std::memcpy(listen_response.data(), &sequence, sizeof(sequence));
            std::copy(peer.begin(), peer.end(), listen_response.begin() + 4);
            emu.memory.write_memory(memory + 0x400, listen_response.data(), listen_response.size());
            request.io_control_code = 0x1200f;
            request.input_buffer_length = 0;
            request.output_buffer_length = static_cast<ULONG>(listen_response.size());
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            request.io_status_block.write({.Status = STATUS_SUCCESS, .Information = listen_response.size()});
            logger.afd_completion(emu, request, STATUS_SUCCESS, false);

            AFD_ACCEPT_INFO accept{};
            accept.Sequence = sequence;
            accept.AcceptHandle.bits = 0x66;
            emu.memory.write_memory(memory + 0x200, &accept, sizeof(accept));
            request.io_control_code = 0x12013;
            request.input_buffer_length = sizeof(accept);
            request.output_buffer_length = 0;
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            request.io_status_block.write({.Status = STATUS_SUCCESS, .Information = 0});
            logger.afd_completion(emu, request, STATUS_SUCCESS, false);

            using Traits = EmulatorTraits<Emu64>;
            const EMU_WSABUF<Traits> descriptor{.len = 3, .buf = memory + 0x500};
            const AFD_SEND_INFO<Traits> send{.BufferArray = memory + 0x300, .BufferCount = 1};
            emu.memory.write_memory(memory + 0x300, &descriptor, sizeof(descriptor));
            emu.memory.write_memory(memory + 0x200, &send, sizeof(send));
            emu.memory.write_memory(memory + 0x500, "abc", 3);
            request.file_handle.bits = 0x66;
            request.io_control_code = 0x1201f;
            request.input_buffer_length = sizeof(send);
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            request.io_status_block.write({.Status = STATUS_SUCCESS, .Information = 3});
            logger.afd_completion(emu, request, STATUS_SUCCESS, false);
        }

        std::ifstream input(path, std::ios::binary);
        ASSERT_TRUE(input.good());
        const std::string journal{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        const auto accepted_send = journal.find("\"file_handle\":\"0x66\"");
        ASSERT_NE(accepted_send, std::string::npos);
        const auto line = journal.substr(accepted_send, journal.find('\n', accepted_send) - accepted_send);
        EXPECT_NE(line.find("\"local_endpoint\":{\"role\":\"local\""), std::string::npos);
        EXPECT_NE(line.find("\"address\":\"127.0.0.1\",\"port\":30974"), std::string::npos);
        EXPECT_NE(line.find("\"remote_endpoint\":{\"role\":\"remote\""), std::string::npos);
        EXPECT_NE(line.find("\"address\":\"127.0.0.2\",\"port\":9000"), std::string::npos);
        input.close();
        std::filesystem::remove(path);
    }

    TEST(NetworkDebug, HostStreamTransfersUseActualReturnedByteCount)
    {
        emulator_settings settings{};
        settings.load_registry = false;
        auto emu = create_emulator(std::move(settings));
        constexpr uint64_t memory = 0x280000;
        ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x1000, memory_permission::read_write));
        auto& vcpu = emu.vcpu(0);
        vcpu.cpu.reg(x86_register::rip, 0x18009d812);
        vcpu.cpu.reg(x86_register::rsp, memory + 0x100);
        const syscall_context issuer{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const auto path = std::filesystem::temp_directory_path() /
                          ("sogen-host-stream-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
        io_device_context request{emu.memory};
        request.file_handle.bits = 0x55;
        request.issuer_thread_id = 41;
        const std::array<std::byte, 6> bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'},
                                             std::byte{'x'}, std::byte{'y'}, std::byte{'z'}};
        uint64_t send_id{};
        uint64_t receive_id{};
        uint64_t eof_id{};
        uint64_t zero_capacity_id{};
        {
            network_debug_logger logger;
            logger.open(path);
            request.io_control_code = 0x1201f;
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            send_id = request.network_request_id;
            logger.host_stream_transfer(request, "send", bytes, 3);

            request.io_control_code = 0x12017;
            request.completing_pending = true;
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            receive_id = request.network_request_id;
            logger.host_stream_transfer(request, "receive", std::span{bytes}.subspan(3), 3);

            request.completing_pending = false;
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            eof_id = request.network_request_id;
            logger.host_stream_transfer(request, "receive", bytes, 0);

            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            zero_capacity_id = request.network_request_id;
            logger.host_stream_transfer(request, "receive", std::span<const std::byte>{}, 0);
        }
        std::ifstream input(path, std::ios::binary);
        ASSERT_TRUE(input.good());
        const std::string journal{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        const auto host_line = [&](const uint64_t id) {
            const auto start = journal.find("\"channel\":\"host_socket\",\"phase\":\"completion\",\"request_id\":" + std::to_string(id));
            EXPECT_NE(start, std::string::npos);
            return start == std::string::npos ? std::string{} : journal.substr(start, journal.find('\n', start) - start);
        };
        const auto send = host_line(send_id);
        EXPECT_NE(send.find("\"submitted_bytes\":6,\"completed_bytes\":3"), std::string::npos);
        EXPECT_NE(send.find("\"partial\":true"), std::string::npos);
        EXPECT_NE(send.find("\"preview_hex\":\"616263\""), std::string::npos);
        EXPECT_NE(send.find("\"payload_fnv1a64\":\"0xe71fa2190541574b\",\"payload_hashed_bytes\":3"), std::string::npos);
        const auto receive = host_line(receive_id);
        EXPECT_NE(receive.find("\"pending_retry\":true"), std::string::npos);
        EXPECT_NE(receive.find("\"preview_hex\":\"78797a\""), std::string::npos);
        EXPECT_NE(receive.find("\"payload_fnv1a64\":\"0xbff4aa198026f420\",\"payload_hashed_bytes\":3"), std::string::npos);
        const auto eof = host_line(eof_id);
        EXPECT_NE(eof.find("\"eof\":true,\"zero_capacity\":false"), std::string::npos);
        EXPECT_NE(eof.find("\"payload_fnv1a64\":\"0xcbf29ce484222325\",\"payload_hashed_bytes\":0"), std::string::npos);
        const auto zero_capacity = host_line(zero_capacity_id);
        EXPECT_NE(zero_capacity.find("\"eof\":false,\"zero_capacity\":true"), std::string::npos);
        input.close();
        std::filesystem::remove(path);
    }

    TEST(NetworkDebug, HostLoopbackAfdStreamMatchesSocketBytes)
    {
        network::socket_factory initialize_winsock;
        network::socket_wrapper listener{AF_INET, SOCK_STREAM, IPPROTO_TCP};
        ASSERT_TRUE(listener.bind(network::address{"127.0.0.1", uint16_t{0}}));
        ASSERT_TRUE(listener.listen(1));
        listener.set_blocking(false);
        const auto local = listener.get_local_address();
        ASSERT_TRUE(local.has_value());
        const auto port = local->get_port();
        const auto path = std::filesystem::temp_directory_path() /
                          ("sogen-host-loopback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
        uint64_t send_id{};
        uint64_t receive_id{};
        {
            emulator_settings settings{};
            settings.load_registry = false;
            emulator_interfaces interfaces{};
            interfaces.socket_factory = std::make_unique<network::socket_factory>();
            auto emu = create_emulator(std::move(settings), {}, std::move(interfaces));
            constexpr uint64_t memory = 0x290000;
            ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x1000, memory_permission::read_write));
            const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
            emu.memory.write_memory(memory, creation.data(), sizeof(creation));
            auto device = create_afd_endpoint({.is_32_bit = false});
            device->create(emu, {.buffer = memory, .length = sizeof(creation)});

            using Traits = EmulatorTraits<Emu64>;
            AFD_CONNECT_JOIN_INFO_TL<Traits> connect{};
            connect.RemoteAddress.sa_family = 2;
            connect.RemoteAddress.sa_data[0] = static_cast<char>(port >> 8);
            connect.RemoteAddress.sa_data[1] = static_cast<char>(port & 0xff);
            connect.RemoteAddress.sa_data[2] = 127;
            connect.RemoteAddress.sa_data[5] = 1;
            io_device_context request{emu.memory};
            request.io_status_block = {emu.memory, memory + 0x80};
            request.input_buffer = memory + 0x100;
            request.io_control_code = 0x12007;
            request.input_buffer_length = sizeof(connect);
            request.io_status_block.write({.Status = STATUS_PENDING, .Information = 0});
            emu.memory.write_memory(memory + 0x100, &connect, sizeof(connect));
            const auto connect_status = device->execute_ioctl(emu, request);
            ASSERT_TRUE(connect_status == STATUS_SUCCESS || connect_status == STATUS_PENDING);

            network::address peer{};
            std::unique_ptr<network::i_socket> accepted;
            for (size_t attempt = 0; attempt < 200 && !accepted; ++attempt)
            {
                accepted = listener.accept(peer);
                if (!accepted)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            ASSERT_NE(accepted, nullptr);
            if (connect_status == STATUS_PENDING)
            {
                for (size_t attempt = 0; attempt < 200; ++attempt)
                {
                    device->work(emu);
                    const auto block = request.io_status_block.try_read();
                    if (block && block->Status == STATUS_SUCCESS)
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                ASSERT_EQ(request.io_status_block.read().Status, STATUS_SUCCESS);
            }

            emu.network_debug.open(path);
            auto& vcpu = emu.vcpu(0);
            vcpu.cpu.reg(x86_register::rip, 0x18009d812);
            vcpu.cpu.reg(x86_register::rsp, memory + 0x500);
            const syscall_context issuer{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
            const EMU_WSABUF<Traits> descriptor{.len = 3, .buf = memory + 0x300};
            emu.memory.write_memory(memory + 0x200, &descriptor, sizeof(descriptor));
            emu.memory.write_memory(memory + 0x300, "abc", 3);
            const AFD_SEND_INFO<Traits> send{.BufferArray = memory + 0x200, .BufferCount = 1};
            emu.memory.write_memory(memory + 0x100, &send, sizeof(send));
            request.file_handle.bits = 0x55;
            request.issuer_thread_id = 41;
            request.io_control_code = 0x1201f;
            request.input_buffer_length = sizeof(send);
            emu.network_debug.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            send_id = request.network_request_id;
            ASSERT_EQ(device->execute_ioctl(emu, request), STATUS_SUCCESS);
            std::array<std::byte, 3> peer_bytes{};
            sent_size peer_received = -1;
            for (size_t attempt = 0; attempt < 200 && peer_received < 0; ++attempt)
            {
                peer_received = accepted->recv(peer_bytes);
                if (peer_received < 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            ASSERT_EQ(peer_received, 3);
            EXPECT_EQ(std::string(reinterpret_cast<const char*>(peer_bytes.data()), peer_bytes.size()), "abc");

            const std::array<std::byte, 3> reply{std::byte{'x'}, std::byte{'y'}, std::byte{'z'}};
            ASSERT_EQ(accepted->send(reply), 3);
            const AFD_RECV_INFO<Traits> receive{.BufferArray = memory + 0x200, .BufferCount = 1};
            emu.memory.write_memory(memory + 0x100, &receive, sizeof(receive));
            request.io_control_code = 0x12017;
            request.input_buffer_length = sizeof(receive);
            request.io_status_block.write({.Status = STATUS_PENDING, .Information = 0});
            emu.network_debug.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            receive_id = request.network_request_id;
            const auto receive_status = device->execute_ioctl(emu, request);
            ASSERT_TRUE(receive_status == STATUS_SUCCESS || receive_status == STATUS_PENDING);
            if (receive_status == STATUS_PENDING)
            {
                for (size_t attempt = 0; attempt < 200; ++attempt)
                {
                    device->work(emu);
                    const auto block = request.io_status_block.try_read();
                    if (block && block->Status == STATUS_SUCCESS)
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                ASSERT_EQ(request.io_status_block.read().Status, STATUS_SUCCESS);
            }
            std::array<char, 3> guest_reply{};
            ASSERT_TRUE(emu.memory.try_read_memory(memory + 0x300, guest_reply.data(), guest_reply.size()));
            EXPECT_EQ(std::string(guest_reply.data(), guest_reply.size()), "xyz");
        }
        std::ifstream input(path, std::ios::binary);
        ASSERT_TRUE(input.good());
        const std::string journal{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        const auto record_line = [&](const std::string& prefix) {
            const auto start = journal.find(prefix);
            EXPECT_NE(start, std::string::npos);
            return start == std::string::npos ? std::string{} : journal.substr(start, journal.find('\n', start) - start);
        };
        const auto host_send =
            record_line("\"channel\":\"host_socket\",\"phase\":\"completion\",\"request_id\":" + std::to_string(send_id));
        const auto host_receive =
            record_line("\"channel\":\"host_socket\",\"phase\":\"completion\",\"request_id\":" + std::to_string(receive_id));
        const auto guest_send = record_line("\"channel\":\"afd\",\"phase\":\"request\",\"request_id\":" + std::to_string(send_id));
        const auto guest_receive = record_line("\"channel\":\"afd\",\"phase\":\"completion\",\"request_id\":" + std::to_string(receive_id));
        EXPECT_NE(host_send.find("\"preview_hex\":\"616263\""), std::string::npos);
        EXPECT_NE(host_receive.find("\"preview_hex\":\"78797a\""), std::string::npos);
        EXPECT_NE(host_send.find("\"payload_fnv1a64\":\"0xe71fa2190541574b\""), std::string::npos);
        EXPECT_NE(guest_send.find("\"payload_fnv1a64\":\"0xe71fa2190541574b\""), std::string::npos);
        EXPECT_NE(host_receive.find("\"payload_fnv1a64\":\"0xbff4aa198026f420\""), std::string::npos);
        EXPECT_NE(guest_receive.find("\"payload_fnv1a64\":\"0xbff4aa198026f420\""), std::string::npos);
        input.close();
        std::filesystem::remove(path);
    }

    TEST(NetworkDebug, AggregatesOnlyIdenticalIdleOutcomes)
    {
        emulator_settings settings{};
        settings.load_registry = false;
        auto emu = create_emulator(std::move(settings));
        constexpr uint64_t memory = 0x260000;
        ASSERT_TRUE(emu.memory.allocate_memory(memory, 0x1000, memory_permission::read_write));
        auto& vcpu = emu.vcpu(0);
        vcpu.cpu.reg(x86_register::rip, 0x18009d812);
        vcpu.cpu.reg(x86_register::rsp, memory + 0x100);
        const syscall_context issuer{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const auto path = std::filesystem::temp_directory_path() /
                          ("sogen-network-idle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
        io_device_context request{emu.memory};
        request.file_handle.bits = 0x77;
        request.issuer_thread_id = 41;
        request.io_status_block = {emu.memory, memory + 0x700};
        request.input_buffer = memory + 0x200;
        request.output_buffer = memory + 0x400;
        request.output_buffer_length = 32;
        request.io_control_code = 0x12024;
        std::array<std::byte, 32> poll{};
        poll[8] = std::byte{1};
        emu.memory.write_memory(memory + 0x200, poll.data(), poll.size());
        request.input_buffer_length = static_cast<ULONG>(poll.size());

        {
            network_debug_logger logger;
            logger.open(path);
            for (size_t index = 0; index < 3; ++index)
            {
                logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
                logger.afd_result(emu, request, STATUS_TIMEOUT);
                request.io_status_block.write({.Status = STATUS_TIMEOUT, .Information = 0});
                logger.afd_completion(emu, request, STATUS_TIMEOUT, false);
            }
            poll[9] = std::byte{1};
            emu.memory.write_memory(memory + 0x200, poll.data(), poll.size());
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            logger.afd_result(emu, request, STATUS_TIMEOUT);
            logger.afd_completion(emu, request, STATUS_TIMEOUT, false);

            using Traits = EmulatorTraits<Emu64>;
            const EMU_WSABUF<Traits> descriptor{.len = 3, .buf = memory + 0x500};
            const AFD_RECV_DATAGRAM_INFO<Traits> receive{.BufferArray = memory + 0x300, .BufferCount = 1};
            emu.memory.write_memory(memory + 0x300, &descriptor, sizeof(descriptor));
            emu.memory.write_memory(memory + 0x200, &receive, sizeof(receive));
            request.io_control_code = 0x1201b;
            request.input_buffer_length = sizeof(receive);
            request.output_buffer_length = 0;
            for (size_t index = 0; index < 3; ++index)
            {
                logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
                logger.afd_result(emu, request, STATUS_DEVICE_NOT_READY);
                request.io_status_block.write({.Status = STATUS_DEVICE_NOT_READY, .Information = 0});
                logger.afd_completion(emu, request, STATUS_DEVICE_NOT_READY, false);
            }
            logger.begin_afd_request(emu, request, issuer, "NtDeviceIoControlFile");
            logger.afd_result(emu, request, STATUS_SUCCESS);
            emu.memory.write_memory(memory + 0x500, "xyz", 3);
            request.io_status_block.write({.Status = STATUS_SUCCESS, .Information = 3});
            logger.afd_completion(emu, request, STATUS_SUCCESS, false);
        }

        std::ifstream input(path, std::ios::binary);
        ASSERT_TRUE(input.good());
        const std::string journal{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        EXPECT_EQ(std::count(journal.begin(), journal.end(), '\n'), 14);
        EXPECT_NE(journal.find("\"kind\":\"afd_idle_repeat\",\"operation\":\"poll\",\"status\":\"0x102\""), std::string::npos);
        EXPECT_NE(journal.find("\"kind\":\"afd_idle_repeat\",\"operation\":\"receive_datagram\",\"status\":\"0xc00000a3\""),
                  std::string::npos);
        const auto first_count = journal.find("\"count\":2");
        ASSERT_NE(first_count, std::string::npos);
        EXPECT_NE(journal.find("\"count\":2", first_count + 1), std::string::npos);
        EXPECT_NE(journal.find("\"first_request_id\":"), std::string::npos);
        EXPECT_NE(journal.find("\"last_time_us\":"), std::string::npos);
        EXPECT_NE(journal.find("\"preview_hex\":\"78797a\""), std::string::npos);
        input.close();
        std::filesystem::remove(path);
    }
}
