#include "emulation_test_utils.hpp"
#include <kusd_mmio.hpp>
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryPerformanceCounter(const syscall_context&, emulator_object<LARGE_INTEGER>,
                                              emulator_object<LARGE_INTEGER>);
}

namespace sogen::test
{
    namespace
    {
        class counted_shared_data_clock final : public utils::clock
        {
          public:
            uint64_t system_reads{};
            uint64_t steady_reads{};

            system_time_point system_now() override
            {
                ++this->system_reads;
                return std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};
            }

            steady_time_point steady_now() override
            {
                ++this->steady_reads;
                return std::chrono::steady_clock::time_point{std::chrono::seconds{1000}};
            }
        };
    }

    TEST(KusdTimeRead, StaticFieldsSkipClockSamplingAndTimeFieldsRemainFresh)
    {
        emulator_settings settings{.disable_logging = true};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        emulator_interfaces interfaces{};
        auto clock = std::make_unique<counted_shared_data_clock>();
        auto* const measured_clock = clock.get();
        interfaces.clock = std::move(clock);
        auto emu = create_sample_emulator(std::move(settings), {}, {}, std::move(interfaces));
        emu.setup_process_if_necessary();

        constexpr auto base = 0x7ffe0000ULL;
        const auto system_before = measured_clock->system_reads;
        const auto steady_before = measured_clock->steady_reads;
        const auto active_processors =
            emu.memory.read_memory<uint32_t>(base + offsetof(KUSER_SHARED_DATA64, ActiveProcessorCount));
        EXPECT_GE(active_processors, 1U);
        EXPECT_EQ(measured_clock->system_reads, system_before);
        EXPECT_EQ(measured_clock->steady_reads, steady_before);

        // The x64 ReactOS tick path reads the first eight bytes at TickCount.
        const auto tick_count = emu.memory.read_memory<uint64_t>(base + offsetof(KUSER_SHARED_DATA64, TickCount));
        EXPECT_GT(tick_count, 0U);
        // A backend may split a guest 64-bit load into multiple MMIO reads.
        // Assert the semantic distinction without fixing the callback count.
        EXPECT_GT(measured_clock->system_reads, system_before);
        EXPECT_GT(measured_clock->steady_reads, steady_before);
        const auto system_after_tick = measured_clock->system_reads;
        const auto steady_after_tick = measured_clock->steady_reads;

        (void)emu.memory.read_memory<uint64_t>(base + offsetof(KUSER_SHARED_DATA64, InterruptTime));
        (void)emu.memory.read_memory<uint64_t>(base + offsetof(KUSER_SHARED_DATA64, SystemTime));
        EXPECT_GT(measured_clock->system_reads, system_after_tick);
        EXPECT_GT(measured_clock->steady_reads, steady_after_tick);
    }

    TEST(KusdTimeRead, GuestQpcAndReactOsTickPathBenchmark)
    {
        emulator_settings settings{.disable_logging = true};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        emulator_interfaces interfaces{};
        interfaces.clock = std::make_unique<counted_shared_data_clock>();
        auto emu = create_sample_emulator(std::move(settings), {}, {}, std::move(interfaces));
        emu.setup_process_if_necessary();

        auto& vcpu = emu.vcpu(0);
        syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const auto output = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<LARGE_INTEGER> counter{emu.memory, output};
        const emulator_object<LARGE_INTEGER> frequency{emu.memory, output + sizeof(LARGE_INTEGER)};
        ASSERT_EQ(syscalls::handle_NtQueryPerformanceCounter(context, counter, frequency), STATUS_SUCCESS);
        const auto qpc = emu.memory.read_memory<LARGE_INTEGER>(output).QuadPart;
        const auto qpc_frequency = emu.memory.read_memory<LARGE_INTEGER>(output + sizeof(LARGE_INTEGER)).QuadPart;
        ASSERT_GT(qpc_frequency, 0);

        constexpr auto base = 0x7ffe0000ULL;
        const auto tick_count = emu.memory.read_memory<uint64_t>(base + offsetof(KUSER_SHARED_DATA64, TickCount));
        const auto multiplier = emu.memory.read_memory<uint32_t>(base + offsetof(KUSER_SHARED_DATA64, TickCountMultiplier));
        // Match the x64 ReactOS KiTickCountToMs conversion on Sogen's guest page.
        const auto guest_tick_ms = (tick_count * multiplier) >> 24;
        EXPECT_EQ(qpc / qpc_frequency, guest_tick_ms / 1000);

        constexpr size_t iterations = 100'000;
        const auto measure = [](const char* label, const auto& operation) {
            const auto start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < iterations; ++i)
            {
                operation();
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
            std::printf("guest timing benchmark: %s %.2f ns/op (%zu operations)\n", label,
                        static_cast<double>(ns) / iterations, iterations);
        };
        NTSTATUS last_status{};
        measure("NtQueryPerformanceCounter handler", [&] {
            last_status = syscalls::handle_NtQueryPerformanceCounter(context, counter, frequency);
        });
        EXPECT_EQ(last_status, STATUS_SUCCESS);
        uint64_t tick_checksum{};
        measure("KUSER TickCount MMIO read", [&] {
            tick_checksum += emu.memory.read_memory<uint64_t>(base + offsetof(KUSER_SHARED_DATA64, TickCount));
        });
        EXPECT_NE(tick_checksum, 0U);
        uint64_t metadata_checksum{};
        measure("KUSER static metadata MMIO read", [&] {
            metadata_checksum += emu.memory.read_memory<uint32_t>(base + offsetof(KUSER_SHARED_DATA64, ActiveProcessorCount));
        });
        EXPECT_NE(metadata_checksum, 0U);
        uint64_t static_qpc_checksum{};
        measure("KUSER static 64-bit MMIO read", [&] {
            static_qpc_checksum +=
                emu.memory.read_memory<uint64_t>(base + offsetof(KUSER_SHARED_DATA64, BaselineSystemTimeQpc));
        });
        EXPECT_NE(static_qpc_checksum, 0U);
    }
} // namespace sogen::test
