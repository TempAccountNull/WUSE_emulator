#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtReadFile(const syscall_context&, handle, uint64_t, uint64_t, uint64_t,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>, uint64_t, ULONG, emulator_object<LARGE_INTEGER>,
                               emulator_object<ULONG>);
    NTSTATUS handle_NtWriteFile(const syscall_context&, handle, uint64_t, uint64_t, uint64_t,
                                emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>, uint64_t, ULONG, emulator_object<LARGE_INTEGER>,
                                emulator_object<ULONG>);
}

namespace sogen::test
{
    TEST(DirectoryFileIo, ReadRejectsDirectoryHandleBeforeHostFileAccess)
    {
        auto emu = create_empty_emulator();
        auto& vcpu = emu.vcpu(0);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const auto directory = emu.process.files.store(file{});
        ASSERT_TRUE(emu.process.files.get(directory)->is_directory());

        const auto memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{emu.memory, memory};
        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{};
        initial.Information = 17;
        io_status.write(initial);

        for (const uint64_t offset : {0ULL, 0xDEAD0000ULL})
        {
            EXPECT_EQ(
                syscalls::handle_NtReadFile(context, directory, 0, 0, 0, io_status, memory + 0x100, 16, {emu.memory, offset}, {emu.memory}),
                STATUS_INVALID_HANDLE);
            EXPECT_EQ(io_status.read().Information, initial.Information);
        }
    }

    TEST(DirectoryFileIo, WriteRejectsDirectoryHandleBeforeHostFileAccess)
    {
        auto emu = create_empty_emulator();
        auto& vcpu = emu.vcpu(0);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const auto directory = emu.process.files.store(file{});
        ASSERT_TRUE(emu.process.files.get(directory)->is_directory());

        const auto memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{emu.memory, memory};
        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial{};
        initial.Information = 17;
        io_status.write(initial);
        constexpr std::array<char, 4> data{'t', 'e', 's', 't'};
        emu.memory.write_memory(memory + 0x100, data.data(), data.size());

        for (const uint64_t offset : {0ULL, 0xDEAD0000ULL})
        {
            EXPECT_EQ(syscalls::handle_NtWriteFile(context, directory, 0, 0, 0, io_status, memory + 0x100, static_cast<ULONG>(data.size()),
                                                   {emu.memory, offset}, {emu.memory}),
                      STATUS_INVALID_HANDLE);
            EXPECT_EQ(io_status.read().Information, initial.Information);
        }
    }
}
