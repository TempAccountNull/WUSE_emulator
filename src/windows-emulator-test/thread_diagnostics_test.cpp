#include "emulation_test_utils.hpp"
#include <win_x86_64_gdb_stub_handler.hpp>

namespace sogen::test
{
    class ThreadDiagnosticsTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
    };

    TEST_F(ThreadDiagnosticsTest, InspectionPreservesSchedulerAndPendingApcs)
    {
        emu.setup_process_if_necessary();
        const auto target_handle = emu.process.create_thread(emu.memory, emu.mod_manager.executable->entry_point, 0, 0x10000, 0);
        auto& target = *emu.process.threads.get(target_handle);
        target.apc_alertable = true;
        target.pending_apcs.push_back({.apc_routine = 0x123400});
        target.await_objects.push_back(emu.process.events.store(event{}));
        const auto dependency_handle = emu.process.create_thread(emu.memory, emu.mod_manager.executable->entry_point, 0, 0x10000, 0);
        const auto dependency_id = emu.process.threads.get(dependency_handle)->id;
        target.await_objects.push_back(dependency_handle);
        target.await_any = true;
        target.suspended = 2;
        target.executed_instructions = 123456;
        target.current_ip = emu.mod_manager.executable->entry_point;
        target.await_host_condition = [] {
            ADD_FAILURE() << "Inspection evaluated a wait predicate";
            return false;
        };
        auto* const active = emu.vcpu(0).active_thread;
        const auto registers = emu.emu().save_registers();
        const auto pending = target.pending_status;
        const auto initialized = target.setup_done;
        const auto waiting = target.await_objects;
        win_x86_64_gdb_stub_handler debugger(emu);
        ASSERT_TRUE(debugger.supports_thread_diagnostics());
        const auto rows = debugger.get_thread_diagnostics();
        const auto found = std::ranges::find_if(rows, [&](const auto& row) { return row.id == target.id; });
        ASSERT_NE(found, rows.end());
        const std::map<std::string, std::string> fields(found->fields.begin(), found->fields.end());
        EXPECT_EQ(fields.at("instructions"), "123456");
        EXPECT_EQ(fields.at("handle"), "0x" + utils::string::to_hex_number(target_handle.bits));
        EXPECT_EQ(fields.at("wait_thread_ids"), "0x" + utils::string::to_hex_number(dependency_id));
        EXPECT_EQ(fields.at("apc_alertable"), "1");
        EXPECT_EQ(fields.at("pending_apcs"), "1");
        EXPECT_EQ(fields.at("suspended"), "2");
        EXPECT_EQ(fields.at("last_instruction_module"), "test-sample.exe");
        EXPECT_FALSE(fields.contains("rip"));
        EXPECT_EQ(emu.vcpu(0).active_thread, active);
        EXPECT_EQ(emu.emu().save_registers(), registers);
        EXPECT_EQ(target.pending_status, pending);
        EXPECT_EQ(target.setup_done, initialized);
        EXPECT_EQ(target.await_objects, waiting);
        EXPECT_TRUE(target.apc_alertable);
        ASSERT_EQ(target.pending_apcs.size(), 1u);
        EXPECT_EQ(target.pending_apcs.front().apc_routine, 0x123400u);
    }

    TEST_F(ThreadDiagnosticsTest, RetainedExitedThreadsRemainVisible)
    {
        emu.setup_process_if_necessary();
        const auto target_handle = emu.process.create_thread(emu.memory, emu.mod_manager.executable->entry_point, 0, 0x10000, 0);
        auto& target = *emu.process.threads.get(target_handle);
        target.exit_status = STATUS_ACCESS_VIOLATION;
        win_x86_64_gdb_stub_handler debugger(emu);
        const auto rows = debugger.get_thread_diagnostics();
        const auto found = std::ranges::find_if(rows, [&](const auto& row) { return row.id == target.id; });
        ASSERT_NE(found, rows.end());
        const std::map<std::string, std::string> fields(found->fields.begin(), found->fields.end());
        EXPECT_EQ(fields.at("terminated"), "1");
        EXPECT_EQ(fields.at("exit_status"), "0xc0000005");
        EXPECT_EQ(emu.vcpu(0).active_thread->id, emu.current_thread().id);
    }
}
