#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetInformationThread(const syscall_context&, handle, THREADINFOCLASS, uint64_t, uint32_t);
    NTSTATUS handle_NtQueryInformationThread(const syscall_context&, handle, uint32_t, uint64_t, uint32_t, emulator_object<uint32_t>);
}

namespace sogen::test
{
    class ThreadIdealProcessorTest : public testing::Test
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
            memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            target = emu.process.create_thread(emu.memory, emu.mod_manager.executable->entry_point, 0, 0x10000, 0);
            emu.process.kusd.access([](KUSER_SHARED_DATA64& value) { value.ActiveProcessorCount = 4; });
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        NTSTATUS set(uint32_t processor, uint32_t length = 4)
        {
            emu.emu().write_memory<uint32_t>(memory, processor);
            return syscalls::handle_NtSetInformationThread(context(), target, ThreadIdealProcessor, memory, length);
        }

        uint32_t ideal()
        {
            return emu.process.threads.get(target)->ideal_processor;
        }
    };

    TEST_F(ThreadIdealProcessorTest, ReturnsPreviousProcessorAndPreservesInput)
    {
        ASSERT_EQ(set(2), 0u);
        ASSERT_EQ(set(1), 2u);
        EXPECT_EQ(ideal(), 1u);
        EXPECT_EQ(emu.emu().read_memory<uint32_t>(memory), 1u);
        EXPECT_EQ(emu.vcpu(0).active_thread->ideal_processor, 0u);
    }

    TEST_F(ThreadIdealProcessorTest, QueryAndInactiveProcessorKeepPreference)
    {
        ASSERT_EQ(set(2), 0u);
        EXPECT_EQ(set(64), 2u);
        EXPECT_EQ(set(63), 2u);
        EXPECT_EQ(ideal(), 2u);
    }

    TEST_F(ThreadIdealProcessorTest, RejectsInvalidValueLengthAndInput)
    {
        ASSERT_EQ(set(2), 0u);
        EXPECT_EQ(set(65), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(set(UINT32_MAX), STATUS_INVALID_PARAMETER);
        for (const auto length : {0u, 3u, 8u})
        {
            EXPECT_EQ(set(1, length), STATUS_INFO_LENGTH_MISMATCH);
        }
        EXPECT_EQ(syscalls::handle_NtSetInformationThread(context(), target, ThreadIdealProcessor, 0, 4), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(syscalls::handle_NtSetInformationThread(context(), target, ThreadIdealProcessor, memory + 1, 4),
                  STATUS_DATATYPE_MISALIGNMENT);
        EXPECT_EQ(ideal(), 2u);
    }

    TEST_F(ThreadIdealProcessorTest, QueriesProcessorNumberAndPreservesBufferOnBadLength)
    {
        ASSERT_EQ(set(2), 0u);
        emu.emu().write_memory<uint64_t>(memory, 0xa5a5a5a5aaaaaaaaULL);
        const emulator_object<uint32_t> length{emu.memory, memory + 16};
        length.write(0xcccccccc);
        EXPECT_EQ(syscalls::handle_NtQueryInformationThread(context(), target, ThreadIdealProcessorEx, memory, 8, length),
                  STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_EQ(length.read(), 0xccccccccu);
        EXPECT_EQ(syscalls::handle_NtQueryInformationThread(context(), target, ThreadIdealProcessorEx, memory, 4, length), STATUS_SUCCESS);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(memory), 0xa5a5a5a500020000ULL);
        EXPECT_EQ(length.read(), 4u);
        EXPECT_EQ(syscalls::handle_NtQueryInformationThread(context(), target, ThreadIdealProcessor, memory, 4, length),
                  STATUS_INVALID_INFO_CLASS);
    }

    TEST_F(ThreadIdealProcessorTest, PreferenceSurvivesFullSnapshotRestore)
    {
        ASSERT_EQ(set(2), 0u);
        utils::buffer_serializer output{};
        emu.serialize(output);
        ASSERT_EQ(set(1), 2u);
        utils::buffer_deserializer input{output.get_buffer()};
        emu.deserialize(input);
        EXPECT_EQ(set(64), 2u);
    }
}
