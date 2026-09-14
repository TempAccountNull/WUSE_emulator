#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiDdDDISetQueuedLimit(const syscall_context&, emulator_object<EMU_D3DKMT_SETQUEUEDLIMIT>);
    NTSTATUS handle_NtGdiDdDDIGetDeviceState(const syscall_context&, emulator_object<EMU_D3DKMT_GETDEVICESTATE>);
    NTSTATUS handle_NtGdiDdDDICreateDevice(const syscall_context&, emulator_object<EMU_D3DKMT_CREATEDEVICE>);
}

namespace sogen::test
{
    class GraphicsQueueLimitTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t memory{};

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        NTSTATUS run(const uint32_t type, const uint32_t value, const uint32_t device = 0x5000)
        {
            const EMU_D3DKMT_SETQUEUEDLIMIT request{.hDevice = device, .Type = type, .QueuedPresentLimit = value, .Reserved = 0xa5a5a5a5};
            emu.memory.write_memory(memory, &request, sizeof(request));
            return syscalls::handle_NtGdiDdDDISetQueuedLimit(context(), {emu.memory, memory});
        }

        auto result() const
        {
            return emu.memory.read_memory<EMU_D3DKMT_SETQUEUEDLIMIT>(memory);
        }
    };

    TEST_F(GraphicsQueueLimitTest, SetAndGetPreserveOtherInputBytes)
    {
        ASSERT_EQ(run(1, 1), STATUS_SUCCESS);
        EXPECT_EQ(result().QueuedPresentLimit, 1u);
        ASSERT_EQ(run(2, 0xcccccccc), STATUS_SUCCESS);
        EXPECT_EQ(result().hDevice, 0x5000u);
        EXPECT_EQ(result().Type, 2u);
        EXPECT_EQ(result().QueuedPresentLimit, 1u);
        EXPECT_EQ(result().Reserved, 0xa5a5a5a5u);
    }

    TEST_F(GraphicsQueueLimitTest, ZeroResetsDefaultAndGetDoesNotSet)
    {
        ASSERT_EQ(run(2, 8), STATUS_SUCCESS);
        EXPECT_EQ(result().QueuedPresentLimit, 3u);
        ASSERT_EQ(run(1, 7), STATUS_SUCCESS);
        ASSERT_EQ(run(1, 0), STATUS_SUCCESS);
        EXPECT_EQ(result().QueuedPresentLimit, 0u);
        ASSERT_EQ(run(2, 0), STATUS_SUCCESS);
        EXPECT_EQ(result().QueuedPresentLimit, 3u);
    }

    TEST_F(GraphicsQueueLimitTest, RejectsInvalidDeviceTypeAndNullWithoutChangingLimit)
    {
        ASSERT_EQ(run(1, 2), STATUS_SUCCESS);
        EXPECT_EQ(run(1, 8, 0), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(run(1, 8, 0x5001), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(run(0, 8), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(run(3, 8), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(syscalls::handle_NtGdiDdDDISetQueuedLimit(context(), {emu.memory, 0}), STATUS_INVALID_PARAMETER);
        ASSERT_EQ(run(2, 0), STATUS_SUCCESS);
        EXPECT_EQ(result().QueuedPresentLimit, 2u);
    }

    TEST_F(GraphicsQueueLimitTest, CreatingDeviceResetsDefault)
    {
        ASSERT_EQ(run(1, 1), STATUS_SUCCESS);
        const EMU_D3DKMT_CREATEDEVICE device{.hAdapter = 0x4000};
        emu.memory.write_memory(memory + 0x100, &device, sizeof(device));
        ASSERT_EQ(syscalls::handle_NtGdiDdDDICreateDevice(context(), {emu.memory, memory + 0x100}), STATUS_SUCCESS);
        ASSERT_EQ(run(2, 0), STATUS_SUCCESS);
        EXPECT_EQ(result().QueuedPresentLimit, 3u);
    }

    TEST_F(GraphicsQueueLimitTest, ExecutionStateIsActiveAndDoesNotOverwriteQueryOrUnionTail)
    {
        std::array<uint8_t, 56> bytes{};
        bytes.fill(0xa5);
        emu.memory.write_memory(memory, bytes.data(), bytes.size());
        const EMU_D3DKMT_GETDEVICESTATE request{.hDevice = 0x5000, .StateType = 1, .State = 0xcccccccc};
        emu.memory.write_memory(memory, &request, sizeof(request));
        ASSERT_EQ(syscalls::handle_NtGdiDdDDIGetDeviceState(context(), {emu.memory, memory}), STATUS_SUCCESS);
        const auto result = emu.memory.read_memory<EMU_D3DKMT_GETDEVICESTATE>(memory);
        EXPECT_EQ(result.hDevice, request.hDevice);
        EXPECT_EQ(result.StateType, request.StateType);
        EXPECT_EQ(result.DeviceExecutionState, 1u);
        emu.memory.read_memory(memory, bytes.data(), bytes.size());
        for (size_t index = sizeof(request); index < bytes.size(); ++index)
        {
            EXPECT_EQ(bytes[index], 0xa5u);
        }
    }

    TEST_F(GraphicsQueueLimitTest, ResetQueryDoesNotUseExecutionStateEnumeration)
    {
        const EMU_D3DKMT_GETDEVICESTATE request{.hDevice = 0x5000, .StateType = 3, .State = 0xcccccccc};
        emu.memory.write_memory(memory, &request, sizeof(request));
        ASSERT_EQ(syscalls::handle_NtGdiDdDDIGetDeviceState(context(), {emu.memory, memory}), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<EMU_D3DKMT_GETDEVICESTATE>(memory).ResetState, 0u);
    }

    TEST_F(GraphicsQueueLimitTest, LimitSurvivesFullSnapshotRestore)
    {
        ASSERT_EQ(run(1, 7), STATUS_SUCCESS);
        utils::buffer_serializer output{};
        emu.serialize(output);
        ASSERT_EQ(run(1, 1), STATUS_SUCCESS);
        utils::buffer_deserializer input{output.get_buffer()};
        emu.deserialize(input);
        ASSERT_EQ(run(2, 0), STATUS_SUCCESS);
        EXPECT_EQ(result().QueuedPresentLimit, 7u);
    }
}
