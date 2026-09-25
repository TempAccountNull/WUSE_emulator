#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtOpenThread(const syscall_context&, emulator_object<handle>, ACCESS_MASK,
                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>, emulator_object<CLIENT_ID64>);
    NTSTATUS handle_NtClose(const syscall_context&, handle);
    NTSTATUS handle_NtWaitForSingleObject(const syscall_context&, handle, BOOLEAN, emulator_object<LARGE_INTEGER>);
}

namespace sogen::test
{
    TEST(ThreadOpenLifetimeTest, ClosingOpenedHandlePreservesOriginalJoinHandle)
    {
        emulator_settings settings{.disable_logging = true};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        auto emu = create_sample_emulator(std::move(settings));
        emu.setup_process_if_necessary();

        const auto original = emu.process.create_thread(emu.memory, emu.mod_manager.executable->entry_point, 0, 0x10000, 0);
        auto* target = emu.process.threads.get(original);
        ASSERT_NE(target, nullptr);
        const auto target_tid = target->id;
        ASSERT_EQ(target->ref_count, 1u);

        const auto buffer = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<CLIENT_ID64> client_id{emu.memory, buffer + 0x20};
        client_id.write({.UniqueProcess = 4, .UniqueThread = target_tid});
        const emulator_object<handle> opened_output{emu.memory, buffer};
        auto& vcpu = emu.vcpu(0);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};

        ASSERT_EQ(syscalls::handle_NtOpenThread(context, opened_output, 0, {emu.memory, 0}, client_id), STATUS_SUCCESS);
        const auto opened = opened_output.read();
        ASSERT_EQ(opened, original);
        ASSERT_NE(emu.process.threads.get(original), nullptr);
        EXPECT_EQ(emu.process.threads.get(original)->ref_count, 2u);

        emu.process.terminate_thread(*target, STATUS_SUCCESS);
        ASSERT_EQ(syscalls::handle_NtClose(context, opened), STATUS_SUCCESS);
        ASSERT_NE(emu.process.threads.get(original), nullptr);
        EXPECT_EQ(emu.process.threads.get(original)->ref_count, 1u);
        EXPECT_EQ(syscalls::handle_NtWaitForSingleObject(context, original, FALSE, {emu.memory, 0}), STATUS_SUCCESS);
    }
}
