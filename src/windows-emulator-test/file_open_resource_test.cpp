#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <utils/finally.hpp>

#include <fstream>
#include <vector>
#include <gtest/gtest.h>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateFile(const syscall_context&, emulator_object<handle>, ACCESS_MASK,
                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>,
                                 emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>, emulator_object<LARGE_INTEGER>, ULONG, ULONG,
                                 ULONG, ULONG, uint64_t, ULONG);
    NTSTATUS handle_NtClose(const syscall_context&, handle);
}

namespace sogen::test
{
    TEST(FileOpenResource, ReportsOpenResultAndSupportsMoreThanDefaultHostStreamLimit)
    {
        const auto host_path = std::filesystem::temp_directory_path() / ("sogen-file-open-resource-" + std::to_string(getpid()) + ".pkg");
        auto cleanup = utils::finally([&] {
            std::error_code ec;
            std::filesystem::remove(host_path, ec);
        });
        {
            std::ofstream output(host_path, std::ios::binary);
            ASSERT_TRUE(output.good());
            output.put('X');
        }

        emulator_settings settings{.disable_logging = true};
        settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
        settings.path_mappings[R"(C:\resource-probe.pkg)"] = host_path;
        auto emu = create_sample_emulator(std::move(settings));
        emu.setup_process_if_necessary();

        const auto memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        constexpr std::u16string_view name = u"\\??\\c:\\resource-probe.pkg";
        emu.memory.write_memory(memory + 0x100, name.data(), name.size() * sizeof(char16_t));
        UNICODE_STRING<EmulatorTraits<Emu64>> descriptor{};
        descriptor.Length = static_cast<USHORT>(name.size() * sizeof(char16_t));
        descriptor.MaximumLength = descriptor.Length;
        descriptor.Buffer = memory + 0x100;
        emu.memory.write_memory(memory + 0x80, &descriptor, sizeof(descriptor));
        OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> attributes{};
        attributes.Length = sizeof(attributes);
        attributes.ObjectName = memory + 0x80;
        emu.memory.write_memory(memory + 0x40, &attributes, sizeof(attributes));

        auto& vcpu = emu.vcpu(0);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{emu.memory, memory + 0x200};
        std::vector<handle> handles;
        handles.reserve(768);
        for (size_t i = 0; i < 768; ++i)
        {
            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> sentinel{};
            sentinel.Status = static_cast<NTSTATUS>(0x12345678);
            sentinel.Information = 0x12345678;
            io_status.write(sentinel);
            ASSERT_EQ(syscalls::handle_NtCreateFile(context, {emu.memory, memory}, 0x80100080, {emu.memory, memory + 0x40}, io_status,
                                                    {emu.memory, 0}, 0, 7, FILE_OPEN,
                                                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, 0, 0),
                      STATUS_SUCCESS)
                << "open " << i;
            EXPECT_EQ(io_status.read().Status, STATUS_SUCCESS);
            EXPECT_EQ(io_status.read().Information, 1u);
            handles.push_back(make_handle(emu.emu().read_memory<uint64_t>(memory)));
        }
        for (const auto file_handle : handles)
        {
            ASSERT_EQ(syscalls::handle_NtClose(context, file_handle), STATUS_SUCCESS);
        }

        std::filesystem::remove(host_path);
        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> sentinel{};
        sentinel.Status = static_cast<NTSTATUS>(0x12345678);
        sentinel.Information = 0x12345678;
        io_status.write(sentinel);
        ASSERT_EQ(syscalls::handle_NtCreateFile(context, {emu.memory, memory}, 0x80100080, {emu.memory, memory + 0x40}, io_status,
                                                {emu.memory, 0}, 0, 7, FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, 0,
                                                0),
                  STATUS_OBJECT_NAME_NOT_FOUND);
        EXPECT_EQ(io_status.read().Status, sentinel.Status);
        EXPECT_EQ(io_status.read().Information, sentinel.Information);
    }
}
