#include "emulation_test_utils.hpp"
#include <io_completion_wait.hpp>
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtAssociateWaitCompletionPacket(const syscall_context&, handle, handle, handle, emulator_pointer, emulator_pointer,
                                                    NTSTATUS, uint64_t, emulator_object<BOOLEAN>);
    NTSTATUS handle_NtCancelWaitCompletionPacket(const syscall_context&, handle, BOOLEAN);
}

namespace sogen::test
{
    class FileWaitCompletion : public testing::TestWithParam<bool>
    {
    };

    TEST_P(FileWaitCompletion, DeliversOnePacketAndRetainsFileUntilDequeued)
    {
        auto emu = create_empty_emulator();
        auto& process = emu.process;
        auto& vcpu = emu.vcpu(0);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = process};
        file target{};
        if (!GetParam())
        {
            target.handle = std::tmpfile();
            ASSERT_TRUE(target.handle);
        }
        const auto file_handle = process.files.store(std::move(target));
        const auto port_handle = process.io_completions.store(io_completion{});
        const auto packet_handle = process.wait_completion_packets.store(wait_completion_packet{});
        const auto memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<BOOLEAN> already_signaled{emu.memory, memory};
        already_signaled.write(FALSE);

        ASSERT_EQ(syscalls::handle_NtAssociateWaitCompletionPacket(context, packet_handle, port_handle, file_handle, 0x123, 0x456,
                                                                   STATUS_SUCCESS, 0x789, already_signaled),
                  STATUS_SUCCESS);
        EXPECT_EQ(already_signaled.read(), TRUE);
        EXPECT_EQ(process.files.get(file_handle)->ref_count, 2u);
        EXPECT_EQ(syscalls::handle_NtAssociateWaitCompletionPacket(context, packet_handle, port_handle, file_handle, 0, 0, STATUS_SUCCESS,
                                                                   0, already_signaled),
                  STATUS_INVALID_PARAMETER);
        process.files.erase(file_handle);
        ASSERT_NE(process.files.get(file_handle), nullptr);

        io_completion_message message{};
        ASSERT_TRUE(io_completion_wait::dequeue_io_completion_message(process, port_handle, message));
        EXPECT_EQ(message.key_context, 0x123u);
        EXPECT_EQ(message.apc_context, 0x456u);
        EXPECT_EQ(message.io_status_block.Status, STATUS_SUCCESS);
        EXPECT_EQ(message.io_status_block.Information, 0x789u);
        EXPECT_EQ(process.files.get(file_handle), nullptr);
        EXPECT_FALSE(io_completion_wait::dequeue_io_completion_message(process, port_handle, message));
        EXPECT_FALSE(process.wait_completion_packets.get(packet_handle)->associated);
    }

    TEST_P(FileWaitCompletion, CancelRemovesQueuedCompletionAndReleasesFile)
    {
        auto emu = create_empty_emulator();
        auto& process = emu.process;
        auto& vcpu = emu.vcpu(0);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = process};
        file target{};
        if (!GetParam())
        {
            target.handle = std::tmpfile();
            ASSERT_TRUE(target.handle);
        }
        const auto file_handle = process.files.store(std::move(target));
        const auto port_handle = process.io_completions.store(io_completion{});
        const auto packet_handle = process.wait_completion_packets.store(wait_completion_packet{});
        ASSERT_EQ(syscalls::handle_NtAssociateWaitCompletionPacket(context, packet_handle, port_handle, file_handle, 0x123, 0x456,
                                                                   STATUS_SUCCESS, 0x789, {emu.memory}),
                  STATUS_SUCCESS);
        ASSERT_EQ(syscalls::handle_NtCancelWaitCompletionPacket(context, packet_handle, TRUE), STATUS_SUCCESS);
        EXPECT_EQ(process.files.get(file_handle)->ref_count, 1u);
        io_completion_message message{};
        EXPECT_FALSE(io_completion_wait::dequeue_io_completion_message(process, port_handle, message));
        EXPECT_EQ(syscalls::handle_NtAssociateWaitCompletionPacket(context, packet_handle, port_handle,
                                                                   make_handle(0x777, handle_types::file, false), 0, 0, STATUS_SUCCESS, 0,
                                                                   {emu.memory}),
                  STATUS_INVALID_HANDLE);
        EXPECT_FALSE(process.wait_completion_packets.get(packet_handle)->associated);
    }

    INSTANTIATE_TEST_SUITE_P(FileAndDirectory, FileWaitCompletion, testing::Bool());
}
