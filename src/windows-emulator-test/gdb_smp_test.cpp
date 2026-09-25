#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <win_x86_64_gdb_stub_handler.hpp>

#include <algorithm>
#include <array>

namespace sogen::test
{
    TEST(GdbSmp, SelectedThreadOnSecondVcpuStepsWithoutRunningItsPeer)
    {
        emulator_settings settings{.disable_logging = true, .use_relative_time = false, .use_instruction_precision = false};
        settings.emulation_root = get_emulator_root();
        settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
        emulator_interfaces interfaces{};
        interfaces.socket_factory = network::create_static_socket_factory();
        interfaces.dns_lookup = create_sample_dns_lookup();
        interfaces.ui = std::make_unique<null_ui_backend>();
        windows_emulator emu{icicle::create_x86_64_emulator(2), get_sample_app_settings({}), settings, {}, std::move(interfaces)};
        emu.setup_process_if_necessary();

        const auto code = emu.mod_manager.executable->entry_point;
        std::array<uint8_t, 64> instructions{};
        instructions.fill(0x90);
        instructions[0] = 0xeb;
        instructions[1] = 0xfe;
        emu.memory.write_memory(code, instructions.data(), instructions.size());
        auto& first = emu.vcpu(0);
        first.cpu.reg(x86_register::rip, code);
        first.cpu.reg(x86_register::rax, 0x1020304050607080ull);
        first.switch_thread = false;
        const auto first_thread = first.active_thread->id;

        const auto target_handle = emu.process.create_thread(emu.memory, code + 16, 0, 0x10000, 0);
        auto& target = *emu.process.threads.get(target_handle);
        const auto original = first.cpu.save_registers();
        first.cpu.reg(x86_register::rip, code + 16);
        first.cpu.reg(x86_register::rax, 0x8877665544332211ull);
        first.cpu.reg(x86_register::rsp, target.stack_base + target.stack_size - 0x100);
        target.last_registers = first.cpu.save_registers();
        target.setup_done = true;
        first.cpu.restore_registers(original);
        ASSERT_TRUE(emu.activate_thread(emu.vcpu(1), target.id));
        emu.vcpu(1).switch_thread = false;

        win_x86_64_gdb_stub_handler debugger{emu};
        EXPECT_EQ(debugger.get_current_thread_id(), first_thread);
        const auto ids = debugger.get_thread_ids();
        EXPECT_NE(std::find(ids.begin(), ids.end(), first_thread), ids.end());
        EXPECT_NE(std::find(ids.begin(), ids.end(), target.id), ids.end());
        ASSERT_TRUE(debugger.select_general_thread(target.id));
        uint64_t rax{};
        ASSERT_EQ(debugger.read_register(0, &rax, sizeof(rax)), sizeof(rax));
        EXPECT_EQ(rax, 0x8877665544332211ull);
        ASSERT_TRUE(debugger.select_continuation_thread(target.id));
        EXPECT_EQ(debugger.singlestep(), gdb_stub::action::resume);
        EXPECT_FALSE(debugger.execution_failed());
        EXPECT_EQ(debugger.get_current_thread_id(), target.id);
        EXPECT_EQ(emu.vcpu(1).cpu.read_instruction_pointer(), code + 17);
        EXPECT_EQ(emu.vcpu(0).cpu.read_instruction_pointer(), code);
        EXPECT_FALSE(emu.vcpu(0).running.load());
        EXPECT_FALSE(emu.vcpu(1).running.load());
        ASSERT_TRUE(debugger.select_general_thread(0));
        ASSERT_EQ(debugger.read_register(0, &rax, sizeof(rax)), sizeof(rax));
        EXPECT_EQ(rax, 0x8877665544332211ull);
        debugger.on_interrupt();
        EXPECT_EQ(debugger.run(), gdb_stub::action::resume);
        EXPECT_EQ(emu.vcpu(1).cpu.read_instruction_pointer(), code + 17);
        EXPECT_EQ(emu.vcpu(0).cpu.read_instruction_pointer(), code);
        ASSERT_TRUE(debugger.set_breakpoint(gdb_stub::breakpoint_type::hardware_exec, code + 18, 1));
        EXPECT_EQ(debugger.run(), gdb_stub::action::resume);
        EXPECT_FALSE(debugger.execution_failed());
        EXPECT_EQ(debugger.get_current_thread_id(), target.id);
        EXPECT_EQ(emu.vcpu(0).cpu.read_instruction_pointer(), code);
        EXPECT_FALSE(emu.vcpu(0).running.load());
        EXPECT_FALSE(emu.vcpu(1).running.load());
        uint8_t byte{};
        ASSERT_TRUE(debugger.read_memory(code + 17, &byte, sizeof(byte)));
        EXPECT_EQ(byte, 0x90);
    }
}
