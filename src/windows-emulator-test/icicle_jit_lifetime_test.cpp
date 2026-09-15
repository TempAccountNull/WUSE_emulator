#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#ifdef OS_WINDOWS
#include <Windows.h>
#endif

namespace sogen::test
{
#ifdef OS_WINDOWS
    namespace
    {
        size_t private_executable_bytes()
        {
            size_t total{};
            uintptr_t address{};
            MEMORY_BASIC_INFORMATION region{};
            while (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) == sizeof(region))
            {
                constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                if (region.State == MEM_COMMIT && region.Type == MEM_PRIVATE && (region.Protect & executable) != 0)
                {
                    total += region.RegionSize;
                }
                const auto base = reinterpret_cast<uintptr_t>(region.BaseAddress);
                if (region.RegionSize > std::numeric_limits<uintptr_t>::max() - base)
                {
                    break;
                }
                const auto next = base + region.RegionSize;
                if (next <= address)
                {
                    break;
                }
                address = next;
            }
            return total;
        }
    }

    TEST(IcicleJitLifetime, RepeatedCodeReplacementReclaimsExecutableAllocations)
    {
        const auto* mode = std::getenv("SOGEN_ICICLE_JIT");
        if (!mode || std::strcmp(mode, "1") != 0)
        {
            GTEST_SKIP() << "Generated-code allocation test requires JIT mode";
        }
        auto emu = create_x86_64_emulator(backend_type::icicle);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(code, 0u);
        const auto before = private_executable_bytes();
        size_t warmed{};
        constexpr size_t rounds = 512;
        constexpr size_t blocks = 8;
        std::array<uint8_t, blocks * 0x20> bytes{};
        bytes.fill(0x90);
        for (size_t round = 0; round < rounds; ++round)
        {
            for (size_t block = 0; block < blocks; ++block)
            {
                const auto offset = block * 0x20;
                const auto value = static_cast<uint32_t>(round * blocks + block + 1);
                bytes[offset] = 0xB8;
                std::memcpy(bytes.data() + offset + 1, &value, sizeof(value));
                bytes[offset + 5] = 0xEB;
                bytes[offset + 6] = 0xF9;
            }
            emu->write_memory(code, bytes.data(), bytes.size());
            for (size_t block = 0; block < blocks; ++block)
            {
                const auto entry = code + block * 0x20;
                emu->reg(x86_register::rip, entry);
                emu->start(2);
                ASSERT_EQ(emu->reg<uint64_t>(x86_register::rax), round * blocks + block + 1);
                ASSERT_EQ(emu->reg<uint64_t>(x86_register::rip), entry);
            }
            if (round == 0)
            {
                warmed = private_executable_bytes();
            }
        }
        const auto after = private_executable_bytes();
        RecordProperty("rounds", static_cast<int>(rounds));
        RecordProperty("block_executions", static_cast<int>(rounds * blocks));
        RecordProperty("private_executable_bytes_before", std::to_string(before));
        RecordProperty("private_executable_bytes_warmed", std::to_string(warmed));
        RecordProperty("private_executable_bytes_after", std::to_string(after));
        ASSERT_GT(warmed, before);
        EXPECT_LE(after, warmed + 4 * 1024 * 1024);
    }

    TEST(IcicleJitLifetime, EmulatorDestructionReleasesGeneratedCode)
    {
        const auto* mode = std::getenv("SOGEN_ICICLE_JIT");
        if (!mode || std::strcmp(mode, "1") != 0)
        {
            GTEST_SKIP() << "Generated-code allocation test requires JIT mode";
        }
        const auto before = private_executable_bytes();
        for (size_t iteration = 0; iteration < 16; ++iteration)
        {
            {
                auto emu = create_x86_64_emulator(backend_type::icicle);
                memory_manager memory(*emu);
                const auto code = memory.allocate_memory(0x1000, memory_permission::all);
                ASSERT_NE(code, 0u);
                const std::array<uint8_t, 7> bytes{0xB8, 0x42, 0, 0, 0, 0xEB, 0xF9};
                emu->write_memory(code, bytes.data(), bytes.size());
                emu->reg(x86_register::rip, code);
                emu->start(2);
                ASSERT_EQ(emu->reg<uint64_t>(x86_register::rax), 0x42u);
                ASSERT_GT(private_executable_bytes(), before);
            }
            EXPECT_EQ(private_executable_bytes(), before);
        }
    }
#endif
}
