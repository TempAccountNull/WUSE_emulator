#include "emulation_test_utils.hpp"

#include <memory_manager.hpp>

#include <array>
#include <atomic>
#include <exception>
#include <thread>

namespace sogen::test
{
    TEST(WhpExactExecutionHook, HostThreadCanRetireDeferredInt3WithoutSkippingTheInstruction)
    {
        auto emu = create_x86_64_emulator(backend_type::whp);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto stack = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0u);
        ASSERT_NE(stack, 0u);

        constexpr std::array<uint8_t, 9> guest{0xB8, 0, 0, 0, 0, 0xFF, 0xC0, 0x90, 0xF4};
        emu->write_memory(code, guest.data(), guest.size());
        emu->reg(x86_register::rip, code);
        emu->reg(x86_register::rsp, stack + 0x800);

        std::atomic<uint32_t> hits{};
        auto* hook = emu->hook_memory_execution_with_mode(code + 5, hook_interface::memory_execution_hook_mode::int3,
                                                          [&](cpu_interface& cpu, uint64_t) {
                                                              hits.fetch_add(1, std::memory_order_relaxed);
                                                              cpu.stop();
                                                          });
        ASSERT_NE(hook, nullptr);

        emu->start(0);
        EXPECT_EQ(hits.load(std::memory_order_relaxed), 1u);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::rax), 0u);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code + 5);

        std::exception_ptr deletion_failure{};
        std::thread maintenance([&] {
            try
            {
                emu->delete_hook(hook);
            }
            catch (...)
            {
                deletion_failure = std::current_exception();
            }
        });
        maintenance.join();
        ASSERT_EQ(deletion_failure, nullptr);

        EXPECT_EQ(emu->read_memory<uint8_t>(code + 5), 0xFFu);
        emu->start(0);

        EXPECT_EQ(hits.load(std::memory_order_relaxed), 1u);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::rax), 1u);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags) & 0x100u, 0u);
        std::array<uint8_t, guest.size()> after{};
        emu->read_memory(code, after.data(), after.size());
        EXPECT_EQ(after, guest);
    }
}
