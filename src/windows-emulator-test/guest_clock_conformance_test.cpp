#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <kusd_mmio.hpp>
#include <syscall_utils.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryPerformanceCounter(const syscall_context&, emulator_object<LARGE_INTEGER>, emulator_object<LARGE_INTEGER>);
}

namespace sogen::test
{
    namespace
    {
        class controlled_guest_clock final : public utils::clock
        {
          public:
            steady_time_point steady_now() override
            {
                return steady_time_point{std::chrono::nanoseconds{this->nanoseconds_.load()}};
            }

            uint64_t timestamp_counter() override
            {
                return this->nanoseconds_.load();
            }

            void advance(const uint64_t nanoseconds)
            {
                this->nanoseconds_.fetch_add(nanoseconds);
            }

          private:
            std::atomic<uint64_t> nanoseconds_{3'000'000'000};
        };
    }

    TEST(GuestClockConformance, TimestampInstructionsFollowQpcAndSharedDataAcrossIdleAndPeerWork)
    {
        for (const auto vcpu_count : {1U, 2U})
        {
            SCOPED_TRACE(vcpu_count);
            emulator_settings settings{.disable_logging = true, .use_relative_time = false, .use_instruction_precision = false};
            settings.load_registry = false;
            emulator_interfaces interfaces{};
            auto clock = std::make_unique<controlled_guest_clock>();
            auto* const guest_clock = clock.get();
            interfaces.clock = std::move(clock);
            interfaces.ui = std::make_unique<null_ui_backend>();
            windows_emulator win{icicle::create_x86_64_emulator(vcpu_count), settings, {}, std::move(interfaces)};
            win.process.kusd.setup(win.version, win.fake_env);

            const auto page = win.memory.allocate_memory(0x1000, memory_permission::all);
            ASSERT_NE(page, 0U);
            std::array<uint8_t, 512> code{};
            code.fill(0x90);
            code[0] = 0x0F;
            code[1] = 0x31;
            code[0x10] = 0x0F;
            code[0x11] = 0x01;
            code[0x12] = 0xF9;
            win.memory.write_memory(page, code.data(), code.size());

            auto& cpu = win.vcpu(0).cpu;
            const auto read_tsc = [&](const uint64_t offset) {
                cpu.reg(x86_register::rip, page + offset);
                cpu.start(1);
                return (cpu.reg<uint64_t>(x86_register::rdx) << 32) | cpu.reg<uint32_t>(x86_register::rax);
            };

            const auto output = win.memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(output, 0U);
            const emulator_object<LARGE_INTEGER> counter{win.memory, output};
            const emulator_object<LARGE_INTEGER> frequency{win.memory, output + sizeof(LARGE_INTEGER)};
            auto& vcpu = win.vcpu(0);
            syscall_context context{.win_emu = win, .emu = cpu, .vcpu = vcpu, .proc = win.process};
            const auto read_qpc = [&] {
                EXPECT_EQ(syscalls::handle_NtQueryPerformanceCounter(context, counter, frequency), STATUS_SUCCESS);
                return win.memory.read_memory<LARGE_INTEGER>(output).QuadPart;
            };
            const auto read_tick_ms = [&] {
                constexpr auto base = 0x7ffe0000ULL;
                const auto tick_count = win.memory.read_memory<uint64_t>(base + offsetof(KUSER_SHARED_DATA64, TickCount));
                const auto multiplier = win.memory.read_memory<uint32_t>(base + offsetof(KUSER_SHARED_DATA64, TickCountMultiplier));
                return (tick_count * multiplier) >> 24;
            };

            const auto before_tsc = read_tsc(0);
            const auto before_rdtscp = read_tsc(0x10);
            const auto before_qpc = read_qpc();
            const auto before_tick_ms = read_tick_ms();

            if (vcpu_count > 1)
            {
                auto& peer = win.vcpu(1).cpu;
                peer.reg(x86_register::rip, page + 0x100);
                peer.start(100);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            guest_clock->advance(50'000'000);

            const auto after_tsc = read_tsc(0);
            const auto after_rdtscp = read_tsc(0x10);
            const auto after_qpc = read_qpc();
            const auto after_tick_ms = read_tick_ms();

            EXPECT_EQ(after_tsc - before_tsc, 50'000'000U);
            EXPECT_EQ(after_rdtscp - before_rdtscp, 50'000'000U);
            EXPECT_EQ(after_qpc - before_qpc, 50'000'000);
            EXPECT_GE(after_tick_ms - before_tick_ms, 34U);
            EXPECT_LE(after_tick_ms - before_tick_ms, 50U);
        }
    }
} // namespace sogen::test
