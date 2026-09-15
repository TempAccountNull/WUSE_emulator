#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetInformationThread(const syscall_context&, handle, THREADINFOCLASS, uint64_t, uint32_t);
    NTSTATUS handle_NtQueryInformationThread(const syscall_context&, handle, uint32_t, uint64_t, uint32_t, emulator_object<uint32_t>);
}

namespace sogen::test
{
    class ThreadDebuggerFlagTest : public testing::TestWithParam<uint64_t>
    {
      public:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t memory{};
        handle target{};

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            memory = emu.memory.allocate_memory(0x3000, memory_permission::read_write, false, GetParam());
            target = emu.process.create_thread(emu.memory, emu.mod_manager.executable->entry_point, 0, 0x10000, 0);
            emu.vcpu(0).active_thread->debugger_hide = false;
            emu.process.threads.get(target)->debugger_hide = false;
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        NTSTATUS query(const handle h, const uint64_t output, const uint32_t length, const uint64_t returned = 0)
        {
            return syscalls::handle_NtQueryInformationThread(context(), h, ThreadHideFromDebugger, output, length, {emu.memory, returned});
        }

        NTSTATUS set(const handle h, const uint64_t input = 0, const uint32_t length = 0)
        {
            return syscalls::handle_NtSetInformationThread(context(), h, ThreadHideFromDebugger, input, length);
        }

        bool target_hidden()
        {
            return emu.process.threads.get(target)->debugger_hide;
        }

        bool caller_hidden()
        {
            return emu.vcpu(0).active_thread->debugger_hide;
        }
    };

    TEST_P(ThreadDebuggerFlagTest, QueryUsesSelectedThread)
    {
        emu.vcpu(0).active_thread->debugger_hide = true;
        ASSERT_EQ(query(target, memory, 1), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<BOOLEAN>(memory), FALSE);
        ASSERT_EQ(query(CURRENT_THREAD, memory, 1), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<BOOLEAN>(memory), TRUE);
        emu.vcpu(0).active_thread->debugger_hide = false;
        emu.process.threads.get(target)->debugger_hide = true;
        ASSERT_EQ(query(target, memory, 1), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<BOOLEAN>(memory), TRUE);
        EXPECT_FALSE(caller_hidden());
    }

    TEST_P(ThreadDebuggerFlagTest, SetterUsesSelectedThreadAndIgnoresZeroLengthPointer)
    {
        ASSERT_EQ(set(target, UINT64_MAX), STATUS_SUCCESS);
        EXPECT_TRUE(target_hidden());
        EXPECT_FALSE(caller_hidden());
        ASSERT_EQ(set(target, memory + 1), STATUS_SUCCESS);
        EXPECT_TRUE(target_hidden());
        ASSERT_EQ(set(make_handle(0xfffffffeu), 1), STATUS_SUCCESS);
        EXPECT_TRUE(caller_hidden());
    }

    TEST_P(ThreadDebuggerFlagTest, SetterRejectsBooleanInputWithoutClearingFlag)
    {
        emu.process.threads.get(target)->debugger_hide = true;
        emulator_object<ULONG>{emu.memory, memory}.write(0);
        EXPECT_EQ(set(target, memory, 1), STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_EQ(set(target, memory, 4), STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_TRUE(target_hidden());
        EXPECT_FALSE(caller_hidden());
        EXPECT_EQ(set(target, 0, 1), STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_EQ(set(target, memory + 1, 1), STATUS_DATATYPE_MISALIGNMENT);
        EXPECT_EQ(set(target, MAX_ALLOCATION_END_EXCL, 1), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(set(make_handle(0), 1, 4), STATUS_DATATYPE_MISALIGNMENT);
        EXPECT_EQ(set(make_handle(0), memory, 1), STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_EQ(set(make_handle(0), UINT64_MAX), STATUS_INVALID_HANDLE);
    }

    TEST_P(ThreadDebuggerFlagTest, QueryWritesOneByteAndUnalignedFourByteReturnLength)
    {
        emu.process.threads.get(target)->debugger_hide = true;
        emulator_object<uint64_t>{emu.memory, memory}.write(0xa5a5a5a5a5a5a5a5ULL);
        emulator_object<uint64_t>{emu.memory, memory + 16}.write(0xccccccccccccccccULL);
        ASSERT_EQ(query(target, memory + 1, 1, memory + 17), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(memory), 0xa5a5a5a5a5a501a5ULL);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(memory + 16), 0xcccccc00000001ccULL);
    }

    TEST_P(ThreadDebuggerFlagTest, QueryProbeAndLengthErrorsPrecedeHandleLookup)
    {
        emulator_object<uint32_t>{emu.memory, memory + 16}.write(0xcccccccc);
        const auto invalid = make_handle(0);
        EXPECT_EQ(query(invalid, memory, 4, memory + 16), STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_EQ(query(invalid, memory + 1, 4, 1), STATUS_DATATYPE_MISALIGNMENT);
        EXPECT_EQ(query(invalid, memory, 0, 1), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(query(invalid, UINT64_MAX, 0, memory + 16), STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_EQ(query(invalid, MAX_ALLOCATION_END_EXCL, 1, memory + 16), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(query(invalid, memory, 1, memory + 16), STATUS_INVALID_HANDLE);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 16), 0xccccccccu);
    }

    TEST_P(ThreadDebuggerFlagTest, QueryReturnsOutputFaultAndPreservesReturnLength)
    {
        emulator_object<uint32_t>{emu.memory, memory + 16}.write(0xcccccccc);
        EXPECT_EQ(query(target, 0, 1, memory + 16), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 16), 0xccccccccu);
        emulator_object<BOOLEAN>{emu.memory, memory + 0x1000}.write(0xa5);
        ASSERT_TRUE(emu.memory.protect_memory(memory + 0x1000, 0x1000, memory_permission::read));
        EXPECT_EQ(query(target, memory + 0x1000, 1, memory + 16), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(emu.memory.read_memory<BOOLEAN>(memory + 0x1000), 0xa5);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 16), 0xccccccccu);
    }

    TEST_P(ThreadDebuggerFlagTest, ReturnLengthFaultPreventsInformationWrite)
    {
        emulator_object<BOOLEAN>{emu.memory, memory}.write(0xa5);
        ASSERT_TRUE(emu.memory.protect_memory(memory + 0x1000, 0x1000, memory_permission::read));
        EXPECT_EQ(query(target, memory, 1, memory + 0x1000), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(query(target, memory, 1, memory + 0xffe), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(query(target, memory, 1, MAX_ALLOCATION_END_EXCL), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(emu.memory.read_memory<BOOLEAN>(memory), 0xa5);
    }

    TEST_P(ThreadDebuggerFlagTest, GuardFaultClearsOnlyTheAccessedPage)
    {
        const nt_memory_permission guarded{memory_permission::read_write, memory_permission_ext::guard};
        ASSERT_TRUE(emu.memory.protect_memory(memory + 0x1000, 0x2000, guarded));
        EXPECT_EQ(query(target, memory + 0x1001, 1), STATUS_GUARD_PAGE_VIOLATION);
        EXPECT_FALSE(emu.memory.get_region_info(memory + 0x1000).permissions.is_guarded());
        EXPECT_TRUE(emu.memory.get_region_info(memory + 0x2000).permissions.is_guarded());
        EXPECT_EQ(query(target, memory + 0x1001, 1), STATUS_SUCCESS);
    }

    TEST_P(ThreadDebuggerFlagTest, ReturnLengthReadPrecedesWriteAndClassValidation)
    {
        emulator_object<BOOLEAN>{emu.memory, memory + 0x2000}.write(0xa5);
        ASSERT_TRUE(emu.memory.protect_memory(memory, 0x1000, memory_permission::read));
        const nt_memory_permission guarded{memory_permission::read, memory_permission_ext::guard};
        ASSERT_TRUE(emu.memory.protect_memory(memory + 0x1000, 0x1000, guarded));
        EXPECT_EQ(query(make_handle(0), memory + 0x2000, 4, memory + 0xffe), STATUS_GUARD_PAGE_VIOLATION);
        EXPECT_FALSE(emu.memory.get_region_info(memory + 0x1000).permissions.is_guarded());
        EXPECT_EQ(emu.memory.read_memory<BOOLEAN>(memory + 0x2000), 0xa5);
        EXPECT_EQ(query(target, memory + 0x2000, 1, memory + 0xffe), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(emu.memory.read_memory<BOOLEAN>(memory + 0x2000), 0xa5);
    }

    TEST_P(ThreadDebuggerFlagTest, ReturnLengthFollowsInformationWhenOutputsOverlap)
    {
        emulator_object<uint64_t>{emu.memory, memory}.write(0xccccccccccccccccULL);
        ASSERT_EQ(query(target, memory + 1, 1, memory + 1), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(memory), 0xcccccc00000001ccULL);
        for (const auto length : {0u, 2u, 4u, 0x1000u})
        {
            EXPECT_EQ(query(target, memory, length, memory + 16), STATUS_INFO_LENGTH_MISMATCH);
        }
    }

    TEST_P(ThreadDebuggerFlagTest, FlagsRemainPerThreadAcrossSnapshotRestore)
    {
        emu.vcpu(0).active_thread->debugger_hide = true;
        utils::buffer_serializer saved{};
        emu.serialize(saved);
        emu.vcpu(0).active_thread->debugger_hide = false;
        emu.process.threads.get(target)->debugger_hide = true;
        utils::buffer_deserializer input{saved.get_buffer()};
        emu.deserialize(input);
        EXPECT_TRUE(caller_hidden());
        EXPECT_FALSE(target_hidden());
        ASSERT_EQ(query(target, memory, 1), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<BOOLEAN>(memory), FALSE);
    }

    INSTANTIATE_TEST_SUITE_P(UserAddressWidths, ThreadDebuggerFlagTest, testing::Values(0x20000000ULL, 0x100000000ULL));
}
