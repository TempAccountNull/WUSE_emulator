#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtOpenEvent(const syscall_context&, emulator_object<uint64_t>, ACCESS_MASK,
                                emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>);
    NTSTATUS handle_NtCreateEvent(const syscall_context&, emulator_object<handle>, ACCESS_MASK,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>, EVENT_TYPE, BOOLEAN);
    NTSTATUS handle_NtQueryEvent(const syscall_context&, handle, uint32_t, emulator_object<EVENT_BASIC_INFORMATION>, uint32_t,
                                 emulator_object<uint32_t>);
    NTSTATUS handle_NtClose(const syscall_context&, handle);
}

namespace sogen::test
{
    class KernelMemoryEventTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t buffer{};

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            buffer = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            set_name(u"\\KernelObjects\\MaximumCommitCondition");
        }

        void set_name(const std::u16string_view name, const ULONG flags = 0)
        {
            using traits = EmulatorTraits<Emu64>;
            emu.memory.write_memory(buffer + 0x100, name.data(), name.size() * sizeof(char16_t));
            UNICODE_STRING<traits> descriptor{};
            descriptor.Length = static_cast<USHORT>(name.size() * sizeof(char16_t));
            descriptor.MaximumLength = descriptor.Length;
            descriptor.Buffer = buffer + 0x100;
            emu.memory.write_memory(buffer + 0x80, &descriptor, sizeof(descriptor));
            OBJECT_ATTRIBUTES<traits> attributes{};
            attributes.Length = sizeof(attributes);
            attributes.Attributes = flags;
            attributes.ObjectName = buffer + 0x80;
            emu.memory.write_memory(buffer + 0x40, &attributes, sizeof(attributes));
        }

        NTSTATUS open()
        {
            return syscalls::handle_NtOpenEvent(context(), {emu.memory, buffer}, 0x100001, {emu.memory, buffer + 0x40});
        }

        handle opened()
        {
            return make_handle(emu.emu().read_memory<uint64_t>(buffer));
        }
    };

    TEST_F(KernelMemoryEventTest, OpensQueryableNonsignaledNotificationEvent)
    {
        ASSERT_EQ(open(), STATUS_SUCCESS);
        const auto h = opened();
        EXPECT_FALSE(h.value.is_pseudo);
        ASSERT_NE(emu.process.events.get(h), nullptr);
        EXPECT_EQ(emu.process.events.get(h)->ref_count, 2u);
        const emulator_object<EVENT_BASIC_INFORMATION> info{emu.memory, buffer + 0x200};
        const emulator_object<uint32_t> length{emu.memory, buffer + 0x220};
        ASSERT_EQ(syscalls::handle_NtQueryEvent(context(), h, 0, info, sizeof(EVENT_BASIC_INFORMATION), length), STATUS_SUCCESS);
        EXPECT_EQ(info.read().EventType, NotificationEvent);
        EXPECT_EQ(info.read().EventState, 0);
        EXPECT_EQ(length.read(), sizeof(EVENT_BASIC_INFORMATION));
    }

    TEST_F(KernelMemoryEventTest, KernelReferenceSurvivesLastUserCloseAndReopen)
    {
        ASSERT_EQ(open(), STATUS_SUCCESS);
        const auto first = opened();
        ASSERT_EQ(open(), STATUS_SUCCESS);
        EXPECT_EQ(opened(), first);
        EXPECT_EQ(emu.process.events.get(first)->ref_count, 3u);
        EXPECT_EQ(syscalls::handle_NtClose(context(), first), STATUS_SUCCESS);
        EXPECT_EQ(syscalls::handle_NtClose(context(), first), STATUS_SUCCESS);
        ASSERT_NE(emu.process.events.get(first), nullptr);
        EXPECT_EQ(emu.process.events.get(first)->ref_count, 1u);
        ASSERT_EQ(open(), STATUS_SUCCESS);
        EXPECT_EQ(opened(), first);
        EXPECT_EQ(emu.process.events.get(first)->ref_count, 2u);
    }

    TEST_F(KernelMemoryEventTest, CreateCannotReplaceKernelTypeOrInitialState)
    {
        ASSERT_EQ(syscalls::handle_NtCreateEvent(context(), {emu.memory, buffer}, 0x100001, {emu.memory, buffer + 0x40},
                                                 SynchronizationEvent, TRUE),
                  STATUS_OBJECT_NAME_EXISTS);
        const auto* event = emu.process.events.get(opened());
        ASSERT_NE(event, nullptr);
        EXPECT_EQ(event->type, NotificationEvent);
        EXPECT_FALSE(event->signaled);
    }

    TEST_F(KernelMemoryEventTest, ObjectNameCaseFollowsCallerAttributes)
    {
        set_name(u"\\kerNELobjects\\maximumcommitcondition");
        EXPECT_EQ(open(), STATUS_NOT_FOUND);
        set_name(u"\\kerNELobjects\\maximumcommitcondition", 0x40);
        ASSERT_EQ(open(), STATUS_SUCCESS);
        const auto first = opened();
        set_name(u"\\KernelObjects\\MaximumCommitCondition");
        ASSERT_EQ(open(), STATUS_SUCCESS);
        EXPECT_EQ(opened(), first);
        set_name(u"\\KernelObjects\\UnknownMemoryEvent", 0x40);
        EXPECT_EQ(open(), STATUS_NOT_FOUND);
    }

    TEST_F(KernelMemoryEventTest, WaitBlocksUntilSignaledAndNotificationDoesNotReset)
    {
        ASSERT_EQ(open(), STATUS_SUCCESS);
        const auto h = opened();
        auto& thread = *emu.vcpu(0).active_thread;
        thread.await_objects = {h};
        thread.await_any = true;
        EXPECT_FALSE(thread.is_thread_ready(emu));
        emu.process.events.get(h)->signaled = true;
        EXPECT_TRUE(thread.is_thread_ready(emu));
        EXPECT_TRUE(emu.process.events.get(h)->signaled);
        thread.await_objects = {h};
        thread.await_any = false;
        EXPECT_TRUE(thread.is_thread_ready(emu));
        EXPECT_TRUE(emu.process.events.get(h)->signaled);
    }

    TEST_F(KernelMemoryEventTest, SnapshotPreservesKernelReferenceAndNotificationState)
    {
        ASSERT_EQ(open(), STATUS_SUCCESS);
        const auto h = opened();
        EXPECT_EQ(syscalls::handle_NtClose(context(), h), STATUS_SUCCESS);
        emu.process.events.get(h)->signaled = true;
        utils::buffer_serializer data{};
        emu.process.events.serialize(data);
        utils::buffer_deserializer restore{data};
        auto fresh = create_empty_emulator();
        fresh.process.events.deserialize(restore);
        const auto* restored = fresh.process.events.get(h);
        ASSERT_NE(restored, nullptr);
        EXPECT_EQ(restored->ref_count, 1u);
        EXPECT_EQ(restored->type, NotificationEvent);
        EXPECT_TRUE(restored->signaled);
        EXPECT_EQ(restored->name, u"\\KernelObjects\\MaximumCommitCondition");
    }
}
