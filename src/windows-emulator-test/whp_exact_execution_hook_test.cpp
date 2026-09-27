#include "emulation_test_utils.hpp"

#include <memory_manager.hpp>

#include <algorithm>
#include <array>
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
        constexpr std::array<uint8_t, 14> function{
            0xB8, 0, 0, 0, 0, 0x85, 0xC0, 0x90,
            0xB8, 1, 0, 0, 0, 0xC3};
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
        EXPECT_EQ(seen, (std::vector<uint64_t>{
            code, code + 7, code + 8, code + 13, code + 0x20}));
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
}