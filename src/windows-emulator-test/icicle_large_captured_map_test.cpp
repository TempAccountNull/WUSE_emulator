#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <memory_manager.hpp>

#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace sogen::test
{
    // Opt-in benchmark for one large map made by the owning vCPU inside a guest
    // callback. Wall times are diagnostics, never pass/fail criteria.
    TEST(IcicleSmp, LargeCapturedMapFromHook)
    {
        const auto* enabled = std::getenv("SOGEN_SMP_LARGE_MAP_BENCH");
        if (!enabled || std::strcmp(enabled, "1") != 0)
        {
            GTEST_SKIP() << "Set SOGEN_SMP_LARGE_MAP_BENCH=1 to run the 64 MiB captured-map benchmark";
        }

        constexpr uint64_t region = 0x40000000;
        constexpr size_t region_size = 64 * 1024 * 1024;
        constexpr size_t page_size = 0x1000;
        constexpr std::array<size_t, 3> page_offsets{0, region_size / 2, region_size - page_size};
        constexpr std::array<uint64_t, 3> values{0x1020304050607080ULL, 0x192a3b4c5d6e7f80ULL, 0x8877665544332211ULL};

        const auto* eight = std::getenv("SOGEN_SMP_LARGE_MAP_EIGHT_VCPUS");
        const size_t vcpu_count = eight && std::strcmp(eight, "1") == 0 ? 8 : 2;
        auto emu = icicle::create_x86_64_emulator(vcpu_count);
        ASSERT_EQ(emu->vcpu_count(), vcpu_count);
        memory_manager memory(*emu);
        // Optional middle-insert topology: populate mappings above the captured
        // range so each serial page insertion shifts existing map entries.
        // The setup is outside the measured owner/peer map windows.
        const auto* middle = std::getenv("SOGEN_SMP_LARGE_MAP_MIDDLE");
        if (middle && std::strcmp(middle, "1") == 0)
        {
            constexpr uint64_t high_region = 0x80000000;
            constexpr size_t high_size = 16 * 1024 * 1024;
            ASSERT_TRUE(memory.allocate_memory(high_region, high_size, memory_permission::read_write));
        }
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        const auto trigger_data = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0U);
        ASSERT_NE(trigger_data, 0U);

        std::array<uint8_t, 0x200> program{};
        program.fill(0x90);
        const auto movabs = [&](const size_t offset, const uint8_t opcode, const uint64_t immediate) {
            program[offset] = 0x48;
            program[offset + 1] = opcode;
            std::memcpy(program.data() + offset + 2, &immediate, sizeof(immediate));
        };

        // Hooked reads privatize the watched page, so the trigger gets its own page.
        movabs(0, 0xB8, trigger_data);
        program[10] = 0x48;
        program[11] = 0x8B;
        program[12] = 0x00;

        // Owner guest stores and peer guest loads cover first, middle, final pages.
        for (size_t i = 0; i < page_offsets.size(); ++i)
        {
            const size_t writer = 0x20 + i * 0x20;
            const size_t reader = 0x100 + i * 0x20;
            movabs(writer, 0xB8, region + page_offsets[i]);
            movabs(writer + 10, 0xBB, values[i]);
            program[writer + 20] = 0x48;
            program[writer + 21] = 0x89;
            program[writer + 22] = 0x18;
            movabs(reader, 0xB8, region + page_offsets[i]);
            program[reader + 10] = 0x48;
            program[reader + 11] = 0x8B;
            program[reader + 12] = 0x00;
        }
        // Reverse direction too: peer guest store, then owner guest load.
        constexpr uint64_t peer_value = 0xdec0dedec0dedec0ULL;
        movabs(0xC0, 0xB8, region + page_offsets[2]);
        movabs(0xCA, 0xBB, peer_value);
        program[0xD4] = 0x48;
        program[0xD5] = 0x89;
        program[0xD6] = 0x18;
        movabs(0x1C0, 0xB8, region + page_offsets[2]);
        program[0x1CA] = 0x48;
        program[0x1CB] = 0x8B;
        program[0x1CC] = 0x00;
        emu->write_memory(code, program.data(), program.size());

        bool hook_called = false;
        bool mapped = false;
        std::chrono::steady_clock::duration owner_map_time{};
        emu->hook_memory_read(trigger_data, sizeof(uint64_t), [&](cpu_interface&, uint64_t, const void*, size_t) {
            hook_called = true;
            const auto start = std::chrono::steady_clock::now();
            mapped = memory.allocate_memory(region, region_size, memory_permission::read_write);
            owner_map_time = std::chrono::steady_clock::now() - start;
        });

        auto& owner = emu->get_cpu(0);
        auto& peer = emu->get_cpu(1);
        owner.reg(x86_register::rip, code);
        ASSERT_NO_THROW(owner.start(2));
        ASSERT_TRUE(hook_called);
        ASSERT_TRUE(mapped);

        std::chrono::steady_clock::duration peer_drain_time{};
        for (size_t i = 1; i < vcpu_count; ++i)
        {
            const auto drain_start = std::chrono::steady_clock::now();
            ASSERT_NO_THROW(emu->sync_worker_context(i));
            peer_drain_time += std::chrono::steady_clock::now() - drain_start;
        }
        std::cout << "[SMP-LARGE-MAP] vcpus=" << vcpu_count << " pages=" << region_size / page_size
                  << " owner_map_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(owner_map_time).count()
                  << " peer_drain_total_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(peer_drain_time).count() << '\n';
        const auto profile = emu->smp_profile();
        if (profile.size() == vcpu_count && profile[1].invalidation_profile_enabled)
        {
            for (size_t i = 1; i < vcpu_count; ++i)
            {
                std::cout << "[SMP-LARGE-MAP] peer=" << i << " peer_map_calls=" << profile[i].peer_map_calls
                          << " peer_map_pages=" << profile[i].peer_map_pages << " peer_map_total_ms=" << profile[i].peer_map_nanos / 1000000
                          << " peer_map_max_ms=" << profile[i].peer_map_max_nanos / 1000000 << '\n';
                EXPECT_EQ(profile[i].peer_map_calls, 1U);
                EXPECT_EQ(profile[i].peer_map_pages, region_size / page_size);
                EXPECT_GT(profile[i].peer_map_nanos, 0U);
                EXPECT_GT(profile[i].peer_map_max_nanos, 0U);
            }
        }

        for (size_t i = 0; i < page_offsets.size(); ++i)
        {
            owner.reg(x86_register::rip, code + 0x20 + i * 0x20);
            ASSERT_NO_THROW(owner.start(3));
            for (size_t j = 1; j < vcpu_count; ++j)
            {
                auto& reader = emu->get_cpu(j);
                reader.reg(x86_register::rip, code + 0x100 + i * 0x20);
                ASSERT_NO_THROW(reader.start(2));
                EXPECT_EQ(reader.reg(x86_register::rax), values[i]) << "peer " << j << " page offset " << page_offsets[i];
            }
            uint64_t host_value{};
            emu->read_memory(region + page_offsets[i], &host_value, sizeof(host_value));
            EXPECT_EQ(host_value, values[i]) << "host read at page offset " << page_offsets[i];
        }

        peer.reg(x86_register::rip, code + 0xC0);
        ASSERT_NO_THROW(peer.start(3));
        owner.reg(x86_register::rip, code + 0x1C0);
        ASSERT_NO_THROW(owner.start(2));
        EXPECT_EQ(owner.reg(x86_register::rax), peer_value);
        for (size_t i = 2; i < vcpu_count; ++i)
        {
            auto& reader = emu->get_cpu(i);
            reader.reg(x86_register::rip, code + 0x1C0);
            ASSERT_NO_THROW(reader.start(2));
            EXPECT_EQ(reader.reg(x86_register::rax), peer_value) << "peer " << i;
        }
        uint64_t host_value{};
        emu->read_memory(region + page_offsets[2], &host_value, sizeof(host_value));
        EXPECT_EQ(host_value, peer_value);
    }
}
