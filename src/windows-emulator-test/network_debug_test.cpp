#include "emulation_test_utils.hpp"
#include <network_debug.hpp>
#include <io_device.hpp>
#include <syscall_utils.hpp>
#include <devices/afd_types.hpp>

#include <chrono>
#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>

namespace sogen::test
{
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
}
