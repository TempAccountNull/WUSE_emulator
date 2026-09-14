#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    bool is_window_mapping_operation(const syscall_context&, uint32_t);
    emulator_pointer handle_NtUserCallOneParam(const syscall_context&, uint64_t, uint32_t);
}

namespace sogen::test
{
    class WindowMappingTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t window_handle{};
        uint64_t pointer{};
        uint64_t selector_address{};
        uint64_t slot{};
        uint64_t syscall{};
        uint32_t selector{};

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            auto [h, win] = emu.process.windows.create(emu.memory);
            win.handle = h.bits;
            win.thread_id = emu.vcpu(0).active_thread->id;
            window_handle = h.bits;
            pointer = win.guest.value();
            win.guest.access([&](USER_WINDOW& data) {
                data.hWnd = window_handle;
                data.ptrBase = pointer;
                data.dwStyle = WS_POPUP;
            });
            const auto* user32 = emu.mod_manager.map_module_or_throw(R"(c:\windows\system32\user32.dll)", emu.log);
            const auto* win32u = emu.mod_manager.map_module_or_throw(R"(c:\windows\system32\win32u.dll)", emu.log);
            const auto address = user32->find_export("GetParent");
            ASSERT_NE(address, 0u);
            syscall = win32u->find_export("NtUserCallOneParam");
            ASSERT_NE(syscall, 0u);
            ASSERT_EQ(emu.emu().read_memory<uint8_t>(address + 6), 0xe8);
            const auto helper = address + 11 + static_cast<uint64_t>(emu.emu().read_memory<int32_t>(address + 7));
            std::array<uint8_t, 0x100> bytes{};
            emu.memory.read_memory(helper, bytes.data(), bytes.size());
            constexpr std::array<uint8_t, 6> pattern{0x48, 0x8b, 0xcf, 0x48, 0xff, 0x15};
            for (size_t i = 0; i + 15 <= bytes.size(); ++i)
            {
                if (bytes[i] == 0xba && std::ranges::equal(pattern, std::span{bytes}.subspan(i + 5, pattern.size())))
                {
                    ASSERT_EQ(selector_address, 0u);
                    selector_address = helper + i + 1;
                    selector = emu.emu().read_memory<uint32_t>(selector_address);
                    slot = helper + i + 15 + static_cast<uint64_t>(emu.emu().read_memory<int32_t>(helper + i + 11));
                }
            }
            ASSERT_NE(slot, 0u);
            emu.emu().write_memory<uint64_t>(slot, syscall);
        }

        uint64_t map(const uint64_t value)
        {
            return syscalls::handle_NtUserCallOneParam(context(), value, selector);
        }
    };

    TEST_F(WindowMappingTest, ReturnsExistingWindowWithoutChangingClientOrObject)
    {
        const auto client_before = emu.vcpu(0).active_thread->teb64->read().Win32ClientInfo;
        ASSERT_EQ(map(window_handle), pointer);
        ASSERT_EQ(map(window_handle), pointer);
        EXPECT_EQ(emu.memory.read_memory<USER_WINDOW>(pointer).hWnd, window_handle);
        EXPECT_EQ(emu.memory.read_memory<USER_WINDOW>(pointer).dwStyle, WS_POPUP);
        const auto client_after = emu.vcpu(0).active_thread->teb64->read().Win32ClientInfo;
        EXPECT_EQ(memcmp(&client_before, &client_after, sizeof(client_before)), 0);
    }

    TEST_F(WindowMappingTest, RejectsInvalidGenerationWrongTypeAndDestroyedWindow)
    {
        EXPECT_EQ(map(0), 0u);
        EXPECT_EQ(map(window_handle ^ 0x10000), 0u);
        EXPECT_EQ(map(0xffff), 0u);
        const auto table = emu.process.user_handles.get_handle_table();
        const auto index = static_cast<uint16_t>(window_handle);
        const auto original = table.read(index);
        table.access([](USER_HANDLEENTRY& entry) { entry.bType = TYPE_MENU; }, index);
        EXPECT_EQ(map(window_handle), 0u);
        table.write(original, index);
        table.access([](USER_HANDLEENTRY& entry) { entry.bFlags = 1; }, index);
        EXPECT_EQ(map(window_handle), 0u);
        table.write(original, index);
        table.access([](USER_HANDLEENTRY& entry) { entry.pHead += 0x1000; }, index);
        EXPECT_EQ(map(window_handle), 0u);
        table.write(original, index);
        ASSERT_TRUE(emu.process.windows.erase(window_handle));
        EXPECT_EQ(map(window_handle), 0u);
    }

    TEST_F(WindowMappingTest, AcceptsWildcardGenerationUsedByGuestValidation)
    {
        EXPECT_EQ(map((window_handle & 0xffff) | 0xffff0000), pointer);
    }

    TEST_F(WindowMappingTest, VerifiesImportTargetAndReadsGuestSelector)
    {
        EXPECT_TRUE(syscalls::is_window_mapping_operation(context(), selector));
        EXPECT_FALSE(syscalls::is_window_mapping_operation(context(), selector + 1));
        emu.emu().write_memory<uint64_t>(slot, syscall + 1);
        EXPECT_FALSE(syscalls::is_window_mapping_operation(context(), selector));
        emu.emu().write_memory<uint64_t>(slot, syscall);
        emu.emu().write_memory<uint32_t>(selector_address, selector + 7);
        EXPECT_FALSE(syscalls::is_window_mapping_operation(context(), selector));
        EXPECT_EQ(syscalls::handle_NtUserCallOneParam(context(), window_handle, selector + 7), pointer);
    }

    TEST_F(WindowMappingTest, ResolvesMappingAfterFullSnapshotRestore)
    {
        utils::buffer_serializer output{};
        emu.serialize(output);
        utils::buffer_deserializer input{output.get_buffer()};
        emu.deserialize(input);
        EXPECT_TRUE(emu.mod_manager.find_by_name("user32.dll")->imports.empty());
        EXPECT_EQ(map(window_handle), pointer);
        EXPECT_EQ(emu.memory.read_memory<USER_WINDOW>(pointer).hWnd, window_handle);
    }

    TEST_F(WindowMappingTest, DispatchesPostQuitMessageToCallingThread)
    {
        const auto* user32 = emu.mod_manager.find_by_name("user32.dll");
        const auto address = user32->find_export("PostQuitMessage");
        ASSERT_NE(address, 0u);
        ASSERT_EQ(emu.emu().read_memory<uint8_t>(address + 3), 0xba);
        const auto operation = emu.emu().read_memory<uint32_t>(address + 4);
        const auto target_slot = address + 15 + static_cast<uint64_t>(emu.emu().read_memory<int32_t>(address + 11));
        emu.emu().write_memory<uint64_t>(target_slot, syscall);
        const auto other = emu.process.create_thread(emu.memory, emu.mod_manager.executable->entry_point, 0, 0x10000, 0);
        auto& messages = emu.vcpu(0).active_thread->message_queue;
        const auto count = messages.size();
        ASSERT_EQ(syscalls::handle_NtUserCallOneParam(context(), 42, operation), TRUE);
        ASSERT_EQ(messages.size(), count + 1);
        EXPECT_EQ(messages.back().message, WM_QUIT);
        EXPECT_EQ(messages.back().wParam, 42u);
        EXPECT_TRUE(emu.process.threads.get(other)->message_queue.empty());
    }

}
