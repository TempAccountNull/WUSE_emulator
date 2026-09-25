#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include "emulation_test_utils.hpp"
#include <kusd_mmio.hpp>
#include <utils/finally.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t shared_data_base = 0x7ffe0000ULL;
        constexpr uint32_t warmup_iterations = 1'000;
        constexpr uint32_t measured_iterations = 100'000;

        bool set_benchmark_environment(const char* name, const char* value)
        {
#ifdef OS_WINDOWS
            return _putenv_s(name, value) == 0;
#else
            return (*value ? setenv(name, value, 1) : unsetenv(name)) == 0;
#endif
        }

        std::string saved_benchmark_environment(const char* name)
        {
            const char* value = std::getenv(name);
            return value ? value : "";
        }

        template <size_t N, typename T>
        void write_immediate(std::array<uint8_t, N>& code, const size_t offset, const T value)
        {
            std::memcpy(code.data() + offset, &value, sizeof(value));
        }

        double ns_per_operation(const std::chrono::steady_clock::time_point start, const std::chrono::steady_clock::time_point end,
                                const uint32_t iterations)
        {
            return std::chrono::duration<double, std::nano>(end - start).count() / iterations;
        }

        void print_result(const char* label, const double nanoseconds, const uint32_t completed_iterations)
        {
            std::printf("guest clock benchmark: %-43s %9.2f ns/completed iteration (%u completed)\n", label, nanoseconds,
                        completed_iterations);
        }

        struct benchmark_sample
        {
            double nanoseconds_per_completed_iteration;
            uint32_t completed_iterations;
        };
    } // namespace

    TEST(IcicleGuestClockBenchmark, HostClockGuestLoadsAndSyscallQpc)
    {
        const char* enabled = std::getenv("SOGEN_ICICLE_GUEST_CLOCK_BENCH");
        if (!enabled || std::strcmp(enabled, "1") != 0)
        {
            GTEST_SKIP() << "Set SOGEN_ICICLE_GUEST_CLOCK_BENCH=1 to run the guest "
                            "clock benchmark";
        }

        const auto old_jit = saved_benchmark_environment("SOGEN_ICICLE_JIT");
        const auto old_hook = saved_benchmark_environment("SOGEN_ICICLE_INSTRUCTION_HOOK");
        const auto restore_environment = utils::finally([&] {
            set_benchmark_environment("SOGEN_ICICLE_JIT", old_jit.c_str());
            set_benchmark_environment("SOGEN_ICICLE_INSTRUCTION_HOOK", old_hook.c_str());
        });
        ASSERT_TRUE(set_benchmark_environment("SOGEN_ICICLE_JIT", "1"));
        ASSERT_TRUE(set_benchmark_environment("SOGEN_ICICLE_INSTRUCTION_HOOK", "0"));

        emulator_settings settings{.disable_logging = true};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        auto emu = create_sample_emulator(std::move(settings), {}, {}, std::move(interfaces));
        emu.setup_process_if_necessary();

        const auto* ntdll = emu.mod_manager.ntdll;
        ASSERT_NE(ntdll, nullptr);
        const auto qpc_stub = ntdll->find_export("NtQueryPerformanceCounter");
        ASSERT_NE(qpc_stub, 0U);
        ASSERT_EQ(emu.memory.read_memory<uint8_t>(qpc_stub + 3), 0xB8);
        const auto qpc_syscall_id = emu.memory.read_memory<uint32_t>(qpc_stub + 4);

        const auto data = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto code_page = emu.memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(data, 0U);
        ASSERT_NE(code_page, 0U);
        constexpr uint64_t regular_value = 0x123456789abcdef0ULL;
        emu.memory.write_memory(data, &regular_value, sizeof(regular_value));
        const auto counter_output = data + 0x100;
        const auto frequency_output = data + 0x108;
        auto& cpu = emu.vcpu(0).cpu;

        uint64_t host_checksum{};
        const auto host_start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < measured_iterations; ++i)
        {
            host_checksum += static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        }
        const auto host_end = std::chrono::steady_clock::now();
        EXPECT_NE(host_checksum, 0U);
        print_result("host steady_clock::now()", ns_per_operation(host_start, host_end, measured_iterations), measured_iterations);

        std::array<uint8_t, 19> read_loop{
            0x48, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x8B, 0x03, 0xFF, 0xC9, 0x75, 0xF9, 0xEB, 0xFE,
        };
        const auto run_read = [&](const uint64_t code_address, const uint32_t iterations) {
            cpu.reg(x86_register::rip, code_address);
            cpu.reg(x86_register::ecx, iterations);
            const auto start = std::chrono::steady_clock::now();
            cpu.start(1 + 3 * iterations);
            const auto end = std::chrono::steady_clock::now();
            const auto remaining = cpu.reg<uint32_t>(x86_register::ecx);
            EXPECT_LE(remaining, iterations);
            const auto completed = iterations - std::min(remaining, iterations);
            EXPECT_GT(completed, 0U);
            return benchmark_sample{completed ? ns_per_operation(start, end, completed) : 0.0, completed};
        };
        const auto benchmark_read = [&](const char* label, const uint64_t code_offset, const uint64_t address, const uint64_t expected) {
            write_immediate(read_loop, 2, address);
            const auto code_address = code_page + code_offset;
            emu.memory.write_memory(code_address, read_loop.data(), read_loop.size());
            run_read(code_address, warmup_iterations);
            const auto sample = run_read(code_address, measured_iterations);
            print_result(label, sample.nanoseconds_per_completed_iteration, sample.completed_iterations);
            EXPECT_EQ(cpu.reg<uint64_t>(x86_register::rax), expected);
        };

        benchmark_read("guest JIT regular 64-bit load + loop", 0x00, data, regular_value);
        const auto qpc_frequency = emu.memory.read_memory<uint64_t>(shared_data_base + offsetof(KUSER_SHARED_DATA64, QpcFrequency));
        ASSERT_GT(qpc_frequency, 0U);
        benchmark_read("guest JIT KUSER static QpcFrequency + loop", 0x40, shared_data_base + offsetof(KUSER_SHARED_DATA64, QpcFrequency),
                       qpc_frequency);

        const auto tick_address = shared_data_base + offsetof(KUSER_SHARED_DATA64, TickCount);
        const auto tick_before = emu.memory.read_memory<uint64_t>(tick_address);
        write_immediate(read_loop, 2, tick_address);
        emu.memory.write_memory(code_page + 0x80, read_loop.data(), read_loop.size());
        run_read(code_page + 0x80, warmup_iterations);
        const auto tick_sample = run_read(code_page + 0x80, measured_iterations);
        print_result("guest JIT KUSER TickCount load + loop", tick_sample.nanoseconds_per_completed_iteration,
                     tick_sample.completed_iterations);
        EXPECT_GE(cpu.reg<uint64_t>(x86_register::rax), tick_before);

        // The syscall route uses the mapped ntdll service number and the emulator
        // dispatcher, not KUSER QPC bypass data.
        std::array<uint8_t, 23> syscall_loop{
            0x48, 0x8B, 0xCB, 0x4C, 0x89, 0xCA, 0x4C, 0x8B, 0xD1, 0xB8, 0, 0, 0, 0, 0x0F, 0x05, 0x41, 0xFF, 0xC8, 0x75, 0xEB, 0xEB, 0xFE,
        };
        write_immediate(syscall_loop, 10, qpc_syscall_id);
        emu.memory.write_memory(code_page + 0x100, syscall_loop.data(), syscall_loop.size());
        const auto run_qpc = [&](const uint32_t iterations) {
            cpu.reg(x86_register::rip, code_page + 0x100);
            cpu.reg(x86_register::rbx, counter_output);
            cpu.reg(x86_register::r9, frequency_output);
            cpu.reg(x86_register::r8d, iterations);
            const auto start = std::chrono::steady_clock::now();
            cpu.start(7 * iterations);
            const auto end = std::chrono::steady_clock::now();
            const auto remaining = cpu.reg<uint32_t>(x86_register::r8d);
            EXPECT_LE(remaining, iterations);
            const auto completed = iterations - std::min(remaining, iterations);
            EXPECT_GT(completed, 0U);
            EXPECT_EQ(cpu.reg<uint32_t>(x86_register::eax), STATUS_SUCCESS);
            return benchmark_sample{completed ? ns_per_operation(start, end, completed) : 0.0, completed};
        };
        run_qpc(warmup_iterations);
        const auto qpc_sample = run_qpc(measured_iterations);
        print_result("guest JIT syscall QPC + loop + dispatch", qpc_sample.nanoseconds_per_completed_iteration,
                     qpc_sample.completed_iterations);
        EXPECT_GT(emu.memory.read_memory<LARGE_INTEGER>(counter_output).QuadPart, 0);
        EXPECT_EQ(emu.memory.read_memory<LARGE_INTEGER>(frequency_output).QuadPart, static_cast<int64_t>(qpc_frequency));
    }
} // namespace sogen::test
