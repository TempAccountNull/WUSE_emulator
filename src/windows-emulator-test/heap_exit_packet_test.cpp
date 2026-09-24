#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtTerminateProcess(const syscall_context&, handle, NTSTATUS);
}

namespace sogen::test
{
    TEST(HeapExitPacket, CapturesBoundedGuestContextOnce)
    {
        emulator_settings settings{.disable_logging = true};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        auto emu = create_sample_emulator(std::move(settings));
        emu.setup_process_if_necessary();
        auto& vcpu = emu.vcpu(0);
        ASSERT_NE(vcpu.active_thread, nullptr);
        ASSERT_NE(emu.mod_manager.executable, nullptr);

        const auto stack = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(stack, 0u);
        const auto rip = emu.mod_manager.executable->entry_point;
        ASSERT_NE(rip, 0u);
        std::array<uint64_t, 16> words{};
        for (size_t i = 0; i < words.size(); ++i)
        {
            words[i] = 0xF000 + i;
        }
        words[0] = rip + 0x20;
        words[3] = rip + 0x40;
        emu.memory.write_memory(stack, words.data(), sizeof(words));
        vcpu.cpu.reg(x86_register::rip, rip);
        vcpu.cpu.reg(x86_register::rsp, stack);

        std::string output;
        emu.log.set_silent(true);
        emu.log.set_sink([&](color, std::string_view line) { output += line; });
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const auto heap_corruption = static_cast<NTSTATUS>(0xC0000374u);
        EXPECT_EQ(syscalls::handle_NtTerminateProcess(context, CURRENT_PROCESS, heap_corruption), STATUS_SUCCESS);
        EXPECT_EQ(emu.process.exit_status, heap_corruption);
        EXPECT_NE(output.find("[GUEST-HEAP-EXIT] status=0xC0000374 guest_tid="), std::string::npos) << output;
        EXPECT_NE(output.find("vcpu=0"), std::string::npos) << output;
        EXPECT_NE(output.find("stack16=[0x"), std::string::npos) << output;
        EXPECT_NE(output.find("s0:" + emu.mod_manager.executable->name), std::string::npos) << output;
        EXPECT_NE(output.find("0xF00F]"), std::string::npos) << output;

        EXPECT_EQ(syscalls::handle_NtTerminateProcess(context, CURRENT_PROCESS, heap_corruption), STATUS_SUCCESS);
        EXPECT_EQ(output.find("[GUEST-HEAP-EXIT]"), output.rfind("[GUEST-HEAP-EXIT]")) << output;
    }
}
