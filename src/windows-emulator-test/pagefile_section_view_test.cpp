#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtMapViewOfSection(const syscall_context&, handle, handle, emulator_object<uint64_t>, uint64_t, uint64_t,
                                       emulator_object<LARGE_INTEGER>, emulator_object<uint64_t>, SECTION_INHERIT, ULONG, ULONG);
    NTSTATUS handle_NtUnmapViewOfSection(const syscall_context&, handle, uint64_t);
    NTSTATUS handle_NtClose(const syscall_context&, handle);
}

namespace sogen::test
{
    class PagefileSectionViewTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t arguments{};
        handle section_handle{};
        static constexpr uint64_t first = 0x70000000;
        static constexpr uint64_t middle = first + 0x10000;
        static constexpr uint64_t last = middle + 0x10000;

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            section object{};
            object.maximum_size = 0x10000;
            object.section_page_protection = PAGE_READWRITE;
            object.allocation_attributes = SEC_COMMIT;
            section_handle = emu.process.sections.store(std::move(object));
            arguments = emu.memory.allocate_memory(0x2000, memory_permission::all);
        }

        NTSTATUS map(const uint64_t address, const uint64_t size = 0x10000, const uint64_t offset = 0,
                     const ULONG protection = PAGE_READWRITE, const uint64_t commit = 0)
        {
            emu.emu().write_memory<uint64_t>(arguments, address);
            emu.emu().write_memory<uint64_t>(arguments + 8, size);
            emu.emu().write_memory<uint64_t>(arguments + 16, offset);
            return syscalls::handle_NtMapViewOfSection(context(), section_handle, CURRENT_PROCESS, {emu.memory, arguments}, 0, commit,
                                                       {emu.memory, arguments + 16}, {emu.memory, arguments + 8}, ViewShare, 0, protection);
        }

        void map_three()
        {
            for (const auto address : {first, middle, last})
            {
                ASSERT_EQ(map(address), STATUS_SUCCESS);
                EXPECT_EQ(emu.emu().read_memory<uint64_t>(arguments), address);
                EXPECT_EQ(emu.emu().read_memory<uint64_t>(arguments + 8), 0x10000u);
            }
        }
    };

    TEST_F(PagefileSectionViewTest, CircularViewsShareGuestInstructionWrites)
    {
        map_three();
        const auto code = std::to_array<uint8_t>({0x66, 0x0f, 0x76, 0xc0, 0x0f, 0x11, 0x01, 0x90});
        const auto entry = arguments + 0x1000;
        emu.memory.write_memory(entry, code.data(), code.size());
        auto& cpu = emu.emu();
        cpu.reg(x86_register::rip, entry);
        cpu.reg(x86_register::rcx, middle + 0xfff8);
        cpu.start(2);
        EXPECT_EQ(cpu.reg<uint64_t>(x86_register::rip), entry + 7);
        EXPECT_EQ(cpu.read_memory<uint64_t>(first), UINT64_MAX);
        EXPECT_EQ(cpu.read_memory<uint64_t>(middle + 0xfff8), UINT64_MAX);
        EXPECT_EQ(cpu.read_memory<uint64_t>(last), UINT64_MAX);
        EXPECT_EQ(emu.memory.get_region_info(middle).kind, memory_region_kind::pagefile_section_view);
    }

    TEST_F(PagefileSectionViewTest, ClosingHandleKeepsViewsAndLastUnmapReclaimsBacking)
    {
        const auto initial = emu.memory.compute_memory_stats().reserved_memory;
        map_three();
        ASSERT_EQ(syscalls::handle_NtClose(context(), section_handle), STATUS_SUCCESS);
        ASSERT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, middle + 0x1234), STATUS_SUCCESS);
        EXPECT_FALSE(emu.memory.get_region_info(middle).is_reserved);
        emu.emu().write_memory<uint32_t>(first, 0xabcdef01);
        EXPECT_EQ(emu.emu().read_memory<uint32_t>(last), 0xabcdef01u);
        EXPECT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, middle), STATUS_NOT_MAPPED_VIEW);
        ASSERT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, first), STATUS_SUCCESS);
        ASSERT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, last), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.compute_memory_stats().reserved_memory, initial);
    }

    TEST_F(PagefileSectionViewTest, OpenHandlePreservesContentsAfterAllViewsUnmap)
    {
        ASSERT_EQ(map(first), STATUS_SUCCESS);
        emu.emu().write_memory<uint32_t>(first, 42);
        ASSERT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, first), STATUS_SUCCESS);
        ASSERT_EQ(map(middle), STATUS_SUCCESS);
        EXPECT_EQ(emu.emu().read_memory<uint32_t>(middle), 42u);
    }

    TEST_F(PagefileSectionViewTest, ViewSizeOffsetProtectionAndCollisionContracts)
    {
        emu.process.sections.get(section_handle)->maximum_size = 0x20000;
        ASSERT_EQ(map(first, 0x1001, 0x10000, PAGE_READONLY), STATUS_SUCCESS);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(arguments + 8), 0x2000u);
        EXPECT_EQ(emu.memory.get_region_info(first).permissions.common, memory_permission::read);
        const auto reserved = emu.memory.compute_memory_stats().reserved_memory;
        EXPECT_EQ(map(first), STATUS_CONFLICTING_ADDRESSES);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(arguments), first);
        EXPECT_EQ(map(middle, 0x20001), static_cast<NTSTATUS>(0xc000001f));
        EXPECT_EQ(map(middle, 0, 0x20000), static_cast<NTSTATUS>(0xc000001f));
        EXPECT_EQ(map(middle, 0x1000, 0, PAGE_EXECUTE_READ), static_cast<NTSTATUS>(0xc000004e));
        EXPECT_EQ(emu.memory.compute_memory_stats().reserved_memory, reserved);
        ASSERT_EQ(map(middle, 0, 0x10000), STATUS_SUCCESS);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(arguments + 8), 0x10000u);
    }

    TEST_F(PagefileSectionViewTest, ReservedSectionCommitPropagatesToEveryView)
    {
        emu.process.sections.get(section_handle)->allocation_attributes = SEC_RESERVE;
        map_three();
        EXPECT_FALSE(emu.memory.get_region_info(first).is_committed);
        ASSERT_TRUE(emu.memory.commit_memory(middle + 0x2000, 0x1000, memory_permission::read_write));
        EXPECT_TRUE(emu.memory.get_region_info(first + 0x2000).is_committed);
        EXPECT_FALSE(emu.memory.get_region_info(first + 0x1000).is_committed);
        emu.emu().write_memory<uint32_t>(last + 0x2000, 1234);
        EXPECT_EQ(emu.emu().read_memory<uint32_t>(first + 0x2000), 1234u);
    }

    TEST_F(PagefileSectionViewTest, SerializedMemoryRestoresViewSharingAndProtection)
    {
        map_three();
        ASSERT_TRUE(emu.memory.protect_memory(middle, 0x1000, memory_permission::read));
        emu.emu().write_memory<uint32_t>(first, 10);
        utils::buffer_serializer saved{};
        emu.memory.serialize_memory_state(saved, false);
        emu.memory.serialize_aslr_state(saved);
        emu.memory.unmap_all_memory();
        utils::buffer_deserializer restored{saved.get_buffer()};
        emu.memory.deserialize_memory_state(restored, false);
        emu.memory.deserialize_aslr_state(restored, false, false, true);
        EXPECT_EQ(emu.memory.get_region_info(middle).permissions.common, memory_permission::read);
        EXPECT_EQ(emu.emu().read_memory<uint32_t>(middle), 10u);
        emu.emu().write_memory<uint32_t>(last, 20);
        EXPECT_EQ(emu.emu().read_memory<uint32_t>(middle), 20u);
        EXPECT_EQ(emu.emu().read_memory<uint32_t>(first), 20u);
    }
}
