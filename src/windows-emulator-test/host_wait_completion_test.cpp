#include "emulation_test_utils.hpp"
#include <devices/native_wsi_completion.hpp>
#include <array>

namespace sogen::test
{
    class HostWaitCompletionTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();

        void SetUp() override
        {
            emu.setup_process_if_necessary();
        }

        emulator_thread& thread()
        {
            return *emu.vcpu(0).active_thread;
        }
    };

    TEST_F(HostWaitCompletionTest, PollsWithoutResumingThenDeliversFailureExactlyOnce)
    {
        auto& waiting = thread();
        waiting.pending_status.reset();
        unsigned polls{};
        waiting.await_host_condition = [&] {
            if (++polls == 1)
            {
                return false;
            }
            waiting.await_host_status = STATUS_ACCESS_VIOLATION;
            return true;
        };
        EXPECT_FALSE(waiting.is_thread_ready(emu));
        EXPECT_FALSE(waiting.pending_status.has_value());
        EXPECT_TRUE(waiting.is_thread_ready(emu));
        ASSERT_TRUE(waiting.pending_status.has_value());
        EXPECT_EQ(*waiting.pending_status, STATUS_ACCESS_VIOLATION);
        EXPECT_FALSE(waiting.await_host_condition);
        EXPECT_EQ(waiting.await_host_status, STATUS_SUCCESS);
        EXPECT_TRUE(waiting.is_thread_ready(emu));
        EXPECT_EQ(polls, 2u);
        EXPECT_EQ(*waiting.pending_status, STATUS_ACCESS_VIOLATION);
        waiting.setup_if_necessary(emu.vcpu(0).cpu, emu.process);
        EXPECT_FALSE(waiting.pending_status.has_value());
        EXPECT_EQ(emu.vcpu(0).cpu.reg<uint64_t>(x86_register::rax), static_cast<uint64_t>(static_cast<NTSTATUS>(STATUS_ACCESS_VIOLATION)));
    }

    TEST_F(HostWaitCompletionTest, OrdinaryPredicateWaitDoesNotInheritPreviousFailure)
    {
        auto& waiting = thread();
        waiting.await_host_status = STATUS_BUFFER_TOO_SMALL;
        waiting.await_host_condition = [] { return true; };
        ASSERT_TRUE(waiting.is_thread_ready(emu));
        ASSERT_EQ(waiting.pending_status, STATUS_BUFFER_TOO_SMALL);
        waiting.pending_status.reset();
        waiting.await_host_condition = [] { return true; };
        EXPECT_TRUE(waiting.is_thread_ready(emu));
        EXPECT_EQ(waiting.pending_status, STATUS_SUCCESS);
    }

    TEST_F(HostWaitCompletionTest, OtherWakeupClearsPendingHostCompletionAndItsCaptures)
    {
        auto& waiting = thread();
        auto lifetime = std::make_shared<int>(1);
        std::weak_ptr<int> observer = lifetime;
        waiting.await_host_condition = [lifetime] { return false; };
        lifetime.reset();
        waiting.await_host_status = STATUS_UNSUCCESSFUL;
        waiting.mark_as_ready(STATUS_USER_APC);
        EXPECT_TRUE(observer.expired());
        EXPECT_FALSE(waiting.await_host_condition);
        EXPECT_EQ(waiting.await_host_status, STATUS_SUCCESS);
        EXPECT_EQ(waiting.pending_status, STATUS_USER_APC);
    }

    TEST_F(HostWaitCompletionTest, OversizedOutputPreservesBufferAndCompletesWithBufferTooSmall)
    {
        const auto address = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(address, 0u);
        const std::array<uint64_t, 2> sentinel{0xaaaaaaaaaaaaaaaaULL, 0xbbbbbbbbbbbbbbbbULL};
        emu.memory.write_memory(address, sentinel.data(), sizeof(sentinel));
        io_device_context context{emu.memory};
        context.io_status_block = {emu.memory, address + 64};
        context.output_buffer = address;
        context.output_buffer_length = 4;
        const std::array<std::byte, 8> bytes{};
        auto& waiting = thread();
        waiting.await_host_condition = [&] {
            waiting.await_host_status = deliver_native_wsi_output(emu.emu(), context, bytes);
            return true;
        };
        ASSERT_TRUE(waiting.is_thread_ready(emu));
        EXPECT_EQ(waiting.pending_status, STATUS_BUFFER_TOO_SMALL);
        EXPECT_EQ((emu.memory.read_memory<std::array<uint64_t, 2>>(address)), sentinel);
        const auto block = context.io_status_block.read();
        EXPECT_EQ(block.Status, STATUS_BUFFER_TOO_SMALL);
        EXPECT_EQ(block.Information, 0u);
    }

    TEST_F(HostWaitCompletionTest, UnmappedOutputReportsAccessViolationWithZeroInformation)
    {
        const auto address = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(address, 0u);
        io_device_context context{emu.memory};
        context.io_status_block = {emu.memory, address};
        context.output_buffer = 0;
        context.output_buffer_length = 8;
        const std::array<std::byte, 8> bytes{};
        EXPECT_EQ(deliver_native_wsi_output(emu.emu(), context, bytes), STATUS_ACCESS_VIOLATION);
        const auto block = context.io_status_block.read();
        EXPECT_EQ(block.Status, STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(block.Information, 0u);
    }

    TEST_F(HostWaitCompletionTest, SuccessfulOutputPublishesExactBytesAndInformation)
    {
        const auto address = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(address, 0u);
        io_device_context context{emu.memory};
        context.io_status_block = {emu.memory, address + 64};
        context.output_buffer = address;
        context.output_buffer_length = 32;
        const std::array<uint64_t, 2> response{0x1234567887654321ULL, 0xfedcba9876543210ULL};
        EXPECT_EQ(deliver_native_wsi_output(emu.emu(), context, std::as_bytes(std::span{response})), STATUS_SUCCESS);
        EXPECT_EQ((emu.memory.read_memory<std::array<uint64_t, 2>>(address)), response);
        const auto block = context.io_status_block.read();
        EXPECT_EQ(block.Status, STATUS_SUCCESS);
        EXPECT_EQ(block.Information, sizeof(response));
    }

    TEST_F(HostWaitCompletionTest, UnmappedStatusBlockDoesNotReportSuccessfulDelivery)
    {
        const auto address = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(address, 0u);
        io_device_context context{emu.memory};
        context.io_status_block = {emu.memory, 1};
        context.output_buffer = address;
        context.output_buffer_length = 8;
        const std::array<std::byte, 8> bytes{};
        EXPECT_EQ(deliver_native_wsi_output(emu.emu(), context, bytes), STATUS_ACCESS_VIOLATION);
    }
}
