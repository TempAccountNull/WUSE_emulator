#include "emulation_test_utils.hpp"

#include <memory_manager.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
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
}
