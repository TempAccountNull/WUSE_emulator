#define NOMINMAX

#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <syscall_utils.hpp>

#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

namespace sogen::syscalls
{
    NTSTATUS handle_NtReadFile(const syscall_context&, handle, uint64_t, uint64_t, uint64_t,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>, uint64_t, ULONG, emulator_object<LARGE_INTEGER>,
                               emulator_object<ULONG>);
    NTSTATUS handle_NtWaitForSingleObject(const syscall_context&, handle, BOOLEAN, emulator_object<LARGE_INTEGER>);
    NTSTATUS handle_NtSuspendThread(const syscall_context&, handle, emulator_object<ULONG>);
    NTSTATUS handle_NtResumeThread(const syscall_context&, handle, emulator_object<ULONG>);
    NTSTATUS handle_NtContinueEx(const syscall_context&, emulator_object<CONTEXT64>, uint64_t);
}

namespace sogen::test
{
    namespace
    {
        std::vector<std::byte> serialized_thread(const emulator_thread& thread)
        {
            utils::buffer_serializer buffer;
            thread.serialize_object(buffer);
            return buffer.move_buffer();
        }

        void queue_regular_file_apc(windows_emulator& emu, vcpu_context& vcpu, const uint64_t scratch, const uint64_t routine)
        {
            FILE* native_file = std::tmpfile();
            ASSERT_NE(native_file, nullptr);
            file target{};
            target.handle = native_file;
            constexpr std::string_view payload = "queued file completion";
            ASSERT_EQ(std::fwrite(payload.data(), 1, payload.size(), native_file), payload.size());
            std::rewind(native_file);
            const auto file_handle = emu.process.files.store(std::move(target));
            const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> iosb{emu.memory, scratch};
            iosb.write({.Status = STATUS_PENDING, .Information = 17});
            const auto status = emu.dispatch_on_cpu(vcpu.cpu, [&] {
                const syscall_context context{emu, vcpu.cpu, vcpu, emu.process};
                return syscalls::handle_NtReadFile(context, file_handle, 0, routine, scratch + 0x80, iosb, scratch + 0x100,
                                                   static_cast<ULONG>(payload.size()), {emu.memory, 0}, {emu.memory, 0});
            });
            ASSERT_EQ(status, STATUS_SUCCESS);
            EXPECT_EQ(iosb.read().Status, STATUS_SUCCESS);
            EXPECT_EQ(iosb.read().Information, payload.size());
            std::array<char, payload.size()> bytes{};
            emu.memory.read_memory(scratch + 0x100, bytes.data(), bytes.size());
            EXPECT_EQ(std::string_view(bytes.data(), bytes.size()), payload);
            ASSERT_EQ(vcpu.thread().pending_apcs.size(), 1U);
            const auto& apc = vcpu.thread().pending_apcs.front();
            EXPECT_EQ(apc.apc_routine, routine);
            EXPECT_EQ(apc.apc_argument1, scratch + 0x80);
            EXPECT_EQ(apc.apc_argument2, scratch);
            EXPECT_EQ(apc.apc_argument3, 0U);
        }
    }

    TEST(AlertableFileApcScheduler, SuspendedIssuerPreservesCompletionUntilResume)
    {
        emulator_settings settings{.disable_logging = true, .use_relative_time = true};
        settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
        auto emu = create_sample_emulator(std::move(settings));
        emu.setup_process_if_necessary();
        auto& vcpu = emu.vcpu(0);
        auto* const fallback = vcpu.active_thread;
        ASSERT_NE(fallback, nullptr);
        ASSERT_TRUE(fallback->is_thread_ready(emu));
        ASSERT_NE(emu.process.ki_user_apc_dispatcher, 0U);
        std::array<std::byte, 16> dispatcher_bytes{};
        ASSERT_TRUE(emu.memory.try_read_memory(emu.process.ki_user_apc_dispatcher, dispatcher_bytes.data(), dispatcher_bytes.size()));

        const auto code = emu.mod_manager.executable->entry_point;
        const uint8_t ret = 0xC3;
        emu.memory.write_memory(code + 32, &ret, sizeof(ret));
        const auto reader_handle = emu.process.create_thread(emu.memory, code + 16, 0, 0x10000, 0);
        auto* const reader = emu.process.threads.get(reader_handle);
        ASSERT_NE(reader, nullptr);
        ASSERT_TRUE(emu.activate_thread(vcpu, reader->id));
        const auto interrupted_sp = reader->stack_base + reader->stack_size - 0x100;
        vcpu.cpu.reg(x86_register::rip, code + 16);
        vcpu.cpu.reg(x86_register::rsp, interrupted_sp);
        vcpu.cpu.reg(x86_register::r12, 0x123456789ABCDEF0ULL);
        const auto scratch = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(scratch, 0U);
        ASSERT_NO_FATAL_FAILURE(queue_regular_file_apc(emu, vcpu, scratch, code + 32));
        ASSERT_TRUE(fallback->pending_apcs.empty());

        std::array<std::byte, 0x800> stack_before{};
        stack_before.fill(std::byte{0xA5});
        emu.memory.write_memory(interrupted_sp - stack_before.size(), stack_before.data(), stack_before.size());
        const auto wait_handle = emu.process.events.store(event{});
        ASSERT_EQ(emu.dispatch_on_cpu(vcpu.cpu,
                                      [&] {
                                          const syscall_context context{emu, vcpu.cpu, vcpu, emu.process};
                                          return syscalls::handle_NtWaitForSingleObject(context, wait_handle, TRUE, {emu.memory, 0});
                                      }),
                  STATUS_SUCCESS);
        ASSERT_TRUE(reader->apc_alertable);
        ASSERT_TRUE(emu.activate_thread(vcpu, fallback->id));
        const auto previous_count = emulator_object<ULONG>{emu.memory, scratch + 0x60};
        ASSERT_EQ(emu.dispatch_on_cpu(vcpu.cpu,
                                      [&] {
                                          const syscall_context context{emu, vcpu.cpu, vcpu, emu.process};
                                          return syscalls::handle_NtSuspendThread(context, reader_handle, previous_count);
                                      }),
                  STATUS_SUCCESS);
        ASSERT_EQ(previous_count.read(), 0U);
        ASSERT_EQ(reader->suspended, 1U);
        ASSERT_FALSE(reader->is_thread_ready(emu));
        ASSERT_FALSE(reader->pending_status.has_value());
        const auto reader_before = serialized_thread(*reader);
        const auto fallback_before = vcpu.cpu.save_registers();
        size_t dispatches{};
        uint32_t dispatch_tid{};
        emu.callbacks.on_generic_activity = [&](const std::string_view description) {
            if (description == "APC Dispatch")
            {
                ++dispatches;
                dispatch_tid = emu.current_thread().id;
            }
        };

        ASSERT_TRUE(emu.perform_thread_switch(vcpu));
        ASSERT_EQ(vcpu.active_thread, fallback);
        EXPECT_EQ(vcpu.cpu.save_registers(), fallback_before);
        EXPECT_EQ(serialized_thread(*reader), reader_before);
        EXPECT_EQ(dispatches, 0U);
        std::array<std::byte, 0x800> stack_after{};
        emu.memory.read_memory(interrupted_sp - stack_after.size(), stack_after.data(), stack_after.size());
        EXPECT_EQ(stack_after, stack_before);
        EXPECT_FALSE(emu.process.events.get(wait_handle)->signaled);

        ASSERT_EQ(emu.dispatch_on_cpu(vcpu.cpu,
                                      [&] {
                                          const syscall_context context{emu, vcpu.cpu, vcpu, emu.process};
                                          return syscalls::handle_NtResumeThread(context, reader_handle, previous_count);
                                      }),
                  STATUS_SUCCESS);
        EXPECT_EQ(previous_count.read(), 1U);
        ASSERT_EQ(reader->suspended, 0U);
        ASSERT_TRUE(emu.perform_thread_switch(vcpu));
        ASSERT_EQ(vcpu.active_thread, reader);
        EXPECT_EQ(dispatches, 1U);
        EXPECT_EQ(dispatch_tid, reader->id);
        EXPECT_TRUE(reader->pending_apcs.empty());
        EXPECT_FALSE(reader->apc_alertable);
        EXPECT_FALSE(reader->pending_status.has_value());
        EXPECT_TRUE(reader->await_objects.empty());
        EXPECT_EQ(vcpu.cpu.reg<uint64_t>(x86_register::rip), emu.process.ki_user_apc_dispatcher);
        const auto frame = vcpu.cpu.reg<uint64_t>(x86_register::rsp);
        ASSERT_GE(frame, interrupted_sp - stack_before.size());
        ASSERT_LT(frame, interrupted_sp);
        const auto saved_context = emu.memory.read_memory<CONTEXT64>(frame);
        EXPECT_EQ(saved_context.P1Home, scratch + 0x80);
        EXPECT_EQ(saved_context.P2Home, scratch);
        EXPECT_EQ(saved_context.P3Home, 0U);
        EXPECT_EQ(saved_context.P4Home, code + 32);
        EXPECT_EQ(saved_context.Rip, code + 16);
        EXPECT_EQ(saved_context.Rsp, interrupted_sp);
        EXPECT_EQ(saved_context.R12, 0x123456789ABCDEF0ULL);
        EXPECT_EQ(saved_context.Rax, static_cast<uint64_t>(STATUS_USER_APC));
        const auto continuation = emu.memory.read_memory<KCONTINUE_ARGUMENT>(frame + 0x4F0);
        EXPECT_EQ(continuation.ContinueFlags, KCONTINUE_FLAG_TEST_ALERT);

        ASSERT_EQ(emu.dispatch_on_cpu(vcpu.cpu,
                                      [&] {
                                          const syscall_context context{emu, vcpu.cpu, vcpu, emu.process};
                                          const auto status = syscalls::handle_NtContinueEx(context, {emu.memory, frame}, frame + 0x4F0);
                                          EXPECT_FALSE(context.write_status);
                                          return status;
                                      }),
                  STATUS_SUCCESS);
        ASSERT_TRUE(emu.perform_thread_switch(vcpu));
        ASSERT_EQ(vcpu.active_thread, fallback);
        ASSERT_TRUE(emu.perform_thread_switch(vcpu));
        ASSERT_EQ(vcpu.active_thread, reader);
        EXPECT_EQ(dispatches, 1U);
        EXPECT_TRUE(reader->pending_apcs.empty());
        EXPECT_EQ(vcpu.cpu.reg<uint64_t>(x86_register::rip), code + 16);
        EXPECT_EQ(vcpu.cpu.reg<uint64_t>(x86_register::rsp), interrupted_sp);
        EXPECT_EQ(vcpu.cpu.reg<uint64_t>(x86_register::r12), 0x123456789ABCDEF0ULL);
    }

    TEST(IcicleSmp, AlertableFileApcCannotStealAnotherVcpuThread)
    {
        emulator_settings settings{.disable_logging = true, .use_relative_time = false, .use_instruction_precision = false};
        settings.emulation_root = get_emulator_root();
        settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
        emulator_interfaces interfaces{};
        interfaces.socket_factory = network::create_static_socket_factory();
        interfaces.ui = std::make_unique<null_ui_backend>();
        windows_emulator emu{icicle::create_x86_64_emulator(2), get_sample_app_settings({}), settings, {}, std::move(interfaces)};
        emu.setup_process_if_necessary();
        auto& first = emu.vcpu(0);
        auto& second = emu.vcpu(1);
        auto* const fallback = first.active_thread;
        ASSERT_NE(fallback, nullptr);
        const auto code = emu.mod_manager.executable->entry_point;
        const uint8_t ret = 0xC3;
        emu.memory.write_memory(code + 32, &ret, sizeof(ret));
        const auto reader_handle = emu.process.create_thread(emu.memory, code + 16, 0, 0x10000, 0);
        auto* const reader = emu.process.threads.get(reader_handle);
        ASSERT_NE(reader, nullptr);
        ASSERT_TRUE(emu.activate_thread(second, reader->id));
        const auto scratch = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(scratch, 0U);
        ASSERT_NO_FATAL_FAILURE(queue_regular_file_apc(emu, second, scratch, code + 32));
        reader->waiting_for_alert = true;
        reader->apc_alertable = true;
        const auto reader_before = serialized_thread(*reader);
        const auto owner_registers = second.cpu.save_registers();
        const auto fallback_registers = first.cpu.save_registers();
        size_t dispatches{};
        emu.callbacks.on_generic_activity = [&](const std::string_view description) {
            if (description == "APC Dispatch")
            {
                ++dispatches;
            }
        };

        ASSERT_TRUE(emu.perform_thread_switch(first));
        EXPECT_EQ(first.active_thread, fallback);
        EXPECT_EQ(second.active_thread, reader);
        EXPECT_EQ(first.cpu.save_registers(), fallback_registers);
        EXPECT_EQ(second.cpu.save_registers(), owner_registers);
        EXPECT_EQ(serialized_thread(*reader), reader_before);
        EXPECT_EQ(dispatches, 0U);
        ASSERT_TRUE(emu.perform_thread_switch(second));
        EXPECT_EQ(second.active_thread, reader);
        EXPECT_TRUE(reader->pending_apcs.empty());
        EXPECT_EQ(dispatches, 1U);
        EXPECT_EQ(second.cpu.reg<uint64_t>(x86_register::rip), emu.process.ki_user_apc_dispatcher);
    }
}
