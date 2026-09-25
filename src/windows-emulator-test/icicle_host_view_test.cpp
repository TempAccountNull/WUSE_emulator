#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <memory_manager.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace sogen::test
{
    namespace
    {
        template <typename Predicate>
        bool wait_for_host_view_condition(Predicate&& condition)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!condition() && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return condition();
        }

        struct running_guest_readers
        {
            std::unique_ptr<x86_64_emulator> emu = icicle::create_x86_64_emulator(2);
            memory_manager memory{*emu};
            uint64_t code = memory.allocate_memory(0x1000, memory_permission::all);
            uint64_t data = memory.allocate_memory(0x1000, memory_permission::read_write);
            std::atomic_bool stop{false};
            std::array<std::thread, 2> workers{};
            std::array<std::string, 2> failures{};

            running_guest_readers()
            {
                if (!code || !data)
                {
                    throw std::runtime_error("Failed to allocate Icicle host-view test pages");
                }
                std::array<uint8_t, 15> program{0x48, 0xBB};
                std::memcpy(program.data() + 2, &data, sizeof(data));
                program[10] = 0x48;
                program[11] = 0x8B;
                program[12] = 0x03;
                program[13] = 0xEB;
                program[14] = 0xFB;
                emu->write_memory(code, program.data(), program.size());
            }

            void start()
            {
                for (size_t index = 0; index < workers.size(); ++index)
                {
                    workers[index] = std::thread([this, index] {
                        try
                        {
                            auto& cpu = emu->get_cpu(index);
                            cpu.reg(x86_register::rip, code);
                            while (!stop.load(std::memory_order_acquire))
                            {
                                cpu.start(4096);
                                emu->sync_worker_context(index);
                            }
                        }
                        catch (const std::exception& error)
                        {
                            failures[index] = error.what();
                        }
                    });
                }
            }

            void finish()
            {
                stop.store(true, std::memory_order_release);
                for (auto& worker : workers)
                {
                    if (worker.joinable())
                    {
                        worker.join();
                    }
                }
            }

            ~running_guest_readers()
            {
                finish();
            }

            bool both_advanced_beyond(const std::array<uint64_t, 2>& counts) const
            {
                const auto activity = emu->vcpu_activity();
                return activity.size() == 2 && activity[0].instructions > counts[0] && activity[1].instructions > counts[1];
            }

            std::array<uint64_t, 2> instructions() const
            {
                const auto activity = emu->vcpu_activity();
                if (activity.size() != 2)
                {
                    return {};
                }
                return {activity[0].instructions, activity[1].instructions};
            }
        };
    } // namespace

    TEST(IcicleHostView, ExternalReadWriteWhileBothVcpusReadSharedMemory)
    {
        running_guest_readers readers;
        readers.start();
        const bool running = wait_for_host_view_condition([&] { return readers.both_advanced_beyond({1000, 1000}); });

        bool transfers_ok = running;
        for (uint64_t index = 1; running && index <= 128; ++index)
        {
            const uint64_t value = 0xA5A5000000000000ULL | index;
            uint64_t observed{};
            if (!readers.emu->try_write_memory(readers.data, &value, sizeof(value)) ||
                !readers.emu->try_read_memory(readers.data, &observed, sizeof(observed)) || observed != value)
            {
                transfers_ok = false;
                break;
            }
        }

        const auto before_final_write = readers.instructions();
        constexpr uint64_t final_value = 0xD15EA5ED12345678ULL;
        const bool final_write = readers.emu->try_write_memory(readers.data, &final_value, sizeof(final_value));
        const bool both_saw_final_write = final_write && wait_for_host_view_condition([&] {
                                              const auto activity = readers.emu->vcpu_activity();
                                              return activity.size() == 2 && activity[0].instructions > before_final_write[0] + 1000 &&
                                                     activity[1].instructions > before_final_write[1] + 1000;
                                          });
        readers.finish();

        EXPECT_TRUE(running);
        EXPECT_TRUE(transfers_ok);
        EXPECT_TRUE(final_write);
        EXPECT_TRUE(both_saw_final_write);
        EXPECT_TRUE(readers.failures[0].empty()) << readers.failures[0];
        EXPECT_TRUE(readers.failures[1].empty()) << readers.failures[1];
        if (both_saw_final_write)
        {
            EXPECT_EQ(readers.emu->get_cpu(0).reg(x86_register::rax), final_value);
            EXPECT_EQ(readers.emu->get_cpu(1).reg(x86_register::rax), final_value);
        }
    }

    TEST(IcicleHostView, ExternalWriteToExecutedCodeInvalidatesBothVcpus)
    {
        running_guest_readers readers;
        constexpr uint64_t initial_value = 0xA5A5A5A5A5A5A5A5ULL;
        readers.emu->write_memory(readers.data, &initial_value, sizeof(initial_value));
        readers.start();
        const bool running = wait_for_host_view_condition([&] { return readers.both_advanced_beyond({1000, 1000}); });

        const auto before_patch = readers.instructions();
        constexpr uint8_t register_operand = 0xC3;
        const bool patched = running && readers.emu->try_write_memory(readers.code + 12, &register_operand, sizeof(register_operand));
        const bool both_ran_after_patch = patched && wait_for_host_view_condition([&] {
                                              const auto activity = readers.emu->vcpu_activity();
                                              return activity.size() == 2 && activity[0].instructions > before_patch[0] + 20000 &&
                                                     activity[1].instructions > before_patch[1] + 20000;
                                          });
        readers.finish();

        EXPECT_TRUE(running);
        EXPECT_TRUE(patched);
        EXPECT_TRUE(both_ran_after_patch);
        EXPECT_TRUE(readers.failures[0].empty()) << readers.failures[0];
        EXPECT_TRUE(readers.failures[1].empty()) << readers.failures[1];
        if (both_ran_after_patch)
        {
            EXPECT_EQ(readers.emu->get_cpu(0).reg(x86_register::rax), readers.data);
            EXPECT_EQ(readers.emu->get_cpu(1).reg(x86_register::rax), readers.data);
        }
    }

    TEST(IcicleHostView, ExternalMapProtectAndReadHookDuringGuestExecution)
    {
        running_guest_readers readers;
        constexpr uint64_t mapped = 0x54000000;
        constexpr uint64_t alias = 0x55000000;
        constexpr uint64_t host_mapping = 0x56000000;
        std::array<uint8_t, 13> guest_write{0x48, 0xB8};
        std::memcpy(guest_write.data() + 2, &mapped, sizeof(mapped));
        guest_write[10] = 0xC6;
        guest_write[11] = 0x00;
        guest_write[12] = 0x5A;
        readers.emu->write_memory(readers.code + 0x100, guest_write.data(), guest_write.size());
        readers.start();
        const bool running = wait_for_host_view_condition([&] { return readers.both_advanced_beyond({1000, 1000}); });

        alignas(0x1000) std::array<uint8_t, 0x1000> host_backing{};
        bool mapped_ok = false;
        bool alias_mapped = false;
        bool alias_ok = false;
        bool host_mapped = false;
        bool host_mapping_ok = false;
        bool protected_ok = false;
        bool restored_ok = false;
        bool alias_released = false;
        bool mapped_released = false;
        bool host_mapping_released = false;
        bool guest_write_denied = false;
        bool guest_write_allowed = false;
        uint64_t aliased_value{};
        if (running)
        {
            mapped_ok = readers.memory.allocate_memory(mapped, 0x1000, memory_permission::read_write);
            if (mapped_ok)
            {
                alias_mapped = readers.memory.allocate_shared_view(alias, mapped, 0x1000, memory_permission::read_write);
                constexpr uint64_t value = 0x8877665544332211ULL;
                if (alias_mapped)
                {
                    alias_ok = readers.emu->try_write_memory(alias, &value, sizeof(value)) &&
                               readers.emu->try_read_memory(mapped, &aliased_value, sizeof(aliased_value));
                }
                protected_ok = readers.memory.protect_memory(mapped, 0x1000, memory_permission::read);
            }
            host_mapped = readers.memory.allocate_host_memory_at(host_mapping, host_backing.size(), host_backing.data(),
                                                                 memory_permission::read_write);
            if (host_mapped)
            {
                constexpr uint64_t host_value = 0xCAFEBABE12345678ULL;
                uint64_t backing_value{};
                host_mapping_ok = readers.emu->try_write_memory(host_mapping, &host_value, sizeof(host_value));
                std::memcpy(&backing_value, host_backing.data(), sizeof(backing_value));
                host_mapping_ok = host_mapping_ok && backing_value == host_value;
            }
        }

        std::array<std::atomic<uint64_t>, 2> read_hits{};
        emulator_hook* hook = nullptr;
        bool both_hit = false;
        if (running)
        {
            hook = readers.emu->hook_memory_read(readers.data, sizeof(uint64_t), [&](cpu_interface& cpu, uint64_t, const void*, size_t) {
                if (cpu.index() < read_hits.size())
                {
                    read_hits[cpu.index()].fetch_add(1, std::memory_order_relaxed);
                }
            });
            both_hit = hook && wait_for_host_view_condition([&] {
                           return read_hits[0].load(std::memory_order_relaxed) != 0 && read_hits[1].load(std::memory_order_relaxed) != 0;
                       });
            if (hook)
            {
                readers.emu->delete_hook(hook);
            }
            if (alias_mapped)
            {
                alias_released = readers.memory.release_memory(alias, 0);
            }
            if (host_mapped)
            {
                host_mapping_released = readers.memory.release_memory(host_mapping, 0);
            }
        }
        readers.finish();

        if (protected_ok)
        {
            auto& cpu = readers.emu->get_cpu(0);
            cpu.reg(x86_register::rip, readers.code + 0x100);
            try
            {
                cpu.start(2);
            }
            catch (const std::runtime_error& error)
            {
                guest_write_denied = std::string(error.what()).find("WritePerm") != std::string::npos;
            }
            restored_ok = readers.memory.protect_memory(mapped, 0x1000, memory_permission::read_write);
            if (restored_ok)
            {
                auto& peer = readers.emu->get_cpu(1);
                peer.reg(x86_register::rip, readers.code + 0x100);
                try
                {
                    peer.start(2);
                    uint8_t value{};
                    guest_write_allowed = readers.emu->try_read_memory(mapped, &value, sizeof(value)) && value == 0x5A;
                }
                catch (const std::runtime_error&)
                {
                }
            }
            mapped_released = readers.memory.release_memory(mapped, 0);
        }

        uint64_t unused{};
        EXPECT_TRUE(running);
        EXPECT_TRUE(mapped_ok);
        EXPECT_TRUE(alias_mapped);
        EXPECT_TRUE(alias_ok);
        EXPECT_TRUE(host_mapped);
        EXPECT_TRUE(host_mapping_ok);
        EXPECT_TRUE(host_mapping_released);
        EXPECT_EQ(aliased_value, 0x8877665544332211ULL);
        EXPECT_TRUE(protected_ok);
        EXPECT_TRUE(guest_write_denied);
        EXPECT_TRUE(restored_ok);
        EXPECT_TRUE(guest_write_allowed);
        EXPECT_TRUE(both_hit);
        EXPECT_TRUE(alias_released);
        EXPECT_TRUE(mapped_released);
        if (mapped_released && alias_released)
        {
            EXPECT_FALSE(readers.emu->try_read_memory(mapped, &unused, sizeof(unused)));
            EXPECT_FALSE(readers.emu->try_read_memory(alias, &unused, sizeof(unused)));
        }
        EXPECT_TRUE(readers.failures[0].empty()) << readers.failures[0];
        EXPECT_TRUE(readers.failures[1].empty()) << readers.failures[1];
    }

    TEST(IcicleHostView, ExternalWriteReportsCompletedHostObservation)
    {
        running_guest_readers readers;
        std::atomic_uint64_t observed_value{};
        std::atomic_uint64_t observation_count{};
        std::atomic_bool observed_completed_host_write{false};
        auto* hook = readers.emu->hook_memory_write_observed(
            readers.data, sizeof(uint64_t),
            [&](cpu_interface&, uint64_t address, const void* data, size_t size, memory_write_result result) {
                if (address == readers.data && size == sizeof(uint64_t) && data)
                {
                    uint64_t value{};
                    std::memcpy(&value, data, sizeof(value));
                    observed_value.store(value, std::memory_order_release);
                }
                if (result.origin == memory_write_origin::host && result.outcome == memory_access_outcome::completed)
                {
                    observed_completed_host_write.store(true, std::memory_order_release);
                }
                observation_count.fetch_add(1, std::memory_order_release);
            });
        ASSERT_NE(hook, nullptr);

        readers.start();
        const bool running = wait_for_host_view_condition([&] { return readers.both_advanced_beyond({1000, 1000}); });
        constexpr uint64_t value = 0x1234567890ABCDEFULL;
        const bool wrote = running && readers.emu->try_write_memory(readers.data, &value, sizeof(value));
        const bool observed = wrote && wait_for_host_view_condition([&] {
                                  return observation_count.load(std::memory_order_acquire) != 0 &&
                                         observed_completed_host_write.load(std::memory_order_acquire) &&
                                         observed_value.load(std::memory_order_acquire) == value;
                              });
        readers.emu->delete_hook(hook);
        readers.finish();

        EXPECT_TRUE(running);
        EXPECT_TRUE(wrote);
        EXPECT_TRUE(observed);
        EXPECT_TRUE(readers.failures[0].empty()) << readers.failures[0];
        EXPECT_TRUE(readers.failures[1].empty()) << readers.failures[1];
    }

    TEST(IcicleHostView, ExternalHookRejectsRunningVcpuRegisterAccess)
    {
        running_guest_readers readers;
        std::atomic_bool read_rejected{false};
        std::atomic_bool write_rejected{false};
        std::atomic_bool observed{false};
        std::atomic_bool no_guest_context{false};
        auto* hook = readers.emu->hook_memory_write_observed(
            readers.data, sizeof(uint64_t), [&](cpu_interface& cpu, uint64_t, const void*, size_t, memory_write_result result) {
                if (result.origin != memory_write_origin::host)
                {
                    return;
                }
                no_guest_context.store(!cpu.has_guest_cpu_context(), std::memory_order_release);
                uint64_t register_value{};
                try
                {
                    (void)cpu.read_raw_register(static_cast<int>(x86_register::rax), &register_value, sizeof(register_value));
                }
                catch (const std::logic_error&)
                {
                    read_rejected.store(true, std::memory_order_release);
                }
                constexpr uint64_t replacement = 0xBADULL;
                try
                {
                    (void)cpu.write_raw_register(static_cast<int>(x86_register::rax), &replacement, sizeof(replacement));
                }
                catch (const std::logic_error&)
                {
                    write_rejected.store(true, std::memory_order_release);
                }
                observed.store(true, std::memory_order_release);
            });
        ASSERT_NE(hook, nullptr);

        readers.start();
        const bool running = wait_for_host_view_condition([&] { return readers.both_advanced_beyond({1000, 1000}); });
        constexpr uint64_t value = 0x1234567890ABCDEFULL;
        const bool wrote = running && readers.emu->try_write_memory(readers.data, &value, sizeof(value));
        readers.emu->delete_hook(hook);
        readers.finish();

        EXPECT_TRUE(running);
        EXPECT_TRUE(wrote);
        EXPECT_TRUE(observed.load(std::memory_order_acquire));
        EXPECT_TRUE(no_guest_context.load(std::memory_order_acquire));
        EXPECT_TRUE(read_rejected.load(std::memory_order_acquire));
        EXPECT_TRUE(write_rejected.load(std::memory_order_acquire));
        EXPECT_TRUE(readers.failures[0].empty()) << readers.failures[0];
        EXPECT_TRUE(readers.failures[1].empty()) << readers.failures[1];
    }

    TEST(IcicleHostView, ExternalHookFailureReturnsToExternalCaller)
    {
        running_guest_readers readers;
        std::atomic_bool callback_entered{false};
        auto* hook = readers.emu->hook_memory_write_observed(
            readers.data, sizeof(uint64_t), [&](cpu_interface&, uint64_t, const void*, size_t, memory_write_result result) {
                if (result.origin == memory_write_origin::host)
                {
                    callback_entered.store(true, std::memory_order_release);
                    throw std::runtime_error("host callback failure");
                }
            });
        ASSERT_NE(hook, nullptr);

        readers.start();
        const bool running = wait_for_host_view_condition([&] { return readers.both_advanced_beyond({1000, 1000}); });
        bool caught_on_external_caller = false;
        if (running)
        {
            constexpr uint64_t value = 0x1234567890ABCDEFULL;
            try
            {
                (void)readers.emu->try_write_memory(readers.data, &value, sizeof(value));
            }
            catch (const std::runtime_error& error)
            {
                caught_on_external_caller = std::string(error.what()) == "host callback failure";
            }
        }
        readers.emu->delete_hook(hook);
        readers.finish();

        EXPECT_TRUE(running);
        EXPECT_TRUE(callback_entered.load(std::memory_order_acquire));
        EXPECT_TRUE(caught_on_external_caller);
        EXPECT_TRUE(readers.failures[0].empty()) << readers.failures[0];
        EXPECT_TRUE(readers.failures[1].empty()) << readers.failures[1];
    }

    TEST(IcicleHostView, StagedWriteObservationIsCanceledBeforeDispatch)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        const auto data = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(data, 0U);

        std::array<std::atomic_uint64_t, 2> hits{};
        std::atomic<int> first{-1};
        std::atomic_bool release_first{false};
        std::atomic_bool first_timed_out{false};
        std::array<emulator_hook*, 2> hooks{};
        for (size_t index = 0; index < hooks.size(); ++index)
        {
            hooks[index] = emu->hook_memory_write_observed(
                data, sizeof(uint64_t), [&, index](cpu_interface&, uint64_t, const void*, size_t, memory_write_result) {
                    hits[index].fetch_add(1, std::memory_order_release);
                    int expected = -1;
                    if (first.compare_exchange_strong(expected, static_cast<int>(index), std::memory_order_acq_rel) &&
                        !wait_for_host_view_condition([&] { return release_first.load(std::memory_order_acquire); }))
                    {
                        first_timed_out.store(true, std::memory_order_release);
                    }
                });
            ASSERT_NE(hooks[index], nullptr);
        }

        bool wrote = false;
        std::exception_ptr writer_error;
        std::thread writer([&] {
            try
            {
                constexpr uint64_t value = 0x1122334455667788ULL;
                wrote = emu->try_write_memory(data, &value, sizeof(value));
            }
            catch (...)
            {
                writer_error = std::current_exception();
            }
        });
        const bool first_entered = wait_for_host_view_condition([&] { return first.load(std::memory_order_acquire) >= 0; });

        std::atomic_bool delete_done{false};
        std::exception_ptr delete_error;
        std::thread deleter;
        bool deleted_before_release = false;
        int deleted_index = -1;
        if (first_entered)
        {
            deleted_index = 1 - first.load(std::memory_order_acquire);
            deleter = std::thread([&] {
                try
                {
                    emu->delete_hook(hooks[deleted_index]);
                }
                catch (...)
                {
                    delete_error = std::current_exception();
                }
                delete_done.store(true, std::memory_order_release);
            });
            deleted_before_release = wait_for_host_view_condition([&] { return delete_done.load(std::memory_order_acquire); });
        }
        release_first.store(true, std::memory_order_release);
        if (deleter.joinable())
        {
            deleter.join();
        }
        writer.join();
        if (first_entered)
        {
            emu->delete_hook(hooks[1 - deleted_index]);
        }
        else
        {
            emu->delete_hook(hooks[0]);
            emu->delete_hook(hooks[1]);
        }

        EXPECT_TRUE(first_entered);
        EXPECT_TRUE(deleted_before_release);
        EXPECT_FALSE(first_timed_out.load(std::memory_order_acquire));
        EXPECT_EQ(delete_error, nullptr);
        EXPECT_EQ(writer_error, nullptr);
        EXPECT_TRUE(wrote);
        if (first_entered)
        {
            EXPECT_EQ(hits[1 - deleted_index].load(std::memory_order_acquire), 1U);
            EXPECT_EQ(hits[deleted_index].load(std::memory_order_acquire), 0U);
        }
    }

    TEST(IcicleHostView, ExternalDeleteWaitsForActiveHostViewCallback)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        const auto data = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(data, 0U);

        std::atomic_uint64_t hits{};
        std::atomic_bool entered{false};
        std::atomic_bool release_callback{false};
        std::atomic_bool callback_exited{false};
        std::atomic_bool callback_timed_out{false};
        auto* hook = emu->hook_memory_write_observed(
            data, sizeof(uint64_t), [&](cpu_interface&, uint64_t, const void*, size_t, memory_write_result) {
                hits.fetch_add(1, std::memory_order_release);
                entered.store(true, std::memory_order_release);
                if (!wait_for_host_view_condition([&] { return release_callback.load(std::memory_order_acquire); }))
                {
                    callback_timed_out.store(true, std::memory_order_release);
                }
                callback_exited.store(true, std::memory_order_release);
            });
        ASSERT_NE(hook, nullptr);

        bool first_write_ok = false;
        std::exception_ptr writer_error;
        std::thread writer([&] {
            try
            {
                constexpr uint64_t value = 0x1122334455667788ULL;
                first_write_ok = emu->try_write_memory(data, &value, sizeof(value));
            }
            catch (...)
            {
                writer_error = std::current_exception();
            }
        });
        const bool callback_entered = wait_for_host_view_condition([&] { return entered.load(std::memory_order_acquire); });

        std::atomic_bool delete_started{false};
        std::atomic_bool delete_done{false};
        std::atomic_bool delete_saw_callback_exit{false};
        std::exception_ptr delete_error;
        std::thread deleter;
        bool still_deleting_while_active = false;
        if (callback_entered)
        {
            deleter = std::thread([&] {
                delete_started.store(true, std::memory_order_release);
                try
                {
                    emu->delete_hook(hook);
                }
                catch (...)
                {
                    delete_error = std::current_exception();
                }
                delete_saw_callback_exit.store(callback_exited.load(std::memory_order_acquire), std::memory_order_release);
                delete_done.store(true, std::memory_order_release);
            });
            const bool started = wait_for_host_view_condition([&] { return delete_started.load(std::memory_order_acquire); });
            if (started)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                still_deleting_while_active = !delete_done.load(std::memory_order_acquire);
            }
        }
        release_callback.store(true, std::memory_order_release);
        writer.join();
        if (deleter.joinable())
        {
            deleter.join();
        }
        else
        {
            emu->delete_hook(hook);
        }
        constexpr uint64_t second_value = 0x8877665544332211ULL;
        const bool second_write_ok = emu->try_write_memory(data, &second_value, sizeof(second_value));

        EXPECT_TRUE(callback_entered);
        EXPECT_TRUE(still_deleting_while_active);
        EXPECT_TRUE(delete_done.load(std::memory_order_acquire));
        EXPECT_EQ(delete_error, nullptr);
        EXPECT_FALSE(callback_timed_out.load(std::memory_order_acquire));
        EXPECT_TRUE(delete_saw_callback_exit.load(std::memory_order_acquire));
        EXPECT_EQ(writer_error, nullptr);
        EXPECT_TRUE(first_write_ok);
        EXPECT_TRUE(second_write_ok);
        EXPECT_EQ(hits.load(std::memory_order_acquire), 1U);
    }

    TEST(IcicleHostView, HostViewCallbackCanDeleteItself)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        const auto data = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(data, 0U);

        std::atomic_uint64_t hits{};
        std::atomic_bool delete_returned{false};
        emulator_hook* hook = nullptr;
        hook = emu->hook_memory_write_observed(data, sizeof(uint64_t),
                                               [&](cpu_interface&, uint64_t, const void*, size_t, memory_write_result) {
                                                   hits.fetch_add(1, std::memory_order_release);
                                                   emu->delete_hook(hook);
                                                   delete_returned.store(true, std::memory_order_release);
                                               });
        ASSERT_NE(hook, nullptr);

        constexpr uint64_t first = 0x0123456789ABCDEFULL;
        constexpr uint64_t second = 0xFEDCBA9876543210ULL;
        const bool first_write_ok = emu->try_write_memory(data, &first, sizeof(first));
        const bool second_write_ok = emu->try_write_memory(data, &second, sizeof(second));

        EXPECT_TRUE(first_write_ok);
        EXPECT_TRUE(delete_returned.load(std::memory_order_acquire));
        EXPECT_TRUE(second_write_ok);
        EXPECT_EQ(hits.load(std::memory_order_acquire), 1U);
    }

    TEST(IcicleHostView, LargeExternalWriteReportsSmallWatchpoint)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        constexpr size_t transfer_size = 9 * 1024 * 1024;
        const auto address = memory.allocate_memory(transfer_size, memory_permission::read_write);
        ASSERT_NE(address, 0U);

        constexpr size_t watched_offset = 0x1000;
        constexpr uint64_t marker = 0x8877665544332211ULL;
        std::vector<uint8_t> payload(transfer_size, 0x5A);
        std::memcpy(payload.data() + watched_offset, &marker, sizeof(marker));
        payload.back() = 0xA7;
        std::atomic_bool observed_host_write{false};
        auto* hook = emu->hook_memory_write_observed(
            address + watched_offset, sizeof(marker),
            [&](cpu_interface&, uint64_t access, const void*, size_t size, memory_write_result result) {
                if (access <= address + watched_offset && size > address + watched_offset - access &&
                    result.origin == memory_write_origin::host && result.outcome == memory_access_outcome::completed)
                {
                    observed_host_write.store(true, std::memory_order_release);
                }
            });
        ASSERT_NE(hook, nullptr);

        const bool wrote = emu->try_write_memory(address, payload.data(), payload.size());
        const bool observed = wrote && wait_for_host_view_condition([&] { return observed_host_write.load(std::memory_order_acquire); });
        uint64_t readback{};
        uint8_t final_byte{};
        const bool readback_ok = emu->try_read_memory(address + watched_offset, &readback, sizeof(readback)) &&
                                 emu->try_read_memory(address + transfer_size - 1, &final_byte, sizeof(final_byte));
        emu->delete_hook(hook);

        EXPECT_TRUE(wrote);
        EXPECT_TRUE(observed);
        EXPECT_TRUE(readback_ok);
        EXPECT_EQ(readback, marker);
        EXPECT_EQ(final_byte, 0xA7);
    }

    TEST(IcicleHostView, ExternalMmioRefusesActiveVmAndWorksWhenParked)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(code, 0U);
        constexpr uint64_t mmio = 0x57000000;
        std::atomic_uint64_t read_calls{};
        std::atomic_uint64_t write_calls{};
        ASSERT_TRUE(memory.allocate_mmio(
            mmio, 0x1000,
            [&](uint64_t, void* output, size_t length) {
                std::memset(output, 0x5A, length);
                read_calls.fetch_add(1, std::memory_order_release);
            },
            [&](uint64_t, const void*, size_t) { write_calls.fetch_add(1, std::memory_order_release); }));
        constexpr uint8_t nop = 0x90;
        emu->write_memory(code, &nop, sizeof(nop));

        std::atomic_bool inside_hook{false};
        std::atomic_bool release_hook{false};
        auto* hook = emu->hook_memory_execution(code, [&](cpu_interface&, uint64_t) {
            inside_hook.store(true, std::memory_order_release);
            wait_for_host_view_condition([&] { return release_hook.load(std::memory_order_acquire); });
        });
        ASSERT_NE(hook, nullptr);

        std::exception_ptr guest_error;
        std::thread guest([&] {
            try
            {
                auto& cpu = emu->get_cpu(0);
                cpu.reg(x86_register::rip, code);
                cpu.start(1);
            }
            catch (...)
            {
                guest_error = std::current_exception();
            }
        });
        const bool active = wait_for_host_view_condition([&] { return inside_hook.load(std::memory_order_acquire); });

        bool active_read_ok = false;
        bool active_write_ok = false;
        std::atomic_bool external_done{false};
        std::exception_ptr external_error;
        std::thread external;
        bool completed_while_active = false;
        uint64_t reads_while_active = 0;
        uint64_t writes_while_active = 0;
        if (active)
        {
            external = std::thread([&] {
                try
                {
                    uint64_t value{};
                    constexpr uint64_t replacement = 0xAABBCCDDEEFF0011ULL;
                    active_read_ok = emu->try_read_memory(mmio, &value, sizeof(value));
                    active_write_ok = emu->try_write_memory(mmio, &replacement, sizeof(replacement));
                }
                catch (...)
                {
                    external_error = std::current_exception();
                }
                external_done.store(true, std::memory_order_release);
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            while (!external_done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            completed_while_active = external_done.load(std::memory_order_acquire);
            reads_while_active = read_calls.load(std::memory_order_acquire);
            writes_while_active = write_calls.load(std::memory_order_acquire);
        }
        release_hook.store(true, std::memory_order_release);
        guest.join();
        if (external.joinable())
        {
            external.join();
        }
        emu->delete_hook(hook);

        uint64_t parked_value{};
        constexpr uint64_t parked_replacement = 0xDEADBEEF12345678ULL;
        const bool parked_read_ok = emu->try_read_memory(mmio, &parked_value, sizeof(parked_value));
        const bool parked_write_ok = emu->try_write_memory(mmio, &parked_replacement, sizeof(parked_replacement));

        EXPECT_TRUE(active);
        EXPECT_TRUE(completed_while_active);
        EXPECT_EQ(guest_error, nullptr);
        EXPECT_EQ(external_error, nullptr);
        EXPECT_FALSE(active_read_ok);
        EXPECT_FALSE(active_write_ok);
        EXPECT_EQ(reads_while_active, 0U);
        EXPECT_EQ(writes_while_active, 0U);
        EXPECT_TRUE(parked_read_ok);
        EXPECT_EQ(parked_value, 0x5A5A5A5A5A5A5A5AULL);
        EXPECT_TRUE(parked_write_ok);
        EXPECT_GT(read_calls.load(std::memory_order_acquire), 0U);
        EXPECT_GT(write_calls.load(std::memory_order_acquire), 0U);
    }
} // namespace sogen::test
