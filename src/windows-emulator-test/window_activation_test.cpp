#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    BOOL handle_NtUserSetForegroundWindow(const syscall_context&, hwnd);
    BOOL handle_NtUserCallHwndLock(const syscall_context&, hwnd, uint32_t);
    BOOL completion_NtUserSetForegroundWindow(const syscall_context&, hwnd);
    hwnd handle_NtUserGetForegroundWindow(const syscall_context&);
    bool is_foreground_window_operation(const syscall_context&, uint32_t);
}

namespace sogen::test
{
    class WindowActivationTest : public testing::Test
    {
      public:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        handle owner{};
        hwnd target{};

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            owner = emu.process.create_thread(emu.memory, emu.mod_manager.executable->entry_point, 0, 0x10000, 0);
            target = create_window(emu.process.threads.get(owner)->id);
        }

        hwnd create_window(const uint32_t thread_id)
        {
            auto [h, win] = emu.process.windows.create(emu.memory);
            win.handle = h.bits;
            win.thread_id = thread_id;
            win.style = WS_POPUP | WS_VISIBLE;
            win.parent_handle = emu.process.default_desktop_window_handle.bits;
            return win.handle;
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        auto& messages()
        {
            return emu.process.threads.get(owner)->message_queue;
        }

        hwnd shared_foreground() const
        {
            return emu.process.user_handles.get_server_info().read().foregroundWindow;
        }
    };

    TEST_F(WindowActivationTest, PublishesForegroundAndQueuesActivationToOwner)
    {
        ASSERT_EQ(syscalls::handle_NtUserSetForegroundWindow(context(), target), TRUE);
        EXPECT_EQ(emu.process.foreground_window, target);
        EXPECT_EQ(shared_foreground(), target);
        EXPECT_EQ(syscalls::handle_NtUserGetForegroundWindow(context()), target);
        ASSERT_EQ(messages().size(), 4u);
        EXPECT_EQ(messages()[0].message, WM_ACTIVATEAPP);
        EXPECT_EQ(messages()[1].message, WM_NCACTIVATE);
        EXPECT_EQ(messages()[2].message, WM_ACTIVATE);
        EXPECT_EQ(messages()[2].wParam, WA_ACTIVE);
        EXPECT_EQ(messages()[3].message, WM_SETFOCUS);
        for (const auto& message : messages())
        {
            EXPECT_EQ(message.window, target);
        }
        EXPECT_TRUE(emu.vcpu(0).active_thread->callback_stack.empty());
    }

    TEST_F(WindowActivationTest, RepeatedActivationDoesNotRepeatMessages)
    {
        ASSERT_EQ(syscalls::handle_NtUserSetForegroundWindow(context(), target), TRUE);
        messages().clear();
        emu.process.user_handles.get_server_info().access([](USER_SERVERINFO& info) { info.foregroundWindow = 0; });
        ASSERT_EQ(syscalls::handle_NtUserSetForegroundWindow(context(), target), TRUE);
        EXPECT_TRUE(messages().empty());
        EXPECT_EQ(shared_foreground(), target);
    }

    TEST_F(WindowActivationTest, RejectsInvalidDisabledChildAndMessageOnlyWindows)
    {
        const auto initial = shared_foreground();
        EXPECT_EQ(syscalls::handle_NtUserSetForegroundWindow(context(), 0), FALSE);
        EXPECT_EQ(syscalls::handle_NtUserSetForegroundWindow(context(), 0x12345678), FALSE);
        auto* win = emu.process.windows.get(target);
        for (const auto style : std::array<uint32_t, 2>{WS_POPUP | WS_DISABLED, WS_CHILD})
        {
            win->style = style;
            EXPECT_EQ(syscalls::handle_NtUserSetForegroundWindow(context(), target), FALSE);
        }
        win->style = WS_POPUP;
        win->message_only = true;
        EXPECT_EQ(syscalls::handle_NtUserSetForegroundWindow(context(), target), FALSE);
        EXPECT_EQ(emu.process.foreground_window, 0u);
        EXPECT_EQ(shared_foreground(), initial);
        EXPECT_TRUE(messages().empty());
    }

    TEST_F(WindowActivationTest, DeactivatesPreviousWindowBeforeActivatingNext)
    {
        ASSERT_EQ(syscalls::handle_NtUserSetForegroundWindow(context(), target), TRUE);
        messages().clear();
        const auto next = create_window(emu.process.threads.get(owner)->id);
        ASSERT_EQ(syscalls::handle_NtUserSetForegroundWindow(context(), next), TRUE);
        ASSERT_EQ(messages().size(), 6u);
        EXPECT_EQ(messages()[0].window, target);
        EXPECT_EQ(messages()[0].message, WM_NCACTIVATE);
        EXPECT_EQ(messages()[0].wParam, FALSE);
        EXPECT_EQ(messages()[1].message, WM_ACTIVATE);
        EXPECT_EQ(messages()[1].wParam, WA_INACTIVE);
        EXPECT_EQ(messages()[1].lParam, next);
        EXPECT_EQ(messages()[2].message, WM_KILLFOCUS);
        EXPECT_EQ(messages()[3].window, next);
        EXPECT_EQ(messages()[4].message, WM_ACTIVATE);
        EXPECT_EQ(messages()[4].lParam, target);
        EXPECT_EQ(messages()[5].message, WM_SETFOCUS);
    }

    TEST_F(WindowActivationTest, MinimizedWindowReceivesActivationWithoutFocus)
    {
        emu.process.windows.get(target)->style |= WS_MINIMIZE;
        ASSERT_EQ(syscalls::handle_NtUserSetForegroundWindow(context(), target), TRUE);
        ASSERT_EQ(messages().size(), 3u);
        EXPECT_EQ(messages().back().message, WM_ACTIVATE);
        EXPECT_EQ(messages().back().wParam, 0x10001u);
    }

    TEST_F(WindowActivationTest, ForegroundAndQueuedMessagesSurviveSnapshot)
    {
        ASSERT_EQ(syscalls::handle_NtUserSetForegroundWindow(context(), target), TRUE);
        utils::buffer_serializer output{};
        emu.serialize(output);
        emu.process.foreground_window = 0;
        messages().clear();
        utils::buffer_deserializer input{output.get_buffer()};
        emu.deserialize(input);
        EXPECT_EQ(emu.process.foreground_window, target);
        EXPECT_EQ(shared_foreground(), target);
        EXPECT_EQ(messages().size(), 4u);
    }

    TEST_F(WindowActivationTest, SameThreadCallbackAndRemainingMessagesSurviveSnapshot)
    {
        emu.process.windows.get(target)->thread_id = emu.vcpu(0).active_thread->id;
        const auto stack = emu.memory.allocate_memory(0x10000, memory_permission::read_write);
        emu.emu().reg(x86_register::rsp, stack + 0xff00);
        const auto c = context();
        EXPECT_EQ(syscalls::handle_NtUserSetForegroundWindow(c, target), FALSE);
        ASSERT_TRUE(c.run_callback);
        ASSERT_EQ(c.thread().callback_stack.size(), 1u);
        EXPECT_EQ(c.thread().callback_stack.back().handler_id, callback_id::NtUserSetForegroundWindow);
        utils::buffer_serializer output{};
        emu.serialize(output);
        utils::buffer_deserializer input{output.get_buffer()};
        emu.deserialize(input);
        auto& frame = emu.vcpu(0).active_thread->callback_stack.back();
        auto* state = dynamic_cast<window_activation_state*>(frame.state.get());
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->target, target);
        ASSERT_EQ(state->messages.size(), 3u);
        EXPECT_EQ(state->messages.back().message, WM_NCACTIVATE);
        emu.process.foreground_window = 0;
        auto completion = context();
        completion.is_callback_completion = true;
        completion.current_completion_state = state;
        EXPECT_EQ(syscalls::completion_NtUserSetForegroundWindow(completion, target), FALSE);
        EXPECT_FALSE(completion.run_callback);
    }

    TEST_F(WindowActivationTest, ResolvesSelectorFromGuestWrapperAndRejectsOtherOperations)
    {
        const auto* module = emu.mod_manager.map_module_or_throw(R"(c:\windows\system32\user32.dll)", emu.log);
        ASSERT_NE(module, nullptr);
        const auto address = module->find_export("SetForegroundWindow");
        ASSERT_NE(address, 0u);
        ASSERT_EQ(emu.emu().read_memory<uint8_t>(address), 0xba);
        const auto selector = emu.emu().read_memory<uint32_t>(address + 1);
        const auto* win32u = emu.mod_manager.map_module_or_throw(R"(c:\windows\system32\win32u.dll)", emu.log);
        const auto syscall = win32u->find_export("NtUserCallHwndLock");
        ASSERT_NE(syscall, 0u);
        const auto displacement = emu.emu().read_memory<int32_t>(address + 8);
        const auto slot = address + 12 + static_cast<uint64_t>(displacement);
        emu.emu().write_memory<uint64_t>(slot, syscall + 1);
        EXPECT_FALSE(syscalls::is_foreground_window_operation(context(), selector));
        emu.emu().write_memory<uint64_t>(slot, syscall);
        EXPECT_TRUE(syscalls::is_foreground_window_operation(context(), selector));
        utils::buffer_serializer output{};
        emu.serialize(output);
        utils::buffer_deserializer input{output.get_buffer()};
        emu.deserialize(input);
        EXPECT_TRUE(emu.mod_manager.find_by_name("user32.dll")->imports.empty());
        EXPECT_TRUE(syscalls::is_foreground_window_operation(context(), selector));
        EXPECT_FALSE(syscalls::is_foreground_window_operation(context(), selector + 1));
        ASSERT_EQ(syscalls::handle_NtUserCallHwndLock(context(), target, selector), TRUE);
        EXPECT_EQ(shared_foreground(), target);
    }
}
