#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <memory_manager.hpp>
#include <array>
#include <thread>

namespace sogen::test
{
    // icicle SMP (multi-vCPU): construct N icicle VMs that share one guest address space via smp_shared
    // pages (SMP steps 1-2), and verify the backend exposes N cpus and that EACH vCPU independently
    // executes over the shared RAM. Mapping + register setup run with the VMs quiesced and each start()
    // runs sequentially on this thread -- the safe window per the backend's SMP invariant (cross-VM
    // mutation during true parallel execution is step 6).
    TEST(IcicleSmp, TwoVcpusConstructShareMemoryAndEachExecutes)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        ASSERT_EQ(emu->vcpu_count(), 2U);
        EXPECT_TRUE(emu->supports_multiple_vcpus());
        EXPECT_NE(&emu->get_cpu(0), &emu->get_cpu(1));

        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(code, 0U);

        // A write through the machine (master VM) is visible via the read surface: the shared page.
        const uint64_t marker = 0x0123456789abcdefULL;
        emu->write_memory(code + 0x100, &marker, sizeof(marker));
        uint64_t readback{};
        emu->read_memory(code + 0x100, &readback, sizeof(readback));
        EXPECT_EQ(readback, marker);

        // Shared executable code (4 NOPs): each vCPU runs it from its own VM and advances its own RIP,
        // proving the code page is aliased into every VM and each VM executes independently.
        const std::array<uint8_t, 4> nops{0x90, 0x90, 0x90, 0x90};
        emu->write_memory(code, nops.data(), nops.size());
        for (size_t i = 0; i < emu->vcpu_count(); ++i)
        {
            auto& cpu = emu->get_cpu(i);
            cpu.reg(x86_register::rip, code);
            cpu.start(4);
            EXPECT_EQ(cpu.reg(x86_register::rip), code + 4) << "vCPU " << i << " did not execute the shared code page";
        }
    }

    // A vcpu_count of 1 keeps the single-vCPU contract: one cpu, no multi-vCPU capability (this is the
    // path every existing icicle test and the default backend selection uses).
    TEST(IcicleSmp, SingleVcpuFactoryReportsOneCpu)
    {
        auto emu = icicle::create_x86_64_emulator(1);
        EXPECT_EQ(emu->vcpu_count(), 1U);
        EXPECT_FALSE(emu->supports_multiple_vcpus());
    }

    // Step 6.1 verification: two vCPUs execute a bounded countdown loop over the SAME shared code page
    // SIMULTANEOUSLY on their own threads. Each vCPU's start() ends by calling the machine's
    // perform_pending_actions on its worker thread, so this races that path — partition_mutex_ must make it
    // safe. Each vCPU has its own RCX; both must reach 0. (No runtime hooks/mutation here — that is 6.3/6.4.)
    TEST(IcicleSmp, TwoVcpusExecuteConcurrentlyOverSharedCode)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        ASSERT_EQ(emu->vcpu_count(), 2U);

        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(code, 0U);

        // dec rcx (48 FF C9); jnz -5 back to dec (75 FB); then a NOP sled to land in after the loop.
        std::array<uint8_t, 32> prog{};
        prog.fill(0x90);
        prog[0] = 0x48;
        prog[1] = 0xFF;
        prog[2] = 0xC9;
        prog[3] = 0x75;
        prog[4] = 0xFB;
        emu->write_memory(code, prog.data(), prog.size());

        constexpr uint64_t iters = 100000;
        const auto run = [&](const size_t i) {
            auto& cpu = emu->get_cpu(i);
            cpu.reg(x86_register::rcx, iters);
            cpu.reg(x86_register::rip, code);
            cpu.start(2 * iters + 16); // N dec + N jnz + a few trailing NOPs
        };

        std::thread t0(run, 0);
        std::thread t1(run, 1);
        t0.join();
        t1.join();

        EXPECT_EQ(emu->get_cpu(0).reg(x86_register::rcx), 0U) << "vCPU 0 loop did not complete";
        EXPECT_EQ(emu->get_cpu(1).reg(x86_register::rcx), 0U) << "vCPU 1 loop did not complete";
    }
}
