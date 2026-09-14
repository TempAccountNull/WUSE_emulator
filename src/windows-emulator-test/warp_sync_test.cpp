#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiDdDDICreateContext(const syscall_context&, emulator_object<EMU_D3DKMT_CREATECONTEXT>);
    NTSTATUS handle_NtGdiDdDDIRender(const syscall_context&, emulator_object<EMU_D3DKMT_RENDER>);
    NTSTATUS handle_NtGdiDdDDIDestroyContext(const syscall_context&, emulator_object<uint32_t>);
    NTSTATUS handle_NtSetEvent(const syscall_context&, uint64_t, emulator_object<LONG>);
}

namespace sogen::test
{
    class WarpSyncTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t memory{};

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            const EMU_D3DKMT_CREATECONTEXT request{.hDevice = 0x5000};
            emu.memory.write_memory(memory, &request, sizeof(request));
            ASSERT_EQ(syscalls::handle_NtGdiDdDDICreateContext(context(), {emu.memory, memory}), STATUS_SUCCESS);
        }

        handle create_event(const EVENT_TYPE type = SynchronizationEvent)
        {
            event entry{};
            entry.type = type;
            return emu.process.events.store(std::move(entry));
        }

        NTSTATUS submit(const uint64_t signal = 0, const uint64_t finish = 0, const uint32_t offset = 0, const uint32_t length = 24,
                        const uint64_t header = 0x18434E5953)
        {
            const std::array<uint64_t, 4> command{header, signal, finish, 0x123456789abcdef0};
            emu.memory.write_memory(emu.process.dxgk.command_buffer.address, command.data(), sizeof(command));
            const EMU_D3DKMT_RENDER request{.hContext = 0x6000, .CommandOffset = offset, .CommandLength = length};
            emu.memory.write_memory(memory, &request, sizeof(request));
            return syscalls::handle_NtGdiDdDDIRender(context(), {emu.memory, memory});
        }

        uint64_t header() const
        {
            return emu.memory.read_memory<uint64_t>(emu.process.dxgk.command_buffer.address);
        }

        NTSTATUS signal(const handle value)
        {
            return syscalls::handle_NtSetEvent(context(), value.bits, {emu.memory, 0});
        }
    };

    TEST_F(WarpSyncTest, AcknowledgementPreservesHandlesAndCanary)
    {
        const auto start = create_event();
        const auto finish = create_event();
        ASSERT_EQ(submit(start.bits, finish.bits), STATUS_SUCCESS);
        const auto data = emu.memory.read_memory<std::array<uint64_t, 4>>(emu.process.dxgk.command_buffer.address);
        EXPECT_EQ(data[0], 0x84B415953u);
        EXPECT_EQ(data[1], start.bits);
        EXPECT_EQ(data[2], finish.bits);
        EXPECT_EQ(data[3], 0x123456789abcdef0u);
        EXPECT_TRUE(emu.process.events.get(start)->signaled);
        EXPECT_FALSE(emu.process.events.get(finish)->signaled);
        EXPECT_EQ(emu.process.events.get(start)->ref_count, 2u);
        EXPECT_EQ(emu.memory.read_memory<EMU_D3DKMT_RENDER>(memory).QueuedBufferCount, 1u);
        ASSERT_EQ(signal(finish), STATUS_SUCCESS);
        EXPECT_TRUE(emu.process.dxgk.pending_sync_commands.empty());
        EXPECT_FALSE(emu.process.events.get(finish)->signaled);
        EXPECT_EQ(emu.process.events.get(start)->ref_count, 1u);
        EXPECT_EQ(emu.process.events.get(finish)->ref_count, 1u);
    }

    TEST_F(WarpSyncTest, LaterCommandsWaitForEarlierFinish)
    {
        const auto first = create_event();
        const auto finish = create_event();
        const auto second = create_event();
        ASSERT_EQ(submit(first.bits, finish.bits), STATUS_SUCCESS);
        ASSERT_EQ(submit(second.bits), STATUS_SUCCESS);
        EXPECT_FALSE(emu.process.events.get(second)->signaled);
        EXPECT_EQ(emu.process.dxgk.pending_sync_commands.size(), 2u);
        ASSERT_EQ(signal(finish), STATUS_SUCCESS);
        EXPECT_TRUE(emu.process.events.get(second)->signaled);
        EXPECT_TRUE(emu.process.dxgk.pending_sync_commands.empty());
    }

    TEST_F(WarpSyncTest, NotificationFinishRemainsSignaled)
    {
        const auto finish = create_event(NotificationEvent);
        ASSERT_EQ(signal(finish), STATUS_SUCCESS);
        ASSERT_EQ(submit(0, finish.bits), STATUS_SUCCESS);
        EXPECT_TRUE(emu.process.dxgk.pending_sync_commands.empty());
        EXPECT_TRUE(emu.process.events.get(finish)->signaled);
    }

    TEST_F(WarpSyncTest, SameEventForSignalAndFinishReleasesBothReferences)
    {
        const auto value = create_event();
        ASSERT_EQ(submit(value.bits, value.bits), STATUS_SUCCESS);
        EXPECT_TRUE(emu.process.dxgk.pending_sync_commands.empty());
        EXPECT_EQ(emu.process.events.get(value)->ref_count, 1u);
        EXPECT_FALSE(emu.process.events.get(value)->signaled);
    }

    TEST_F(WarpSyncTest, InvalidEventLeavesPacketAndValidEventUntouched)
    {
        const auto start = create_event();
        EXPECT_EQ(submit(start.bits, 0x180ffff), STATUS_INVALID_HANDLE);
        EXPECT_EQ(header(), 0x18434E5953u);
        EXPECT_EQ(emu.process.events.get(start)->ref_count, 1u);
        EXPECT_FALSE(emu.process.events.get(start)->signaled);
        EXPECT_TRUE(emu.process.dxgk.pending_sync_commands.empty());
    }

    TEST_F(WarpSyncTest, RejectsTruncationRangeOverflowAndUnknownPacket)
    {
        EXPECT_EQ(submit(0, 0, 0, 16), STATUS_NOT_SUPPORTED);
        EXPECT_EQ(submit(0, 0, 25, 24), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(submit(0, 0, 0, 0xffffffff), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(submit(0, 0, 0, 24, 0x1801020304), STATUS_NOT_SUPPORTED);
        EXPECT_EQ(header(), 0x1801020304u);
        EXPECT_TRUE(emu.process.dxgk.pending_sync_commands.empty());
        EXPECT_EQ(syscalls::handle_NtGdiDdDDIRender(context(), {emu.memory, 0}), STATUS_INVALID_PARAMETER);
    }

    TEST_F(WarpSyncTest, ClosedUserEventStaysAliveUntilCommandCompletes)
    {
        const auto start = create_event();
        const auto finish = create_event();
        ASSERT_EQ(submit(start.bits, finish.bits), STATUS_SUCCESS);
        ASSERT_TRUE(emu.process.events.erase(start));
        ASSERT_NE(emu.process.events.get(start), nullptr);
        ASSERT_EQ(signal(finish), STATUS_SUCCESS);
        EXPECT_EQ(emu.process.events.get(start), nullptr);
    }

    TEST_F(WarpSyncTest, SnapshotPreservesPendingOrderAndDoesNotRepeatStartSignal)
    {
        const auto start = create_event();
        const auto finish = create_event();
        const auto second = create_event();
        ASSERT_EQ(submit(start.bits, finish.bits), STATUS_SUCCESS);
        ASSERT_EQ(submit(second.bits), STATUS_SUCCESS);
        emu.process.events.get(start)->signaled = false;
        utils::buffer_serializer output{};
        emu.serialize(output);
        ASSERT_EQ(signal(finish), STATUS_SUCCESS);
        utils::buffer_deserializer input{output.get_buffer()};
        emu.deserialize(input);
        emu.process.process_graphics_commands();
        EXPECT_FALSE(emu.process.events.get(start)->signaled);
        EXPECT_FALSE(emu.process.events.get(second)->signaled);
        EXPECT_EQ(emu.process.dxgk.pending_sync_commands.size(), 2u);
        ASSERT_EQ(signal(finish), STATUS_SUCCESS);
        EXPECT_TRUE(emu.process.events.get(second)->signaled);
        EXPECT_TRUE(emu.process.dxgk.pending_sync_commands.empty());
    }

    TEST_F(WarpSyncTest, LegacySnapshotLeavesFollowingAslrStateIntact)
    {
        utils::buffer_serializer output{};
        emu.serialize(output);
        auto legacy = output.move_buffer();
        utils::buffer_serializer extension{};
        extension.write<uint64_t>(0x31434E5953504744);
        extension.write<uint64_t>(0);
        const auto& bytes = extension.get_buffer();
        const auto found = std::ranges::search(legacy, bytes);
        ASSERT_FALSE(found.empty());
        legacy.erase(found.begin(), found.end());
        utils::buffer_deserializer input{legacy};
        EXPECT_NO_THROW(emu.deserialize(input));
        EXPECT_TRUE(emu.process.dxgk.pending_sync_commands.empty());
        EXPECT_EQ(input.get_remaining_size(), 0u);
    }

    TEST_F(WarpSyncTest, DestroyContextDiscardsCommandsAndSignalsUnstartedWork)
    {
        const auto start = create_event();
        const auto finish = create_event();
        const auto second = create_event();
        ASSERT_EQ(submit(start.bits, finish.bits), STATUS_SUCCESS);
        ASSERT_EQ(submit(second.bits), STATUS_SUCCESS);
        const uint32_t context_handle = 0x6000;
        emu.memory.write_memory(memory, &context_handle, sizeof(context_handle));
        ASSERT_EQ(syscalls::handle_NtGdiDdDDIDestroyContext(context(), {emu.memory, memory}), STATUS_SUCCESS);
        EXPECT_TRUE(emu.process.dxgk.pending_sync_commands.empty());
        EXPECT_TRUE(emu.process.events.get(second)->signaled);
        EXPECT_EQ(emu.process.events.get(finish)->ref_count, 1u);
        EXPECT_EQ(emu.process.events.get(second)->ref_count, 1u);
    }
}
