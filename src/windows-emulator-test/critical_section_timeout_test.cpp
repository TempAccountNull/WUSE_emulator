#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtWaitForAlertByThreadId(const syscall_context&, uint64_t, emulator_object<LARGE_INTEGER>);
    NTSTATUS handle_NtAlertThreadByThreadId(const syscall_context&, uint64_t);
}

namespace sogen::test
{
    TEST(CriticalSectionTimeoutTest, DefaultWaitRemainsBlockedUntilAlerted)
    {
        emulator_settings settings{.disable_logging = true};
        settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
        auto emu = create_sample_emulator(std::move(settings));
        emu.setup_process_if_necessary();
        auto& vcpu = emu.vcpu(0);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const emulator_object<LARGE_INTEGER> timeout{emu.memory, emu.process.peb64.value() + offsetof(PEB64, CriticalSectionTimeout)};
        ASSERT_EQ(syscalls::handle_NtWaitForAlertByThreadId(context, 0, timeout), STATUS_SUCCESS);
        auto& thread = context.thread();
        EXPECT_FALSE(thread.is_thread_ready(emu));
        ASSERT_EQ(syscalls::handle_NtAlertThreadByThreadId(context, thread.id), STATUS_SUCCESS);
        EXPECT_TRUE(thread.is_thread_ready(emu));
        EXPECT_EQ(thread.pending_status, STATUS_ALERTED);
        EXPECT_FALSE(thread.waiting_for_alert);
    }
}
