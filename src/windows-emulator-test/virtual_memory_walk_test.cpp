#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryVirtualMemory(const syscall_context&, handle, uint64_t, uint32_t, uint64_t, uint64_t, emulator_object<uint64_t>);
}

namespace sogen::test
{
    class VirtualMemoryWalkTest : public testing::Test
    {
      public:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t output{};

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            output = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        }

        NTSTATUS query(const uint64_t address, const uint32_t info_class = MemoryBasicInformation)
        {
            auto& vcpu = emu.vcpu(0);
            const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
            const std::array<uint8_t, 64> canary = [] {
                std::array<uint8_t, 64> bytes{};
                bytes.fill(0xa5);
                return bytes;
            }();
            emu.memory.write_memory(output, canary.data(), canary.size());
            return syscalls::handle_NtQueryVirtualMemory(context, CURRENT_PROCESS, address, info_class, output,
                                                         sizeof(EMU_MEMORY_BASIC_INFORMATION64), {emu.memory, output + 56});
        }

        EMU_MEMORY_BASIC_INFORMATION64 result()
        {
            return emu.memory.read_memory<EMU_MEMORY_BASIC_INFORMATION64>(output);
        }

        void expect_free(const uint64_t address)
        {
            ASSERT_EQ(query(address), STATUS_SUCCESS);
            const auto info = result();
            EXPECT_EQ(info.BaseAddress, page_align_down(address));
            EXPECT_EQ(info.AllocationBase, 0u);
            EXPECT_EQ(info.AllocationProtect, 0u);
            EXPECT_EQ(info.PartitionId, 0u);
            EXPECT_GT(info.RegionSize, 0);
            EXPECT_EQ(info.State, MEM_FREE);
            EXPECT_EQ(info.Protect, PAGE_NOACCESS);
            EXPECT_EQ(info.Type, 0u);
            EXPECT_EQ(emu.memory.read_memory<uint16_t>(output + 22), 0xa5a5u);
            EXPECT_EQ(emu.memory.read_memory<uint32_t>(output + 44), 0xa5a5a5a5u);
            EXPECT_EQ(emu.memory.read_memory<uint64_t>(output + 48), 0xa5a5a5a5a5a5a5a5ULL);
            EXPECT_EQ(emu.memory.read_memory<uint64_t>(output + 56), 48u);
        }
    };

    TEST_F(VirtualMemoryWalkTest, StartsAtZeroAndReachesTheNextRegion)
    {
        expect_free(0);
        const auto first = result();
        ASSERT_GE(first.RegionSize, 0x10000);
        ASSERT_LT(static_cast<uint64_t>(first.RegionSize), MAX_ALLOCATION_END_EXCL);
        ASSERT_EQ(query(first.RegionSize), STATUS_SUCCESS);
        EXPECT_EQ(result().BaseAddress, static_cast<uint64_t>(first.RegionSize));
        EXPECT_NE(result().State, MEM_FREE);
    }

    TEST_F(VirtualMemoryWalkTest, AllowsEveryPageBelowTheAllocationMinimum)
    {
        for (const auto address : {1ULL, 0xfffULL, 0x1000ULL, 0xfffeULL, 0xffffULL})
        {
            expect_free(address);
        }
    }

    TEST_F(VirtualMemoryWalkTest, ReportsFreeHoleUpToTheNextAllocation)
    {
        constexpr uint64_t hole = 0x24000000;
        ASSERT_FALSE(emu.memory.get_region_info(hole).is_reserved);
        ASSERT_EQ(emu.memory.allocate_memory(0x1000, memory_permission::read_write, false, hole + 0x10000), hole + 0x10000);
        expect_free(hole + 0x2345);
        EXPECT_EQ(result().BaseAddress, hole + 0x2000);
        EXPECT_EQ(result().RegionSize, 0xe000);
    }

    TEST_F(VirtualMemoryWalkTest, UpperBoundaryDoesNotWrap)
    {
        ASSERT_EQ(query(MAX_ALLOCATION_END_EXCL), STATUS_INVALID_PARAMETER);
        ASSERT_EQ(query(UINT64_MAX), STATUS_INVALID_PARAMETER);
        expect_free(MAX_ALLOCATION_END_EXCL - 1);
        EXPECT_EQ(result().RegionSize, 0x1000);
    }

    TEST_F(VirtualMemoryWalkTest, PrivilegedBasicQueryAlsoAcceptsZero)
    {
        ASSERT_EQ(query(0, MemoryPrivilegedBasicInformation), STATUS_SUCCESS);
        EXPECT_EQ(result().State, MEM_FREE);
        EXPECT_EQ(result().AllocationProtect, 0u);
    }
}
