#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    uint16_t handle_NtUserRegisterClassExWOW(const syscall_context&, emulator_object<EMU_WNDCLASSEX>,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>,
                                             emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>>, DWORD, DWORD, emulator_pointer);
    emulator_pointer handle_NtUserSetClassLongPtr(const syscall_context&, handle, int, emulator_pointer, BOOL);
}

namespace sogen::test
{
    class SharedClassTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t storage{};
        uint64_t query_export{};
        uint64_t window_handle{};
        uint64_t procedure{};
        uint64_t class_address{};
        uint16_t atom{};
        EMU_WNDCLASSEX definition{};

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            storage = emu.memory.allocate_memory(0x2000, memory_permission::all);
            procedure = emu.mod_manager.executable->image_base + 0x1000;
            definition.cbSize = sizeof(definition);
            definition.lpfnWndProc = procedure;
            definition.hInstance = emu.mod_manager.executable->image_base;
            definition.style = 3;
            definition.cbClsExtra = 512;
            definition.cbWndExtra = 24;
            definition.hIconSm = 0x12345678;
            emu.emu().write_memory(storage, definition);
            constexpr auto name = std::to_array(u"SharedClassTest");
            emu.memory.write_memory(storage + 0x100, name.data(), sizeof(name));
            const UNICODE_STRING<EmulatorTraits<Emu64>> unicode{
                .Length = sizeof(name) - sizeof(char16_t), .MaximumLength = sizeof(name), .Buffer = storage + 0x100};
            emu.emu().write_memory(storage + 0x80, unicode);
            atom = syscalls::handle_NtUserRegisterClassExWOW(context(), {emu.memory, storage}, {emu.memory, storage + 0x80},
                                                             {emu.memory, 0}, {emu.memory, storage + 0xC0}, 0, 0, 0);
            ASSERT_NE(atom, 0u);
            class_address = emu.process.classes.at(u"SharedClassTest").guest_obj_addr;
            auto [handle, win] = emu.process.windows.create(emu.memory);
            window_handle = handle.bits;
            win.handle = handle.bits;
            win.class_name = u"SharedClassTest";
            win.wnd_proc = procedure;
            win.guest.access([&](USER_WINDOW& value) {
                value.hWnd = window_handle;
                value.ptrBase = win.guest.value();
                value.pcls = class_address;
                value.lpfnWndProc = procedure;
            });
            emu.vcpu(0).active_thread->teb64->access([&](TEB64& teb) {
                teb.Win32ClientInfo.arr[8] = window_handle;
                teb.Win32ClientInfo.arr[9] = win.guest.value();
            });
            const auto* user32 = emu.mod_manager.map_module_or_throw(R"(c:\windows\system32\user32.dll)", emu.log);
            query_export = user32->find_export("GetClassLongPtrW");
            ASSERT_NE(query_export, 0u);
        }

        uint64_t query(const int index)
        {
            auto& cpu = emu.emu();
            const auto stack = storage + 0x1F08;
            const auto done = storage + 0x800;
            cpu.write_memory<uint64_t>(stack, done);
            cpu.write_memory<uint8_t>(done, 0x90);
            cpu.reg(x86_register::rsp, stack);
            cpu.reg(x86_register::rip, query_export);
            cpu.reg(x86_register::rcx, window_handle);
            cpu.reg(x86_register::rdx, static_cast<uint32_t>(index));
            bool returned = false;
            auto* hook = cpu.hook_memory_execution(done, [&](cpu_interface& current, uint64_t) {
                returned = true;
                current.stop();
            });
            cpu.start(500);
            cpu.delete_hook(hook);
            EXPECT_TRUE(returned);
            EXPECT_EQ(cpu.reg<uint64_t>(x86_register::rsp), stack + 8);
            return cpu.reg<uint64_t>(x86_register::rax);
        }
    };

    TEST_F(SharedClassTest, ActualUser32ReadsRegisteredFields)
    {
        EXPECT_EQ(query(-24), procedure);
        EXPECT_EQ(query(-16), definition.hInstance);
        EXPECT_EQ(query(-26), definition.style);
        EXPECT_EQ(query(-20), 512u);
        EXPECT_EQ(query(-18), 24u);
        EXPECT_EQ(query(-34), definition.hIconSm);
        EXPECT_EQ(query(-32), atom);
    }

    TEST_F(SharedClassTest, ClassExtraStorageDoesNotOverlapName)
    {
        const auto before = emu.memory.read_memory<USER_CLASS>(class_address);
        ASSERT_GE(before.lpszAnsiClassName, class_address + 0x58 + 512);
        constexpr uint64_t value = 0xFEDCBA9876543210;
        emu.emu().write_memory<uint64_t>(class_address + 0x58 + 504, value);
        EXPECT_EQ(query(504), value);
        process_context::sync_user_class(emu.memory, emu.process.classes.at(u"SharedClassTest"));
        EXPECT_EQ(query(504), value);
        EXPECT_EQ(emu.memory.read_memory<uint8_t>(before.lpszAnsiClassName), 'S');
    }

    TEST_F(SharedClassTest, ClassUpdatesSynchronizeAliasesWithoutSubclassingExistingWindow)
    {
        const auto replacement = procedure + 0x40;
        EXPECT_EQ(syscalls::handle_NtUserSetClassLongPtr(context(), {.bits = window_handle}, -24, replacement, FALSE), procedure);
        EXPECT_EQ(query(-24), replacement);
        for (const auto& entry : emu.process.classes | std::views::values)
        {
            if (entry.guest_obj_addr == class_address)
            {
                EXPECT_EQ(entry.wnd_class.lpfnWndProc, replacement);
            }
        }
        EXPECT_EQ(emu.process.windows.get(window_handle)->wnd_proc, procedure);
        EXPECT_EQ(emu.process.windows.get(window_handle)->guest.read().lpfnWndProc, procedure);
    }

    TEST_F(SharedClassTest, SnapshotRebuildsDescriptorFromRegisteredClass)
    {
        constexpr uint64_t extra = 0x987654321;
        emu.emu().write_memory<uint64_t>(class_address + 0x58, extra);
        emulator_object<USER_CLASS>{emu.memory, class_address}.access([](USER_CLASS& value) {
            value.window_procedure = 0;
            value.style = 0;
        });
        utils::buffer_serializer output{};
        emu.serialize(output);
        utils::buffer_deserializer input{output.get_buffer()};
        emu.deserialize(input);
        EXPECT_EQ(query(-24), procedure);
        EXPECT_EQ(query(-26), definition.style);
        EXPECT_EQ(query(0), extra);
    }
}
