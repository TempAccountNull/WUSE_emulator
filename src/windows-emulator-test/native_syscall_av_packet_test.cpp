#include "emulation_test_utils.hpp"
#include <utils/finally.hpp>

#ifdef _WIN32
#include <Windows.h>

namespace sogen::test
{
    // Run only with SOGEN_SYSCALL_NATIVE_AV_PACKET=1. RaiseException gives the VEH a
    // deterministic first-chance AV without dereferencing an invalid host pointer.
    TEST(NativeSyscallAvPacket, OptInRecordsOriginalFaultAndPreservesStop)
    {
        const char* enabled = std::getenv("SOGEN_SYSCALL_NATIVE_AV_PACKET");
        if (!enabled || enabled[0] != '1' || enabled[1] != '\0')
        {
            GTEST_SKIP() << "Set SOGEN_SYSCALL_NATIVE_AV_PACKET=1";
        }

        bool raised = false;
        emulator_callbacks callbacks{};
        callbacks.on_syscall = [&](uint32_t, std::string_view) -> instruction_hook_continuation {
            if (!raised)
            {
                raised = true;
                const ULONG_PTR exception_information[2] = {0, 0xF00D1234};
                RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 2, exception_information);
            }
            return instruction_hook_continuation::run_instruction;
        };

        emulator_settings settings{.disable_logging = true};
        auto emu = create_sample_emulator(std::move(settings), {}, std::move(callbacks));
        emu.log.set_silent(true);
        HANDLE read_pipe{};
        HANDLE write_pipe{};
        ASSERT_TRUE(CreatePipe(&read_pipe, &write_pipe, nullptr, 0));
        const auto old_stderr = GetStdHandle(STD_ERROR_HANDLE);
        const auto cleanup = utils::finally([&] {
            SetStdHandle(STD_ERROR_HANDLE, old_stderr);
            if (write_pipe)
            {
                CloseHandle(write_pipe);
            }
            if (read_pipe)
            {
                CloseHandle(read_pipe);
            }
        });
        ASSERT_TRUE(SetStdHandle(STD_ERROR_HANDLE, write_pipe));

        emu.start();

        ASSERT_TRUE(SetStdHandle(STD_ERROR_HANDLE, old_stderr));
        ASSERT_TRUE(CloseHandle(write_pipe));
        write_pipe = nullptr;
        char bytes[1024]{};
        DWORD length{};
        ASSERT_TRUE(ReadFile(read_pipe, bytes, sizeof(bytes), &length, nullptr));
        const std::string packet(bytes, length);

        EXPECT_TRUE(raised);
        EXPECT_EQ(emu.last_stop_reason(), stop_reason::syscall_exception);
        EXPECT_NE(packet.find("[HOSTAV-FIRST] code=0xC0000005"), std::string::npos) << packet;
        EXPECT_NE(packet.find("guest_tid="), std::string::npos) << packet;
        EXPECT_NE(packet.find("vcpu=0"), std::string::npos) << packet;
        EXPECT_NE(packet.find("host_tid="), std::string::npos) << packet;
        EXPECT_NE(packet.find("fault=0xF00D1234"), std::string::npos) << packet;
        EXPECT_NE(packet.find("native_rip=0x"), std::string::npos) << packet;
        EXPECT_NE(packet.find("phase=1"), std::string::npos) << packet;
    }
}
#endif
