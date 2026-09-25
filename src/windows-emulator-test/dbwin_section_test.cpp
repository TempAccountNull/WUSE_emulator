#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS
    handle_NtOpenSection(const syscall_context&, emulator_object<handle>, ACCESS_MASK,
                         emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>);
    NTSTATUS handle_NtMapViewOfSection(const syscall_context&, handle, handle, emulator_object<uint64_t>, uint64_t, uint64_t,
                                       emulator_object<LARGE_INTEGER>, emulator_object<uint64_t>, SECTION_INHERIT, ULONG, ULONG);
    NTSTATUS handle_NtUnmapViewOfSection(const syscall_context&, handle, uint64_t);
    NTSTATUS handle_NtClose(const syscall_context&, handle);
    NTSTATUS handle_NtSetEvent(const syscall_context&, uint64_t, emulator_object<LONG>);
} // namespace sogen::syscalls

namespace sogen::test
{
    class DbwinSectionTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t arguments{};

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            arguments = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            constexpr auto name = std::u16string_view{u"DBWIN_BUFFER"};
            emu.memory.write_memory(arguments + 0x100, name.data(), name.size() * sizeof(char16_t));
            UNICODE_STRING<EmulatorTraits<Emu64>> descriptor{};
            descriptor.Length = static_cast<USHORT>(name.size() * sizeof(char16_t));
            descriptor.MaximumLength = descriptor.Length;
            descriptor.Buffer = arguments + 0x100;
            emu.memory.write_memory(arguments + 0x80, &descriptor, sizeof(descriptor));
            OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> attributes{};
            attributes.Length = sizeof(attributes);
            attributes.ObjectName = arguments + 0x80;
            emu.memory.write_memory(arguments + 0x40, &attributes, sizeof(attributes));
        }

        handle open()
        {
            EXPECT_EQ(syscalls::handle_NtOpenSection(context(), {emu.memory, arguments}, 0x100001, {emu.memory, arguments + 0x40}),
                      STATUS_SUCCESS);
            return make_handle(emu.emu().read_memory<uint64_t>(arguments));
        }

        uint64_t map(const handle section_handle)
        {
            emu.emu().write_memory<uint64_t>(arguments + 0x200, 0);
            emu.emu().write_memory<uint64_t>(arguments + 0x208, 0);
            EXPECT_EQ(syscalls::handle_NtMapViewOfSection(context(), section_handle, CURRENT_PROCESS, {emu.memory, arguments + 0x200}, 0, 0,
                                                          {emu.memory, 0}, {emu.memory, arguments + 0x208}, ViewUnmap, 0, PAGE_READWRITE),
                      STATUS_SUCCESS);
            EXPECT_EQ(emu.emu().read_memory<uint64_t>(arguments + 0x208), 0x1000u);
            return emu.emu().read_memory<uint64_t>(arguments + 0x200);
        }
    };

    TEST_F(DbwinSectionTest, OpensShareBackingAndUnmapEachViewIndependently)
    {
        const auto initial = emu.memory.compute_memory_stats().reserved_memory;
        const auto first_handle = open();
        const auto first = map(first_handle);
        ASSERT_NE(first, 0u);
        ASSERT_EQ(syscalls::handle_NtClose(context(), first_handle), STATUS_SUCCESS);

        const auto second_handle = open();
        const auto second = map(second_handle);
        ASSERT_NE(second, 0u);
        ASSERT_NE(second, first);
        emu.emu().write_memory<uint32_t>(first, 0x12345678);
        EXPECT_EQ(emu.emu().read_memory<uint32_t>(second), 0x12345678u);

        ASSERT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, first), STATUS_SUCCESS);
        EXPECT_FALSE(emu.memory.get_region_info(first).is_reserved);
        EXPECT_TRUE(emu.memory.get_region_info(second).is_reserved);
        EXPECT_EQ(emu.process.dbwin_buffer, second);
        ASSERT_EQ(syscalls::handle_NtClose(context(), second_handle), STATUS_SUCCESS);
        ASSERT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, second), STATUS_SUCCESS);
        EXPECT_FALSE(emu.memory.get_region_info(second).is_reserved);
        EXPECT_EQ(emu.process.dbwin_buffer, 0u);
        EXPECT_EQ(emu.memory.compute_memory_stats().reserved_memory, initial + 0x1000);
    }

    TEST_F(DbwinSectionTest, MultipleViewsOfOneOpenRestoreRemainingDebugBuffer)
    {
        const auto section_handle = open();
        const auto first = map(section_handle);
        const auto second = map(section_handle);
        ASSERT_NE(first, second);
        EXPECT_EQ(emu.process.dbwin_buffer, second);
        ASSERT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, second), STATUS_SUCCESS);
        EXPECT_EQ(emu.process.dbwin_buffer, first);
        std::string observed;
        emu.callbacks.on_debug_string.add([&](std::string_view message) { observed = message; });
        constexpr char payload[] = "remaining view";
        emu.memory.write_memory(first + 4, payload, sizeof(payload));
        EXPECT_EQ(syscalls::handle_NtSetEvent(context(), DBWIN_DATA_READY.bits, {emu.memory, 0}), STATUS_SUCCESS);
        EXPECT_EQ(observed, "remaining view");
        ASSERT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, first), STATUS_SUCCESS);
        EXPECT_EQ(emu.process.dbwin_buffer, 0u);
        EXPECT_EQ(syscalls::handle_NtClose(context(), section_handle), STATUS_SUCCESS);
    }

    TEST_F(DbwinSectionTest, RejectedViewsLeaveSectionAndDebugBufferUsable)
    {
        const auto section_handle = open();
        const auto initial = emu.memory.compute_memory_stats().reserved_memory;
        emu.emu().write_memory<uint64_t>(arguments + 0x200, 0);
        emu.emu().write_memory<uint64_t>(arguments + 0x208, 0x2000);
        EXPECT_EQ(syscalls::handle_NtMapViewOfSection(context(), section_handle, CURRENT_PROCESS, {emu.memory, arguments + 0x200}, 0, 0,
                                                      {emu.memory, 0}, {emu.memory, arguments + 0x208}, ViewUnmap, 0, PAGE_READWRITE),
                  static_cast<NTSTATUS>(0xc000001f));
        EXPECT_EQ(emu.memory.compute_memory_stats().reserved_memory, initial);
        EXPECT_EQ(emu.process.dbwin_buffer, 0u);

        emu.emu().write_memory<uint64_t>(arguments + 0x200, arguments);
        emu.emu().write_memory<uint64_t>(arguments + 0x208, 0x1000);
        EXPECT_EQ(syscalls::handle_NtMapViewOfSection(context(), section_handle, CURRENT_PROCESS, {emu.memory, arguments + 0x200}, 0, 0,
                                                      {emu.memory, 0}, {emu.memory, arguments + 0x208}, ViewUnmap, 0, PAGE_READWRITE),
                  STATUS_CONFLICTING_ADDRESSES);
        EXPECT_EQ(emu.memory.compute_memory_stats().reserved_memory, initial);
        EXPECT_EQ(emu.process.dbwin_buffer, 0u);

        const auto view = map(section_handle);
        ASSERT_NE(view, 0u);
        ASSERT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, view), STATUS_SUCCESS);
        EXPECT_EQ(syscalls::handle_NtClose(context(), section_handle), STATUS_SUCCESS);
    }
} // namespace sogen::test
