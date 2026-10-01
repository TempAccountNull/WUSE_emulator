#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>

namespace sogen::syscalls
{
    void publish_file_read_status(emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>, NTSTATUS, EmulatorTraits<Emu64>::ULONG_PTR);
    void commit_file_data(std::string_view, memory_interface&, emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>, uint64_t);
    NTSTATUS handle_NtReadFile(const syscall_context&, handle, uint64_t, uint64_t, uint64_t,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>, uint64_t, ULONG, emulator_object<LARGE_INTEGER>,
                               emulator_object<ULONG>);
}

namespace sogen::test
{
    namespace
    {
        class observed_file_memory final : public memory_interface
        {
          public:
            std::array<std::byte, 0x400> bytes{};
            std::function<void()> after_store{};
            std::optional<uint64_t> failed_write{};

            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                if (!this->try_read_memory(address, data, size))
                {
                    throw std::out_of_range("Invalid observed file read");
                }
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                if (address > this->bytes.size() || size > this->bytes.size() - address)
                {
                    return false;
                }
                std::memcpy(data, this->bytes.data() + address, size);
                return true;
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (!this->try_write_memory(address, data, size))
                {
                    throw std::out_of_range("Invalid observed file write");
                }
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (address > this->bytes.size() || size > this->bytes.size() - address || this->failed_write == address)
                {
                    return false;
                }
                const auto* source = static_cast<const std::byte*>(data);
                for (size_t i = 0; i < size; ++i)
                {
                    this->bytes[address + i] = source[i];
                    if (this->after_store)
                    {
                        this->after_store();
                    }
                }
                return true;
            }

          private:
            void map_mmio(uint64_t, size_t, mmio_read_callback, mmio_write_callback) override
            {
            }

            void map_memory(uint64_t, size_t, memory_permission) override
            {
            }

            void unmap_memory(uint64_t, size_t) override
            {
            }

            void apply_memory_protection(uint64_t, size_t, memory_permission) override
            {
            }
        };
    }

    TEST(FileReadCompletion, SuccessPublishesCompletedDataAndByteCount)
    {
        observed_file_memory memory{};
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{memory, 0x20};
        io_status.write({.Pointer = 0xAABBCCDD00000103ULL, .Information = 0x1122334455667788ULL});
        constexpr uint64_t buffer = 0x100;
        constexpr std::string_view payload = "completed file data";
        size_t success_observations = 0;
        memory.after_store = [&] {
            const auto status = io_status.read();
            if (status.Status == STATUS_SUCCESS)
            {
                ++success_observations;
                EXPECT_EQ(status.Information, payload.size());
                EXPECT_EQ(status.Pointer >> 32, 0xAABBCCDD);
                std::array<char, payload.size()> completed{};
                memory.read_memory(buffer, completed.data(), completed.size());
                EXPECT_EQ(std::string_view(completed.data(), completed.size()), payload);
            }
        };

        syscalls::commit_file_data(payload, memory, io_status, buffer);

        EXPECT_GT(success_observations, 0);
        EXPECT_EQ(io_status.read().Status, STATUS_SUCCESS);
        EXPECT_EQ(io_status.read().Information, payload.size());
        EXPECT_EQ(io_status.read().Pointer >> 32, 0xAABBCCDD);
    }

    TEST(FileReadCompletion, FailedBufferWriteDoesNotPublishSuccess)
    {
        observed_file_memory memory{};
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{memory, 0x20};
        io_status.write({.Status = STATUS_PENDING, .Information = 17});
        memory.failed_write = 0x100;

        EXPECT_THROW(syscalls::commit_file_data("unwritten", memory, io_status, 0x100), std::out_of_range);

        EXPECT_EQ(io_status.read().Status, STATUS_PENDING);
        EXPECT_EQ(io_status.read().Information, 17);
    }

    TEST(FileReadCompletion, NullStatusBlockStillCopiesData)
    {
        observed_file_memory memory{};
        constexpr std::string_view payload = "no status block";

        syscalls::commit_file_data(payload, memory, {memory, 0}, 0x100);

        std::array<char, payload.size()> completed{};
        memory.read_memory(0x100, completed.data(), completed.size());
        EXPECT_EQ(std::string_view(completed.data(), completed.size()), payload);
    }

    TEST(FileReadCompletion, RegularReadCompletesDataStatusEventAndApc)
    {
        auto emu = create_empty_emulator();
        const auto thread_handle = emu.process.threads.store(emulator_thread{emu.memory});
        auto& vcpu = emu.vcpu(0);
        vcpu.active_thread = emu.process.threads.get(thread_handle);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        FILE* native_file = std::tmpfile();
        ASSERT_NE(native_file, nullptr);
        constexpr std::string_view payload = "read completion";
        ASSERT_EQ(std::fwrite(payload.data(), 1, payload.size(), native_file), payload.size());
        std::rewind(native_file);
        file target{};
        target.handle = native_file;
        const auto file_handle = emu.process.files.store(std::move(target));
        const auto event_handle = emu.process.events.store(event{});
        const auto memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{emu.memory, memory};
        io_status.write({.Status = STATUS_PENDING, .Information = 17});
        constexpr uint64_t apc_routine = 0x1234;
        constexpr uint64_t apc_context = 0x5678;

        EXPECT_EQ(syscalls::handle_NtReadFile(context, file_handle, event_handle.bits, apc_routine, apc_context, io_status, memory + 0x100,
                                              static_cast<ULONG>(payload.size()), {emu.memory, 0}, {emu.memory, 0}),
                  STATUS_SUCCESS);

        std::array<char, payload.size()> completed{};
        emu.memory.read_memory(memory + 0x100, completed.data(), completed.size());
        EXPECT_EQ(std::string_view(completed.data(), completed.size()), payload);
        EXPECT_EQ(io_status.read().Status, STATUS_SUCCESS);
        EXPECT_EQ(io_status.read().Information, payload.size());
        EXPECT_TRUE(emu.process.events.get(event_handle)->signaled);
        ASSERT_EQ(vcpu.thread().pending_apcs.size(), 1);
        const auto& apc = vcpu.thread().pending_apcs.front();
        EXPECT_EQ(apc.apc_routine, apc_routine);
        EXPECT_EQ(apc.apc_argument1, apc_context);
        EXPECT_EQ(apc.apc_argument2, io_status.value());
        EXPECT_EQ(apc.apc_argument3, 0);
    }

    TEST(FileReadCompletion, OverflowingStatusBlockDoesNotWriteMemory)
    {
        observed_file_memory memory{};
        const auto original = memory.bytes;
        size_t stores = 0;
        memory.after_store = [&] { ++stores; };
        constexpr auto last_address = std::numeric_limits<uint64_t>::max();

        for (const auto address : {last_address - 7, last_address - 8, last_address - 1})
        {
            const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{memory, address};
            EXPECT_THROW(syscalls::publish_file_read_status(io_status, STATUS_END_OF_FILE, 17), std::out_of_range);
            EXPECT_EQ(stores, 0);
            EXPECT_EQ(memory.bytes, original);
        }
    }

    TEST(FileReadCompletion, ZeroTransferPublishesInformationBeforeDwordStatus)
    {
        static_assert(sizeof(NTSTATUS) == 4);
        static_assert(sizeof(EmulatorTraits<Emu64>::ULONG_PTR) == 8);
        static_assert(sizeof(IO_STATUS_BLOCK<EmulatorTraits<Emu64>>) == 16);
        static_assert(offsetof(IO_STATUS_BLOCK<EmulatorTraits<Emu64>>, Status) == 0);
        static_assert(offsetof(IO_STATUS_BLOCK<EmulatorTraits<Emu64>>, Information) == 8);
        observed_file_memory memory{};
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{memory, 0x20};

        for (const auto terminal_status : {STATUS_SUCCESS, STATUS_END_OF_FILE})
        {
            io_status.write({.Pointer = 0xAABBCCDD00000103ULL, .Information = 0x1122334455667788ULL});
            size_t completed_observations = 0;
            memory.after_store = [&] {
                const auto status = io_status.read();
                EXPECT_EQ(status.Pointer >> 32, 0xAABBCCDD);
                if (status.Status == terminal_status)
                {
                    ++completed_observations;
                    EXPECT_EQ(status.Information, 0);
                }
            };

            syscalls::publish_file_read_status(io_status, terminal_status, 0);

            EXPECT_GT(completed_observations, 0);
            EXPECT_EQ(io_status.read().Status, terminal_status);
            EXPECT_EQ(io_status.read().Information, 0);
            EXPECT_EQ(io_status.read().Pointer >> 32, 0xAABBCCDD);
            memory.after_store = {};
        }
    }

    TEST(FileReadCompletion, RegularEofRespectsFileModeAndNotificationChannels)
    {
        auto emu = create_empty_emulator();
        const auto thread_handle = emu.process.threads.store(emulator_thread{emu.memory});
        auto& vcpu = emu.vcpu(0);
        vcpu.active_thread = emu.process.threads.get(thread_handle);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        FILE* native_file = std::tmpfile();
        ASSERT_NE(native_file, nullptr);
        constexpr std::string_view payload = "filedata";
        ASSERT_EQ(std::fwrite(payload.data(), 1, payload.size(), native_file), payload.size());
        file target{};
        target.handle = native_file;
        const auto file_handle = emu.process.files.store(std::move(target));
        const auto event_handle = emu.process.events.store(event{});
        const auto memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{emu.memory, memory};
        const emulator_object<LARGE_INTEGER> byte_offset{emu.memory, memory + 0x80};
        constexpr std::array<char, 16> untouched{'u', 'n', 't', 'o', 'u', 'c', 'h', 'e', 'd'};

        struct notification_channel
        {
            bool event;
            bool apc;
        };

        constexpr std::array channels{notification_channel{true, false}, notification_channel{false, true},
                                      notification_channel{true, true}};

        for (const auto mode : std::array<uint32_t, 3>{0, FILE_SYNCHRONOUS_IO_ALERT, FILE_SYNCHRONOUS_IO_NONALERT})
        {
            emu.process.files.get(file_handle)->file_mode = mode;
            for (const auto channel : channels)
            {
                for (const auto offset : {payload.size(), payload.size() + 16})
                {
                    io_status.write({.Pointer = 0xAABBCCDD4A4B4C4DULL, .Information = 0x1122334455667788ULL});
                    LARGE_INTEGER position{};
                    position.QuadPart = static_cast<LONGLONG>(offset);
                    byte_offset.write(position);
                    emu.memory.write_memory(memory + 0x100, untouched.data(), untouched.size());
                    emu.process.events.get(event_handle)->signaled = false;
                    vcpu.thread().pending_apcs.clear();

                    EXPECT_EQ(syscalls::handle_NtReadFile(context, file_handle, channel.event ? event_handle.bits : 0,
                                                          channel.apc ? 0x1234 : 0, memory + 0x200, io_status, memory + 0x100,
                                                          static_cast<ULONG>(untouched.size()), byte_offset, {emu.memory, 0}),
                              mode == 0 ? STATUS_PENDING : STATUS_END_OF_FILE);
                    EXPECT_EQ(io_status.read().Status, STATUS_END_OF_FILE);
                    EXPECT_EQ(io_status.read().Information, 0);
                    EXPECT_EQ(io_status.read().Pointer >> 32, 0xAABBCCDD);
                    if (channel.event)
                    {
                        EXPECT_TRUE(emu.process.events.get(event_handle)->signaled);
                    }
                    else
                    {
                        EXPECT_FALSE(emu.process.events.get(event_handle)->signaled);
                    }
                    const size_t expected_apcs = mode == 0 && channel.apc ? 1 : 0;
                    ASSERT_EQ(vcpu.thread().pending_apcs.size(), expected_apcs);
                    if (expected_apcs != 0)
                    {
                        const auto& apc = vcpu.thread().pending_apcs.front();
                        EXPECT_EQ(apc.apc_routine, 0x1234);
                        EXPECT_EQ(apc.apc_argument1, memory + 0x200);
                        EXPECT_EQ(apc.apc_argument2, io_status.value());
                        EXPECT_EQ(apc.apc_argument3, 0);
                    }
                    std::array<char, untouched.size()> completed{};
                    emu.memory.read_memory(memory + 0x100, completed.data(), completed.size());
                    EXPECT_EQ(completed, untouched);
                }
            }
        }
    }

    TEST(FileReadCompletion, RegularEofPreservesPreviouslyQueuedApc)
    {
        auto emu = create_empty_emulator();
        const auto thread_handle = emu.process.threads.store(emulator_thread{emu.memory});
        auto& vcpu = emu.vcpu(0);
        vcpu.active_thread = emu.process.threads.get(thread_handle);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        FILE* native_file = std::tmpfile();
        ASSERT_NE(native_file, nullptr);
        file target{};
        target.handle = native_file;
        const auto file_handle = emu.process.files.store(std::move(target));
        const auto memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{emu.memory, memory};

        for (const auto mode : std::array<uint32_t, 3>{0, FILE_SYNCHRONOUS_IO_ALERT, FILE_SYNCHRONOUS_IO_NONALERT})
        {
            emu.process.files.get(file_handle)->file_mode = mode;
            io_status.write({.Pointer = 0xAABBCCDD00000103ULL, .Information = 17});
            vcpu.thread().pending_apcs.clear();
            vcpu.thread().pending_apcs.push_back(
                {.apc_routine = 0xAAAA, .apc_argument1 = 0xBBBB, .apc_argument2 = 0xCCCC, .apc_argument3 = 0xDDDD});

            EXPECT_EQ(syscalls::handle_NtReadFile(context, file_handle, 0, 0x1234, 0x5678, io_status, memory + 0x100, 16, {emu.memory, 0},
                                                  {emu.memory, 0}),
                      mode == 0 ? STATUS_PENDING : STATUS_END_OF_FILE);

            ASSERT_EQ(vcpu.thread().pending_apcs.size(), mode == 0 ? 2 : 1);
            const auto& retained = vcpu.thread().pending_apcs.front();
            EXPECT_EQ(retained.apc_routine, 0xAAAA);
            EXPECT_EQ(retained.apc_argument1, 0xBBBB);
            EXPECT_EQ(retained.apc_argument2, 0xCCCC);
            EXPECT_EQ(retained.apc_argument3, 0xDDDD);
            if (mode == 0)
            {
                const auto& appended = vcpu.thread().pending_apcs.back();
                EXPECT_EQ(appended.apc_routine, 0x1234);
                EXPECT_EQ(appended.apc_argument1, 0x5678);
                EXPECT_EQ(appended.apc_argument2, io_status.value());
                EXPECT_EQ(appended.apc_argument3, 0);
            }
            EXPECT_EQ(io_status.read().Status, STATUS_END_OF_FILE);
            EXPECT_EQ(io_status.read().Information, 0);
        }
    }

    TEST(FileReadCompletion, RegularZeroLengthRetainsAcceptedInlineCompletion)
    {
        auto emu = create_empty_emulator();
        const auto thread_handle = emu.process.threads.store(emulator_thread{emu.memory});
        auto& vcpu = emu.vcpu(0);
        vcpu.active_thread = emu.process.threads.get(thread_handle);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        FILE* native_file = std::tmpfile();
        ASSERT_NE(native_file, nullptr);
        ASSERT_EQ(std::fwrite("x", 1, 1, native_file), 1);
        file target{};
        target.handle = native_file;
        const auto file_handle = emu.process.files.store(std::move(target));
        const auto memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{emu.memory, memory};
        const emulator_object<LARGE_INTEGER> byte_offset{emu.memory, memory + 0x80};

        for (const auto mode : std::array<uint32_t, 3>{0, FILE_SYNCHRONOUS_IO_ALERT, FILE_SYNCHRONOUS_IO_NONALERT})
        {
            emu.process.files.get(file_handle)->file_mode = mode;
            for (const auto offset : {0LL, 1LL, 4097LL})
            {
                io_status.write({.Pointer = 0xAABBCCDD00000103ULL, .Information = 0x1122334455667788ULL});
                LARGE_INTEGER position{};
                position.QuadPart = offset;
                byte_offset.write(position);
                vcpu.thread().pending_apcs.clear();

                EXPECT_EQ(
                    syscalls::handle_NtReadFile(context, file_handle, 0, 0x1234, 0x5678, io_status, 0, 0, byte_offset, {emu.memory, 0}),
                    STATUS_SUCCESS);

                EXPECT_EQ(io_status.read().Status, STATUS_SUCCESS);
                EXPECT_EQ(io_status.read().Information, 0);
                EXPECT_EQ(io_status.read().Pointer >> 32, 0xAABBCCDD);
                ASSERT_EQ(vcpu.thread().pending_apcs.size(), 1);
                const auto& apc = vcpu.thread().pending_apcs.front();
                EXPECT_EQ(apc.apc_routine, 0x1234);
                EXPECT_EQ(apc.apc_argument1, 0x5678);
                EXPECT_EQ(apc.apc_argument2, io_status.value());
                EXPECT_EQ(apc.apc_argument3, 0);
            }
        }
    }

    TEST(FileReadCompletion, InvalidHandleRejectsBeforeStatusEventAndApc)
    {
        auto emu = create_empty_emulator();
        const auto thread_handle = emu.process.threads.store(emulator_thread{emu.memory});
        auto& vcpu = emu.vcpu(0);
        vcpu.active_thread = emu.process.threads.get(thread_handle);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const auto event_handle = emu.process.events.store(event{});
        const auto memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{emu.memory, memory};
        io_status.write({.Pointer = 0xAABBCCDD00000103ULL, .Information = 0x1122334455667788ULL});

        EXPECT_EQ(syscalls::handle_NtReadFile(context, {}, event_handle.bits, 0x1234, 0x5678, io_status, memory + 0x100, 16,
                                              {emu.memory, 0}, {emu.memory, 0}),
                  STATUS_INVALID_HANDLE);

        EXPECT_EQ(io_status.read().Pointer, 0xAABBCCDD00000103ULL);
        EXPECT_EQ(io_status.read().Information, 0x1122334455667788ULL);
        EXPECT_FALSE(emu.process.events.get(event_handle)->signaled);
        EXPECT_TRUE(vcpu.thread().pending_apcs.empty());
    }

    TEST(FileReadCompletion, PositiveReadUsesOriginalStatusMemoryInterface)
    {
        observed_file_memory payload_memory{};
        observed_file_memory status_memory{};
        payload_memory.bytes.fill(std::byte{0xA5});
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{status_memory, 0x20};
        io_status.write({.Pointer = 0xAABBCCDD00000103ULL, .Information = 0x1122334455667788ULL});
        constexpr uint64_t buffer = 0x100;
        constexpr std::string_view payload = "separate memory interfaces";
        auto expected_payload = payload_memory.bytes;
        std::memcpy(expected_payload.data() + buffer, payload.data(), payload.size());
        size_t payload_stores = 0;
        size_t status_stores = 0;
        payload_memory.after_store = [&] {
            ++payload_stores;
            EXPECT_EQ(io_status.read().Status, STATUS_PENDING);
        };
        status_memory.after_store = [&] {
            ++status_stores;
            EXPECT_EQ(payload_memory.bytes, expected_payload);
            EXPECT_EQ(io_status.read().Pointer >> 32, 0xAABBCCDD);
            if (io_status.read().Status == STATUS_SUCCESS)
            {
                EXPECT_EQ(io_status.read().Information, payload.size());
            }
        };

        syscalls::commit_file_data(payload, payload_memory, io_status, buffer);

        EXPECT_EQ(payload_stores, payload.size());
        EXPECT_EQ(status_stores, 12);
        EXPECT_EQ(payload_memory.bytes, expected_payload);
        EXPECT_EQ(io_status.read().Status, STATUS_SUCCESS);
        EXPECT_EQ(io_status.read().Information, payload.size());
        EXPECT_EQ(io_status.read().Pointer >> 32, 0xAABBCCDD);
    }

    TEST(FileReadCompletion, ZeroReadUsesOriginalStatusMemoryBeforeNotification)
    {
        auto emu = create_empty_emulator();
        const auto thread_handle = emu.process.threads.store(emulator_thread{emu.memory});
        auto& vcpu = emu.vcpu(0);
        vcpu.active_thread = emu.process.threads.get(thread_handle);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        FILE* native_file = std::tmpfile();
        ASSERT_NE(native_file, nullptr);
        file target{};
        target.handle = native_file;
        const auto file_handle = emu.process.files.store(std::move(target));
        const auto event_handle = emu.process.events.store(event{});
        observed_file_memory status_memory{};
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{status_memory, 0x20};

        for (const auto mode : std::array<uint32_t, 3>{0, FILE_SYNCHRONOUS_IO_ALERT, FILE_SYNCHRONOUS_IO_NONALERT})
        {
            emu.process.files.get(file_handle)->file_mode = mode;
            io_status.write({.Pointer = 0xAABBCCDD00000103ULL, .Information = 0x1122334455667788ULL});
            emu.process.events.get(event_handle)->signaled = false;
            vcpu.thread().pending_apcs.clear();
            size_t status_stores = 0;
            status_memory.after_store = [&] {
                ++status_stores;
                EXPECT_FALSE(emu.process.events.get(event_handle)->signaled);
                EXPECT_TRUE(vcpu.thread().pending_apcs.empty());
                EXPECT_EQ(io_status.read().Pointer >> 32, 0xAABBCCDD);
                if (io_status.read().Status == STATUS_SUCCESS)
                {
                    EXPECT_EQ(io_status.read().Information, 0);
                }
            };

            EXPECT_EQ(syscalls::handle_NtReadFile(context, file_handle, event_handle.bits, 0x1234, 0x5678, io_status, 0, 0, {emu.memory, 0},
                                                  {emu.memory, 0}),
                      STATUS_SUCCESS);

            EXPECT_EQ(status_stores, 12);
            EXPECT_EQ(io_status.read().Status, STATUS_SUCCESS);
            EXPECT_EQ(io_status.read().Information, 0);
            EXPECT_EQ(io_status.read().Pointer >> 32, 0xAABBCCDD);
            EXPECT_TRUE(emu.process.events.get(event_handle)->signaled);
            ASSERT_EQ(vcpu.thread().pending_apcs.size(), 1);
            EXPECT_EQ(vcpu.thread().pending_apcs.front().apc_argument2, io_status.value());
            status_memory.after_store = {};
        }
    }

    TEST(FileReadCompletion, FailedStatusFieldStoresDoNotSignalOrQueueCompletion)
    {
        auto emu = create_empty_emulator();
        const auto thread_handle = emu.process.threads.store(emulator_thread{emu.memory});
        auto& vcpu = emu.vcpu(0);
        vcpu.active_thread = emu.process.threads.get(thread_handle);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        FILE* native_file = std::tmpfile();
        ASSERT_NE(native_file, nullptr);
        constexpr std::string_view payload = "filedata";
        ASSERT_EQ(std::fwrite(payload.data(), 1, payload.size(), native_file), payload.size());
        file target{};
        target.handle = native_file;
        const auto file_handle = emu.process.files.store(std::move(target));
        const auto event_handle = emu.process.events.store(event{});
        const auto memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<LARGE_INTEGER> byte_offset{emu.memory, memory + 0x80};
        observed_file_memory status_memory{};
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{status_memory, 0x20};
        constexpr auto initial_pointer = 0xAABBCCDD00000103ULL;
        constexpr auto initial_information = 0x1122334455667788ULL;
        constexpr std::array<char, 8> untouched{'u', 'n', 't', 'o', 'u', 'c', 'h', 'd'};

        struct read_case
        {
            LONGLONG offset;
            ULONG length;
            uint64_t information;
        };

        constexpr std::array cases{read_case{0, 8, 8}, read_case{8, 8, 0}, read_case{0, 0, 0}};

        for (const auto mode : std::array<uint32_t, 3>{0, FILE_SYNCHRONOUS_IO_ALERT, FILE_SYNCHRONOUS_IO_NONALERT})
        {
            emu.process.files.get(file_handle)->file_mode = mode;
            for (const auto request : cases)
            {
                for (const auto failed_field : {io_status.value() + 8, io_status.value()})
                {
                    status_memory.failed_write.reset();
                    io_status.write({.Pointer = initial_pointer, .Information = initial_information});
                    status_memory.failed_write = failed_field;
                    emu.process.events.get(event_handle)->signaled = false;
                    vcpu.thread().pending_apcs.clear();
                    vcpu.thread().pending_apcs.push_back(
                        {.apc_routine = 0xAAAA, .apc_argument1 = 0xBBBB, .apc_argument2 = 0xCCCC, .apc_argument3 = 0xDDDD});
                    LARGE_INTEGER position{};
                    position.QuadPart = request.offset;
                    byte_offset.write(position);
                    emu.memory.write_memory(memory + 0x100, untouched.data(), untouched.size());
                    size_t status_stores = 0;
                    status_memory.after_store = [&] {
                        ++status_stores;
                        EXPECT_FALSE(emu.process.events.get(event_handle)->signaled);
                        EXPECT_EQ(vcpu.thread().pending_apcs.size(), 1);
                        EXPECT_EQ(io_status.read().Pointer, initial_pointer);
                    };

                    EXPECT_THROW(syscalls::handle_NtReadFile(context, file_handle, event_handle.bits, 0x1234, 0x5678, io_status,
                                                             memory + 0x100, request.length, byte_offset, {emu.memory, 0}),
                                 std::out_of_range);

                    const bool information_failed = failed_field == io_status.value() + 8;
                    EXPECT_EQ(status_stores, information_failed ? 0 : 8);
                    EXPECT_EQ(io_status.read().Pointer, initial_pointer);
                    EXPECT_EQ(io_status.read().Information, information_failed ? initial_information : request.information);
                    EXPECT_FALSE(emu.process.events.get(event_handle)->signaled);
                    ASSERT_EQ(vcpu.thread().pending_apcs.size(), 1);
                    const auto& retained = vcpu.thread().pending_apcs.front();
                    EXPECT_EQ(retained.apc_routine, 0xAAAA);
                    EXPECT_EQ(retained.apc_argument1, 0xBBBB);
                    EXPECT_EQ(retained.apc_argument2, 0xCCCC);
                    EXPECT_EQ(retained.apc_argument3, 0xDDDD);
                    std::array<char, 8> completed{};
                    emu.memory.read_memory(memory + 0x100, completed.data(), completed.size());
                    const auto expected = request.information != 0 ? payload : std::string_view(untouched.data(), untouched.size());
                    EXPECT_EQ(std::string_view(completed.data(), completed.size()), expected);
                    status_memory.after_store = {};
                }
            }
        }
    }
}
