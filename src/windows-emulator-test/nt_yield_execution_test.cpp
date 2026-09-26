#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtYieldExecution(const syscall_context&);
}

namespace sogen::test
{
    class NtYieldExecutionTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();

        void SetUp() override
        {
            emu.setup_process_if_necessary();
        }

        NTSTATUS invoke()
        {
            auto& vcpu = emu.vcpu(0);
            return emu.dispatch_on_cpu(vcpu.cpu, [&] {
                const syscall_context c{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
                return syscalls::handle_NtYieldExecution(c);
            });
        }

        emulator_thread& add_thread()
        {
            const auto handle = emu.process.create_thread(emu.memory, emu.mod_manager.executable->entry_point, 0, 0x10000, 0);
            return *emu.process.threads.get(handle);
        }
    };

    TEST_F(NtYieldExecutionTest, DoesNotStopCpuWhenNoOtherThreadCanRun)
    {
        auto& vcpu = emu.vcpu(0);
        ASSERT_FALSE(vcpu.switch_thread.load());
        EXPECT_EQ(invoke(), nt_status_no_yield_performed);
        EXPECT_FALSE(vcpu.switch_thread.load());

        auto& waiting = add_thread();
        waiting.suspended = 1;
        EXPECT_EQ(invoke(), nt_status_no_yield_performed);
        EXPECT_FALSE(vcpu.switch_thread.load());
    }

    TEST_F(NtYieldExecutionTest, StopsCpuForAnotherReadyThread)
    {
        auto& vcpu = emu.vcpu(0);
        auto& other = add_thread();
        ASSERT_TRUE(other.is_thread_ready(emu));
        EXPECT_EQ(invoke(), STATUS_SUCCESS);
        EXPECT_TRUE(vcpu.switch_thread.load());
    }

    TEST_F(NtYieldExecutionTest, AllowsAlertableApcDispatch)
    {
        auto& vcpu = emu.vcpu(0);
        auto& other = add_thread();
        other.waiting_for_alert = true;
        other.apc_alertable = true;
        other.pending_apcs.push_back({.apc_routine = 0x123400});
        ASSERT_FALSE(other.is_thread_ready(emu));
        EXPECT_EQ(invoke(), STATUS_SUCCESS);
        EXPECT_TRUE(vcpu.switch_thread.load());
    }

    TEST(NtYieldExecutionSmpTest, DoesNotYieldToReadyThreadOwnedByAnotherVcpu)
    {
        emulator_settings settings{.disable_logging = true, .use_instruction_precision = false};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        auto emu = create_sample_emulator(std::move(settings), {}, {}, {}, 2);
        emu.setup_process_if_necessary();
        const auto other_handle =
            emu.process.create_thread(emu.memory, emu.mod_manager.executable->entry_point, 0, 0x10000, 0);
        auto* other = emu.process.threads.get(other_handle);
        ASSERT_NE(other, nullptr);
        ASSERT_TRUE(other->is_thread_ready(emu));
        emu.vcpu(1).active_thread = other;

        auto& vcpu = emu.vcpu(0);
        const auto status = emu.dispatch_on_cpu(vcpu.cpu, [&] {
            const syscall_context c{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
            return syscalls::handle_NtYieldExecution(c);
        });
        EXPECT_EQ(status, nt_status_no_yield_performed);
        EXPECT_FALSE(vcpu.switch_thread.load());
    }

} // namespace sogen::test