#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiDdDDICreateAllocation(const syscall_context&, emulator_object<EMU_D3DKMT_CREATEALLOCATION>);
    NTSTATUS handle_NtGdiDdDDILock(const syscall_context&, emulator_object<EMU_D3DKMT_LOCK>);
}

namespace sogen::test
{
    class WarpAllocationTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t memory{};

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        NTSTATUS create(const std::array<uint32_t, 24>& descriptor, const uint32_t private_size = 96)
        {
            emu.memory.write_memory(memory + 0x200, descriptor.data(), sizeof(descriptor));
            const EMU_D3DDDI_ALLOCATIONINFO info{.pPrivateDriverData = memory + 0x200, .PrivateDriverDataSize = private_size};
            emu.memory.write_memory(memory + 0x100, &info, sizeof(info));
            const EMU_D3DKMT_CREATEALLOCATION request{.hDevice = 0x5000, .NumAllocations = 1, .pAllocationInfo = memory + 0x100};
            emu.memory.write_memory(memory, &request, sizeof(request));
            return syscalls::handle_NtGdiDdDDICreateAllocation(context(), {emu.memory, memory});
        }

        void verify_copy(const uint32_t size)
        {
            const auto info = emu.memory.read_memory<EMU_D3DDDI_ALLOCATIONINFO>(memory + 0x100);
            const auto* allocation = emu.process.dxgk.get_allocation(info.hAllocation);
            ASSERT_NE(allocation, nullptr);
            ASSERT_EQ(allocation->backing_size, page_align_up(size));
            const EMU_D3DKMT_LOCK request{.hDevice = 0x5000, .hAllocation = info.hAllocation};
            emu.memory.write_memory(memory + 0x300, &request, sizeof(request));
            ASSERT_EQ(syscalls::handle_NtGdiDdDDILock(context(), {emu.memory, memory + 0x300}), STATUS_SUCCESS);
            const auto mapped = emu.memory.read_memory<EMU_D3DKMT_LOCK>(memory + 0x300).pData;
            ASSERT_EQ(mapped, allocation->backing_memory);
            const std::vector<uint8_t> source(size, 0xa5);
            std::vector<uint8_t> result(size);
            emu.memory.write_memory(mapped, source.data(), source.size());
            emu.memory.read_memory(mapped, result.data(), result.size());
            EXPECT_EQ(result, source);
            ASSERT_TRUE(emu.process.dxgk.destroy_allocation(emu.memory, info.hAllocation));
        }
    };

    TEST_F(WarpAllocationTest, VolumeTextureMapsAllSlices)
    {
        // Reconstructed from the faulted WARP ResourceShape: 32 x 160 x 8, row pitch 128, total 160 KiB.
        ASSERT_EQ(create({4, 32, 160, 8, 0x22, 128, 0x28000}), STATUS_SUCCESS);
        verify_copy(0x28000);
    }

    TEST_F(WarpAllocationTest, ExplicitSizeIncludesMipsArraysAndCompressedRows)
    {
        for (const auto dimension : {1u, 2u, 3u, 4u, 5u, 6u})
        {
            SCOPED_TRACE(dimension);
            ASSERT_EQ(create({dimension, 256, 256, 1, 0x47, 512, 0x18001}), STATUS_SUCCESS);
            verify_copy(0x18001);
        }
    }

    TEST_F(WarpAllocationTest, InvalidLaterDescriptorLeavesTheWholeRequestUnchanged)
    {
        const EMU_WARP_ALLOCATION_DESCRIPTOR valid{.ResourceDimension = 4, .AllocationSize = 0x28000};
        const EMU_WARP_ALLOCATION_DESCRIPTOR invalid{.ResourceDimension = 7, .AllocationSize = 0x28000};
        emu.memory.write_memory(memory + 0x200, &valid, sizeof(valid));
        emu.memory.write_memory(memory + 0x300, &invalid, sizeof(invalid));
        const std::array<EMU_D3DDDI_ALLOCATIONINFO, 2> info{{
            {.pPrivateDriverData = memory + 0x200, .PrivateDriverDataSize = sizeof(valid)},
            {.pPrivateDriverData = memory + 0x300, .PrivateDriverDataSize = sizeof(invalid)},
        }};
        emu.memory.write_memory(memory + 0x100, info.data(), sizeof(info));
        const EMU_D3DKMT_CREATEALLOCATION request{.hDevice = 0x5000, .NumAllocations = 2, .pAllocationInfo = memory + 0x100};
        emu.memory.write_memory(memory, &request, sizeof(request));
        EXPECT_EQ(syscalls::handle_NtGdiDdDDICreateAllocation(context(), {emu.memory, memory}), STATUS_NOT_SUPPORTED);
        EXPECT_EQ(emu.memory.read_memory<EMU_D3DKMT_CREATEALLOCATION>(memory).hResource, 0u);
        EXPECT_EQ(emu.memory.read_memory<EMU_D3DDDI_ALLOCATIONINFO>(memory + 0x100).hAllocation, 0u);
        EXPECT_TRUE(emu.process.dxgk.allocations.empty());
        EXPECT_EQ(emu.process.dxgk.next_resource_handle, 0x8000u);
    }

    TEST_F(WarpAllocationTest, UnsupportedDescriptorsDoNotCreateFallbackAllocations)
    {
        for (const auto dimension : {0u, 7u})
        {
            EXPECT_EQ(create({dimension, 32, 160, 8, 0x22, 128, 0x28000}), STATUS_NOT_SUPPORTED);
        }
        EXPECT_EQ(create({4, 32, 160, 8, 0x22, 128, 0x28000}, 28), STATUS_NOT_SUPPORTED);
        EXPECT_EQ(create({4, 32, 160, 8, 0x22, 128, 0}), STATUS_NOT_SUPPORTED);
        EXPECT_TRUE(emu.process.dxgk.allocations.empty());
        EXPECT_EQ(emu.process.dxgk.next_resource_handle, 0x8000u);
    }
}
