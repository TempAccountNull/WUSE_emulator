#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include "../emulator/scoped_hook.hpp"
#include <memory_manager.hpp>
#include <utils/finally.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <span>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

    TEST(IcicleSmp, ForcedSharedMemoryOnSingleVcpu)
    {
        const char* prior_value = std::getenv("SOGEN_ICICLE_FORCE_SMP_MEMORY");
        const std::string prior = prior_value ? prior_value : "";
        ASSERT_EQ(_putenv_s("SOGEN_ICICLE_FORCE_SMP_MEMORY", "1"), 0);
        const auto restore = utils::finally([&] {
            _putenv_s("SOGEN_ICICLE_FORCE_SMP_MEMORY", prior.c_str());
        });

        auto emu = icicle::create_x86_64_emulator(1);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto data = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0U);
        ASSERT_NE(data, 0U);

        std::array<uint8_t, 12> program{0x48, 0xB8};
        std::memcpy(program.data() + 2, &data, sizeof(data));
        program[10] = 0xFE;
        program[11] = 0x00;
        emu->write_memory(code, program.data(), program.size());

        auto& cpu = emu->get_cpu(0);
        cpu.reg(x86_register::rip, code);
        cpu.start(2);

        uint8_t result = 0;
        emu->read_memory(data, &result, sizeof(result));
        EXPECT_EQ(result, 1U);
        EXPECT_EQ(cpu.reg(x86_register::rip), code + program.size());
    }

    // A reservation claims guest VA without backing it in Icicle. A host write into that
    // range must fail until the guest commits it; treating the failure as an implicit
    // commit would hide stale or invalid output pointers from syscall/device callers.
    TEST(IcicleSmp, ReserveOnlyHostWriteFailsUntilCommit)
    {
        auto emu = icicle::create_x86_64_emulator(1);
        memory_manager memory(*emu);
        constexpr uint64_t base = 0x60000000;
        constexpr size_t reservation_size = 0x3000;
        const nt_memory_permission permissions{memory_permission::read_write};
        ASSERT_TRUE(memory.allocate_memory(base, reservation_size, permissions, true));
        ASSERT_TRUE(memory.get_reserved_regions().at(base).committed_regions.empty());

        const std::array<uint8_t, 6264> payload{};
        EXPECT_FALSE(emu->try_write_memory(base, payload.data(), payload.size()));
        EXPECT_TRUE(memory.get_reserved_regions().at(base).committed_regions.empty());

        ASSERT_TRUE(memory.commit_memory(base, reservation_size, permissions));
        EXPECT_TRUE(emu->try_write_memory(base, payload.data(), payload.size()));
    }

    TEST(IcicleSmp, ContextWriteRetryReportsFinalFailureOnly)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        constexpr uint64_t base = 0x61000000;
        constexpr size_t reservation_size = 0x3000;
        const nt_memory_permission permissions{memory_permission::read_write};
        ASSERT_TRUE(memory.allocate_memory(base, reservation_size, permissions, true));

        const std::array<uint8_t, 1232> context{};
        testing::internal::CaptureStderr();
        EXPECT_THROW(emu->write_memory(base, context.data(), context.size()), std::runtime_error);
        const auto failure = testing::internal::GetCapturedStderr();
        EXPECT_NE(failure.find("[ICWRITE] context-retry"), std::string::npos);
        EXPECT_NE(failure.find("final=failure"), std::string::npos);
        EXPECT_NE(failure.find("caller_vcpu=-1"), std::string::npos);

        ASSERT_TRUE(memory.commit_memory(base, reservation_size, permissions));
        testing::internal::CaptureStderr();
        EXPECT_NO_THROW(emu->write_memory(base, context.data(), context.size()));
        const auto success = testing::internal::GetCapturedStderr();
        EXPECT_EQ(success.find("[ICWRITE] context-retry"), std::string::npos);
    }

    // A protect issued inside vCPU 0's hook changes the shared permission bytes immediately.
    // vCPU 1 must also discard its warmed write TLB before its next quantum; the old deferred
    // protect guard skipped this peer work because the issuer's own protect advanced perm_epoch.
    TEST(IcicleSmp, HookProtectionInvalidatesPeerWriteTlb)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto target = memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto trigger_code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto trigger_data = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0U);
        ASSERT_NE(target, 0U);
        ASSERT_NE(trigger_code, 0U);
        ASSERT_NE(trigger_data, 0U);

        // movabs rax,target; inc byte ptr [rax]; jmp $.
        std::array<uint8_t, 14> writer{0x48, 0xB8};
        std::memcpy(writer.data() + 2, &target, sizeof(target));
        writer[10] = 0xFE; writer[11] = 0x00;
        writer[12] = 0xEB; writer[13] = 0xFE;
        emu->write_memory(code, writer.data(), writer.size());
        auto& peer = emu->get_cpu(1);
        peer.reg(x86_register::rip, code);
        peer.start(2); // execute the write once and warm the data TLB
        uint8_t value{};
        emu->read_memory(target, &value, sizeof(value));
        ASSERT_EQ(value, 1U);

        // movabs rax,trigger_data; mov rax,[rax] enters the hook on vCPU 0.
        std::array<uint8_t, 13> trigger{0x48, 0xB8};
        std::memcpy(trigger.data() + 2, &trigger_data, sizeof(trigger_data));
        trigger[10] = 0x48; trigger[11] = 0x8B; trigger[12] = 0x00;
        emu->write_memory(trigger_code, trigger.data(), trigger.size());
        bool protected_in_hook = false;
        emu->hook_memory_read(trigger_data, 1, [&](cpu_interface&, uint64_t, const void*, size_t) {
            protected_in_hook = memory.protect_memory(target, 0x1000, memory_permission::read);
        });
        auto& issuer = emu->get_cpu(0);
        issuer.reg(x86_register::rip, trigger_code);
        issuer.start(2);
        ASSERT_TRUE(protected_in_hook);

        peer.reg(x86_register::rip, code);
        try
        {
            peer.start(2); // drains the queued peer cache refresh before guest execution
            FAIL() << "peer write unexpectedly succeeded after protection";
        }
        catch (const std::runtime_error& error)
        {
            EXPECT_NE(std::string(error.what()).find("WritePerm"), std::string::npos);
        }
        emu->read_memory(target, &value, sizeof(value));
        EXPECT_EQ(value, 1U) << "peer wrote through a stale TLB after protection";
    }

    // A vcpu_count of 1 keeps the single-vCPU contract: one cpu, no multi-vCPU capability (this is the
    // path every existing icicle test and the default backend selection uses).
    TEST(IcicleSmp, SingleVcpuFactoryReportsOneCpu)
    {
        auto emu = icicle::create_x86_64_emulator(1);
        EXPECT_EQ(emu->vcpu_count(), 1U);
        EXPECT_FALSE(emu->supports_multiple_vcpus());
    }

    // A callback can own a scoped hook referring to the same registration.
    // Destroying the emulator must empty its registration table before the
    // callback destructor reenters delete_hook(); clearing in place double-frees.
    TEST(IcicleSmp, DestroyReentrantScopedHook)
    {
        for (const auto vcpus : {1U, 2U})
        {
            auto emu = icicle::create_x86_64_emulator(vcpus);
            constexpr uint64_t page = 0x60000000;
            // Hook registration itself does not require mapped guest memory.
            auto scope = std::make_shared<scoped_hook>(*emu);
            auto* hook = emu->hook_memory_read(page, 8,
                [scope](cpu_interface&, uint64_t, const void*, size_t) {});
            *scope = hook;
            scope.reset();
            EXPECT_NO_THROW(emu.reset()) << "vCPUs=" << vcpus;
        }
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

    // Step 6.3/6.4 verification: while both vCPUs SPIN on their own threads, a third thread performs many
    // cross-VM map_memory calls. Each map must pause both vCPUs (run_with_vcpus_paused) so it can touch
    // their single-threaded MMUs safely — the whole point of step 6. Success = no crash/hang/UB and every
    // mapped region is coherent afterwards. (Guarded by an external timeout when run, in case of a deadlock.)
    TEST(IcicleSmp, CrossVmMapDuringConcurrentExecutionIsSafe)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        ASSERT_EQ(emu->vcpu_count(), 2U);

        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(code, 0U);
        const std::array<uint8_t, 2> spin{0xEB, 0xFE}; // jmp $ (spin forever until stopped)
        emu->write_memory(code, spin.data(), spin.size());

        std::atomic<bool> stop_flag{false};
        const auto spin_vcpu = [&](const size_t i) {
            auto& cpu = emu->get_cpu(i);
            cpu.reg(x86_register::rip, code);
            while (!stop_flag.load())
            {
                cpu.start(1000000); // returns on quiesce-resume/exhaust or when stopped
            }
        };

        std::thread t0(spin_vcpu, 0);
        std::thread t1(spin_vcpu, 1);

        // Mutator: allocate_memory drives the (now paused-routed) map_memory on all N VMs. Runs while the
        // two vCPUs spin, so each allocation pauses them mid-execution.
        constexpr size_t map_count = 400;
        std::vector<uint64_t> mapped;
        mapped.reserve(map_count);
        for (size_t k = 0; k < map_count; ++k)
        {
            mapped.push_back(memory.allocate_memory(0x1000, memory_permission::all));
        }

        stop_flag.store(true);
        emu->get_cpu(0).stop();
        emu->get_cpu(1).stop();
        t0.join();
        t1.join();

        // Each region mapped during concurrent execution is coherent + usable (checked single-threaded now).
        for (size_t k = 0; k < mapped.size(); ++k)
        {
            const auto addr = mapped[k];
            ASSERT_NE(addr, 0U);
            const uint64_t value = 0xA5A5A5A500000000ULL | k;
            emu->write_memory(addr, &value, sizeof(value));
            uint64_t readback{};
            emu->read_memory(addr, &readback, sizeof(readback));
            EXPECT_EQ(readback, value) << "mapped region " << k << " not coherent";
        }
    }

    // Step 6.5 verification — the backend peer-in-hook deadlock reproducer. vCPU B is parked INSIDE an
    // execution-hook callback (unstoppable: still in icicle run(), waiting for a flag only vCPU A sets
    // AFTER its install call returns). vCPU A, from inside its own read hook (in-hook context, exactly the
    // windows_emulator BEL-held shape, e.g. on_module_load installing section hooks), requests a ranged
    // execution hook. The pre-6.5 code paused peers from there and waited for B's run_active_ — A waits
    // for B, B waits for A's flag: hang. The fix defers the install to A's own next instruction (async
    // routing). Pass = completes without hanging, the deferred hook actually fires, and both loops finish.
    TEST(IcicleSmp, RangedExecHookDeferredFromInHookContextWithPeerParkedInHook)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        ASSERT_EQ(emu->vcpu_count(), 2U);

        memory_manager memory(*emu);
        const auto page = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(page, 0U);
        const uint64_t data = page + 0x400; // A's hooked read target (shared page)

        // A @ +0x000: movabs rbx,data; mov rcx,[rbx] (fires read hook); movabs rcx,N; dec/jnz loop @ +0x17.
        // B @ +0x200: movabs rcx,N; dec/jnz loop, dec @ +0x20A carries B's exact-address exec hook.
        std::array<uint8_t, 0x600> prog{};
        prog.fill(0x90);
        const auto emit_movabs = [](uint8_t* p, uint8_t rex_op, const uint64_t imm) {
            p[0] = 0x48;
            p[1] = rex_op;
            std::memcpy(p + 2, &imm, sizeof(imm));
        };
        emit_movabs(&prog[0x00], 0xBB, data);         // movabs rbx, data
        prog[0x0A] = 0x48; prog[0x0B] = 0x8B; prog[0x0C] = 0x03; // mov rcx, [rbx]
        constexpr uint64_t iters = 50000;
        emit_movabs(&prog[0x0D], 0xB9, iters);        // movabs rcx, N
        prog[0x17] = 0x48; prog[0x18] = 0xFF; prog[0x19] = 0xC9; // a_loop: dec rcx  ← deferred ranged hook range
        prog[0x1A] = 0x75; prog[0x1B] = 0xFB;                     // jnz a_loop
        for (auto& b : std::span(prog).subspan(0x400, 0x200))
        {
            b = 0; // data cells must read 0, not NOP bytes (Test 2 polls `target` for nonzero)
        }
        emit_movabs(&prog[0x200], 0xB9, iters);       // movabs rcx, N
        prog[0x20A] = 0x48; prog[0x20B] = 0xFF; prog[0x20C] = 0xC9; // b_loop: dec rcx ← exact exec hook
        prog[0x20D] = 0x75; prog[0x20E] = 0xFB;                     // jnz b_loop
        emu->write_memory(page, prog.data(), prog.size());

        std::atomic<bool> b_parked{false};
        std::atomic<bool> release_b{false};
        std::atomic<bool> install_requested{false};
        std::atomic<size_t> ranged_hits{0};

        // B parks inside its FIRST hook invocation until A's install call has returned (old code: A could
        // never get there because it was waiting for B to leave run() — the deadlock).
        emu->hook_memory_execution(page + 0x20A, [&](cpu_interface&, const uint64_t) {
            static std::atomic<bool> parked_once{false};
            if (!parked_once.exchange(true))
            {
                b_parked.store(true);
                // Bounded so a regression fails the run instead of hanging the suite forever; generous
                // enough that the fixed path (release within microseconds) never trips it.
                for (int i = 0; i < 3000 && !release_b.load(std::memory_order_acquire); ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        });

        // A's read hook = the in-hook (BEL-held shape) context: request a ranged exec hook over A's loop.
        emu->hook_memory_read(data, 8, [&](cpu_interface&, uint64_t, const void*, size_t) {
            for (int i = 0; i < 2000 && !b_parked.load(std::memory_order_acquire); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // With 6.5 this DEFERS (no peer pause): pre-6.5 this call hung waiting for parked B.
            emu->hook_memory_range_execution(page + 0x17, 16, [&](cpu_interface&, const uint64_t) {
                ranged_hits.fetch_add(1);
            });
            install_requested.store(true);
            release_b.store(true); // only reachable once the install call RETURNED
        });

        emu->get_cpu(0).reg(x86_register::rip, page + 0x000);
        emu->get_cpu(1).reg(x86_register::rip, page + 0x200);

        const auto run = [&](const size_t i) {
            try
            {
                emu->get_cpu(i).start(2 * iters + 64);
            }
            catch (const std::exception& e)
            {
                ADD_FAILURE() << "vCPU " << i << " start threw: " << e.what();
            }
        };
        std::thread t0(run, 0);
        std::thread t1(run, 1);
        t0.join();
        t1.join();

        EXPECT_TRUE(install_requested.load());
        EXPECT_GT(ranged_hits.load(), 0U) << "deferred ranged hook never fired on vCPU 0";
        EXPECT_EQ(emu->get_cpu(0).reg(x86_register::rcx), 0U);
        EXPECT_EQ(emu->get_cpu(1).reg(x86_register::rcx), 0U);
    }

    // Keep vCPU 0 executing one cached JIT loop while vCPU 1 writes the loop's immediate
    // through GUEST code. The second guest store uses its warmed direct-write TLB pointer.
    // The disabled arm is a bounded negative control; the enabled arm must observe the patch
    // without restarting vCPU 0 from the test.
    static void continuous_peer_guest_code_write(bool wake_enabled)
    {
        std::vector<std::pair<std::string, std::string>> prior_env;
        const auto set_env = [&](const char* name, const char* value) {
            const char* old = std::getenv(name);
            prior_env.emplace_back(name, old ? old : "");
            EXPECT_EQ(_putenv_s(name, value), 0);
        };
        const auto restore = utils::finally([&] {
            for (auto it = prior_env.rbegin(); it != prior_env.rend(); ++it)
            {
                _putenv_s(it->first.c_str(), it->second.c_str());
            }
        });
        set_env("SOGEN_ICICLE_JIT", "1");
        set_env("SOGEN_ICICLE_INSTRUCTION_HOOK", "0");
        set_env("SOGEN_ICICLE_LEAN_BARRIER", "1");
        set_env("SOGEN_SMP_LEAN_EPOCH_HOOK", "0");
        set_env("SOGEN_SMP_PRELIFT_EPOCH", "0");
        set_env("SOGEN_SMP_FAST_WRITE_EPOCH", "1");
        set_env("SOGEN_SMP_CODE_EPOCH_ONLY", "1");
        set_env("ICICLE_ALWAYS_FLUSH_VARS", "1");
        set_env("SOGEN_SMP_EXEC_WRITE_WAKE", wake_enabled ? "1" : "0");

        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        const auto page_p = memory.allocate_memory(0x1000, memory_permission::all);
        const auto page_q = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(page_p, 0U);
        ASSERT_NE(page_q, 0U);

        // P's immediate begins at aligned P+4 so vCPU 1 can patch it with a dword store.
        // vCPU 0 stays in the same JIT hot loop across the writer's guest store.
        const std::array<uint8_t, 10> p{0x90, 0x90, 0x90, 0xB8, 0x11, 0x11, 0x00, 0x00, 0xEB, 0xF9};
        emu->write_memory(page_p, p.data(), p.size());

        // Q: movabs rax, P+4; store 0x1111 to warm the write TLB; store 0x2222; jmp $.
        std::array<uint8_t, 24> q{0x48, 0xB8};
        const uint64_t target = page_p + 4;
        for (size_t i = 0; i < 8; ++i)
        {
            q[2 + i] = static_cast<uint8_t>(target >> (i * 8));
        }
        q[10] = 0xC7;
        q[11] = 0x00;
        q[12] = 0x11;
        q[13] = 0x11;
        q[16] = 0xC7;
        q[17] = 0x00;
        q[18] = 0x22;
        q[19] = 0x22;
        q[22] = 0xEB;
        q[23] = 0xFE;
        emu->write_memory(page_q, q.data(), q.size());

        auto& cpu0 = emu->get_cpu(0);
        auto& cpu1 = emu->get_cpu(1);
        cpu0.reg(x86_register::rip, page_p + 3);
        cpu0.start(10); // lift and warm vCPU 0's translation before concurrency
        ASSERT_EQ(cpu0.reg(x86_register::rax), 0x1111U);
        cpu0.reg(x86_register::rip, page_p + 3);
        cpu1.reg(x86_register::rip, page_q);

        std::atomic<bool> peer_done{false};
        std::exception_ptr peer_error;
        std::thread peer([&] {
            try
            {
                cpu0.start(0);
            }
            catch (...)
            {
                peer_error = std::current_exception();
            }
            peer_done.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const bool peer_running = !peer_done.load(std::memory_order_acquire);

        std::exception_ptr main_error;
        bool writer_ok = false;
        try
        {
            if (peer_running)
            {
                cpu1.start(3);
                writer_ok = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        catch (...)
        {
            main_error = std::current_exception();
        }
        cpu0.stop();
        peer.join();
        if (peer_error)
        {
            std::rethrow_exception(peer_error);
        }
        if (main_error)
        {
            std::rethrow_exception(main_error);
        }
        ASSERT_TRUE(peer_running) << "vCPU 0 exited its continuous hot loop before the guest write";
        ASSERT_TRUE(writer_ok) << "guest writer did not complete";
        const auto wake_profile = emu->smp_profile();
        if (wake_enabled)
        {
            ASSERT_EQ(wake_profile.size(), 2U);
            EXPECT_GE(wake_profile[0].exec_write_guest_jit_total + wake_profile[0].exec_write_guest_mmu_total, 2U);
            EXPECT_EQ(wake_profile[0].exec_write_wake_events_total, wake_profile[0].exec_write_guest_jit_total +
                                                                        wake_profile[0].exec_write_guest_mmu_total +
                                                                        wake_profile[0].exec_write_host_total);
            EXPECT_GE(wake_profile[0].exec_write_owner_flushes_total, 1U);
        }
        else
        {
            EXPECT_TRUE(wake_profile.empty());
        }
        EXPECT_EQ(cpu0.reg(x86_register::rax), wake_enabled ? 0x2222U : 0x1111U)
            << "continuous peer used the wrong code version after the guest store";
        if (wake_enabled)
        {
            const uint32_t same_code = 0x2222;
            emu->write_memory(page_p + 4, &same_code, sizeof(same_code));
            const auto after_host_write = emu->smp_profile();
            ASSERT_EQ(after_host_write.size(), 2U);
            EXPECT_GT(after_host_write[0].exec_write_host_total, wake_profile[0].exec_write_host_total);
        }
    }

    TEST(IcicleSmp, ContinuousPeerGuestWriteWithoutWakeIsStale)
    {
        continuous_peer_guest_code_write(false);
    }

    TEST(IcicleSmp, ContinuousPeerGuestWriteWithWakeSeesNewCode)
    {
        continuous_peer_guest_code_write(true);
    }

    TEST(IcicleSmp, ExecutablePageDataWriteFiltersUnrelatedStores)
    {
        std::vector<std::pair<std::string, std::string>> prior_env;
        const auto set_env = [&](const char* name, const char* value) {
            const char* old = std::getenv(name);
            prior_env.emplace_back(name, old ? old : "");
            ASSERT_EQ(_putenv_s(name, value), 0);
        };
        const auto restore = utils::finally([&] {
            for (auto it = prior_env.rbegin(); it != prior_env.rend(); ++it)
            {
                _putenv_s(it->first.c_str(), it->second.c_str());
            }
        });
        set_env("SOGEN_ICICLE_JIT", "1");
        set_env("SOGEN_ICICLE_INSTRUCTION_HOOK", "0");
        set_env("SOGEN_ICICLE_LEAN_BARRIER", "1");
        set_env("SOGEN_SMP_LEAN_EPOCH_HOOK", "0");
        set_env("SOGEN_SMP_PRELIFT_EPOCH", "0");
        set_env("SOGEN_SMP_FAST_WRITE_EPOCH", "1");
        set_env("SOGEN_SMP_CODE_EPOCH_ONLY", "1");
        set_env("SOGEN_SMP_EXEC_WRITE_WAKE", "1");

        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        const auto page_p = memory.allocate_memory(0x1000, memory_permission::all);
        const auto page_q = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(page_p, 0U);
        ASSERT_NE(page_q, 0U);
        const std::array<uint8_t, 7> p_code{0xB8, 0x11, 0x11, 0x00, 0x00, 0xEB, 0xF9};
        emu->write_memory(page_p, p_code.data(), p_code.size());
        auto& idle_peer = emu->get_cpu(0);
        idle_peer.reg(x86_register::rip, page_p);
        idle_peer.start(10);
        ASSERT_EQ(idle_peer.reg(x86_register::rax), 0x1111U);

        // Q: movabs rax,P+0x100; mov dword ptr [rax],0x1234; jmp back to store.
        std::array<uint8_t, 18> q_code{0x48, 0xB8};
        const uint64_t data_address = page_p + 0x100;
        for (size_t i = 0; i < 8; ++i)
        {
            q_code[2 + i] = static_cast<uint8_t>(data_address >> (i * 8));
        }
        q_code[10] = 0xC7;
        q_code[11] = 0x00;
        q_code[12] = 0x34;
        q_code[13] = 0x12;
        q_code[14] = 0x00;
        q_code[15] = 0x00;
        q_code[16] = 0xEB;
        q_code[17] = 0xF8;
        emu->write_memory(page_q, q_code.data(), q_code.size());

        auto& writer = emu->get_cpu(1);
        writer.reg(x86_register::rip, page_q);
        writer.start(1); // enter the store loop
        for (size_t i = 0; i < 100; ++i)
        {
            writer.start(2); // one store plus jump, then the next scheduler boundary
        }
        const auto profile = emu->smp_profile();
        ASSERT_EQ(profile.size(), 2U);
        const auto guest_writes = profile[0].exec_write_guest_jit_total + profile[0].exec_write_guest_mmu_total;
        EXPECT_EQ(guest_writes, 0U);
        EXPECT_GE(profile[0].exec_write_filtered_noncode_total, 90U);
        EXPECT_EQ(profile[0].exec_write_notified_overlap_total, 0U);
        EXPECT_EQ(profile[0].exec_write_notified_concurrent_total, 0U);
        EXPECT_EQ(profile[1].exec_write_owner_flushes_total, 0U);
        EXPECT_EQ(profile[0].exec_write_owner_flushes_total, 0U);
    }

    // Step 6.6: cross-vCPU self-modifying code — a host write over a TRANSLATED shared page must
    // invalidate every VM's translation (the peer would otherwise keep executing stale code; the
    // minimal repro showed rax=0x1111 after the peer wrote 0x2222 code). Values printed per step.
    TEST(IcicleSmp, CrossVcpuSelfModifyingCodeInvalidatesPeerTranslation)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        ASSERT_EQ(emu->vcpu_count(), 2U);

        memory_manager memory(*emu);
        const auto page = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(page, 0U);

        // v1: mov eax,0x1111 (B8 11 11 00 00); jmp $-5 (EB F9).  v2: same with 0x2222.
        const std::array<uint8_t, 7> v1{0xB8, 0x11, 0x11, 0x00, 0x00, 0xEB, 0xF9};
        const std::array<uint8_t, 7> v2{0xB8, 0x22, 0x22, 0x00, 0x00, 0xEB, 0xF9};

        auto& cpu0 = emu->get_cpu(0);
        const auto eax = [&] { return cpu0.reg(x86_register::rax); };

        std::fprintf(stderr, "[SMC6] STEP1 write v1 + execute on vCPU 0\n");
        emu->write_memory(page, v1.data(), v1.size());
        cpu0.reg(x86_register::rip, page);
        cpu0.start(10);
        std::fprintf(stderr, "[SMC6] STEP2 rax=%#llx (expect 0x1111)\n", (unsigned long long)eax());
        ASSERT_EQ(eax(), 0x1111u);

        std::fprintf(stderr, "[SMC6] STEP3 host-write v2 over the translated page\n");
        emu->write_memory(page, v2.data(), v2.size());

        cpu0.reg(x86_register::rip, page);
        cpu0.start(10);
        std::fprintf(stderr, "[SMC6] STEP4 rax=%#llx (expect 0x2222, stale=0x1111)\n", (unsigned long long)eax());
        EXPECT_EQ(eax(), 0x2222u) << "vCPU 0 executed its stale translation after the peer write";
    }

        // Step 6.6a: GUEST-write SMC — the writer is GUEST CODE (stores through the TLB write
    // pointer), so no host-write fan-out runs. vCPU 0 loops on code page P (v1); vCPU 1 executes
    // guest stores (from a separate page Q) that overwrite P with v2; vCPU 0 must then execute
    // the NEW code. Values printed per step.
    TEST(IcicleSmp, GuestSelfModifyingCodeSeenByOtherVcpu)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        ASSERT_EQ(emu->vcpu_count(), 2U);

        memory_manager memory(*emu);
        const auto page_p = memory.allocate_memory(0x1000, memory_permission::all);
        const auto page_q = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(page_p, 0U);
        ASSERT_NE(page_q, 0U);

        // P: v1 = mov eax,0x1111; jmp $-5.  v2 = mov eax,0x2222; jmp $-5.
        const std::array<uint8_t, 7> v1{0xB8, 0x11, 0x11, 0x00, 0x00, 0xEB, 0xF9};
        const std::array<uint8_t, 7> v2{0xB8, 0x22, 0x22, 0x00, 0x00, 0xEB, 0xF9};
        emu->write_memory(page_p, v1.data(), v1.size());

        // v2 as one little-endian qword (B8 22 22 00 00 EB F9 padded with 90):
        //   bytes B8 22 22 00 00 EB F9 90 -> u64 0x90F9EB00002222B8
        // Q (guest writer): movabs rax, P; movabs rcx, <v2 qword>; mov [rax], rcx; jmp $
        // Encoded: 48 B8 <P>                     movabs rax, P
        //          48 B9 <v2qword>               movabs rcx, 0x90F9EB00002222B8
        //          48 89 08                      mov [rax], rcx
        //          EB FE                         jmp $
        std::array<uint8_t, 64> q{};
        size_t n = 0;
        const auto emit = [&](const std::initializer_list<uint8_t> bytes) {
            for (const auto b : bytes) { q[n++] = b; }
        };
        emit({0x48, 0xB8});
        const uint64_t p64 = page_p;
        for (size_t i = 0; i < 8; ++i) { q[n++] = static_cast<uint8_t>(p64 >> (i * 8)); }
        emit({0x48, 0xB9});
        const uint64_t v2q = 0x90F9EB00002222B8ULL; // B8 22 22 00 00 EB F9 90 little-endian
        for (size_t i = 0; i < 8; ++i) { q[n++] = static_cast<uint8_t>(v2q >> (i * 8)); }
        emit({0x48, 0x89, 0x08});
        emit({0xEB, 0xFE});
        emu->write_memory(page_q, q.data(), q.size());

        auto& cpu0 = emu->get_cpu(0);
        auto& cpu1 = emu->get_cpu(1);
        const auto eax0 = [&] { return cpu0.reg(x86_register::rax); };

        std::fprintf(stderr, "[GSMC] STEP1 vCPU0 executes v1 on P\n");
        cpu0.reg(x86_register::rip, page_p);
        cpu0.start(10);
        std::fprintf(stderr, "[GSMC] STEP2 rax=%#llx (expect 0x1111)\n", (unsigned long long)eax0());
        ASSERT_EQ(eax0(), 0x1111u);

        std::fprintf(stderr, "[GSMC] STEP3 vCPU1 guest-stores v2 over P\n");
        cpu1.reg(x86_register::rip, page_q);
        cpu1.start(20);
        uint64_t check{};
        emu->read_memory(page_p, &check, 8);
        std::fprintf(stderr, "[GSMC] STEP4 P head=%#llx (expect 0x00002222_0000b8 or similar v2 bytes)\n", (unsigned long long)check);

        std::fprintf(stderr, "[GSMC] STEP5 vCPU0 re-executes P\n");
        cpu0.reg(x86_register::rip, page_p);
        try
        {
            cpu0.start(10);
        }
        catch (const std::exception& e)
        {
            uint64_t head{};
            emu->read_memory(page_p, &head, 8);
            std::fprintf(stderr,
                         "[GSMC] STEP5-FAULT %s | master P head=%#llx | vcpu0 rip=%#llx rax=%#llx\n", e.what(),
                         (unsigned long long)head, (unsigned long long)cpu0.reg(x86_register::rip),
                         (unsigned long long)cpu0.reg(x86_register::rax));
        }
        std::fprintf(stderr, "[GSMC] STEP6 rax=%#llx (expect 0x2222, stale=0x1111)\n", (unsigned long long)eax0());
        EXPECT_EQ(eax0(), 0x2222u) << "vCPU 0 executed stale code after vCPU 1's GUEST stores";

        // Repeat the guest store after the write TLB is warm. The second write must
        // invalidate the peer's translated code just like the first one.
        cpu1.reg(x86_register::rax, page_p);
        cpu1.reg(x86_register::rcx, 0x90F9EB00003333B8ULL);
        cpu1.reg(x86_register::rip, page_q + 20); // mov [rax],rcx; jmp $
        cpu1.start(10);
        uint64_t warm_check{};
        emu->read_memory(page_p, &warm_check, sizeof(warm_check));
        ASSERT_EQ(warm_check, 0x90F9EB00003333B8ULL);
        cpu0.reg(x86_register::rip, page_p);
        cpu0.start(10);
        EXPECT_EQ(eax0(), 0x3333u) << "vCPU 0 executed stale code after vCPU 1's warmed guest store";
    }

        // Step 6.5 verification — Arc-capture async mapping from an in-hook context reaches peers and they
    // EXECUTE from the region. vCPU A maps a fresh region from inside its own read hook (async path:
    // map+capture on A's VM, queued alias-from-capture + kick for B), writes code + a pointer into shared
    // memory, and vCPU B — polling that pointer from its own loop — jumps into the region and runs it. If
    // B's queued mapping were lost (or read the source VM cross-thread), B would fault executing there.
    // DISABLED regression target (open 6.5 bug, precisely characterized): a vCPU-context write to an
    // SMP-shared page (A's read-hook writing `target`) is visible through the master VM but vCPU 1
    // keeps reading the pre-write bytes (its own rax/probe stay 0), even with the shared-page COW
    // guard in icicle invalidate_code_range. Cross-VM read visibility of vCPU writes is the next
    // increment; setup-time (external) writes and both vCPUs' execution over shared pages are proven.
    TEST(IcicleSmp, PeerExecutesRegionMappedFromInsideHook)
    {
        auto emu = icicle::create_x86_64_emulator(2);
        ASSERT_EQ(emu->vcpu_count(), 2U);

        memory_manager memory(*emu);
        const auto page = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(page, 0U);
        // The hooked read cell gets its OWN page: icicle's read-hook interception is page-granular and
        // privatizes the hooked page per VM, so sharing data through the same page would diverge the
        // vCPUs' views (A's write visible on the master, B reading its own stale copy).
        const auto data_page = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(data_page, 0U);
        const uint64_t data = data_page;        // A's hooked read target (own page)
        const uint64_t target = page + 0x408;   // B polls this for the new region's code address
        const uint64_t marker = page + 0x410;   // B's code writes the magic here
        const uint64_t ready = page + 0x418;    // publish after all pointer bytes are written

        std::array<uint8_t, 0x600> prog{};
        prog.fill(0x90);
        const auto emit_movabs = [](uint8_t* p, uint8_t rex_op, const uint64_t imm) {
            p[0] = 0x48;
            p[1] = rex_op;
            std::memcpy(p + 2, &imm, sizeof(imm));
        };
        // A @ +0x000: movabs rbx,data; mov rcx,[rbx] (hook: maps region, arms target); spin dec/jnz.
        emit_movabs(&prog[0x00], 0xBB, data);
        prog[0x0A] = 0x48; prog[0x0B] = 0x8B; prog[0x0C] = 0x03; // mov rcx, [rbx]
        constexpr uint64_t iters = 200000;
        emit_movabs(&prog[0x0D], 0xB9, iters);
        prog[0x17] = 0x48; prog[0x18] = 0xFF; prog[0x19] = 0xC9; // dec rcx
        prog[0x1A] = 0x75; prog[0x1B] = 0xFB;                   // jnz
        // B waits for a one-byte ready flag before reading the pointer. Host writes
        // of multi-byte values are bytewise in Icicle, so polling target directly can
        // observe a torn address and jump into unmapped memory.
        emit_movabs(&prog[0x200], 0xBB, ready);
        prog[0x20A] = 0x8A; prog[0x20B] = 0x03;                     // poll: mov al,[rbx]
        prog[0x20C] = 0x84; prog[0x20D] = 0xC0;                     // test al,al
        prog[0x20E] = 0x74; prog[0x20F] = 0xFA;                     // jz poll
        emit_movabs(&prog[0x210], 0xBB, target);
        prog[0x21A] = 0x48; prog[0x21B] = 0x8B; prog[0x21C] = 0x03; // mov rax,[rbx]
        for (auto& b : std::span(prog).subspan(0x400, 0x200))
        {
            b = 0; // data cells must read 0, not NOP bytes (B polls `target` for nonzero)
        }
        prog[0x21D] = 0xFF; prog[0x21E] = 0xE0;                     // jmp rax
        emu->write_memory(page, prog.data(), prog.size());

        constexpr uint64_t magic = 0x5EEDC0DE5EEDC0DEULL;
        std::atomic<bool> mapped{false};
        std::atomic<uint64_t> mapped_region{0};

        emu->hook_memory_read(data, 8, [&](cpu_interface&, uint64_t, const void*, size_t) {
            if (mapped.exchange(true))
            {
                return;
            }
            // Async in-hook map: allocate on A's VM + Arc-capture + queued alias for B (+ kick).
            const auto region = memory.allocate_memory(0x1000, memory_permission::all);
            if (region == 0)
            {
                return;
            }
            mapped_region.store(region, std::memory_order_release);
            std::array<uint8_t, 0x40> code{};
            code.fill(0x90);
            emit_movabs(&code[0x00], 0xBB, marker);                 // movabs rbx, marker
            emit_movabs(&code[0x0A], 0xB8, magic);                  // movabs rax, magic
            code[0x14] = 0x48; code[0x15] = 0x89; code[0x16] = 0x03; // mov [rbx], rax
            code[0x17] = 0x90;
            code[0x18] = 0xEB; code[0x19] = 0xFE; // jmp $ — stay in the region until the quantum ends
            emu->write_memory(region, code.data(), code.size());
            // Give the kicked peer a moment to drain the queued mapping before the pointer becomes
            // visible (bounded-latency smoke; the hard cross-quantum race is the documented 6.6 window).
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            emu->write_memory(target, &region, sizeof(region));
            const uint8_t published = 1;
            emu->write_memory(ready, &published, sizeof(published));
        });

        emu->get_cpu(0).reg(x86_register::rip, page + 0x000);
        emu->get_cpu(1).reg(x86_register::rip, page + 0x200);

        std::thread t0([&] {
            try
            {
                emu->get_cpu(0).start(2 * iters + 64);
            }
            catch (const std::exception& e)
            {
                ADD_FAILURE() << "vCPU 0 start threw: " << e.what();
            }
        });
        // B runs in repeated quanta (each start() = one quantum → drains queued peer ops between them);
        // a single quantum cannot span A's 100ms in-hook sleep. Stop when the marker lands or capped.
        std::thread t1([&] {
            auto& cpu = emu->get_cpu(1);
            for (int chunk = 0; chunk < 200; ++chunk)
            {
                try
                {
                    cpu.start(100000);
                }
                catch (const std::exception& e)
                {
                    ADD_FAILURE() << "vCPU 1 start threw at chunk " << chunk << ": " << e.what()
                                  << " page=" << std::hex << page << " data_page=" << data_page
                                  << " region=" << mapped_region.load(std::memory_order_acquire)
                                  << " target=" << target << " rax=" << cpu.reg(x86_register::rax);
                    return;
                }
                uint64_t observed{};
                emu->read_memory(marker, &observed, sizeof(observed));
                if (observed == magic)
                {
                    return;
                }
            }
        });
        t0.join();
        t1.join();

        uint64_t observed{};
        emu->read_memory(marker, &observed, sizeof(observed));
        EXPECT_EQ(observed, magic) << "vCPU 1 never executed the region mapped from vCPU 0's hook";
    }

    // Step 6.5/6.8 regression: runs the multi-threaded test-sample on 2 icicle vCPUs through the
    // REAL windows_emulator. This is the exact shape that deadlocked before 6.5's async cross-VM
    // mutation (memory + hooks). N>1 requires the lean, wall-clock path
    // (use_instruction_precision=false + use_relative_time=false; both otherwise hard-error
    // "requires a single vCPU"). Requires the emulator root to contain filesys/c/test-sample.exe.
    // GREEN since 6.7 + the static-NSI adapter row: 8/8 deterministic passes on 2 vCPUs.
    // The sample's Interrupts self-test needs per-instruction precision, which the lean path
    // disables by design, so the guest env gets EMULATOR_ICICLE=1 (the sample skips it).
    // Debug knobs: SOGEN_SMP_PROBE_VCPUS=N (default 2) to compare 1-vCPU vs N-vCPU behavior of
    // the same lean configuration; SOGEN_SMP_TRACE=1 adds guest stdout + every guest file/registry
    // access; EMULATOR_VERBOSE=1 adds guest stdout. Run for a quick health check:
    //   EMULATOR_ROOT=<root> EMULATOR_ICICLE=1 windows-emulator-test.exe --gtest_filter='IcicleSmp.*'
    TEST(IcicleSmp, MultiThreadedSampleRunsOnTwoVcpus)
    {
        emulator_settings settings{};
        settings.use_relative_time = false;         // N>1 requires wall-clock time
        settings.use_instruction_precision = false; // N>1 requires the lean (no per-instruction precision) path
        settings.emulation_root = get_emulator_root();
        settings.path_mappings["C:\\a.txt"] =
            std::filesystem::temp_directory_path() / ("emu-smp-test-" + std::to_string(getpid()) + ".txt");

        emulator_interfaces interfaces{};
        interfaces.socket_factory = network::create_static_socket_factory();
        interfaces.dns_lookup = create_sample_dns_lookup();
        interfaces.ui = std::make_unique<null_ui_backend>();

        emulator_callbacks callbacks{};
        if (enable_verbose_logging())
        {
            // Print guest stdout so failing sample self-tests (puts/printf lines) are visible in
            // the gtest output alongside SMPTRC/TERMCTX diagnostics.
            callbacks.on_stdout = [](const std::string_view data) {
                std::cout << data; //
            };
        }
        if (const auto* trace = getenv("SOGEN_SMP_TRACE"); trace && *trace == '1')
        {
            // REGDIAG: log every registry key open + file open the guest performs, so DNS/adapter
            // discovery failures can be traced to the exact missing key.
            callbacks.on_generic_access = [](const std::string_view type, const std::u16string_view name) {
                std::string narrow{};
                narrow.reserve(name.size());
                for (const auto c16 : name)
                {
                    narrow.push_back(static_cast<char>(c16));
                }
                std::cout << "[ACC] " << type << ": " << narrow << "\n"; //
            };
        }

        // Debug knob: SOGEN_SMP_PROBE_VCPUS=N (default 2) to compare 1-vCPU vs N-vCPU behavior
        // of the exact same lean/wall-clock configuration without rebuilding.
        int vcpu_count = 2;
        if (const auto* vcpu_env = getenv("SOGEN_SMP_PROBE_VCPUS"))
        {
            vcpu_count = std::max(1, atoi(vcpu_env));
        }

        auto app_settings = get_sample_app_settings({});
        app_settings.environment[u"EMULATOR_ICICLE"] = u"1";

        windows_emulator emu{
            create_x86_64_emulator(backend_type::icicle, vcpu_count),
            std::move(app_settings),
            settings,
            std::move(callbacks),
            std::move(interfaces),
        };

        emu.start();
        ASSERT_TERMINATED_SUCCESSFULLY(emu);
    }
}
