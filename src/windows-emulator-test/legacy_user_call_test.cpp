#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    bool is_message_beep_operation(const syscall_context&, uint32_t);
    bool is_update_window_operation(const syscall_context&, uint32_t);
    emulator_pointer handle_NtUserCallOneParam(const syscall_context&, uint64_t, uint32_t);
    BOOL handle_NtUserCallHwndLock(const syscall_context&, hwnd, uint32_t);
    BOOL completion_NtUserUpdateWindow(const syscall_context&, hwnd);
}

namespace sogen::test
{
    class LegacyUserCallTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t beep{};
        uint64_t update{};
        uint64_t beep_slot{};
        uint64_t update_slot{};
        uint64_t one_param{};
        uint64_t hwnd_lock{};
        uint32_t beep_selector{};
        uint32_t update_selector{};
        hwnd target{};

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            const auto* user32 = emu.mod_manager.map_module_or_throw(R"(c:\windows\system32\user32.dll)", emu.log);
            const auto* win32u = emu.mod_manager.map_module_or_throw(R"(c:\windows\system32\win32u.dll)", emu.log);
            beep = user32->find_export("MessageBeep");
            update = user32->find_export("UpdateWindow");
            one_param = win32u->find_export("NtUserCallOneParam");
            hwnd_lock = win32u->find_export("NtUserCallHwndLock");
            ASSERT_NE(beep, 0u);
            ASSERT_NE(update, 0u);
            ASSERT_NE(one_param, 0u);
            ASSERT_NE(hwnd_lock, 0u);
            ASSERT_EQ(emu.emu().read_memory<uint8_t>(beep + 2), 0xba);
            ASSERT_EQ(emu.emu().read_memory<uint8_t>(update + 0x36), 0xba);
            beep_selector = emu.emu().read_memory<uint32_t>(beep + 3);
            update_selector = emu.emu().read_memory<uint32_t>(update + 0x37);
            beep_slot = beep + 14 + static_cast<uint64_t>(emu.emu().read_memory<int32_t>(beep + 10));
            update_slot = update + 0x4a + static_cast<uint64_t>(emu.emu().read_memory<int32_t>(update + 0x46));
            emu.emu().write_memory<uint64_t>(beep_slot, one_param);
            emu.emu().write_memory<uint64_t>(update_slot, hwnd_lock);
            auto [handle, win] = emu.process.windows.create(emu.memory);
            target = handle.bits;
            win.handle = target;
            win.thread_id = emu.vcpu(0).active_thread->id;
            win.style = WS_POPUP | WS_VISIBLE;
            win.parent_handle = emu.process.default_desktop_window_handle.bits;
            const auto stack = emu.memory.allocate_memory(0x10000, memory_permission::read_write);
            emu.emu().reg(x86_register::rsp, stack + 0xff00);
        }
    };

    TEST_F(LegacyUserCallTest, BeepUsesGuestSelectorAndVerifiedImport)
    {
        EXPECT_TRUE(syscalls::is_message_beep_operation(context(), beep_selector));
        emu.emu().write_memory<uint32_t>(beep + 3, beep_selector + 5);
        EXPECT_FALSE(syscalls::is_message_beep_operation(context(), beep_selector));
        EXPECT_TRUE(syscalls::is_message_beep_operation(context(), beep_selector + 5));
        emu.emu().write_memory<uint64_t>(beep_slot, one_param + 1);
        EXPECT_FALSE(syscalls::is_message_beep_operation(context(), beep_selector + 5));
    }

    TEST_F(LegacyUserCallTest, BeepDispatchesWithoutStoppingGuest)
    {
        const auto c = context();
        EXPECT_EQ(syscalls::handle_NtUserCallOneParam(c, 0x10, beep_selector), TRUE);
        EXPECT_FALSE(c.run_callback);
        EXPECT_FALSE(emu.process.exit_status.has_value());
    }

    TEST_F(LegacyUserCallTest, UpdateUsesGuestSelectorAndVerifiedImport)
    {
        EXPECT_TRUE(syscalls::is_update_window_operation(context(), update_selector));
        emu.emu().write_memory<uint32_t>(update + 0x37, update_selector + 3);
        EXPECT_FALSE(syscalls::is_update_window_operation(context(), update_selector));
        EXPECT_TRUE(syscalls::is_update_window_operation(context(), update_selector + 3));
        emu.emu().write_memory<uint64_t>(update_slot, hwnd_lock + 1);
        EXPECT_FALSE(syscalls::is_update_window_operation(context(), update_selector + 3));
    }

    TEST_F(LegacyUserCallTest, EmptyUpdateDoesNotDispatchPaint)
    {
        const auto c = context();
        EXPECT_EQ(syscalls::handle_NtUserCallHwndLock(c, target, update_selector), TRUE);
        EXPECT_FALSE(c.run_callback);
        EXPECT_TRUE(c.thread().callback_stack.empty());
        EXPECT_EQ(syscalls::handle_NtUserCallHwndLock(c, 0, update_selector), FALSE);
    }

    TEST_F(LegacyUserCallTest, PendingUpdateDispatchesExistingPaintCompletion)
    {
        auto* win = emu.process.windows.get(target);
        win->update_pending = true;
        win->update_rect = RECT{0, 0, 80, 40};
        const auto c = context();
        EXPECT_EQ(syscalls::handle_NtUserCallHwndLock(c, target, update_selector), FALSE);
        ASSERT_TRUE(c.run_callback);
        ASSERT_EQ(c.thread().callback_stack.size(), 1u);
        auto& callback = c.thread().callback_stack.back();
        EXPECT_EQ(callback.handler_id, callback_id::NtUserUpdateWindow);
        auto* state = dynamic_cast<window_update_state*>(callback.state.get());
        ASSERT_NE(state, nullptr);
        ASSERT_EQ(state->pending.size(), 1u);
        EXPECT_EQ(state->pending.back(), target);
        auto completion = context();
        completion.is_callback_completion = true;
        completion.current_completion_state = state;
        EXPECT_EQ(syscalls::completion_NtUserUpdateWindow(completion, target), TRUE);
        EXPECT_TRUE(state->pending.empty());
    }

    TEST_F(LegacyUserCallTest, DispatchRecognitionSurvivesSnapshotWithoutImportMetadata)
    {
        utils::buffer_serializer output{};
        emu.serialize(output);
        utils::buffer_deserializer input{output.get_buffer()};
        emu.deserialize(input);
        ASSERT_TRUE(emu.mod_manager.find_by_name("user32.dll")->imports.empty());
        EXPECT_TRUE(syscalls::is_message_beep_operation(context(), beep_selector));
        EXPECT_TRUE(syscalls::is_update_window_operation(context(), update_selector));
    }
}
