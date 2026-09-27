#include "emulation_test_utils.hpp"
#include <io_device.hpp>
#include <syscall_utils.hpp>
#include <array>

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetInformationFile(const syscall_context&, handle, emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>, uint64_t,
                                         ULONG, FILE_INFORMATION_CLASS);
    NTSTATUS handle_NtClose(const syscall_context&, handle);
    NTSTATUS handle_NtCancelIoFile(const syscall_context&, handle, emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>);
    NTSTATUS handle_NtCancelIoFileEx(const syscall_context&, handle, uint64_t, emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>);
}

namespace sogen::test
{
    class DeviceCompletionTest : public testing::TestWithParam<bool>
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t memory{};
        handle device{};
        handle port{};

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            emu.process.is_wow64_process = GetParam();
            memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(memory, 0u);
            device = emu.process.devices.store(io_device_container{u"DeviceApi\\Dev\\Query", emu, {}});
            port = emu.process.io_completions.store(io_completion{});
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        NTSTATUS associate(const FILE_INFORMATION_CLASS info_class = FileCompletionInformation)
        {
            const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> iosb{emu.memory, memory + 0x100};
            if (GetParam())
            {
                const FILE_COMPLETION_INFORMATION<EmulatorTraits<Emu32>> info{.Port = static_cast<uint32_t>(port.bits), .Key = 0x1234};
                emu.memory.write_memory(memory, &info, sizeof(info));
                return syscalls::handle_NtSetInformationFile(context(), device, iosb, memory, sizeof(info), info_class);
            }
            const FILE_COMPLETION_INFORMATION<EmulatorTraits<Emu64>> info{.Port = port.bits, .Key = 0x1234};
            emu.memory.write_memory(memory, &info, sizeof(info));
            return syscalls::handle_NtSetInformationFile(context(), device, iosb, memory, sizeof(info), info_class);
        }

        io_device_context request()
        {
            io_device_context c{emu.memory};
            c.file_handle = device;
            c.apc_context = 0x9876;
            c.io_status_block = {emu.memory, memory + 0x200};
            c.io_status_block.write({.Status = STATUS_SUCCESS, .Information = 7});
            return c;
        }
    };

    TEST_P(DeviceCompletionTest, AssociationUsesGuestLayoutAndRetainsPortAcrossDuplicateClose)
    {
        ASSERT_EQ(associate(), STATUS_SUCCESS);
        auto* endpoint = emu.process.devices.get(device);
        ASSERT_NE(endpoint, nullptr);
        EXPECT_EQ(endpoint->completion_port, port);
        EXPECT_EQ(endpoint->completion_key, 0x1234u);
        ASSERT_TRUE(emu.process.devices.duplicate(device).has_value());
        ASSERT_EQ(syscalls::handle_NtClose(context(), port), STATUS_SUCCESS);
        ASSERT_NE(emu.process.io_completions.get(port), nullptr);
        ASSERT_EQ(syscalls::handle_NtClose(context(), device), STATUS_SUCCESS);
        ASSERT_NE(emu.process.io_completions.get(port), nullptr);
        ASSERT_EQ(syscalls::handle_NtClose(context(), device), STATUS_SUCCESS);
        EXPECT_EQ(emu.process.io_completions.get(port), nullptr);
    }

    TEST_P(DeviceCompletionTest, AssociationRejectsInvalidInputAndRequiresReplaceToChangePort)
    {
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> iosb{emu.memory, memory + 0x100};
        EXPECT_EQ(syscalls::handle_NtSetInformationFile(context(), device, iosb, memory, 1, FileCompletionInformation),
                  STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_EQ(syscalls::handle_NtSetInformationFile(context(), device, iosb, 0x1, 16, FileCompletionInformation),
                  STATUS_ACCESS_VIOLATION);
        ASSERT_EQ(associate(), STATUS_SUCCESS);
        EXPECT_EQ(associate(), STATUS_INVALID_PARAMETER);
        const auto replacement = emu.process.io_completions.store(io_completion{});
        port = replacement;
        EXPECT_EQ(associate(FileReplaceCompletionInformation), STATUS_SUCCESS);
        EXPECT_EQ(emu.process.devices.get(device)->completion_port, replacement);
        const FILE_COMPLETION_INFORMATION<EmulatorTraits<Emu64>> invalid{.Port = 0xdeadbeef, .Key = 0};
        emu.memory.write_memory(memory, &invalid, sizeof(invalid));
        if (!GetParam())
        {
            EXPECT_EQ(
                syscalls::handle_NtSetInformationFile(context(), device, iosb, memory, sizeof(invalid), FileReplaceCompletionInformation),
                STATUS_INVALID_HANDLE);
        }
    }

    TEST_P(DeviceCompletionTest, ImmediateAndDeferredCompletionsCarryKeyContextStatusAndInformation)
    {
        ASSERT_EQ(associate(), STATUS_SUCCESS);
        auto c = request();
        complete_device_ioctl(emu, c, STATUS_SUCCESS, true);
        auto* queue = emu.process.io_completions.get(port);
        ASSERT_NE(queue, nullptr);
        ASSERT_EQ(queue->queue.size(), 1u);
        EXPECT_EQ(queue->queue.front().key_context, 0x1234u);
        EXPECT_EQ(queue->queue.front().apc_context, 0x9876u);
        EXPECT_EQ(queue->queue.front().io_status_block.Information, 7u);
        queue->queue.clear();

        const FILE_IO_COMPLETION_NOTIFICATION_INFORMATION flags{.Flags = 1};
        emu.memory.write_memory(memory, &flags, sizeof(flags));
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> iosb{emu.memory, memory + 0x100};
        ASSERT_EQ(
            syscalls::handle_NtSetInformationFile(context(), device, iosb, memory, sizeof(flags), FileIoCompletionNotificationInformation),
            STATUS_SUCCESS);
        complete_device_ioctl(emu, c, STATUS_SUCCESS, true);
        EXPECT_TRUE(queue->queue.empty());
        c.io_status_block.write({.Status = STATUS_NOT_SUPPORTED, .Information = 0});
        complete_device_ioctl(emu, c, STATUS_NOT_SUPPORTED, false);
        ASSERT_EQ(queue->queue.size(), 1u);
        EXPECT_EQ(queue->queue.front().io_status_block.Status, STATUS_NOT_SUPPORTED);
        queue->queue.clear();
        complete_device_ioctl(emu, c, STATUS_NOT_SUPPORTED, true);
        EXPECT_TRUE(queue->queue.empty());

        const FILE_IO_COMPLETION_NOTIFICATION_INFORMATION unsupported{.Flags = 2};
        emu.memory.write_memory(memory, &unsupported, sizeof(unsupported));
        EXPECT_EQ(syscalls::handle_NtSetInformationFile(context(), device, iosb, memory, sizeof(unsupported),
                                                        FileIoCompletionNotificationInformation),
                  STATUS_NOT_SUPPORTED);
    }

    TEST_P(DeviceCompletionTest, ApcIsQueuedOnIssuingThreadWithIoStatus)
    {
        auto c = request();
        auto& issuer = *emu.vcpu(0).active_thread;
        c.issuer_thread_id = issuer.id;
        c.apc_routine = 0x12345000;
        c.apc_context = 0x7890;
        complete_device_ioctl(emu, c, STATUS_SUCCESS, false);
        ASSERT_FALSE(issuer.pending_apcs.empty());
        const auto& apc = issuer.pending_apcs.back();
        EXPECT_EQ(apc.apc_routine, 0x12345000u);
        EXPECT_EQ(apc.apc_argument1, 0x7890u);
        EXPECT_EQ(apc.apc_argument2, c.io_status_block.value());
        EXPECT_EQ(apc.io_information, 7u);
    }

    TEST_P(DeviceCompletionTest, SynchronousAfdCompletionSignalsEventWithoutQueueingApc)
    {
        const std::array<uint32_t, 12> creation{0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 6, 0};
        emu.memory.write_memory(memory + 0x400, creation.data(), sizeof(creation));
        const auto afd =
            emu.process.devices.store(io_device_container{u"Afd\\Endpoint", emu, {.buffer = memory + 0x400, .length = sizeof(creation)}});

        event signal{};
        signal.type = SynchronizationEvent;
        const auto event_handle = emu.process.events.store(std::move(signal));

        auto c = request();
        auto& issuer = *emu.vcpu(0).active_thread;
        c.file_handle = afd;
        c.issuer_thread_id = issuer.id;
        c.event = event_handle;
        c.apc_routine = 0x12345000;
        complete_device_ioctl(emu, c, STATUS_SUCCESS, true);

        EXPECT_TRUE(emu.process.events.get(event_handle)->signaled);
        EXPECT_TRUE(issuer.pending_apcs.empty());
        EXPECT_EQ(c.io_status_block.read().Information, 7u);
    }

    TEST_P(DeviceCompletionTest, CancelSyscallsAcceptDevicesAndReportNoPendingRequest)
    {
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> iosb{emu.memory, memory + 0x100};
        iosb.write({.Status = STATUS_SUCCESS, .Information = 99});
        EXPECT_EQ(syscalls::handle_NtCancelIoFileEx(context(), device, memory + 0x200, iosb), STATUS_NOT_FOUND);
        EXPECT_EQ(iosb.read().Status, STATUS_NOT_FOUND);
        EXPECT_EQ(iosb.read().Information, 0u);
        EXPECT_EQ(syscalls::handle_NtCancelIoFile(context(), device, iosb), STATUS_SUCCESS);
        EXPECT_EQ(iosb.read().Status, STATUS_SUCCESS);
        EXPECT_EQ(iosb.read().Information, 0u);
        EXPECT_EQ(syscalls::handle_NtCancelIoFileEx(context(), {}, 0, iosb), STATUS_INVALID_HANDLE);
    }

    INSTANTIATE_TEST_SUITE_P(GuestBitness, DeviceCompletionTest, testing::Bool());
}
