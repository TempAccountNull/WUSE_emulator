#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    uint16_t handle_NtUserRegisterClassExWOW(const syscall_context& c, emulator_object<EMU_WNDCLASSEX> wnd_class_ex,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_version,
                                             emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name, DWORD function_id,
                                             DWORD flags, emulator_pointer wow);
    emulator_pointer handle_NtUserSetClassLongPtr(const syscall_context& c, handle hWnd, int nIndex, emulator_pointer dwNewLong, BOOL Ansi);
}

namespace sogen::test
{
    namespace
    {
        constexpr std::u16string_view class_name = u"SogenTestClass";
        constexpr std::u16string_view menu_name = u"SogenTestMenu";
        constexpr uint64_t menu_resource_id = 0x7B;
        constexpr uint32_t unicode_class_flags = 0xC0;
        constexpr int gclp_wndproc = -24;

        constexpr EMU_WNDCLASSEX test_class{
            .cbSize = sizeof(EMU_WNDCLASSEX),
            .style = 0x3,
            .lpfnWndProc = 0x140001000,
            .cbClsExtra = 8,
            .cbWndExtra = 16,
            .hInstance = 0x140000000,
            .hbrBackground = 6,
            .hIconSm = 0x1234,
        };

        class WindowClassTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                this->menu_name_buffer = this->storage + 0x400;
                this->empty_menu_name_buffer = this->storage + 0x500;
                this->emu.memory.write_memory(this->menu_name_buffer, menu_name.data(), menu_name.size() * sizeof(char16_t));
            }

            // user32's InitClsMenuNameW points pusMenuName at the caller's string, or carries the bare resource ID.
            uint16_t register_class(const emulator_pointer menu_name_value)
            {
                const emulator_object<EMU_WNDCLASSEX> wnd_class{this->emu.memory, this->storage};
                const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> name{this->emu.memory, this->storage + 0x100};
                const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name{this->emu.memory, this->storage + 0x200};
                const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> menu_string{this->emu.memory, this->storage + 0x280};
                const auto name_buffer = this->storage + 0x300;

                wnd_class.write(test_class);
                this->emu.memory.write_memory(name_buffer, class_name.data(), class_name.size() * sizeof(char16_t));
                name.write({.Length = static_cast<USHORT>(class_name.size() * sizeof(char16_t)),
                            .MaximumLength = static_cast<USHORT>(class_name.size() * sizeof(char16_t)),
                            .Buffer = name_buffer});
                menu_string.write({.Buffer = menu_name_value});
                class_menu_name.write(
                    {.pszClientAnsiMenuName = 0x5000, .pwszClientUnicodeMenuName = 0x6000, .pusMenuName = this->storage + 0x280});

                return syscalls::handle_NtUserRegisterClassExWOW(this->context, wnd_class, name, {this->emu.memory}, class_menu_name, 0,
                                                                 unicode_class_flags, 0);
            }

            USER_CLASS class_object()
            {
                const auto entry = this->emu.process.classes.find(std::u16string{class_name});
                EXPECT_NE(entry, this->emu.process.classes.end());
                return this->emu.memory.read_memory<USER_CLASS>(entry->second.guest_obj_addr);
            }

            windows_emulator emu = create_empty_emulator();
            syscall_context context{
                .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            uint64_t storage{};
            uint64_t menu_name_buffer{};
            uint64_t empty_menu_name_buffer{};
        };
    }

    TEST_F(WindowClassTest, ClassObjectCarriesTheRegisteredValues)
    {
        const auto atom = this->register_class(this->menu_name_buffer);
        ASSERT_NE(0, atom);

        const auto cls = this->class_object();
        EXPECT_EQ(atom, cls.atomClassName);
        EXPECT_EQ(unicode_class_flags, cls.CSF_flags);
        EXPECT_EQ(test_class.style, cls.style);
        EXPECT_EQ(8u, cls.cbclsExtra);
        EXPECT_EQ(16u, cls.cbwndExtra);
        EXPECT_EQ(test_class.lpfnWndProc, cls.lpfnWndProc);
        EXPECT_EQ(~0ull, cls.lpszMenuName);
        EXPECT_EQ(0x5000u, cls.lpszClientAnsiMenuName);
        EXPECT_EQ(0x6000u, cls.lpszClientUnicodeMenuName);
        EXPECT_EQ(test_class.hIconSm, cls.hIconSm);
        EXPECT_EQ(test_class.hInstance, cls.hModule);
        EXPECT_EQ(test_class.hbrBackground, cls.hbrBackground);
    }

    TEST_F(WindowClassTest, ResourceIdMenuNameIsNotMarked)
    {
        this->register_class(menu_resource_id);
        EXPECT_EQ(0u, this->class_object().lpszMenuName);
    }

    TEST_F(WindowClassTest, EmptyMenuNameIsNotMarked)
    {
        this->register_class(this->empty_menu_name_buffer);
        EXPECT_EQ(0u, this->class_object().lpszMenuName);
    }

    TEST_F(WindowClassTest, SetClassLongPtrUpdatesTheClassObject)
    {
        this->register_class(menu_resource_id);
        this->emu.process.user_handles.setup(false);
        auto [handle, window] = this->emu.process.windows.create(this->emu.memory);
        window.class_name = class_name;

        EXPECT_EQ(test_class.lpfnWndProc, syscalls::handle_NtUserSetClassLongPtr(this->context, handle, gclp_wndproc, 0x140002000, FALSE));
        EXPECT_EQ(0x140002000u, this->class_object().lpfnWndProc);
    }
}
