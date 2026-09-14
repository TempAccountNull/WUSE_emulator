#include "emulation_test_utils.hpp"
#include <directory_notifications.hpp>
#include <io_completion_wait.hpp>
#include <syscall_utils.hpp>
#include <fstream>

namespace sogen::syscalls
{
    NTSTATUS handle_NtAssociateWaitCompletionPacket(const syscall_context&, handle, handle, handle, emulator_pointer, emulator_pointer,
                                                    NTSTATUS, uint64_t, emulator_object<BOOLEAN>);
}

namespace sogen::test
{
    class DirectoryNotifications : public testing::Test
    {
      protected:
        windows_emulator emu{create_empty_emulator()};
        std::filesystem::path directory{};
        handle target{};
        uint64_t memory{};

        void SetUp() override
        {
#ifndef OS_WINDOWS
            GTEST_SKIP() << "Windows host directory notification backend";
#endif
            static std::atomic<uint64_t> sequence{};
            directory = std::filesystem::temp_directory_path() /
                        ("sogen-directory-notification-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                         "-" + std::to_string(sequence.fetch_add(1)));
            ASSERT_TRUE(std::filesystem::create_directory(directory));
            file object{};
            object.host_path = directory;
            object.name = directory.u16string();
            object.access_mask = FILE_LIST_DIRECTORY;
            target = emu.process.files.store(std::move(object));
            memory = emu.memory.allocate_memory(0x2000, memory_permission::read_write);
        }

        void TearDown() override
        {
            if (!directory.empty())
            {
                emu.process.directory_notifications.close(emu, target);
                std::filesystem::remove_all(directory);
            }
        }

        NTSTATUS begin(uint32_t length = 512, uint32_t filter = 1, bool watch_tree = false, handle event = {})
        {
            return emu.process.directory_notifications.begin(
                emu, target, {.event = event, .io_status_block = memory, .output_buffer = length ? memory + 0x100 : 0, .length = length},
                filter, watch_tree);
        }

        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> iosb()
        {
            return emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(memory);
        }

        void change(std::string_view name = "changed.txt")
        {
            std::ofstream output(directory / name);
            output << "directory notification";
        }

        bool await_completion()
        {
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            do
            {
                emu.process.directory_notifications.process_completions(emu);
                if (emu.process.directory_notifications.is_signaled(target))
                {
                    return true;
                }
                std::this_thread::sleep_for(1ms);
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        }
    };

    TEST_F(DirectoryNotifications, PendingRequestDoesNotProduceWaitCompletion)
    {
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status{emu.memory, memory};
        status.write({.Status = 0xabababab, .Information = 0x12345678});
        ASSERT_EQ(begin(32, 3, true), STATUS_PENDING);
        EXPECT_EQ(iosb().Status, 0xababababu);
        EXPECT_EQ(iosb().Information, 0x12345678u);
        EXPECT_FALSE(emu.process.directory_notifications.is_signaled(target));

        auto& vcpu = emu.vcpu(0);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const auto port = emu.process.io_completions.store(io_completion{});
        const auto packet = emu.process.wait_completion_packets.store(wait_completion_packet{});
        const emulator_object<BOOLEAN> signaled{emu.memory, memory + 0x80};
        signaled.write(TRUE);
        ASSERT_EQ(
            syscalls::handle_NtAssociateWaitCompletionPacket(context, packet, port, target, 0x123, 0x456, STATUS_SUCCESS, 0x789, signaled),
            STATUS_SUCCESS);
        EXPECT_EQ(signaled.read(), FALSE);
        io_completion_message message{};
        for (int i = 0; i < 100; ++i)
        {
            emu.process.directory_notifications.process_completions(emu);
            EXPECT_FALSE(io_completion_wait::dequeue_io_completion_message(emu.process, port, message));
        }
        change();
        ASSERT_TRUE(await_completion());
        EXPECT_EQ(iosb().Status, static_cast<NTSTATUS>(0x10c));
        ASSERT_TRUE(io_completion_wait::dequeue_io_completion_message(emu.process, port, message));
        EXPECT_EQ(message.key_context, 0x123u);
        EXPECT_EQ(message.apc_context, 0x456u);
        EXPECT_EQ(message.io_status_block.Information, 0x789u);
        EXPECT_FALSE(io_completion_wait::dequeue_io_completion_message(emu.process, port, message));
    }

    TEST_F(DirectoryNotifications, WritesFilenameAndSignalsEventAfterChange)
    {
        event signal{};
        signal.signaled = true;
        const auto event_handle = emu.process.events.store(std::move(signal));
        ASSERT_EQ(begin(512, 1, false, event_handle), STATUS_PENDING);
        EXPECT_FALSE(emu.process.events.get(event_handle)->signaled);
        EXPECT_EQ(emu.process.events.get(event_handle)->ref_count, 2u);
        change();
        ASSERT_TRUE(await_completion());
        EXPECT_EQ(iosb().Status, STATUS_SUCCESS);
        EXPECT_EQ(iosb().Information, 34u);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x104), 1u);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x108), 22u);
        const auto name = emu.memory.read_memory<std::array<char16_t, 11>>(memory + 0x10c);
        EXPECT_EQ(std::u16string(name.begin(), name.end()), u"changed.txt");
        EXPECT_TRUE(emu.process.events.get(event_handle)->signaled);
        EXPECT_EQ(emu.process.events.get(event_handle)->ref_count, 1u);
        ASSERT_EQ(begin(), STATUS_PENDING);
        EXPECT_FALSE(emu.process.directory_notifications.is_signaled(target));
    }

    TEST_F(DirectoryNotifications, ZeroBufferCompletesWithNotifyEnumDir)
    {
        ASSERT_EQ(begin(0), STATUS_PENDING);
        change();
        ASSERT_TRUE(await_completion());
        EXPECT_EQ(iosb().Status, static_cast<NTSTATUS>(0x10c));
        EXPECT_EQ(iosb().Information, 0u);
    }

    TEST_F(DirectoryNotifications, CancelCompletesPendingRequest)
    {
        ASSERT_EQ(begin(), STATUS_PENDING);
        ASSERT_EQ(emu.process.directory_notifications.cancel(emu, target, memory), STATUS_SUCCESS);
        EXPECT_TRUE(emu.process.directory_notifications.is_signaled(target));
        EXPECT_EQ(iosb().Status, static_cast<NTSTATUS>(0xc0000120));
        EXPECT_EQ(iosb().Information, 0u);
        EXPECT_EQ(emu.process.directory_notifications.cancel(emu, target, memory), STATUS_NOT_FOUND);
    }

    TEST_F(DirectoryNotifications, PendingRequestSurvivesSnapshotRoundTrip)
    {
        ASSERT_EQ(begin(), STATUS_PENDING);
        utils::buffer_serializer buffer{};
        emu.process.directory_notifications.serialize(buffer);
        emu.process.directory_notifications.close(emu, target);
        utils::buffer_deserializer input{buffer.get_buffer()};
        emu.process.directory_notifications.deserialize(input);
        EXPECT_FALSE(emu.process.directory_notifications.is_signaled(target));
        emu.process.directory_notifications.process_completions(emu);
        change();
        ASSERT_TRUE(await_completion());
        EXPECT_EQ(iosb().Status, STATUS_SUCCESS);
        EXPECT_EQ(iosb().Information, 34u);
    }

    TEST_F(DirectoryNotifications, NonRecursiveWatchIgnoresDescendantChanges)
    {
        std::filesystem::create_directory(directory / "child");
        ASSERT_EQ(begin(512, 1, false), STATUS_PENDING);
        change("child/inside.txt");
        for (int i = 0; i < 10; ++i)
        {
            emu.process.directory_notifications.process_completions(emu);
            EXPECT_FALSE(emu.process.directory_notifications.is_signaled(target));
            std::this_thread::sleep_for(1ms);
        }
        change();
        ASSERT_TRUE(await_completion());
        EXPECT_EQ(iosb().Status, STATUS_SUCCESS);
    }

    TEST_F(DirectoryNotifications, RejectsUnmappedOutputBeforeStartingRequest)
    {
        EXPECT_EQ(emu.process.directory_notifications.begin(
                      emu, target, {.io_status_block = memory, .output_buffer = UINT64_MAX - 4, .length = 32}, 1, false),
                  STATUS_ACCESS_VIOLATION);
        EXPECT_TRUE(emu.process.directory_notifications.is_signaled(target));
    }

    TEST_F(DirectoryNotifications, FullSnapshotRearmsPendingRequestBeforeGuestRuns)
    {
        ASSERT_EQ(begin(), STATUS_PENDING);
        utils::buffer_serializer output{};
        emu.serialize(output);
        utils::buffer_deserializer input{output.get_buffer()};
        emu.deserialize(input);
        EXPECT_FALSE(emu.process.directory_notifications.is_signaled(target));
        change();
        ASSERT_TRUE(await_completion());
        EXPECT_EQ(iosb().Status, STATUS_SUCCESS);
        EXPECT_EQ(iosb().Information, 34u);
    }

    TEST_F(DirectoryNotifications, Wow64CompletionKeepsFollowingMemoryIntact)
    {
        emu.process.is_wow64_process = true;
        const uint64_t sentinel = 0x123456789abcdef0;
        emu.memory.write_memory(memory + 8, &sentinel, sizeof(sentinel));
        ASSERT_EQ(begin(), STATUS_PENDING);
        change();
        ASSERT_TRUE(await_completion());
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 4), 34u);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(memory + 8), sentinel);
    }

    TEST_F(DirectoryNotifications, ReleasesWatchWhenRetainedFileReferenceDisappears)
    {
        ASSERT_EQ(begin(), STATUS_PENDING);
        const auto retained = emu.process.files.duplicate(target);
        ASSERT_TRUE(retained.has_value());
        emu.process.files.erase(target);
        emu.process.files.erase(*retained);
        ASSERT_EQ(emu.process.files.get(target), nullptr);
        emu.process.directory_notifications.process_completions(emu);
        EXPECT_TRUE(emu.process.directory_notifications.is_signaled(target));
        EXPECT_EQ(iosb().Status, static_cast<NTSTATUS>(0xc0000120));
        EXPECT_EQ(emu.process.directory_notifications.cancel(emu, target), STATUS_NOT_FOUND);
    }

    TEST_F(DirectoryNotifications, AcceptsGenericReadDirectoryAccess)
    {
        emu.process.files.get(target)->access_mask = GENERIC_READ;
        ASSERT_EQ(begin(), STATUS_PENDING);
        change();
        ASSERT_TRUE(await_completion());
        EXPECT_EQ(iosb().Status, STATUS_SUCCESS);
    }
}
