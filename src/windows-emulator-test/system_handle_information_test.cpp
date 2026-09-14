#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <system_handle_information.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQuerySystemInformation(const syscall_context&, uint32_t, uint64_t, uint32_t, emulator_object<uint32_t>);
    NTSTATUS handle_NtQueryObject(const syscall_context&, handle, OBJECT_INFORMATION_CLASS, emulator_pointer, ULONG,
                                  emulator_object<ULONG>);
    NTSTATUS handle_NtClose(const syscall_context&, handle);
}

namespace sogen::test
{
    class SystemHandleInformationTest : public testing::Test
    {
      protected:
        windows_emulator emu = create_empty_emulator();
        uint64_t output{};
        uint64_t returned_size{};
        std::vector<handle> handles{};

        void SetUp() override
        {
            output = emu.memory.allocate_memory(0x2000, memory_permission::read_write);
            returned_size = output + 0x1000;
            handles = {emu.process.events.store(event{}), emu.process.sections.store(section{}), emu.process.semaphores.store(semaphore{}),
                       emu.process.mutants.store(mutant{}), emu.process.timers.store(timer{})};
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        NTSTATUS query(const uint32_t length)
        {
            return syscalls::handle_NtQuerySystemInformation(context(), SystemExtendedHandleInformation, output, length,
                                                             {emu.memory, returned_size});
        }

        uint32_t required()
        {
            return emu.memory.read_memory<uint32_t>(returned_size);
        }

        std::vector<system_handle_information_entry> entries()
        {
            const auto header = emu.memory.read_memory<system_handle_information_header>(output);
            std::vector<system_handle_information_entry> result(header.number_of_handles);
            emu.memory.read_memory(output + sizeof(header), result.data(), result.size() * sizeof(result[0]));
            return result;
        }

        void fill_output()
        {
            std::array<uint8_t, 0x1000> guard{};
            guard.fill(0xa5);
            emu.memory.write_memory(output, guard.data(), guard.size());
            emu.emu().write_memory<uint32_t>(returned_size, 0xdeadbeef);
        }
    };

    TEST_F(SystemHandleInformationTest, ShortHeaderReturnsMismatchWithoutWritingBuffer)
    {
        fill_output();
        EXPECT_EQ(query(15), STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_EQ(required(), 0u);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(output), 0xa5a5a5a5a5a5a5a5ULL);
        EXPECT_EQ(syscalls::handle_NtQuerySystemInformation(context(), SystemExtendedHandleInformation, 0, 0, {emu.memory, returned_size}),
                  STATUS_INFO_LENGTH_MISMATCH);
    }

    TEST_F(SystemHandleInformationTest, PartialBufferReportsAllHandlesAndPreservesTail)
    {
        fill_output();
        EXPECT_EQ(query(56), STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_EQ(required(), 16u + 40u * (handles.size() + 1));
        const auto header = emu.memory.read_memory<system_handle_information_header>(output);
        EXPECT_EQ(header.number_of_handles, handles.size() + 1);
        EXPECT_EQ(header.reserved, 0u);
        EXPECT_EQ(emu.memory.read_memory<system_handle_information_entry>(output + 16).handle_value, GUEST_PROCESS_HANDLE.bits);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(output + 56), 0xa5a5a5a5a5a5a5a5ULL);
    }

    TEST_F(SystemHandleInformationTest, EnumeratesGuestHandlesAndKeepsObjectTypesConsistent)
    {
        fill_output();
        const auto length = static_cast<uint32_t>(16 + 40 * (handles.size() + 1));
        ASSERT_EQ(query(length), STATUS_SUCCESS);
        EXPECT_EQ(required(), length);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(output + length), 0xa5a5a5a5a5a5a5a5ULL);
        const auto values = entries();
        std::set<uint64_t> objects;
        std::set<uint64_t> actual_handles;
        for (const auto& entry : values)
        {
            EXPECT_EQ(entry.process_id, process_context::process_id);
            EXPECT_EQ(entry.granted_access, GENERIC_ALL);
            EXPECT_EQ(entry.creator_backtrace_index, 0);
            EXPECT_EQ(entry.handle_attributes, 0u);
            EXPECT_EQ(entry.reserved, 0u);
            EXPECT_EQ(entry.object >> 48, 0xffffu);
            EXPECT_TRUE(objects.insert(entry.object).second);
            EXPECT_TRUE(actual_handles.insert(entry.handle_value).second);
            const auto handle = make_handle(entry.handle_value);
            ASSERT_EQ(syscalls::handle_NtQueryObject(context(), handle, ObjectTypeInformation, output + 0x1100, 0x400,
                                                     {emu.memory, returned_size}),
                      STATUS_SUCCESS);
            const auto type = emu.memory.read_memory<OBJECT_TYPE_INFORMATION>(output + 0x1100);
            EXPECT_EQ(type.TypeIndex, entry.object_type_index);
        }
        EXPECT_TRUE(actual_handles.contains(GUEST_PROCESS_HANDLE.bits));
        for (const auto h : handles)
        {
            EXPECT_TRUE(actual_handles.contains(h.bits));
        }
    }

    TEST_F(SystemHandleInformationTest, ClosedHandlesDisappearAndSnapshotRestoresInventory)
    {
        ASSERT_EQ(query(0x1000), STATUS_SUCCESS);
        const auto before = entries();
        utils::buffer_serializer saved{};
        emu.serialize(saved);
        ASSERT_EQ(syscalls::handle_NtClose(context(), handles.front()), STATUS_SUCCESS);
        ASSERT_EQ(query(0x1000), STATUS_SUCCESS);
        const auto closed = entries();
        EXPECT_EQ(closed.size(), before.size() - 1);
        EXPECT_TRUE(std::ranges::none_of(closed, [&](const auto& entry) { return entry.handle_value == handles.front().bits; }));
        utils::buffer_deserializer restore(saved);
        emu.deserialize(restore);
        ASSERT_EQ(query(0x1000), STATUS_SUCCESS);
        const auto after = entries();
        ASSERT_EQ(after.size(), before.size());
        EXPECT_EQ(memcmp(after.data(), before.data(), before.size() * sizeof(before.front())), 0);
    }

    TEST_F(SystemHandleInformationTest, InvalidOutputReturnsAccessViolationAndOptionalLengthIsAccepted)
    {
        EXPECT_EQ(
            syscalls::handle_NtQuerySystemInformation(context(), SystemExtendedHandleInformation, 0, 0x1000, {emu.memory, returned_size}),
            STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(syscalls::handle_NtQuerySystemInformation(context(), SystemExtendedHandleInformation, output, 0x1000, {emu.memory, 0}),
                  STATUS_SUCCESS);
    }
}
