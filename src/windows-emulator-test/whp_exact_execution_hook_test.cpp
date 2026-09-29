#include "emulation_test_utils.hpp"

#include <memory_manager.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace sogen::test
{
    TEST(WhpExactExecutionHook, ObservesBranchAndReturnWithoutChangingGuestCode)
    {
        auto emu = create_x86_64_emulator(backend_type::whp);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto stack = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0u);
        ASSERT_NE(stack, 0u);

        // mov eax,0; test eax,eax; nop; mov eax,1; ret; ...; nop (return target).
        std::array<uint8_t, 0x21> guest{};
        guest.fill(0x90);
        constexpr std::array<uint8_t, 14> function{0xB8, 0, 0, 0, 0, 0x85, 0xC0, 0x90, 0xB8, 1, 0, 0, 0, 0xC3};
        std::copy(function.begin(), function.end(), guest.begin());
        emu->write_memory(code, guest.data(), guest.size());

        const auto stack_top = stack + 0x800;
        emu->write_memory<uint64_t>(stack_top, code + 0x20);
        emu->reg(x86_register::rip, code);
        emu->reg(x86_register::rsp, stack_top);

        std::vector<uint64_t> seen{};
        uint64_t predicate_rax = UINT64_MAX;
        uint32_t predicate_flags = 0;
        uint64_t return_rax = UINT64_MAX;
        bool reached_return_target = false;
        std::array<emulator_hook*, 5> hooks{};
        const std::array<uint64_t, 5> offsets{0, 7, 8, 13, 0x20};
        for (size_t i = 0; i < offsets.size(); ++i)
        {
            hooks[i] = emu->hook_memory_execution(code + offsets[i], [&, i](cpu_interface& cpu, uint64_t rip) {
                seen.push_back(rip);
                if (i == 1)
                {
                    predicate_rax = emu->reg<uint64_t>(x86_register::rax);
                    predicate_flags = emu->reg<uint32_t>(x86_register::eflags);
                }
                if (i == 3)
                {
                    return_rax = emu->reg<uint64_t>(x86_register::rax);
                }
                if (i == 4)
                {
                    reached_return_target = true;
                    cpu.stop();
                }
            });
            ASSERT_NE(hooks[i], nullptr);
        }

        emu->start(0);
        EXPECT_TRUE(reached_return_target);
        EXPECT_EQ(seen, (std::vector<uint64_t>{code, code + 7, code + 8, code + 13, code + 0x20}));
        EXPECT_EQ(predicate_rax, 0u);
        EXPECT_NE(predicate_flags & 0x40u, 0u);
        EXPECT_EQ(return_rax, 1u);
        std::array<uint8_t, guest.size()> after{};
        emu->read_memory(code, after.data(), after.size());
        EXPECT_EQ(after, guest);
        for (auto* hook : hooks)
        {
            emu->delete_hook(hook);
        }
    }

    TEST(WhpExactExecutionHook, ArmsPostStoreHookFromPreStoreCallback)
    {
        auto emu = create_x86_64_emulator(backend_type::whp);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto data = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0u);
        ASSERT_NE(data, 0u);

        // mov rbx,data; mov rax,0x100000001; mov [rbx],rax; mov rax,[rbx]; hlt.
        std::array<uint8_t, 27> guest{
            0x48, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0xB8, 1, 0, 0, 0, 1, 0, 0, 0, 0x48, 0x89, 0x03, 0x48, 0x8B, 0x03, 0xF4,
        };
        std::memcpy(guest.data() + 2, &data, sizeof(data));
        emu->write_memory(code, guest.data(), guest.size());
        emu->write_memory<uint64_t>(data, 1);
        emu->reg(x86_register::rip, code);

        emulator_hook* post_hook{};
        uint64_t before = UINT64_MAX;
        uint64_t after = UINT64_MAX;
        std::array<uint8_t, 3> observed_opcode{};
        auto* pre_hook = emu->hook_memory_execution_with_mode(
            code + 20, hook_interface::memory_execution_hook_mode::int3, [&](cpu_interface& cpu, uint64_t) {
                (void)cpu;
                before = emu->read_memory<uint64_t>(data);
                emu->read_memory(code + 20, observed_opcode.data(), observed_opcode.size());
                post_hook = emu->hook_memory_execution_with_mode(code + 23, hook_interface::memory_execution_hook_mode::int3,
                                                                 [&](cpu_interface& post_cpu, uint64_t) {
                                                                     (void)post_cpu;
                                                                     after = emu->read_memory<uint64_t>(data);
                                                                 });
            });
        ASSERT_NE(pre_hook, nullptr);
        emu->start(0);
        EXPECT_NE(post_hook, nullptr);
        EXPECT_EQ(before, 1u);
        EXPECT_EQ(observed_opcode, (std::array<uint8_t, 3>{0x48, 0x89, 0x03}));
        EXPECT_EQ(after, 0x100000001ull);
        EXPECT_EQ(emu->read_memory<uint64_t>(data), 0x100000001ull);
        if (post_hook)
        {
            emu->delete_hook(post_hook);
        }
        emu->delete_hook(pre_hook);
    }

    TEST(WhpSharedMemory, AliasedPagesStayCoherentAfterOriginalViewIsUnmapped)
    {
        auto emu = create_x86_64_emulator(backend_type::whp);
        memory_manager memory(*emu);
        const auto source = memory.allocate_memory(0x2000, memory_permission::read_write);
        constexpr uint64_t alias = 0x70000000;
        ASSERT_NE(source, 0u);
        ASSERT_TRUE(memory.allocate_shared_view(alias, source, 0x2000, memory_permission::read_write));

        emu->write_memory<uint32_t>(source + 0x1000, 0x12345678);
        EXPECT_EQ(emu->read_memory<uint32_t>(alias + 0x1000), 0x12345678u);
        emu->write_memory<uint32_t>(alias, 0xabcdef01);
        EXPECT_EQ(emu->read_memory<uint32_t>(source), 0xabcdef01u);

        ASSERT_TRUE(memory.release_memory(source, 0));
        EXPECT_EQ(emu->read_memory<uint32_t>(alias), 0xabcdef01u);
        EXPECT_EQ(emu->read_memory<uint32_t>(alias + 0x1000), 0x12345678u);
        ASSERT_TRUE(memory.release_memory(alias, 0));
    }

    TEST(WhpExactExecutionHook, ClearsPendingStepWhenFaultHandlerSwitchesContext)
    {
        auto emu = create_x86_64_emulator(backend_type::whp);
        memory_manager memory(*emu);
        const auto source = memory.allocate_memory(0x1000, memory_permission::all);
        const auto destination = memory.allocate_memory(0x1000, memory_permission::all);
        const auto stack = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(source, 0u);
        ASSERT_NE(destination, 0u);
        ASSERT_NE(stack, 0u);

        constexpr std::array<uint8_t, 2> invalid_instruction{0x0F, 0x0B};
        constexpr std::array<uint8_t, 2> destination_code{0x90, 0xF4};
        emu->write_memory(source, invalid_instruction.data(), invalid_instruction.size());
        emu->write_memory(destination, destination_code.data(), destination_code.size());
        emu->reg(x86_register::rip, source);
        emu->reg(x86_register::rsp, stack + 0x800);

        size_t source_hits = 0;
        size_t destination_hits = 0;
        size_t invalid_traps = 0;
        auto* source_hook = emu->hook_memory_execution(source, [&](cpu_interface&, uint64_t) { ++source_hits; });
        auto* destination_hook = emu->hook_memory_execution(destination, [&](cpu_interface& cpu, uint64_t) {
            ++destination_hits;
            if (destination_hits == 1)
            {
                cpu.stop();
            }
        });
        auto* interrupt_hook = emu->hook_interrupt([&](cpu_interface& cpu, const int vector) {
            if (vector == 6)
            {
                ++invalid_traps;
                emu->reg(x86_register::rip, destination);
            }
            else
            {
                cpu.stop();
            }
        });
        ASSERT_NE(source_hook, nullptr);
        ASSERT_NE(destination_hook, nullptr);
        ASSERT_NE(interrupt_hook, nullptr);

        EXPECT_NO_THROW(emu->start(0));
        EXPECT_EQ(source_hits, 1u);
        EXPECT_EQ(invalid_traps, 1u);
        EXPECT_EQ(destination_hits, 1u);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), destination);
        EXPECT_NO_THROW(emu->start(0));
        EXPECT_EQ(destination_hits, 1u);
        EXPECT_GE(emu->reg<uint64_t>(x86_register::rip), destination + 1);
        emu->delete_hook(source_hook);
        emu->delete_hook(destination_hook);
        emu->delete_hook(interrupt_hook);
    }

    TEST(WhpExactExecutionHook, RegisterRestoreClearsPendingStepBeforeAnotherThreadRuns)
    {
        auto emu = create_x86_64_emulator(backend_type::whp);
        memory_manager memory(*emu);
        const auto source = memory.allocate_memory(0x1000, memory_permission::all);
        const auto destination = memory.allocate_memory(0x1000, memory_permission::all);
        const auto stack = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(source, 0u);
        ASSERT_NE(destination, 0u);
        ASSERT_NE(stack, 0u);

        constexpr std::array<uint8_t, 2> invalid_instruction{0x0F, 0x0B};
        constexpr std::array<uint8_t, 2> destination_code{0x90, 0xF4};
        emu->write_memory(source, invalid_instruction.data(), invalid_instruction.size());
        emu->write_memory(destination, destination_code.data(), destination_code.size());
        emu->reg(x86_register::rip, destination);
        emu->reg(x86_register::rsp, stack + 0x800);
        const auto next_thread = emu->save_registers();
        emu->reg(x86_register::rip, source);

        size_t source_hits = 0;
        size_t destination_hits = 0;
        size_t invalid_traps = 0;
        bool guest_tf_leaked = false;
        auto* source_hook = emu->hook_memory_execution(source, [&](cpu_interface&, uint64_t) { ++source_hits; });
        auto* destination_hook = emu->hook_memory_execution(destination, [&](cpu_interface& cpu, uint64_t) {
            ++destination_hits;
            guest_tf_leaked = (emu->reg<uint64_t>(x86_register::rflags) & 0x100u) != 0;
            cpu.stop();
        });
        auto* interrupt_hook = emu->hook_interrupt([&](cpu_interface& cpu, const int vector) {
            if (vector == 6)
            {
                ++invalid_traps;
                emu->restore_registers(next_thread);
            }
            else
            {
                cpu.stop();
            }
        });
        ASSERT_NE(source_hook, nullptr);
        ASSERT_NE(destination_hook, nullptr);
        ASSERT_NE(interrupt_hook, nullptr);

        EXPECT_NO_THROW(emu->start(0));
        EXPECT_EQ(source_hits, 1u);
        EXPECT_EQ(invalid_traps, 1u);
        EXPECT_EQ(destination_hits, 1u);
        EXPECT_FALSE(guest_tf_leaked);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), destination);
        EXPECT_NO_THROW(emu->start(0));
        EXPECT_GE(emu->reg<uint64_t>(x86_register::rip), destination + 1);
        emu->delete_hook(source_hook);
        emu->delete_hook(destination_hook);
        emu->delete_hook(interrupt_hook);
    }

    TEST(WhpExactExecutionHook, EightVcpusRetireInstructionsOnSharedHookPage)
    {
        constexpr size_t vcpu_count = 8;
        constexpr uint32_t iterations = 512;
        auto emu = create_x86_64_emulator(backend_type::whp, vcpu_count);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto stacks = memory.allocate_memory(vcpu_count * 0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0u);
        ASSERT_NE(stacks, 0u);

        constexpr std::array<uint8_t, 11> guest{0xB9, 0x00, 0x02, 0x00, 0x00, 0x90, 0xFF, 0xC9, 0x75, 0xFB, 0xF4};
        std::array<std::atomic<uint32_t>, vcpu_count> hits{};
        std::array<emulator_hook*, vcpu_count> hooks{};
        for (size_t index = 0; index < vcpu_count; ++index)
        {
            const auto entry = code + index * 0x100;
            emu->write_memory(entry, guest.data(), guest.size());
            auto& cpu = emu->get_cpu(index);
            cpu.reg(x86_register::rip, entry);
            cpu.reg(x86_register::rsp, stacks + index * 0x1000 + 0x800);
            hooks[index] = emu->hook_memory_execution(
                entry + 5, [&, index](cpu_interface&, uint64_t) { hits[index].fetch_add(1, std::memory_order_relaxed); });
            ASSERT_NE(hooks[index], nullptr);
        }

        std::array<std::exception_ptr, vcpu_count> failures{};
        std::array<std::thread, vcpu_count> workers{};
        for (size_t index = 0; index < vcpu_count; ++index)
        {
            workers[index] = std::thread([&, index] {
                try
                {
                    emu->get_cpu(index).start(0);
                }
                catch (...)
                {
                    failures[index] = std::current_exception();
                }
            });
        }
        for (auto& worker : workers)
        {
            worker.join();
        }

        uint32_t total_hits = 0;
        for (size_t index = 0; index < vcpu_count; ++index)
        {
            if (failures[index])
            {
                EXPECT_NO_THROW(std::rethrow_exception(failures[index]));
            }
            EXPECT_EQ(emu->get_cpu(index).reg(x86_register::rcx), 0u);
            const auto count = hits[index].load(std::memory_order_relaxed);
            EXPECT_LE(count, iterations);
            total_hits += count;
            emu->delete_hook(hooks[index]);
        }
        EXPECT_GT(total_hits, 0u);
    }

    TEST(WhpExactExecutionHook, PerHookInt3LeavesAutomaticModeUnchanged)
    {
        auto emu = create_x86_64_emulator(backend_type::whp);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto stack = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0u);
        ASSERT_NE(stack, 0u);

        constexpr std::array<uint8_t, 11> guest{0xB9, 0xA0, 0x86, 0x01, 0x00, 0xFF, 0xC9, 0x75, 0xFC, 0x90, 0xF4};
        emu->write_memory(code, guest.data(), guest.size());
        emu->reg(x86_register::rip, code);
        emu->reg(x86_register::rsp, stack + 0x800);

        uint32_t hits = 0;
        auto* hook = emu->hook_memory_execution_with_mode(code + 9, hook_interface::memory_execution_hook_mode::int3,
                                                          [&](cpu_interface& cpu, uint64_t rip) {
                                                              EXPECT_EQ(rip, code + 9);
                                                              ++hits;
                                                              cpu.stop();
                                                          });
        ASSERT_NE(hook, nullptr);
        emu->start(0);
        EXPECT_EQ(hits, 1u);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::rcx), 0u);
        emu->delete_hook(hook);
        EXPECT_EQ(emu->read_memory<uint8_t>(code + 9), 0x90u);
        emu->start(0);
    }

    TEST(WhpExactExecutionHook, MmioRefreshesPublishInSampleOrderAcrossVcpus)
    {
        auto emu = create_x86_64_emulator(backend_type::whp, 2);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto stacks = memory.allocate_memory(0x2000, memory_permission::read_write);
        ASSERT_NE(code, 0u);
        ASSERT_NE(stacks, 0u);

        constexpr uint64_t mmio_address = 0x7000000000;
        std::mutex gate{};
        std::condition_variable changed{};
        std::atomic<uint32_t> refreshes{};
        bool first_refresh_entered = false;
        bool second_guest_completed = false;
        ASSERT_TRUE(memory.allocate_mmio(
            mmio_address, 0x1000,
            [&](uint64_t, void* data, size_t size) {
                const auto sample = refreshes.fetch_add(1, std::memory_order_relaxed) + 1;
                const uint64_t value = sample == 1 ? 100 : 101;
                std::memset(data, 0, size);
                std::memcpy(data, &value, sizeof(value));
                if (sample == 1)
                {
                    std::unique_lock lock(gate);
                    first_refresh_entered = true;
                    changed.notify_all();
                    changed.wait_for(lock, std::chrono::seconds(1), [&] { return second_guest_completed; });
                }
            },
            [](uint64_t, const void*, size_t) {}));

        std::array<uint8_t, 11> guest{0x48, 0xA1, 0, 0, 0, 0, 0, 0, 0, 0, 0xF4};
        std::memcpy(guest.data() + 2, &mmio_address, sizeof(mmio_address));
        for (size_t index = 0; index < 2; ++index)
        {
            auto& cpu = emu->get_cpu(index);
            const auto entry = code + index * 0x100;
            emu->write_memory(entry, guest.data(), guest.size());
            cpu.reg(x86_register::rip, entry);
            cpu.reg(x86_register::rsp, stacks + index * 0x1000 + 0x800);
        }

        std::array<uint64_t, 2> values{};
        std::array<uint32_t, 2> completion_order{};
        std::array<std::exception_ptr, 2> failures{};
        std::atomic<uint32_t> completions{};
        auto run_cpu = [&](size_t index) {
            try
            {
                auto& cpu = emu->get_cpu(index);
                cpu.start(0);
                values[index] = cpu.reg<uint64_t>(x86_register::rax);
                completion_order[index] = completions.fetch_add(1, std::memory_order_relaxed) + 1;
            }
            catch (...)
            {
                failures[index] = std::current_exception();
            }
            if (index == 1)
            {
                const std::scoped_lock lock(gate);
                second_guest_completed = true;
                changed.notify_all();
            }
        };

        std::thread first([&] { run_cpu(0); });
        bool first_seen = false;
        {
            std::unique_lock lock(gate);
            first_seen = changed.wait_for(lock, std::chrono::seconds(2), [&] { return first_refresh_entered; });
        }
        if (!first_seen)
        {
            first.join();
            FAIL() << "First MMIO refresh did not start";
            return;
        }

        std::thread second([&] { run_cpu(1); });
        first.join();
        second.join();
        for (const auto& failure : failures)
        {
            if (failure)
            {
                EXPECT_NO_THROW(std::rethrow_exception(failure));
                return;
            }
        }
        ASSERT_GE(refreshes.load(std::memory_order_relaxed), 2u);
        ASSERT_NE(completion_order[0], completion_order[1]);
        if (completion_order[0] < completion_order[1])
        {
            EXPECT_LE(values[0], values[1]);
        }
        else
        {
            EXPECT_LE(values[1], values[0]);
        }
    }

    TEST(WhpKuserTime, InterruptTimeReadStaysMonotonicDuringPeerRefresh)
    {
        auto emu = create_x86_64_emulator(backend_type::whp, 2);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto stacks = memory.allocate_memory(0x2000, memory_permission::read_write);
        ASSERT_NE(code, 0u);
        ASSERT_NE(stacks, 0u);

        constexpr uint64_t kusd_address = 0x7FFE0000;
        std::atomic<uint32_t> refreshes{};
        ASSERT_TRUE(memory.allocate_mmio(
            kusd_address, 0x1000,
            [&](uint64_t, void* data, size_t size) {
                const auto sample = refreshes.fetch_add(1, std::memory_order_relaxed) + 1;
                const uint64_t value = 0x00000001FFFFFE00ull + static_cast<uint64_t>(sample) * 0x100;
                const auto high = static_cast<uint32_t>(value >> 32);
                const auto low = static_cast<uint32_t>(value);
                std::memset(data, 0, size);
                std::memcpy(static_cast<uint8_t*>(data) + 8, &low, sizeof(low));
                std::memcpy(static_cast<uint8_t*>(data) + 12, &high, sizeof(high));
                std::memcpy(static_cast<uint8_t*>(data) + 16, &high, sizeof(high));
            },
            [](uint64_t, const void*, size_t) {}));

        std::array<uint8_t, 47> reader{
            0x48, 0xBB, 0,    0,    0,    0,    0,    0,    0,    0,    0x45, 0x31, 0xC0, 0x45, 0x31, 0xC9,
            0x8B, 0x13, 0x8B, 0x43, 0xFC, 0x3B, 0x53, 0x04, 0x75, 0xF6, 0x48, 0xC1, 0xE2, 0x20, 0x48, 0x09,
            0xD0, 0x4C, 0x39, 0xC0, 0x72, 0x05, 0x49, 0x89, 0xC0, 0xEB, 0xE5, 0x49, 0x89, 0xC1, 0xF4,
        };
        constexpr uint64_t interrupt_high1 = kusd_address + 12;
        std::memcpy(reader.data() + 2, &interrupt_high1, sizeof(interrupt_high1));
        emu->write_memory(code, reader.data(), reader.size());

        std::array<uint8_t, 11> refresher{0x48, 0xA1, 0, 0, 0, 0, 0, 0, 0, 0, 0xF4};
        constexpr uint64_t interrupt_low = kusd_address + 8;
        std::memcpy(refresher.data() + 2, &interrupt_low, sizeof(interrupt_low));
        emu->write_memory(code + 0x100, refresher.data(), refresher.size());

        auto& reader_cpu = emu->get_cpu(0);
        auto& refresher_cpu = emu->get_cpu(1);
        reader_cpu.reg(x86_register::rip, code);
        reader_cpu.reg(x86_register::rsp, stacks + 0x800);
        refresher_cpu.reg(x86_register::rsp, stacks + 0x1800);

        std::exception_ptr reader_failure{};
        std::atomic<bool> reader_finished{};
        std::thread reader_thread([&] {
            try
            {
                reader_cpu.start(0);
            }
            catch (...)
            {
                reader_failure = std::current_exception();
            }
            reader_finished.store(true, std::memory_order_release);
        });

        for (uint32_t attempt = 0; attempt < 100 && refreshes.load(std::memory_order_relaxed) < 1; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool reader_started = refreshes.load(std::memory_order_relaxed) >= 1;
        std::exception_ptr refresher_failure{};
        if (reader_started)
        {
            for (uint32_t attempt = 0; attempt < 100 && !reader_finished.load(std::memory_order_acquire); ++attempt)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                refresher_cpu.reg(x86_register::rip, code + 0x100);
                try
                {
                    refresher_cpu.start(0);
                }
                catch (...)
                {
                    refresher_failure = std::current_exception();
                    break;
                }
            }
        }
        reader_cpu.stop();
        reader_thread.join();

        ASSERT_TRUE(reader_started);
        if (reader_failure)
        {
            std::rethrow_exception(reader_failure);
        }
        if (refresher_failure)
        {
            std::rethrow_exception(refresher_failure);
        }
        EXPECT_GT(refreshes.load(std::memory_order_relaxed), 2u);
        EXPECT_EQ(reader_cpu.reg<uint64_t>(x86_register::r9), 0u)
            << "Previous InterruptTime=" << reader_cpu.reg<uint64_t>(x86_register::r8);
        EXPECT_GE(reader_cpu.reg<uint64_t>(x86_register::r8), 0x0000000200000000ull);
        EXPECT_GE(refresher_cpu.reg<uint64_t>(x86_register::rax), 0x100u);
    }
}
