#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <array>
#include <cstring>
#include <functional>
#include <optional>
#include <stdexcept>

namespace sogen::syscalls
{
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
        io_status.write({.Status = STATUS_PENDING, .Information = 0xDEADBEEF});
        constexpr uint64_t buffer = 0x100;
        constexpr std::string_view payload = "completed file data";
        size_t success_observations = 0;
        memory.after_store = [&] {
            const auto status = io_status.read();
            if (status.Status == STATUS_SUCCESS)
            {
                ++success_observations;
                EXPECT_EQ(status.Information, payload.size());
                std::array<char, payload.size()> completed{};
                memory.read_memory(buffer, completed.data(), completed.size());
                EXPECT_EQ(std::string_view(completed.data(), completed.size()), payload);
            }
        };

        syscalls::commit_file_data(payload, memory, io_status, buffer);

        EXPECT_GT(success_observations, 0);
        EXPECT_EQ(io_status.read().Status, STATUS_SUCCESS);
        EXPECT_EQ(io_status.read().Information, payload.size());
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
}
