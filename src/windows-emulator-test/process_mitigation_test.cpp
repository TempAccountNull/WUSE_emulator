#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <module/module_mapping.hpp>
#include <platform/win_pefile.hpp>
#include <fstream>

namespace sogen::syscalls
{
    NTSTATUS handle_NtMapViewOfSection(const syscall_context&, handle, handle, emulator_object<uint64_t>, uint64_t, uint64_t,
                                       emulator_object<LARGE_INTEGER>, emulator_object<uint64_t>, SECTION_INHERIT, ULONG, ULONG);
    NTSTATUS handle_NtSetInformationProcess(const syscall_context&, handle, uint32_t, uint64_t, uint32_t);
    NTSTATUS handle_NtQueryInformationProcess(const syscall_context&, handle, uint32_t, uint64_t, uint32_t, emulator_object<uint32_t>);
}

namespace sogen::test
{
    TEST(ProcessMitigationStateTest, UninitializedSnapshotRestores)
    {
        auto emu = create_empty_emulator();
        utils::buffer_serializer buffer{};
        emu.serialize(buffer);
        utils::buffer_deserializer restored{buffer};
        emu.deserialize(restored);
        EXPECT_EQ(restored.get_remaining_size(), 0u);
        EXPECT_EQ(emu.memory.get_aslr_policy(), 0u);
    }

    TEST(ProcessMitigationStateTest, RelativeTimeSetupReproducesRandomizedLayout)
    {
        emulator_settings settings{.disable_logging = true, .use_relative_time = true};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        auto first = create_sample_emulator(settings);
        auto second = create_sample_emulator(settings);
        first.setup_process_if_necessary();
        second.setup_process_if_necessary();
        EXPECT_EQ(first.memory.get_default_allocation_address(), second.memory.get_default_allocation_address());
        EXPECT_GT(first.memory.get_default_allocation_address(), DEFAULT_ALLOCATION_ADDRESS_64BIT);
        for (unsigned int i = 0; i < 4; ++i)
        {
            EXPECT_EQ(first.memory.find_randomized_image_base(0x1000, false), second.memory.find_randomized_image_base(0x1000, false));
        }
    }

    class ProcessMitigationTest : public testing::Test
    {
      public:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            settings.path_mappings["C:\\aslr-test.exe"] =
                std::filesystem::temp_directory_path() / ("sogen-aslr-image-" + std::to_string(getpid()) + ".exe");
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t input{};

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            input = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        }

        void TearDown() override
        {
            std::filesystem::remove(emu.file_sys.translate(windows_path{"C:\\aslr-test.exe"}));
        }

        NTSTATUS map_image(bool stripped)
        {
            const auto bytes = image(stripped);
            const auto path = emu.file_sys.translate(windows_path{"C:\\aslr-test.exe"});
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            file.close();
            section image_section{};
            image_section.file_name = u"C:\\aslr-test.exe";
            image_section.allocation_attributes = SEC_IMAGE;
            const auto section_handle = emu.process.sections.store(std::move(image_section));
            emu.emu().write_memory<uint64_t>(input + 32, 0);
            emu.emu().write_memory<uint64_t>(input + 40, 0);
            return syscalls::handle_NtMapViewOfSection(context(), section_handle, CURRENT_PROCESS, {emu.memory, input + 32}, 0, 0,
                                                       {emu.memory, 0}, {emu.memory, input + 40}, ViewShare, 0, PAGE_READONLY);
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        NTSTATUS set(uint32_t flags, uint32_t length = 8)
        {
            emu.emu().write_memory(input, PROCESS_MITIGATION_POLICY_RAW_DATA{ProcessASLRPolicy, flags});
            return syscalls::handle_NtSetInformationProcess(context(), CURRENT_PROCESS, ProcessMitigationPolicy, input, length);
        }

        uint32_t query()
        {
            emu.emu().write_memory(input, PROCESS_MITIGATION_POLICY_RAW_DATA{ProcessASLRPolicy, 0xaaaaaaaa});
            const emulator_object<uint32_t> length{emu.memory, input + 16};
            EXPECT_EQ(syscalls::handle_NtQueryInformationProcess(context(), CURRENT_PROCESS, ProcessMitigationPolicy, input, 8, length),
                      STATUS_SUCCESS);
            EXPECT_EQ(length.read(), 8u);
            return emu.emu().read_memory<PROCESS_MITIGATION_POLICY_RAW_DATA>(input).Value;
        }

        static std::vector<std::byte> image(bool stripped)
        {
            std::vector<std::byte> data(0x600);
            const auto put = [&]<typename T>(size_t offset, const T& value) { std::memcpy(data.data() + offset, &value, sizeof(value)); };
            PEDosHeader_t dos{};
            dos.e_magic = 0x5a4d;
            dos.e_lfanew = 0x80;
            put(0, dos);
            PENTHeaders_t<uint64_t> nt{};
            nt.Signature = 0x4550;
            nt.FileHeader.Machine = PEMachineType::AMD64;
            nt.FileHeader.NumberOfSections = 2;
            nt.FileHeader.SizeOfOptionalHeader = sizeof(nt.OptionalHeader);
            nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | (stripped ? IMAGE_FILE_RELOCS_STRIPPED : 0);
            nt.OptionalHeader.Magic = 0x20b;
            nt.OptionalHeader.ImageBase = 0x600000000;
            nt.OptionalHeader.SizeOfImage = 0x3000;
            nt.OptionalHeader.SizeOfHeaders = 0x200;
            nt.OptionalHeader.SectionAlignment = 0x1000;
            nt.OptionalHeader.FileAlignment = 0x200;
            nt.OptionalHeader.NumberOfRvaAndSizes = 16;
            nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] = {.VirtualAddress = 0x2000, .Size = 12};
            put(0x80, nt);
            IMAGE_SECTION_HEADER text{};
            text.VirtualAddress = 0x1000;
            text.Misc.VirtualSize = 0x1000;
            text.SizeOfRawData = 0x200;
            text.PointerToRawData = 0x200;
            text.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
            put(0x80 + sizeof(nt), text);
            auto reloc = text;
            reloc.VirtualAddress = 0x2000;
            reloc.PointerToRawData = 0x400;
            put(0x80 + sizeof(nt) + sizeof(text), reloc);
            put(0x200, uint64_t{0x600001234});
            put(0x400, uint32_t{0x1000});
            put(0x404, uint32_t{12});
            put(0x408, uint16_t{0xa000});
            return data;
        }
    };

    TEST_F(ProcessMitigationTest, MatchesNativeMonotonicPolicyAndImmutableEntropy)
    {
        const auto initial = query();
        EXPECT_EQ(initial & 0xb, 1u);
        ASSERT_EQ(set(0xf), STATUS_SUCCESS);
        EXPECT_EQ(query(), 0xbu | (initial & 4));
        ASSERT_EQ(set(0xb), STATUS_SUCCESS);
        EXPECT_EQ(query(), 0xbu | (initial & 4));
        EXPECT_EQ(set(7), STATUS_ACCESS_DENIED);
        EXPECT_EQ(set(1), STATUS_ACCESS_DENIED);
        EXPECT_EQ(query(), 0xbu | (initial & 4));
    }

    TEST_F(ProcessMitigationTest, RejectsBadFlagsAndInputWithoutChangingPolicy)
    {
        const auto initial = query();
        EXPECT_EQ(set(9), STATUS_INVALID_PARAMETER_MIX);
        EXPECT_EQ(set(0x11), STATUS_INVALID_PARAMETER);
        for (const auto length : {0u, 4u, 7u, 9u, 16u})
        {
            EXPECT_EQ(set(0xf, length), STATUS_INFO_LENGTH_MISMATCH);
        }
        EXPECT_EQ(syscalls::handle_NtSetInformationProcess(context(), CURRENT_PROCESS, ProcessMitigationPolicy, input + 1, 8),
                  STATUS_DATATYPE_MISALIGNMENT);
        EXPECT_EQ(syscalls::handle_NtSetInformationProcess(context(), CURRENT_PROCESS, ProcessMitigationPolicy, 0, 8),
                  STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(syscalls::handle_NtSetInformationProcess(context(), make_handle(1), ProcessMitigationPolicy, input, 8),
                  STATUS_INVALID_PARAMETER);
        EXPECT_EQ(query(), initial);
    }

    TEST_F(ProcessMitigationTest, QueryRejectsBadLengthWithoutWritingOutput)
    {
        emu.emu().write_memory<uint64_t>(input, 0xa5a5a5a500000001);
        for (const auto size : {0u, 4u, 16u})
        {
            EXPECT_EQ(syscalls::handle_NtQueryInformationProcess(context(), CURRENT_PROCESS, ProcessMitigationPolicy, input, size,
                                                                 {emu.memory, 0}),
                      STATUS_INFO_LENGTH_MISMATCH);
            EXPECT_EQ(emu.emu().read_memory<uint64_t>(input), 0xa5a5a5a500000001ULL);
        }
    }

    TEST_F(ProcessMitigationTest, AutomaticAllocationsUseRandomizedOriginAndKeepExplicitAddresses)
    {
        const auto origin = emu.memory.get_default_allocation_address();
        EXPECT_GT(origin, DEFAULT_ALLOCATION_ADDRESS_64BIT);
        EXPECT_EQ(origin % ALLOCATION_GRANULARITY, 0u);
        const auto address = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        EXPECT_GE(address, origin);
        EXPECT_TRUE(emu.memory.allocate_memory(0x500000000, 0x1000, memory_permission::read_write));
        EXPECT_TRUE(emu.memory.get_region_info(0x500000000).is_reserved);
    }

    TEST_F(ProcessMitigationTest, ForcedImageRelocationUpdatesAbsolutePointers)
    {
        ASSERT_EQ(set(0xf), STATUS_SUCCESS);
        ASSERT_FALSE(emu.memory.get_region_info(0x600000000).is_reserved);
        const auto data = image(false);
        const auto mapped = map_module_from_data<uint64_t>(emu.memory, data, "aslr-test.exe", windows_path{"C:\\aslr-test.exe"});
        EXPECT_NE(mapped.image_base, 0x600000000ULL);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(mapped.image_base + 0x1000), mapped.image_base + 0x1234);
    }

    TEST_F(ProcessMitigationTest, RejectsStrippedImagesBeforeMapping)
    {
        ASSERT_EQ(set(0xf), STATUS_SUCCESS);
        const auto data = image(true);
        const auto regions = emu.memory.get_reserved_regions().size();
        EXPECT_THROW(map_module_from_data<uint64_t>(emu.memory, data, "stripped.exe", windows_path{"C:\\stripped.exe"}),
                     image_relocation_error);
        EXPECT_EQ(emu.memory.get_reserved_regions().size(), regions);
    }

    TEST_F(ProcessMitigationTest, ImageSyscallReturnsRelocatedViewAndAdjustedPointers)
    {
        ASSERT_EQ(set(0xf), STATUS_SUCCESS);
        ASSERT_EQ(map_image(false), STATUS_IMAGE_NOT_AT_BASE);
        const auto base = emu.emu().read_memory<uint64_t>(input + 32);
        EXPECT_NE(base, 0x600000000ULL);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(input + 40), 0x3000u);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(base + 0x1000), base + 0x1234);
    }

    TEST_F(ProcessMitigationTest, ImageSyscallRejectsStrippedImageWithoutOutputOrAllocation)
    {
        ASSERT_EQ(set(0xf), STATUS_SUCCESS);
        const auto regions = emu.memory.get_reserved_regions().size();
        EXPECT_EQ(map_image(true), STATUS_ILLEGAL_DLL_RELOCATION);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(input + 32), 0u);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(input + 40), 0u);
        EXPECT_EQ(emu.memory.get_reserved_regions().size(), regions);
    }

    TEST_F(ProcessMitigationTest, PolicyAllocationOriginAndRandomSequenceSurviveSnapshot)
    {
        ASSERT_EQ(set(0xf), STATUS_SUCCESS);
        const auto flags = query();
        const auto origin = emu.memory.get_default_allocation_address();
        utils::buffer_serializer output{};
        emu.serialize(output);
        const auto expected = emu.memory.find_randomized_image_base(0x1000, false);
        emu.memory.initialize_aslr_policy(false, false);
        utils::buffer_deserializer restored{output.get_buffer()};
        emu.deserialize(restored);
        EXPECT_EQ(query(), flags);
        EXPECT_EQ(emu.memory.get_default_allocation_address(), origin);
        EXPECT_EQ(emu.memory.find_randomized_image_base(0x1000, false), expected);
        EXPECT_EQ(restored.get_remaining_size(), 0u);
    }

    TEST_F(ProcessMitigationTest, LoadsLegacySnapshotWithoutAslrExtension)
    {
        utils::buffer_serializer output{};
        emu.serialize(output);
        auto bytes = output.move_buffer();
        utils::buffer_serializer extensions{};
        emu.memory.serialize_aslr_state(extensions);
        const auto& suffix = extensions.get_buffer();
        ASSERT_GE(bytes.size(), suffix.size());
        ASSERT_TRUE(std::equal(suffix.rbegin(), suffix.rend(), bytes.rbegin()));
        bytes.resize(bytes.size() - suffix.size());
        utils::buffer_deserializer restored{bytes};
        emu.deserialize(restored);
        EXPECT_EQ(query() & 0xb, 1u);
        EXPECT_EQ(restored.get_remaining_size(), 0u);
    }
}
