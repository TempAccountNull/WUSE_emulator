#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <fstream>

namespace sogen::syscalls
{
    NTSTATUS handle_NtMapViewOfSection(const syscall_context&, handle, handle, emulator_object<uint64_t>, uint64_t, uint64_t,
                                       emulator_object<LARGE_INTEGER>, emulator_object<uint64_t>, SECTION_INHERIT, ULONG, ULONG);
    NTSTATUS handle_NtUnmapViewOfSection(const syscall_context&, handle, uint64_t);
}

namespace sogen::test
{
    class FileSectionViewTest : public testing::Test
    {
      protected:
        windows_emulator emu = create_empty_emulator();
        std::filesystem::path path = std::filesystem::temp_directory_path() / ("sogen-file-view-" + std::to_string(getpid()));
        std::vector<uint8_t> data = std::vector<uint8_t>(0x30025);
        uint64_t arguments{};
        handle section_handle{};

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        void SetUp() override
        {
            for (size_t i = 0; i < data.size(); ++i)
            {
                data[i] = static_cast<uint8_t>((i / 0x1000 + i) % 251);
            }
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            output.close();
            emu.file_sys.map(R"(C:\file-view.bin)", path);
            section object{};
            object.file_name = u"C:\\file-view.bin";
            object.section_page_protection = PAGE_READONLY;
            object.allocation_attributes = SEC_COMMIT;
            section_handle = emu.process.sections.store(std::move(object));
            arguments = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        }

        void TearDown() override
        {
            std::filesystem::remove(path);
        }

        NTSTATUS map(const uint64_t size, const uint64_t offset = 0)
        {
            emu.emu().write_memory<uint64_t>(arguments, 0);
            emu.emu().write_memory<uint64_t>(arguments + 8, size);
            emu.emu().write_memory<uint64_t>(arguments + 16, offset);
            return syscalls::handle_NtMapViewOfSection(context(), section_handle, CURRENT_PROCESS, {emu.memory, arguments}, 0, 0,
                                                       {emu.memory, arguments + 16}, {emu.memory, arguments + 8}, ViewUnmap, 0,
                                                       PAGE_READONLY);
        }

        uint64_t base()
        {
            return emu.emu().read_memory<uint64_t>(arguments);
        }

        uint64_t size()
        {
            return emu.emu().read_memory<uint64_t>(arguments + 8);
        }
    };

    TEST_F(FileSectionViewTest, RequestedRangeControlsAllocationAndCopiedBytes)
    {
        const auto committed = emu.memory.compute_memory_stats().committed_memory;
        ASSERT_EQ(map(0x1001), STATUS_SUCCESS);
        EXPECT_EQ(size(), 0x2000u);
        EXPECT_EQ(emu.memory.compute_memory_stats().committed_memory - committed, 0x2000u);
        std::vector<uint8_t> mapped(0x2000);
        emu.memory.read_memory(base(), mapped.data(), mapped.size());
        EXPECT_TRUE(std::equal(mapped.begin(), mapped.end(), data.begin()));
        EXPECT_EQ(emu.memory.get_region_info(base()).kind, memory_region_kind::file_section_view);
        ASSERT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, base()), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.compute_memory_stats().committed_memory, committed);
    }

    TEST_F(FileSectionViewTest, ZeroSizeMapsRemainingSectionAndZeroFillsLastPage)
    {
        ASSERT_EQ(map(0, 0x30000), STATUS_SUCCESS);
        EXPECT_EQ(size(), 0x1000u);
        std::vector<uint8_t> mapped(0x1000);
        emu.memory.read_memory(base(), mapped.data(), mapped.size());
        EXPECT_TRUE(std::equal(data.begin() + 0x30000, data.end(), mapped.begin()));
        EXPECT_TRUE(std::all_of(mapped.begin() + 0x25, mapped.end(), [](uint8_t value) { return value == 0; }));
    }

    TEST_F(FileSectionViewTest, OffsetIsRoundedToAllocationGranularity)
    {
        ASSERT_EQ(map(0x1000, 0x12345), STATUS_SUCCESS);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(arguments + 16), 0x10000u);
        std::array<uint8_t, 0x1000> mapped{};
        emu.memory.read_memory(base(), mapped.data(), mapped.size());
        EXPECT_TRUE(std::equal(mapped.begin(), mapped.end(), data.begin() + 0x10000));
    }

    TEST_F(FileSectionViewTest, OversizedViewDoesNotAllocateOrChangeOutput)
    {
        const auto committed = emu.memory.compute_memory_stats().committed_memory;
        EXPECT_EQ(map(0x40000), static_cast<NTSTATUS>(0xc000001f));
        EXPECT_EQ(base(), 0u);
        EXPECT_EQ(size(), 0x40000u);
        EXPECT_EQ(map(UINT64_MAX), static_cast<NTSTATUS>(0xc000001f));
        EXPECT_EQ(map(0, UINT64_MAX), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(map(0, 0x40000), static_cast<NTSTATUS>(0xc000001f));
        EXPECT_EQ(emu.memory.compute_memory_stats().committed_memory, committed);
    }

    TEST_F(FileSectionViewTest, MaximumSectionSizeLimitsZeroLengthView)
    {
        emu.process.sections.get(section_handle)->maximum_size = 0x10000;
        ASSERT_EQ(map(0), STATUS_SUCCESS);
        EXPECT_EQ(size(), 0x10000u);
        EXPECT_EQ(map(0x11000), static_cast<NTSTATUS>(0xc000001f));
    }
}
