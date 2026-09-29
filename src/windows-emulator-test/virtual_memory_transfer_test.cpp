#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <array>

namespace sogen::syscalls
{
    NTSTATUS handle_NtReadVirtualMemory(const syscall_context&, handle, emulator_pointer, emulator_pointer, SIZE_T,
                                        emulator_object<SIZE_T>);
    NTSTATUS handle_NtWriteVirtualMemory(const syscall_context&, handle, emulator_pointer, emulator_pointer, SIZE_T,
                                         emulator_object<SIZE_T>);
}

namespace sogen::test
{
    class VirtualMemoryTransferTest : public testing::Test
    {
      protected:
        windows_emulator emu{create_empty_emulator()};
        uint64_t source{};
        uint64_t destination{};
        uint64_t count{};

        void SetUp() override
        {
            source = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            destination = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            count = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(source, 0U);
            ASSERT_NE(destination, 0U);
            ASSERT_NE(count, 0U);
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }
    };

    TEST_F(VirtualMemoryTransferTest, ReadWritesFullSizeTCount)
    {
        constexpr std::array<uint8_t, 5> bytes{1, 2, 3, 4, 5};
        emu.memory.write_memory(source, bytes.data(), bytes.size());
        constexpr uint64_t sentinel = 0xa5a5a5a500000000ULL;
        emu.memory.write_memory(count, &sentinel, sizeof(sentinel));

        EXPECT_EQ(syscalls::handle_NtReadVirtualMemory(context(), CURRENT_PROCESS, source, destination, bytes.size(), {emu.memory, count}),
                  STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(count), bytes.size());
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(destination + i), bytes[i]);
        }
    }

    TEST_F(VirtualMemoryTransferTest, ZeroLengthReadClearsFullSizeTCount)
    {
        constexpr uint64_t sentinel = 0xa5a5a5a5ffffffffULL;
        emu.memory.write_memory(count, &sentinel, sizeof(sentinel));

        EXPECT_EQ(syscalls::handle_NtReadVirtualMemory(context(), CURRENT_PROCESS, source, destination, 0, {emu.memory, count}),
                  STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(count), 0U);
    }

    TEST_F(VirtualMemoryTransferTest, WriteWritesFullSizeTCount)
    {
        constexpr std::array<uint8_t, 5> bytes{6, 7, 8, 9, 10};
        emu.memory.write_memory(source, bytes.data(), bytes.size());
        constexpr uint64_t sentinel = 0xa5a5a5a500000000ULL;
        emu.memory.write_memory(count, &sentinel, sizeof(sentinel));

        EXPECT_EQ(syscalls::handle_NtWriteVirtualMemory(context(), CURRENT_PROCESS, destination, source, bytes.size(), {emu.memory, count}),
                  STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(count), bytes.size());
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(destination + i), bytes[i]);
        }
    }
}
