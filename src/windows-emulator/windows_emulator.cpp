#include "std_include.hpp"
#include "windows_emulator.hpp"
#include "guest_thread_affinity.hpp"
#include "scheduler_vm_gate.hpp"
#include "telemetry_shared_memory.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cpu_context.hpp"

#include <utils/io.hpp>
#include <utils/timer.hpp>
#include <utils/finally.hpp>
#include <utils/lazy_object.hpp>

#include "exception_dispatch.hpp"
#include "apiset/apiset.hpp"
#include "syscall_dispatcher.hpp"

#include "network/static_socket_factory.hpp"
#include "memory_permission_ext.hpp"

namespace sogen
{
    constexpr auto MAX_INSTRUCTIONS_PER_TIME_SLICE = 0x20000;
    constexpr auto MAX_BASIC_BLOCKS_PER_TIME_SLICE = 0x8000;

    namespace
    {
        enum class worker_lock_phase : uint8_t
        {
            none,
            worker_loop,
            context_switch_work,
            selection_restore,
            idle_maintenance,
            sync_worker_context,
            activity_publication,
        };

        struct worker_phase_state
        {
            std::atomic<uint64_t> value{0};
        };

        thread_local worker_phase_state* current_worker_phase = nullptr;

        void set_worker_lock_phase(const worker_lock_phase phase)
        {
            if (current_worker_phase)
            {
                const auto timestamp = static_cast<uint64_t>(GetTickCount64());
                current_worker_phase->value.store((timestamp << 8) | static_cast<uint8_t>(phase), std::memory_order_relaxed);
            }
        }

        const char* worker_lock_phase_name(const worker_lock_phase phase)
        {
            switch (phase)
            {
            case worker_lock_phase::worker_loop:
                return "worker_loop";
            case worker_lock_phase::context_switch_work:
                return "context_switch_work";
            case worker_lock_phase::selection_restore:
                return "selection_restore";
            case worker_lock_phase::idle_maintenance:
                return "idle_maintenance";
            case worker_lock_phase::sync_worker_context:
                return "sync_worker_context";
            case worker_lock_phase::activity_publication:
                return "activity_publication";
            default:
                return "none";
            }
        }

        bool scheduler_profiling_enabled()
        {
            static const bool enabled = [] {
                const auto* value = std::getenv("SOGEN_SCHEDULER_PROFILE");
                return value && std::strcmp(value, "1") == 0;
            }();
            return enabled;
        }

        bool guest_thread_affinity_enabled()
        {
            static const bool enabled = [] {
                const auto* value = std::getenv("SOGEN_SMP_GUEST_THREAD_AFFINITY");
                return value && std::strcmp(value, "1") == 0;
            }();
            return enabled;
        }

        bool is_vcruntime_throw_module(const std::string_view name)
        {
            constexpr std::string_view prefix = "vcruntime";
            constexpr std::string_view suffix = ".dll";
            if (name.size() < prefix.size() + suffix.size())
            {
                return false;
            }
            for (size_t i = 0; i < name.size(); ++i)
            {
                const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
                if (i < prefix.size() && lower != prefix[i])
                {
                    return false;
                }
                if (i >= name.size() - suffix.size() && lower != suffix[i - (name.size() - suffix.size())])
                {
                    return false;
                }
            }
            return true;
        }

        bool is_d3d11_throw_module(const std::string_view name)
        {
            constexpr std::string_view expected = "d3d11.dll";
            if (name.size() != expected.size())
            {
                return false;
            }
            for (size_t i = 0; i < name.size(); ++i)
            {
                if (static_cast<char>(std::tolower(static_cast<unsigned char>(name[i]))) != expected[i])
                {
                    return false;
                }
            }
            return true;
        }

        template <size_t N>
        uint32_t capture_guest_hex(x86_64_cpu& cpu, const uint64_t address, std::array<char, N>& output)
        {
            static_assert(N > 1 && (N - 1) % 2 == 0);
            constexpr char digits[] = "0123456789abcdef";
            uint32_t readable = 0;
            for (size_t i = 0; i < (N - 1) / 2; ++i)
            {
                uint8_t byte{};
                const bool ok = address != 0 && address <= UINT64_MAX - i &&
                                cpu.try_read_memory(address + i, &byte, sizeof(byte));
                output[i * 2] = ok ? digits[byte >> 4] : '?';
                output[i * 2 + 1] = ok ? digits[byte & 15] : '?';
                readable += ok;
            }
            output[N - 1] = '\0';
            return readable;
        }

        // Register reads and exception construction materialize temporary objects. Keep them out of
        // the instruction observer's common path without weakening its callback or error handling.
        NO_INLINE void capture_callback_return(vcpu_context& vcpu, emulator_thread& thread)
        {
            thread.callback_return_rax = vcpu.cpu.reg<uint64_t>(x86_register::rax);
        }

        [[noreturn]] NO_INLINE void throw_missing_execution_thread()
        {
            throw std::runtime_error("No active thread!");
        }

        void adjust_working_directory(application_settings& app_settings)
        {
            if (!app_settings.working_directory.empty())
            {
                // Do nothing
            }
#ifdef OS_WINDOWS
            else if (app_settings.application.is_relative())
            {
                app_settings.working_directory = std::filesystem::current_path();
            }
#endif
            else
            {
                app_settings.working_directory = app_settings.application.parent();
            }
        }

        void adjust_application(application_settings& app_settings)
        {
            if (app_settings.application.is_relative())
            {
                app_settings.application = app_settings.working_directory / app_settings.application;
            }
        }

        void fixup_application_settings(application_settings& app_settings)
        {
            adjust_working_directory(app_settings);
            adjust_application(app_settings);
        }

        int16_t point_x(const uint64_t lparam)
        {
            return static_cast<int16_t>(lparam & 0xFFFF);
        }

        int16_t point_y(const uint64_t lparam)
        {
            return static_cast<int16_t>((lparam >> 16) & 0xFFFF);
        }

        uint16_t high_word(const uint64_t value)
        {
            return static_cast<uint16_t>((value >> 16) & 0xFFFFu);
        }

        struct child_hit_test_result
        {
            const window* win{};
            int x{};
            int y{};
        };

        std::optional<child_hit_test_result> find_child_window_at(const process_context& process, const hwnd parent, const int x,
                                                                  const int y)
        {
            std::optional<child_hit_test_result> result{};
            for (const auto& [index, child] : process.windows)
            {
                (void)index;
                if (child.parent_handle != parent || (child.style & WS_VISIBLE) == 0 || (child.style & WS_DISABLED) != 0)
                {
                    continue;
                }

                if (x >= child.x && x < child.x + child.width && y >= child.y && y < child.y + child.height)
                {
                    const auto child_x = x - child.x;
                    const auto child_y = y - child.y;
                    if (auto descendant = find_child_window_at(process, child.handle, child_x, child_y))
                    {
                        result = descendant;
                    }
                    else
                    {
                        result = child_hit_test_result{.win = &child, .x = child_x, .y = child_y};
                    }
                }
            }

            return result;
        }

        std::optional<POINT> get_window_origin_relative_to_ancestor(const process_context& process, const hwnd window, const hwnd ancestor)
        {
            POINT origin{};
            auto current_handle = window;
            while (current_handle != 0 && current_handle != ancestor)
            {
                const auto* current = process.windows.get(current_handle);
                if (!current)
                {
                    return std::nullopt;
                }

                origin.x += current->x;
                origin.y += current->y;
                current_handle = current->parent_handle;
            }

            if (current_handle != ancestor)
            {
                return std::nullopt;
            }

            return origin;
        }

        bool is_mouse_wheel_message(const uint32_t message)
        {
            return message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
        }

        bool is_mouse_button_message(const uint32_t message)
        {
            switch (message)
            {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
                return true;
            default:
                return false;
            }
        }

        bool is_mouse_button_down_message(const uint32_t message)
        {
            return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
        }

        bool is_mouse_button_up_message(const uint32_t message)
        {
            return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP;
        }

        bool is_pointer_message(const uint32_t message)
        {
            // All mouse messages go through capture/child hit-testing: while a window holds the mouse
            // capture every mouse message must reach it (so a pressed button still completes its click),
            // and otherwise each is delivered to the child under the cursor (hover, right-click, etc.).
            return message == WM_MOUSEMOVE || is_mouse_button_message(message) || is_mouse_wheel_message(message);
        }

        bool is_key_down_message(const uint32_t message)
        {
            return message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        }

        bool is_key_up_message(const uint32_t message)
        {
            return message == WM_KEYUP || message == WM_SYSKEYUP;
        }

        bool is_keyboard_message(const uint32_t message)
        {
            return is_key_down_message(message) || is_key_up_message(message);
        }

        // Window button message -> RAWMOUSE usButtonFlags transition bit (winuser.h RI_MOUSE_* values).
        uint16_t raw_mouse_button_flags(const uint32_t message, const uint64_t wparam)
        {
            switch (message)
            {
            case WM_LBUTTONDOWN:
                return RI_MOUSE_LEFT_BUTTON_DOWN;
            case WM_LBUTTONUP:
                return RI_MOUSE_LEFT_BUTTON_UP;
            case WM_RBUTTONDOWN:
                return RI_MOUSE_RIGHT_BUTTON_DOWN;
            case WM_RBUTTONUP:
                return RI_MOUSE_RIGHT_BUTTON_UP;
            case WM_MBUTTONDOWN:
                return RI_MOUSE_MIDDLE_BUTTON_DOWN;
            case WM_MBUTTONUP:
                return RI_MOUSE_MIDDLE_BUTTON_UP;
            case WM_XBUTTONDOWN:
                return high_word(wparam) == XBUTTON2 ? RI_MOUSE_BUTTON_5_DOWN : RI_MOUSE_BUTTON_4_DOWN;
            case WM_XBUTTONUP:
                return high_word(wparam) == XBUTTON2 ? RI_MOUSE_BUTTON_5_UP : RI_MOUSE_BUTTON_4_UP;
            case WM_MOUSEWHEEL:
                return RI_MOUSE_WHEEL;
            case WM_MOUSEHWHEEL:
                return RI_MOUSE_HWHEEL;
            default:
                return 0;
            }
        }

        uint16_t raw_mouse_button_data(const uint32_t message, const uint64_t wparam)
        {
            if (is_mouse_wheel_message(message))
            {
                return high_word(wparam);
            }
            return 0;
        }

        uint8_t mouse_button_virtual_key(const uint32_t message, const uint64_t wparam)
        {
            switch (message)
            {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
                return VK_LBUTTON;
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
                return VK_RBUTTON;
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
                return VK_MBUTTON;
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
                return high_word(wparam) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
            default:
                return 0;
            }
        }

        // Best-effort US-layout virtual-key -> PS/2 set-1 scan code for the RAWKEYBOARD MakeCode field
        // (games that bind by scan code need it; the VKey is delivered too for those that use it).
        uint16_t vk_to_scan_code(const uint16_t vk)
        {
            switch (vk)
            {
            case VK_ESCAPE:
                return 0x01;
            case VK_RETURN:
                return 0x1C;
            case VK_SPACE:
                return 0x39;
            case VK_TAB:
                return 0x0F;
            case VK_BACK:
                return 0x0E;
            case VK_SHIFT:
                return 0x2A;
            case VK_CONTROL:
                return 0x1D;
            case VK_UP:
                return 0x48;
            case VK_DOWN:
                return 0x50;
            case VK_LEFT:
                return 0x4B;
            case VK_RIGHT:
                return 0x4D;
            default:
                if (vk >= 'A' && vk <= 'Z')
                {
                    static constexpr std::array<uint8_t, 26> letter_scan = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
                                                                            0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
                                                                            0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
                    return letter_scan[static_cast<size_t>(vk - 'A')];
                }
                if (vk >= '1' && vk <= '9')
                {
                    return static_cast<uint16_t>(0x02 + (vk - '1'));
                }
                if (vk == '0')
                {
                    return 0x0B;
                }
                return 0;
            }
        }

        uint64_t pack_point(const int x, const int y)
        {
            return static_cast<uint16_t>(x) | (static_cast<uint64_t>(static_cast<uint16_t>(y)) << 16);
        }

        struct pointer_target
        {
            hwnd window{};
            int x{};
            int y{};
        };

        // Single authority for routing a top-level-local pointer event to its destination window.
        // Backends only forward (top-level window, top-level-local x/y); capture and child hit-testing
        // are decided here, never in the host backends.
        pointer_target route_pointer(process_context& process, const hwnd top_level, const int x, const int y)
        {
            if (process.mouse_capture_window != 0)
            {
                if (const auto* captured = process.windows.get(process.mouse_capture_window);
                    captured && (captured->style & WS_VISIBLE) != 0)
                {
                    // Capture sends every pointer event to the capturing window, even one reported for
                    // another top-level. Translate via screen coordinates (origin relative to the root)
                    // so it works across top-levels; for a child of top_level this is the same offset.
                    const auto captured_origin = get_window_origin_relative_to_ancestor(process, captured->handle, 0);
                    const auto top_level_origin = get_window_origin_relative_to_ancestor(process, top_level, 0);
                    if (captured_origin && top_level_origin)
                    {
                        const auto screen_x = top_level_origin->x + x;
                        const auto screen_y = top_level_origin->y + y;
                        return {.window = captured->handle, .x = screen_x - captured_origin->x, .y = screen_y - captured_origin->y};
                    }
                }
                else
                {
                    process.mouse_capture_window = 0;
                }

                return {.window = top_level, .x = x, .y = y};
            }

            // Otherwise deliver to the deepest visible/enabled child under the cursor.
            if (const auto child = find_child_window_at(process, top_level, x, y))
            {
                return {.window = child->win->handle, .x = child->x, .y = child->y};
            }

            return {.window = top_level, .x = x, .y = y};
        }

        bool has_pending_host_wait(const process_context& process)
        {
            for (const auto& thread_entry : process.threads)
            {
                if (thread_entry.second.await_host_condition)
                {
                    return true;
                }
            }

            return false;
        }

        vcpu_context* find_vcpu_running_thread(windows_emulator& win_emu, const emulator_thread& thread);

        void perform_context_switch_work(windows_emulator& win_emu, vcpu_context& vcpu)
        {
            const auto profile = scheduler_profiling_enabled();
            const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            auto& threads = win_emu.process.threads;

            for (auto it = threads.begin(); it != threads.end();)
            {
                if (!it->second.is_terminated() || it->second.ref_count > 0)
                {
                    ++it;
                    continue;
                }

                if (auto* running_on = find_vcpu_running_thread(win_emu, it->second))
                {
                    if (running_on != &vcpu)
                    {
                        // Another vCPU still has this thread loaded; it will detach it soon.
                        ++it;
                        continue;
                    }

                    running_on->active_thread = nullptr;
                }

                win_emu.process.thread_handle_events.record({.action = thread_handle_journal::operation::reap,
                                                             .value = threads.make_handle(it->first),
                                                             .target_tid = it->second.id,
                                                             .refs_before = it->second.ref_count,
                                                             .removed = true});
                const auto [new_it, deleted] = threads.erase(it);
                if (!deleted)
                {
                    ++it;
                }
                else
                {
                    it = new_it;
                }
            }

            win_emu.process.directory_notifications.process_completions(win_emu);
            win_emu.process.process_graphics_commands();

            auto& devices = win_emu.process.devices;

            // Crappy mechanism to prevent mutation while iterating.
            const auto was_blocked = devices.block_mutation(true);
            const auto _ = utils::finally([&] { devices.block_mutation(was_blocked); });

            const auto device_start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            for (auto& dev : devices | std::views::values)
            {
                dev.work(win_emu);
            }
            if (profile)
            {
                const auto end = std::chrono::steady_clock::now();
                auto& stats = vcpu.scheduler_profile;
                ++stats.context_switch_calls;
                stats.context_switch_nanos +=
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                ++stats.device_work_calls;
                stats.device_work_nanos +=
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - device_start).count());
            }
        }

        emulator_thread* get_thread_by_id(process_context& process, const uint32_t id)
        {
            for (auto& t : process.threads | std::views::values)
            {
                if (t.id == id)
                {
                    return &t;
                }
            }

            return nullptr;
        }

        void dispatch_next_apc(windows_emulator& win_emu, vcpu_context& vcpu, emulator_thread& thread)
        {
            assert(vcpu.active_thread == &thread);

            auto& emu = vcpu.cpu;
            auto& apcs = thread.pending_apcs;
            if (apcs.empty())
            {
                return;
            }

            thread.setup_if_necessary(vcpu.cpu, win_emu.process);

            win_emu.callbacks.on_generic_activity("APC Dispatch");

            const auto next_apx = apcs.front();
            apcs.erase(apcs.begin());

            if (next_apx.restamp_io_status_block && next_apx.apc_argument2)
            {
                // Stamp the WoW64 32-bit I/O status block (Status @0, Information @4) here, immediately
                // before the completion routine runs. This mirrors the real kernel, which writes the
                // IO_STATUS_BLOCK as it dispatches the completion APC rather than when the I/O is first
                // queued -- so any value the guest left in that buffer while the async request was pending
                // (e.g. reusing it for an intervening synchronous call) is correctly superseded.
                const auto status32 = static_cast<uint32_t>(next_apx.io_status);
                emu.write_memory(next_apx.apc_argument2, &status32, sizeof(status32));
                emu.write_memory(next_apx.apc_argument2 + sizeof(status32), &next_apx.io_information, sizeof(next_apx.io_information));
            }

            struct
            {
                CONTEXT64 context{};
                CONTEXT_EX context_ex{};
                KCONTINUE_ARGUMENT continue_argument{};
            } stack_layout;

            static_assert(offsetof(decltype(stack_layout), continue_argument) == 0x4F0);

            stack_layout.context.P1Home = next_apx.apc_argument1;
            stack_layout.context.P2Home = next_apx.apc_argument2;
            stack_layout.context.P3Home = next_apx.apc_argument3;
            stack_layout.context.P4Home = next_apx.apc_routine;

            stack_layout.continue_argument.ContinueFlags |= KCONTINUE_FLAG_TEST_ALERT;

            auto& ctx = stack_layout.context;
            ctx.ContextFlags = CONTEXT64_ALL;
            cpu_context::save(emu, ctx);

            const auto initial_sp = emu.reg(x86_register::rsp);
            const auto new_sp = align_down(initial_sp - sizeof(stack_layout), 0x100);

            emu.write_memory(new_sp, stack_layout);

            emu.reg(x86_register::rsp, new_sp);
            emu.reg(x86_register::rip, win_emu.process.ki_user_apc_dispatcher);
        }

        vcpu_context* find_vcpu_running_thread(windows_emulator& win_emu, const emulator_thread& thread)
        {
            for (uint32_t i = 0; i < win_emu.vcpu_count(); ++i)
            {
                auto& vcpu = win_emu.vcpu(i);
                if (vcpu.active_thread == &thread)
                {
                    return &vcpu;
                }
            }

            return nullptr;
        }

        bool switch_to_thread(windows_emulator& win_emu, vcpu_context& vcpu, emulator_thread& thread, const bool force = false)
        {
            if (thread.is_terminated())
            {
                return false;
            }

            // A thread that is loaded on another vCPU can only run there.
            if (auto* running_on = find_vcpu_running_thread(win_emu, thread); running_on && running_on != &vcpu)
            {
                return false;
            }

            auto& emu = vcpu.cpu;
            auto& context = win_emu.process;

            const auto is_ready = thread.is_thread_ready(win_emu);
            const auto has_pending_status = thread.pending_status.has_value();
            const auto can_dispatch_apcs = thread.apc_alertable && !thread.pending_apcs.empty();

            if (!is_ready && !force && !can_dispatch_apcs)
            {
                return false;
            }

            auto* active_thread = vcpu.active_thread;

            if (active_thread != &thread)
            {
                if (active_thread)
                {
                    win_emu.callbacks.on_thread_switch(*active_thread, thread);
                    active_thread->save(emu);
                }

                vcpu.active_thread = &thread;

                thread.restore(emu);
            }

            thread.setup_if_necessary(emu, context);

            if (can_dispatch_apcs && !has_pending_status)
            {
                thread.mark_as_ready(STATUS_USER_APC);
                dispatch_next_apc(win_emu, vcpu, thread);
            }

            thread.apc_alertable = false;
            if (guest_thread_affinity_enabled())
            {
                thread.last_vcpu = static_cast<uint32_t>(vcpu.cpu.index());
                thread.affinity_deferred_once = false;
            }
            return true;
        }

        bool switch_to_thread(windows_emulator& win_emu, vcpu_context& vcpu, const handle thread_handle)
        {
            auto* thread = win_emu.process.threads.get(thread_handle);
            if (!thread)
            {
                throw std::runtime_error("Bad thread handle");
            }

            return switch_to_thread(win_emu, vcpu, *thread);
        }

        bool switch_to_next_thread(windows_emulator& win_emu, vcpu_context& vcpu)
        {
            set_worker_lock_phase(worker_lock_phase::context_switch_work);
            perform_context_switch_work(win_emu, vcpu);
            set_worker_lock_phase(worker_lock_phase::selection_restore);

            auto& context = win_emu.process;
            return detail::select_next_guest_thread(
                context.threads | std::views::values, vcpu.active_thread, static_cast<uint32_t>(vcpu.cpu.index()),
                guest_thread_affinity_enabled() && win_emu.vcpu_count() > 1,
                [&](emulator_thread& thread) { return switch_to_thread(win_emu, vcpu, thread); });
        }

        struct instruction_tick_clock : utils::tick_clock
        {
            const uint64_t* instructions_{};

            instruction_tick_clock(const uint64_t& instructions, const system_time_point system_start = {},
                                   const steady_time_point steady_start = {})
                : tick_clock(1000, system_start, steady_start),
                  instructions_(&instructions)
            {
            }

            uint64_t ticks() override
            {
                return *this->instructions_;
            }
        };

        // LEAN timing fix: with per-instruction hooks off (fast path) but N>1 forcing wall-clock
        // mode, the guest's QPC/InterruptTime/system-elapsed read REAL host time. The emulated
        // CPU then appears to do hours of work in seconds - Authenticode/anti-tamper timing
        // checks in the loader (wintrust trail: msvcrt __C_specific_handler loop + wintrust
        // internals -> STATUS_DLL_INIT_FAILED) fail on exactly that. With hooks ON the same
        // code passes because the run is 5-13x slower. Drive the guest-visible clocks from the
        // retired-instruction count instead: 1 instruction = 1ns ("a 1 GHz CPU"), so elapsed
        // time tracks work done. System DATE stays anchored to the real clock at boot. Only
        // used when the backend always knows the icount (icicle lean paths).
        class icount_clock : public utils::clock
        {
          public:
            icount_clock(const x86_64_emulator& emu)
                : emu_(emu),
                  base_instructions_(emu.executed_instructions_total()),
                  boot_system_(std::chrono::system_clock::now()),
                  boot_steady_(std::chrono::steady_clock::now())
            {
            }

            system_time_point system_now() override
            {
                return this->boot_system_ +
                       std::chrono::duration_cast<system_duration>(std::chrono::nanoseconds(this->elapsed_instructions()));
            }

            steady_time_point steady_now() override
            {
                return this->boot_steady_ + std::chrono::nanoseconds(this->elapsed_instructions());
            }

            uint64_t timestamp_counter() override
            {
                // Mirror the dedicated virtual TSC (timestamp_counter_for_guest) so QPC-vs-TSC
                // comparisons stay coherent: cycles == instructions on both.
                return virtual_tsc_origin + this->elapsed_instructions();
            }

          private:
            uint64_t elapsed_instructions() const
            {
                const auto now = this->emu_.executed_instructions_total();
                return now > this->base_instructions_ ? now - this->base_instructions_ : 0;
            }

            static constexpr uint64_t virtual_tsc_origin = 0x0000'0100'0000'0000ULL;

            const x86_64_emulator& emu_;
            uint64_t base_instructions_;
            system_time_point boot_system_;
            steady_time_point boot_steady_;
        };

        std::unique_ptr<utils::clock> get_clock(emulator_interfaces& interfaces, const uint64_t& instructions,
                                                const bool use_relative_time);

        std::unique_ptr<utils::clock> make_guest_clock(const x86_64_emulator& emu, emulator_interfaces& interfaces,
                                                       const uint64_t& instructions, const bool use_relative_time)
        {
            auto base = get_clock(interfaces, instructions, use_relative_time);
            // Opt-in only (SOGEN_ICOUNT_CLOCK=1): a pure icount clock FREEZES while a guest
            // thread sleeps/waits (no instructions retire), so wait deadlines never arrive and
            // the sample's thread joins deadlock. Falsified as the lean-crash fix anyway.
            const bool icount_clock_requested =
                [] { const char* v = std::getenv("SOGEN_ICOUNT_CLOCK"); return v && *v == '1'; }();
            if (use_relative_time || !emu.has_deterministic_instruction_count() || !icount_clock_requested)
            {
                return base;
            }
            return std::make_unique<icount_clock>(emu);
        }

        std::unique_ptr<utils::clock> get_clock(emulator_interfaces& interfaces, const uint64_t& instructions, const bool use_relative_time)
        {
            if (interfaces.clock)
            {
                return std::move(interfaces.clock);
            }

            if (use_relative_time)
            {
                return std::make_unique<instruction_tick_clock>(instructions);
            }

            return std::make_unique<utils::clock>();
        }

        std::unique_ptr<network::dns_lookup> get_dns_lookup(emulator_interfaces& interfaces)
        {
            if (interfaces.dns_lookup)
            {
                return std::move(interfaces.dns_lookup);
            }

            return std::make_unique<network::dns_lookup>();
        }

        std::unique_ptr<network::socket_factory> get_socket_factory(emulator_interfaces& interfaces)
        {
            if (interfaces.socket_factory)
            {
                return std::move(interfaces.socket_factory);
            }

#ifdef OS_EMSCRIPTEN
            return network::create_static_socket_factory();
#else
            return std::make_unique<network::socket_factory>();
#endif
        }

        std::unique_ptr<ui_backend> get_ui_backend(emulator_interfaces& interfaces)
        {
            if (interfaces.ui)
            {
                return std::move(interfaces.ui);
            }

            return create_default_ui_backend();
        }

        std::unique_ptr<audio_backend> get_audio_backend(emulator_interfaces& interfaces)
        {
            if (interfaces.audio)
            {
                return std::move(interfaces.audio);
            }

            return create_default_audio_backend();
        }

        // The guest must see at least as many logical processors as there are vCPUs, otherwise a
        // thread running on a higher-indexed vCPU would report a processor number the guest
        // considers out of range. The configured fake value still wins when it is larger (e.g. the
        // anti-analysis default of 4 with a single vCPU).
        fake_environment_config effective_fake_env(const emulator_settings& settings, const uint32_t vcpu_count)
        {
            auto fake_env = settings.fake_env;
            fake_env.number_of_processors = std::max(fake_env.number_of_processors, vcpu_count);
            return fake_env;
        }
    }

    windows_emulator::windows_emulator(std::unique_ptr<x86_64_emulator> emu, application_settings app_settings,
                                       const emulator_settings& settings, emulator_callbacks callbacks, emulator_interfaces interfaces)
        : windows_emulator(std::move(emu), settings, std::move(callbacks), std::move(interfaces))
    {
        fixup_application_settings(app_settings);
        this->application_settings_ = std::move(app_settings);
    }

    windows_emulator::windows_emulator(std::unique_ptr<x86_64_emulator> emu, const emulator_settings& settings,
                                       emulator_callbacks callbacks, emulator_interfaces interfaces)
        : emu_(std::move(emu)),
          clock_(make_guest_clock(*this->emu_, interfaces, this->executed_instructions_, settings.use_relative_time)),
          dns_lookup_(get_dns_lookup(interfaces)),
          socket_factory_(get_socket_factory(interfaces)),
          ui_backend_(get_ui_backend(interfaces)),
          audio_backend_(get_audio_backend(interfaces)),
          emulation_root{settings.emulation_root.empty() ? settings.emulation_root : absolute(settings.emulation_root)},
          fake_env(effective_fake_env(settings, static_cast<uint32_t>(this->emu_->vcpu_count()))),
          callbacks(std::move(callbacks)),
          file_sys(emulation_root.empty() ? emulation_root : emulation_root / "filesys"),
          memory(*this->emu_),
          registry(settings.load_registry
                       ? registry_manager{emulation_root.empty() ? settings.registry_directory : emulation_root / "registry"}
                       : registry_manager{}),
          mod_manager(memory, file_sys, this->callbacks),
          process(*this->emu_, memory, *this->clock_, this->callbacks),
          use_relative_time_(settings.use_relative_time),
          instruction_precision_(settings.use_instruction_precision && this->emu_->supports_instruction_counting()),
          vcpu_count_(static_cast<uint32_t>(this->emu_->vcpu_count())),
          use_section_first_execution_hooks_(!this->emu_->supports_global_memory_execution_hooks())
    {
        if (this->vcpu_count_ == 0)
        {
            throw std::invalid_argument("At least one vCPU is required");
        }

        if (this->vcpu_count_ > 1 && !this->emu_->supports_multiple_vcpus())
        {
            throw std::invalid_argument("The " + this->emu_->get_name() + " backend does not support multiple vCPUs");
        }

        if (this->vcpu_count_ > 1)
        {
            // Deliberate hard errors instead of silent clamping (docs/multi-vcpu-design.md).
            if (this->instruction_precision_)
            {
                throw std::invalid_argument("Instruction precision requires a single vCPU");
            }

            if (this->use_relative_time_)
            {
                throw std::invalid_argument("Relative time requires a single vCPU");
            }
        }

        this->vcpus_.reserve(this->vcpu_count_);
        for (uint32_t i = 0; i < this->vcpu_count_; ++i)
        {
            this->vcpus_.push_back(std::make_unique<vcpu_context>(this->emu_->get_cpu(i)));
        }

        this->ui_backend_->set_event_sink([this](const ui_event& event) { this->handle_ui_event(event); });
#ifndef OS_WINDOWS
        if (this->emulation_root.empty())
        {
            throw std::runtime_error("Emulation root directory can not be empty!");
        }
#endif

        for (const auto& mapping : settings.path_mappings)
        {
            this->file_sys.map(mapping.first, mapping.second);
        }

        for (const auto& mapping : settings.port_mappings)
        {
            this->map_port(mapping.first, mapping.second);
        }

        this->setup_hooks();
    }

    windows_emulator::~windows_emulator()
    {
        if (this->ui().native_presentation_active())
        {
            for (auto& [id, thread] : this->process.threads)
            {
                (void)id;
                thread.await_host_condition = {};
            }
            this->process.devices = {};
            this->ui().drain_native_shutdown();
            if (this->ui().native_presentation_quarantined())
            {
                auto* const quarantined_backend = this->ui_backend_.release();
                this->log.warn("Native Vulkan presentation completion is unresolved; retaining host and hidden HWND ownership at %p until "
                               "process exit\n",
                               static_cast<void*>(quarantined_backend));
            }
        }
    }

    void windows_emulator::setup_process_if_necessary()
    {
        if (this->setup_completed_)
        {
            return;
        }

        this->setup_completed_ = true;

        this->setup_process();
    }

    void windows_emulator::setup_process()
    {
        const auto& emu = this->emu();
        auto& context = this->process;

        this->version.load_from_registry(this->registry, this->log);

        this->mod_manager.map_main_modules(this->emu(), this->application_settings_.application, this->version, context, this->log);
        this->install_section_first_execution_hooks();

        const auto* executable = this->mod_manager.executable;
        // A narrow, opt-in diagnostic for the Destiny 2 /GS failure at image RVA 0x187d164.
        // These exact-address hooks preserve the guest instruction and do not enable broad
        // per-instruction analysis. The RVAs belong to the 21122.0.0.0 Shadowkeep image.
        if (const char* probe = std::getenv("SOGEN_DESTINY_GS_COOKIE_PROBE"); probe && *probe == '1' &&
            executable && executable->name == "destiny2.exe")
        {
            constexpr std::array<uint64_t, 3> sites{0x3a52a9, 0x3b41ca, 0x187c480};
            constexpr uint64_t cookie_rva = 0x20a9a88;
            if (executable->size_of_image > cookie_rva + sizeof(uint64_t))
            {
                const auto base = executable->image_base;
                // Five fixed routes: both callers, both known checker returns, and other
                // checker returns. Keep separate tid 12 samples so another guest thread
                // cannot hide its first passing check; cap output at 30 records.
                auto samples = std::make_shared<std::array<std::array<bool, 3>, 10>>();
                for (size_t site = 0; site < sites.size(); ++site)
                {
                    const auto address = base + sites[site];
                    this->emu().hook_memory_execution(address, [this, base, samples, site](cpu_interface& cpu, uint64_t rip) {
                        const std::scoped_lock lock(this->kernel_lock_);
                        auto& vcpu = this->vcpu(cpu.index());
                        const auto tid = vcpu.active_thread ? vcpu.active_thread->id : 0;
                        const auto rcx = vcpu.cpu.reg<uint64_t>(x86_register::rcx);
                        const auto rsp = vcpu.cpu.reg<uint64_t>(x86_register::rsp);
                        uint64_t expected_cookie{};
                        uint64_t stack_word{};
                        const bool cookie_read = vcpu.cpu.try_read_memory(base + cookie_rva, &expected_cookie, sizeof(expected_cookie));
                        const bool stack_read = vcpu.cpu.try_read_memory(rsp, &stack_word, sizeof(stack_word));
                        const bool mismatch = cookie_read && rcx != expected_cookie;
                        const bool upper16_nonzero = (rcx >> 48) != 0;
                        size_t route = site;
                        if (site == 2)
                        {
                            route = stack_read && stack_word == base + 0x3a52ae ? 2 :
                                    stack_read && stack_word == base + 0x3b41cf ? 3 : 4;
                        }
                        const size_t kind = !cookie_read ? 2 : mismatch ? 1 : 0;
                        const size_t bucket = route * 2 + (tid == 12 ? 1 : 0);
                        if ((*samples)[bucket][kind]) return;
                        (*samples)[bucket][kind] = true;
                        this->log.error("GSCOOKIE site=%s route=%zu tid=%u vcpu=%zu rip=%#llx rsp=%#llx rcx=%#llx expected=%#llx stack0=%#llx cookie_read=%u stack_read=%u mismatch=%u upper16_nonzero=%u\n",
                                        site == 0 ? "caller_new" : site == 1 ? "caller_old" : "checker",
                                        route, tid, cpu.index(),
                                        (unsigned long long)rip, (unsigned long long)rsp,
                                        (unsigned long long)rcx, (unsigned long long)expected_cookie,
                                        (unsigned long long)stack_word, cookie_read, stack_read, mismatch, upper16_nonzero);
                    });
                }
            }
        }
        const auto* ntdll = this->mod_manager.ntdll;
        const auto* win32u = this->mod_manager.win32u;

        this->memory.initialize_aslr_policy((executable->dll_characteristics & IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA) != 0,
                                            context.is_wow64_process, this->uses_relative_time());
        const auto apiset_data = apiset::obtain(this->emulation_root);

        this->process.setup(*this, this->application_settings_, *executable, *ntdll, apiset_data, this->mod_manager.wow64_modules_.ntdll32);
        this->configure_xstate();

        const auto ntdll_data = emu.read_memory(ntdll->image_base, static_cast<size_t>(ntdll->size_of_image));
        const auto win32u_data = emu.read_memory(win32u->image_base, static_cast<size_t>(win32u->size_of_image));

        this->dispatcher.setup(ntdll->exports, ntdll_data, win32u->exports, win32u_data);

        const auto main_thread_id = context.create_thread(this->memory, this->mod_manager.executable->entry_point, 0,
                                                          this->mod_manager.executable->size_of_stack_reserve, 0, true);

        switch_to_thread(*this, this->vcpu(0), main_thread_id);
    }

    void windows_emulator::yield_thread(vcpu_context& vcpu, const bool alertable)
    {
        this->kernel_lock_.assert_held();

        vcpu.switch_thread = true;
        vcpu.thread().apc_alertable = alertable;
        vcpu.cpu.stop();
    }

    bool windows_emulator::perform_thread_switch(vcpu_context& vcpu)
    {
        std::unique_lock lock(this->kernel_lock_);
        return this->perform_thread_switch(vcpu, lock);
    }

    bool windows_emulator::perform_thread_switch(vcpu_context& vcpu, std::unique_lock<kernel_lock>& lock)
    {
        this->kernel_lock_.assert_held();

        const auto needed_switch = vcpu.switch_thread.exchange(false);
        vcpu.cpu.acknowledge_stop();

        while (!switch_to_next_thread(*this, vcpu))
        {
            if (scheduler_profiling_enabled())
            {
                ++vcpu.scheduler_profile.idle_retries;
            }
            if (this->vcpu_count_ > 1 && vcpu.active_thread)
            {
                // Nothing runnable for this vCPU: detach the stale thread so another
                // vCPU can pick it up once it becomes ready.
                vcpu.active_thread->save(vcpu.cpu);
                vcpu.active_thread = nullptr;
            }

            const auto host_wait_pending = has_pending_host_wait(this->process);

            // Idle: nothing is ready. Release the kernel lock while pumping UI events
            // and sleeping so other threads (UI delivery, vCPU workers) can run.
            // SMP 6.7 RC#2: draining OUR OWN queued cross-vm ops here is the missing drain
            // trigger - it lets a thread-visibility gate (is_thread_ready) make progress even
            // when no thread is runnable, instead of parking with an undrained queue forever.
            // Index-keyed: a worker without its first quantum yet has no thread-local.
            set_worker_lock_phase(worker_lock_phase::sync_worker_context);
            this->emu().sync_worker_context(vcpu.cpu.index());
            set_worker_lock_phase(worker_lock_phase::activity_publication);
            this->publish_activity_status();
            set_worker_lock_phase(worker_lock_phase::idle_maintenance);

            // GATEDIAG (rate-limited): if a thread-visibility gate stays closed for seconds,
            // dump its mark plus the backend's per-queue/vCPU state so the stuck queue is named.
            {
                static thread_local std::chrono::steady_clock::time_point last_gate_diag{};
                const auto now = std::chrono::steady_clock::now();
                if (now - last_gate_diag > std::chrono::seconds(2))
                {
                    last_gate_diag = now;
                    for (const auto& thread : this->process.threads | std::views::values)
                    {
                        if (thread.smp_visibility_mark != 0 && !this->emu().smp_op_applied(thread.smp_visibility_mark))
                        {
                            this->log.error("GATEDIAG tid=%u mark=%llu stuck: %s\n", thread.id,
                                            static_cast<unsigned long long>(thread.smp_visibility_mark),
                                            this->emu().smp_gate_debug().c_str());
                            break;
                        }
                    }
                }
            }
            if (scheduler_profiling_enabled())
            {
                auto& stats = vcpu.scheduler_profile;
                if (this->use_relative_time_)
                {
                    ++stats.idle_relative_ticks;
                }
                else if (host_wait_pending)
                {
                    ++stats.idle_host_yields;
                }
                else
                {
                    ++stats.idle_host_sleeps;
                }
            }
            this->emu().set_scheduler_vm_parked(vcpu.cpu.index(), false);
            set_worker_lock_phase(worker_lock_phase::none);
            lock.unlock();

            if (this->vcpu_count_ == 1)
            {
                // With workers, the thread calling start() pumps UI events instead.
                this->ui_backend_->pump_events();
            }

            if (this->use_relative_time_)
            {
                this->executed_instructions_ += MAX_INSTRUCTIONS_PER_TIME_SLICE;
            }
            else if (host_wait_pending)
            {
                // A host wait (e.g. a GPU semaphore) is parked - re-poll immediately to wake it promptly.
                std::this_thread::yield();
            }
            else
            {
                // Only timed waits remain; nothing host-side wakes sooner than wall-clock, so don't busy-spin.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            acquire_scheduler_vm_parked(this->emu(), vcpu.cpu.index(), lock);
            set_worker_lock_phase(worker_lock_phase::context_switch_work);

            if (this->should_stop)
            {
                vcpu.switch_thread = needed_switch;
                return false;
            }
        }

        return true;
    }

    void windows_emulator::vcpu_worker(vcpu_context& vcpu)
    {
        const kernel_lock::attribution_scope lock_site("vcpu_worker", vcpu.cpu.index());
        this->emu().set_scheduler_worker_context(vcpu.cpu.index(), true);
        const auto clear_scheduler_worker = utils::finally([this, &vcpu] {
            this->emu().set_scheduler_worker_context(vcpu.cpu.index(), false);
        });
        // One line per worker, only when requested. The host TID lets a bounded external
        // sampler attribute CPU use during a status stall without per-call tracing.
        if (const auto* probe = std::getenv("SOGEN_SMP_WORKER_PROBE"); probe && *probe == '1')
        {
            std::fprintf(stderr, "[SMPWORKER] vcpu=%zu host_tid=%lu\n", vcpu.cpu.index(),
                         static_cast<unsigned long>(GetCurrentThreadId()));
        }
        std::unique_lock<kernel_lock> lock(this->kernel_lock_, std::defer_lock);
        acquire_scheduler_vm_parked(this->emu(), vcpu.cpu.index(), lock);
        set_worker_lock_phase(worker_lock_phase::worker_loop);
        const auto clear_parked_vm = utils::finally([this, &vcpu] { this->emu().set_scheduler_vm_parked(vcpu.cpu.index(), false); });

        while (!this->should_stop)
        {
            if (vcpu.switch_thread || !vcpu.active_thread || !vcpu.thread().is_thread_ready(*this))
            {
                set_worker_lock_phase(worker_lock_phase::context_switch_work);
                const auto switched = this->perform_thread_switch(vcpu, lock);
                set_worker_lock_phase(worker_lock_phase::worker_loop);
                if (!switched)
                {
                    break;
                }
            }

            // Guest code executes with the kernel lock released; hook callbacks
            // (syscalls, exceptions, exec hooks) re-acquire it on VM exit.
            vcpu.running.store(true, std::memory_order_relaxed);
            this->emu().set_scheduler_vm_parked(vcpu.cpu.index(), false);
            set_worker_lock_phase(worker_lock_phase::none);
            lock.unlock();
            {
                const auto clear_running = utils::finally([&vcpu] { vcpu.running.store(false, std::memory_order_relaxed); });
                this->start_cpu(vcpu);
            }
            acquire_scheduler_vm_parked(this->emu(), vcpu.cpu.index(), lock);
            set_worker_lock_phase(worker_lock_phase::sync_worker_context);

            // SMP: between quanta (kernel lock held, outside any write path) apply every cross-VM
            // op queued for this vCPU, so the scheduler's own host writes below (thread-context
            // save/restore) see freshly queued maps instead of racing them ('Unmapped').
            this->emu().sync_worker_context(vcpu.cpu.index());
            set_worker_lock_phase(worker_lock_phase::activity_publication);

            // Progress meter: publish the per-vCPU activity snapshot (rate-limited inside). The
            // kernel lock is held here, so concurrent workers cannot interleave file writes.
            this->publish_activity_status();
            set_worker_lock_phase(worker_lock_phase::worker_loop);

            if (!vcpu.switch_thread && !vcpu.cpu.has_violation())
            {
                if (!this->should_stop && !this->process.exit_status.has_value())
                {
                    this->log.error("SMPBARE vCPU %u tid %u RIP 0x%llX: backend returned without switch, violation, or process exit\n",
                                    vcpu.cpu.index(), vcpu.active_thread ? vcpu.active_thread->id : 0,
                                    static_cast<unsigned long long>(vcpu.cpu.reg<uint64_t>(x86_register::rip)));
                }
                break;
            }
        }

        this->emu().set_scheduler_vm_parked(vcpu.cpu.index(), false);
        set_worker_lock_phase(worker_lock_phase::none);
        lock.unlock();

        // One vCPU winding down (process exit, fatal error) ends the whole run.
        this->stop();
    }

    uint64_t windows_emulator::timestamp_counter_for_guest()
    {
        // Retired instructions stop during waits and add up across active peers. Keep wall-clock
        // timestamp instructions on the configured guest clock on every vCPU.
        return this->clock_->timestamp_counter();
    }

    void windows_emulator::publish_activity_status()
    {
        static const std::filesystem::path directory = [] {
            const char* configured = std::getenv("SOGEN_GPU_STATUS_DIR");
            return configured && *configured ? std::filesystem::path(configured) : std::filesystem::path{};
        }();
        static const bool shared_memory_enabled = [] {
            const char* configured = std::getenv("SOGEN_TELEMETRY_SHM");
            return configured && std::strcmp(configured, "1") == 0;
        }();
        if (!shared_memory_enabled && directory.empty())
        {
            return; // telemetry not requested for this run
        }

        const auto now = std::chrono::steady_clock::now();
        if (this->activity_status_last_.time_since_epoch().count() != 0 &&
            now - this->activity_status_last_ < std::chrono::seconds(1))
        {
            return; // 1 Hz is plenty for a human-facing meter
        }

        const auto activity = this->emu().vcpu_activity();
        const auto smp_profile = this->emu().smp_profile();
        const auto jit_profile = this->emu().jit_profile();
        if (activity.empty())
        {
            return; // backend does not report activity (e.g. WHP) - nothing truthful to publish
        }

        double elapsed_seconds = 0.0;
        if (this->activity_status_last_.time_since_epoch().count() != 0)
        {
            elapsed_seconds = std::chrono::duration<double>(now - this->activity_status_last_).count();
        }
        this->activity_status_last_ = now;

        if (this->activity_status_prev_instructions_.size() != activity.size())
        {
            this->activity_status_prev_instructions_.assign(activity.size(), 0);
            elapsed_seconds = 0.0; // first sample after a resize carries no rate
        }

        uint64_t total_instructions = 0;
        for (const auto& entry : activity)
        {
            total_instructions += entry.instructions;
        }

        std::string json{"{"};
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        char buf[512];

        std::snprintf(buf, sizeof(buf), "\"t\":%lld,\"vcpu_count\":%zu,\"total_instructions\":%llu,\"elapsed_seconds\":%.3f",
                      static_cast<long long>(stamp), activity.size(), static_cast<unsigned long long>(total_instructions),
                      elapsed_seconds);
        json += buf;

        uint64_t total_previous = 0;
        json += ",\"vcpus\":[";
        for (size_t i = 0; i < activity.size(); ++i)
        {
            const auto& entry = activity[i];
            const auto previous = this->activity_status_prev_instructions_[i];
            total_previous += previous;

            const auto* module_name = this->mod_manager.find_name(entry.rip);
            const auto instructions_delta = entry.instructions > previous ? entry.instructions - previous : 0;
            const auto mips = elapsed_seconds > 0.0
                                  ? static_cast<double>(instructions_delta) / (elapsed_seconds * 1000000.0)
                                  : 0.0;
            const auto& vcpu = *this->vcpus_[i];
            const auto* owner = vcpu.active_thread;
            const auto running = vcpu.running.load(std::memory_order_relaxed);

            std::snprintf(buf, sizeof(buf),
                          "%s{\"i\":%zu,\"rip\":%llu,\"module\":\"%s\",\"instructions\":%llu,\"mips\":%.2f,\"active\":%s,\"tid\":%u,"
                          "\"running\":%s,\"idle\":%s}",
                          i == 0 ? "" : ",", i, static_cast<unsigned long long>(entry.rip), module_name ? module_name : "",
                          static_cast<unsigned long long>(entry.instructions), mips, instructions_delta > 0 ? "true" : "false",
                          owner ? owner->id : 0, running ? "true" : "false", !running && !owner ? "true" : "false");
            json += buf;

            this->activity_status_prev_instructions_[i] = entry.instructions;
        }
        json += "]";

        const auto total_delta = total_instructions > total_previous ? total_instructions - total_previous : 0;
        const auto total_mips = elapsed_seconds > 0.0 ? static_cast<double>(total_delta) / (elapsed_seconds * 1000000.0) : 0.0;
        std::snprintf(buf, sizeof(buf), ",\"total_mips\":%.2f", total_mips);
        json += buf;

        if (!smp_profile.empty() && smp_profile.size() == activity.size())
        {
            // The first snapshot establishes a baseline; subsequent entries are 1 Hz deltas.
            if (this->activity_status_prev_smp_profile_.size() != smp_profile.size())
            {
                this->activity_status_prev_smp_profile_ = smp_profile;
            }
            const auto delta = [](uint64_t current, uint64_t previous) { return current >= previous ? current - previous : uint64_t{0}; };
            json += ",\"smp_profile\":{\"window_seconds\":" + std::to_string(elapsed_seconds);
            if (smp_profile[0].exec_write_wake_enabled)
            {
                json += ",\"exec_write_wake_events_total\":" + std::to_string(smp_profile[0].exec_write_wake_events_total);
                json += ",\"exec_write_guest_jit_total\":" + std::to_string(smp_profile[0].exec_write_guest_jit_total);
                json += ",\"exec_write_guest_mmu_total\":" + std::to_string(smp_profile[0].exec_write_guest_mmu_total);
                json += ",\"exec_write_host_total\":" + std::to_string(smp_profile[0].exec_write_host_total);
                json += ",\"exec_write_filtered_noncode_total\":" + std::to_string(smp_profile[0].exec_write_filtered_noncode_total);
                json += ",\"exec_write_notified_overlap_total\":" + std::to_string(smp_profile[0].exec_write_notified_overlap_total);
                json += ",\"exec_write_notified_concurrent_total\":" + std::to_string(smp_profile[0].exec_write_notified_concurrent_total);
            }
            json += ",\"vcpus\":[";
            for (size_t i = 0; i < smp_profile.size(); ++i)
            {
                const auto& current = smp_profile[i];
                const auto& previous = this->activity_status_prev_smp_profile_[i];
                std::snprintf(buf, sizeof(buf),
                              "%s{\"i\":%zu,\"map_calls\":%llu,\"map_nanos\":%llu,\"protect_calls\":%llu,"
                              "\"protect_nanos\":%llu,\"queue_ops\":%llu,\"queue_nanos\":%llu,"
                              "\"kick_calls\":%llu,\"kick_targets\":%llu,\"kick_nanos\":%llu",
                              i == 0 ? "" : ",", i, static_cast<unsigned long long>(delta(current.map_calls, previous.map_calls)),
                              static_cast<unsigned long long>(delta(current.map_nanos, previous.map_nanos)),
                              static_cast<unsigned long long>(delta(current.protect_calls, previous.protect_calls)),
                              static_cast<unsigned long long>(delta(current.protect_nanos, previous.protect_nanos)),
                              static_cast<unsigned long long>(delta(current.queue_ops, previous.queue_ops)),
                              static_cast<unsigned long long>(delta(current.queue_nanos, previous.queue_nanos)),
                              static_cast<unsigned long long>(delta(current.kick_calls, previous.kick_calls)),
                              static_cast<unsigned long long>(delta(current.kick_targets, previous.kick_targets)),
                              static_cast<unsigned long long>(delta(current.kick_nanos, previous.kick_nanos)));
                json += buf;
                // Peer aliasing runs during queue drain, outside the issuer's map timer.
                // The maximum is cumulative; the other peer-map fields are 1 Hz deltas.
                json += ",\"invalidate_queued\":" + std::to_string(delta(current.invalidate_queued, previous.invalidate_queued));
                json += ",\"invalidate_queue_nanos\":" + std::to_string(delta(current.invalidate_queue_nanos, previous.invalidate_queue_nanos));
                json += ",\"invalidate_applied\":" + std::to_string(delta(current.invalidate_applied, previous.invalidate_applied));
                json += ",\"invalidate_no_change\":" + std::to_string(delta(current.invalidate_no_change, previous.invalidate_no_change));
                json += ",\"invalidate_apply_nanos\":" + std::to_string(delta(current.invalidate_apply_nanos, previous.invalidate_apply_nanos));
                json += ",\"invalidate_adjacent_same_pages\":" + std::to_string(delta(current.invalidate_adjacent_same_pages, previous.invalidate_adjacent_same_pages));
                json += ",\"peer_map_calls\":" + std::to_string(delta(current.peer_map_calls, previous.peer_map_calls));
                json += ",\"peer_map_pages\":" + std::to_string(delta(current.peer_map_pages, previous.peer_map_pages));
                json += ",\"peer_map_nanos\":" + std::to_string(delta(current.peer_map_nanos, previous.peer_map_nanos));
                json += ",\"peer_map_max_nanos\":" + std::to_string(current.peer_map_max_nanos);
                if (current.invalidation_profile_enabled)
                {
                    json += ",\"epoch_mismatches_total\":" + std::to_string(current.epoch_mismatches_total);
                    json += ",\"jit_resets_total\":" + std::to_string(current.jit_resets_total);
                    json += ",\"jit_reset_epoch_total\":" + std::to_string(current.jit_reset_epoch_total);
                    json += ",\"jit_reset_wake_total\":" + std::to_string(current.jit_reset_wake_total);
                    json += ",\"jit_reset_manual_total\":" + std::to_string(current.jit_reset_manual_total);
                    json += ",\"jit_reset_mixed_total\":" + std::to_string(current.jit_reset_mixed_total);
                    json += ",\"jit_reset_unknown_total\":" + std::to_string(current.jit_reset_unknown_total);
                    json += ",\"manual_origin_resets_total\":" + std::to_string(current.manual_origin_resets_total);
                    json += ",\"manual_peer_protection_total\":" + std::to_string(current.manual_peer_protection_total);
                    json += ",\"manual_public_invalidate_total\":" + std::to_string(current.manual_public_invalidate_total);
                    json += ",\"manual_self_modifying_total\":" + std::to_string(current.manual_self_modifying_total);
                    json += ",\"manual_host_cache_total\":" + std::to_string(current.manual_host_cache_total);
                    json += ",\"manual_unmap_total\":" + std::to_string(current.manual_unmap_total);
                    json += ",\"manual_protect_total\":" + std::to_string(current.manual_protect_total);
                    json += ",\"manual_host_write_total\":" + std::to_string(current.manual_host_write_total);
                    json += ",\"manual_multiple_origins_total\":" + std::to_string(current.manual_multiple_origins_total);
                    json += ",\"manual_unknown_origin_total\":" + std::to_string(current.manual_unknown_origin_total);
                }
                if (current.exec_write_wake_enabled)
                {
                    json += ",\"exec_write_owner_flushes_total\":" + std::to_string(current.exec_write_owner_flushes_total);
                }
                json += "}";
            }
            json += "]}";
            this->activity_status_prev_smp_profile_ = smp_profile;
        }

        if (!jit_profile.empty() && jit_profile.size() == activity.size())
        {
            if (this->activity_status_prev_jit_profile_.size() != jit_profile.size())
            {
                this->activity_status_prev_jit_profile_ = jit_profile;
            }
            const auto delta = [](uint64_t current, uint64_t previous) {
                return current >= previous ? current - previous : uint64_t{0};
            };
            json += ",\"jit_profile\":{\"window_seconds\":" + std::to_string(elapsed_seconds) + ",\"vcpus\":[";
            for (size_t i = 0; i < jit_profile.size(); ++i)
            {
                const auto& current = jit_profile[i];
                const auto& previous = this->activity_status_prev_jit_profile_[i];
                std::snprintf(buf, sizeof(buf),
                              "%s{\"i\":%zu,\"compile_calls\":%llu,\"compile_nanos\":%llu,\"reset_calls\":%llu,"
                              "\"recompile_calls\":%llu,\"recompile_nanos\":%llu,"
                              "\"recompile_compile_calls\":%llu,\"recompile_compile_nanos\":%llu,",
                              i == 0 ? "" : ",", i,
                              static_cast<unsigned long long>(delta(current.compile_calls, previous.compile_calls)),
                              static_cast<unsigned long long>(delta(current.compile_nanos, previous.compile_nanos)),
                              static_cast<unsigned long long>(delta(current.reset_calls, previous.reset_calls)),
                              static_cast<unsigned long long>(delta(current.recompile_calls, previous.recompile_calls)),
                              static_cast<unsigned long long>(delta(current.recompile_nanos, previous.recompile_nanos)),
                              static_cast<unsigned long long>(delta(current.recompile_compile_calls, previous.recompile_compile_calls)),
                              static_cast<unsigned long long>(delta(current.recompile_compile_nanos, previous.recompile_compile_nanos)));
                json += buf;
                std::snprintf(buf, sizeof(buf),
                              "\"reset_generation\":%llu,\"reset_cause_flags\":%llu,"
                              "\"reset_manual_origin_flags\":%llu,\"generation_compile_calls\":%llu,"
                              "\"generation_compile_nanos\":%llu,\"flush_code_nanos\":%llu,"
                              "\"jit_reset_nanos\":%llu,",
                              static_cast<unsigned long long>(current.reset_generation),
                              static_cast<unsigned long long>(current.reset_cause_flags),
                              static_cast<unsigned long long>(current.reset_manual_origin_flags),
                              static_cast<unsigned long long>(current.generation_compile_calls),
                              static_cast<unsigned long long>(current.generation_compile_nanos),
                              static_cast<unsigned long long>(delta(current.flush_code_nanos, previous.flush_code_nanos)),
                              static_cast<unsigned long long>(delta(current.jit_reset_nanos, previous.jit_reset_nanos)));
                json += buf;
                std::snprintf(buf, sizeof(buf),
                              "\"origin_first_address_compiles\":%llu,\"origin_repeat_after_reset_compiles\":%llu,"
                              "\"origin_repeat_in_generation_compiles\":%llu,\"origin_periodic_recompile_compiles\":%llu,"
                              "\"origin_unclassified_compiles\":%llu,\"origin_generation_number\":%llu}",
                              static_cast<unsigned long long>(delta(current.origin_first_address_compiles, previous.origin_first_address_compiles)),
                              static_cast<unsigned long long>(delta(current.origin_repeat_after_reset_compiles, previous.origin_repeat_after_reset_compiles)),
                              static_cast<unsigned long long>(delta(current.origin_repeat_in_generation_compiles, previous.origin_repeat_in_generation_compiles)),
                              static_cast<unsigned long long>(delta(current.origin_periodic_recompile_compiles, previous.origin_periodic_recompile_compiles)),
                              static_cast<unsigned long long>(delta(current.origin_unclassified_compiles, previous.origin_unclassified_compiles)),
                              static_cast<unsigned long long>(current.origin_generation_number));
                json += buf;
            }
            json += "]}";
            this->activity_status_prev_jit_profile_ = jit_profile;
        }

        if (scheduler_profiling_enabled())
        {
            json += ",\"scheduler_profile\":{\"vcpus\":[";
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                const auto& stats = this->vcpus_[i]->scheduler_profile;
                json += i == 0 ? "" : ",";
                json += "{\"i\":" + std::to_string(i);
                json += ",\"context_switch_calls_total\":" + std::to_string(stats.context_switch_calls);
                json += ",\"context_switch_nanos_total\":" + std::to_string(stats.context_switch_nanos);
                json += ",\"device_work_calls_total\":" + std::to_string(stats.device_work_calls);
                json += ",\"device_work_nanos_total\":" + std::to_string(stats.device_work_nanos);
                json += ",\"idle_retries_total\":" + std::to_string(stats.idle_retries);
                json += ",\"idle_host_yields_total\":" + std::to_string(stats.idle_host_yields);
                json += ",\"idle_host_sleeps_total\":" + std::to_string(stats.idle_host_sleeps);
                json += ",\"idle_relative_ticks_total\":" + std::to_string(stats.idle_relative_ticks);
                json += ",\"timer_preempt_requests_total\":" + std::to_string(stats.timer_preempt_requests);
                json += "}";
            }
            json += "]";
            if (kernel_lock::profiling_enabled())
            {
                const auto lock_stats = this->kernel_lock_.profile();
                json += ",\"kernel_lock\":{\"acquisitions_total\":" + std::to_string(lock_stats.acquisitions);
                json += ",\"contended_total\":" + std::to_string(lock_stats.contended);
                json += ",\"wait_nanos_total\":" + std::to_string(lock_stats.wait_nanos);
                json += ",\"held_nanos_total\":" + std::to_string(lock_stats.held_nanos) + "}";
            }
            json += "}";
        }

        uint32_t runnable_unowned = 0;
        json += ",\"threads\":[";
        bool first_thread = true;
        for (const auto& thread : this->process.threads | std::views::values)
        {
            const char* state = "ready";
            const char* wait_reason = "none";
            if (thread.is_terminated())
            {
                state = "terminated";
            }
            else if (thread.suspended > 0)
            {
                state = "suspended";
            }
            else if (thread.smp_visibility_mark != 0 && !this->emu().smp_op_applied(thread.smp_visibility_mark))
            {
                state = "wait";
                wait_reason = "visibility";
            }
            else if (thread.waiting_for_alert)
            {
                state = "wait";
                wait_reason = "alert";
            }
            else if (!thread.await_objects.empty())
            {
                state = "wait";
                wait_reason = "objects";
            }
            else if (thread.await_msg_mask)
            {
                state = "wait";
                wait_reason = "message_mask";
            }
            else if (thread.await_time)
            {
                state = "wait";
                wait_reason = "time";
            }
            else if (thread.await_host_condition)
            {
                state = "wait";
                wait_reason = "host_condition";
            }
            else if (thread.await_io_completion)
            {
                state = "wait";
                wait_reason = "io_completion";
            }
            else if (thread.await_msg)
            {
                state = "wait";
                wait_reason = "message";
            }

            std::optional<size_t> owner_index;
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                if (this->vcpus_[i]->active_thread == &thread)
                {
                    owner_index = i;
                    break;
                }
            }
            if (!owner_index && state == std::string_view{"ready"})
            {
                ++runnable_unowned;
            }

            json += first_thread ? "" : ",";
            first_thread = false;
            std::snprintf(buf, sizeof(buf), "{\"tid\":%u,\"state\":\"%s\",\"wait_reason\":\"%s\",\"owner_vcpu\":", thread.id, state,
                          wait_reason);
            json += buf;
            json += owner_index ? std::to_string(*owner_index) : "null";
            std::snprintf(buf, sizeof(buf), ",\"suspend_count\":%u,\"wait_objects\":%zu}", thread.suspended, thread.await_objects.size());
            json += buf;
        }
        json += "]";
        json += ",\"runnable_unowned\":" + std::to_string(runnable_unowned);
        json += "}";

        // Telemetry failure must never alter emulation: swallow everything.
        try
        {
            if (shared_memory_enabled)
            {
                static detail::telemetry_shared_memory mapping;
                (void)mapping.publish(json);
            }
            if (!directory.empty())
            {
                std::ofstream output(directory / "emu-status.json", std::ios::binary | std::ios::trunc);
                output << json << '\n';
            }
        }
        catch (...)
        {
        }
    }

    bool windows_emulator::activate_thread(vcpu_context& vcpu, const uint32_t id)
    {
        const std::scoped_lock lock(this->kernel_lock_);

        auto* thread = get_thread_by_id(this->process, id);
        if (!thread)
        {
            return false;
        }

        return switch_to_thread(*this, vcpu, *thread, true);
    }

    void windows_emulator::on_instruction_execution(vcpu_context& vcpu, const uint64_t address)
    {
        if (!vcpu.active_thread)
        {
            throw_missing_execution_thread();
        }
        auto& thread = *vcpu.active_thread;

        if (!thread.callback_stack.empty() && address == this->process.zw_callback_return)
        {
            capture_callback_return(vcpu, thread);
        }

        ++this->executed_instructions_;
        const auto thread_insts = ++thread.executed_instructions;
        if (thread_insts % MAX_INSTRUCTIONS_PER_TIME_SLICE == 0)
        {
            this->yield_thread(vcpu);
        }

        thread.previous_ip = thread.current_ip;
        thread.current_ip = address;

        // A cache hit needs no module lookup, hook removal, or first-execution event construction.
        // Keep the range test here so it also avoids entering the larger tracking function.
        if (!this->use_section_first_execution_hooks_ &&
            address - this->last_executed_section_.start >= this->last_executed_section_.length)
        {
            this->track_section_first_execution(address);
        }

        this->callbacks.on_instruction(address);
    }

    bool windows_emulator::uses_section_first_execution_hooks() const
    {
        return this->use_section_first_execution_hooks_;
    }

    NO_INLINE void windows_emulator::track_section_first_execution(const uint64_t address)
    {
        if (address - this->last_executed_section_.start < this->last_executed_section_.length)
        {
            return;
        }
        auto* mod = this->mod_manager.find_by_address(address);
        if (!mod)
        {
            return;
        }

        auto* hook_states = [&]() -> std::vector<emulator_hook*>* {
            const auto entry = this->section_first_execution_hooks_.find(mod->image_base);
            return entry == this->section_first_execution_hooks_.end() ? nullptr : &entry->second;
        }();

        for (size_t i = 0; i < mod->sections.size(); ++i)
        {
            auto& section = mod->sections[i];
            if (!is_within_start_and_length(address, section.region.start, section.region.length))
            {
                continue;
            }

            this->last_executed_section_ = section.region;
            if (section.first_execute.has_value())
            {
                return;
            }

            section.first_execute = address;

            if (hook_states && i < hook_states->size() && (*hook_states)[i])
            {
                auto* hook = (*hook_states)[i];
                (*hook_states)[i] = nullptr;
                this->emu().delete_hook(hook);
            }

            this->callbacks.on_section_first_execution(*mod, section, address);

            return;
        }
    }

    void windows_emulator::clear_section_first_execution_hooks()
    {
        this->last_executed_section_ = {};
        for (const auto& hooks : this->section_first_execution_hooks_ | std::views::values)
        {
            for (auto* hook : hooks)
            {
                if (hook)
                {
                    this->emu().delete_hook(hook);
                }
            }
        }

        this->section_first_execution_hooks_.clear();
    }

    void windows_emulator::install_section_first_execution_hook(const mapped_module& mod, const size_t section_index)
    {
        if (!this->uses_section_first_execution_hooks() || section_index >= mod.sections.size())
        {
            return;
        }

        const auto& section = mod.sections[section_index];
        if (section.first_execute.has_value() || section.region.length == 0)
        {
            return;
        }

        auto& hooks = this->section_first_execution_hooks_[mod.image_base];
        if (hooks.size() < mod.sections.size())
        {
            hooks.resize(mod.sections.size());
        }

        if (hooks[section_index])
        {
            return;
        }

        hooks[section_index] = this->emu().hook_memory_range_execution(section.region.start, section.region.length,
                                                                       [this](cpu_interface&, const uint64_t address) {
                                                                           const std::scoped_lock lock(this->kernel_lock_);
                                                                           this->track_section_first_execution(address); //
                                                                       });
    }

    void windows_emulator::install_section_first_execution_hooks()
    {
        if (!this->uses_section_first_execution_hooks())
        {
            return;
        }

        for (const auto& mod : this->mod_manager.modules() | std::views::values)
        {
            for (size_t i = 0; i < mod.sections.size(); ++i)
            {
                this->install_section_first_execution_hook(mod, i);
            }
        }
    }

    namespace
    {
        // LEANDIAG block-trail storage (SOGEN_LEANDIAG_BLOCKTRACE=1, diagnosis only): the last
        // few executed guest block addresses per guest thread id, newest last.
        struct leandiag_block_trail
        {
            static constexpr size_t k_depth = 128;
            std::array<uint64_t, k_depth> addresses{};
            size_t next{0};
            bool full{false};
            // The loader does hundreds of ntdll-only teardown blocks between a failing
            // DllMain and the raise; keep the last non-ntdll blocks separately so the
            // answer survives that chatter.
            static constexpr size_t k_foreign_depth = 24;
            std::array<uint64_t, k_foreign_depth> foreign_addresses{};
            size_t foreign_next{0};
            bool foreign_full{false};
        };
        std::mutex leandiag_trail_mutex{};
        std::unordered_map<uint32_t, leandiag_block_trail> leandiag_trails{};
    } // namespace

    void windows_emulator::leandiag_note_thread_birth(const uint32_t tid)
    {
        std::lock_guard<std::mutex> lock(leandiag_trail_mutex);
        leandiag_trails[tid]; // creates the trail - birth is implicit in first record
    }

    bool windows_emulator::leandiag_thread_is_young(const uint32_t tid)
    {
        // "Young" = fewer than 20k recorded blocks. Boot-time threads age out fast; a thread
        // that fails during its own initialization never gets close to the cap, so its entire
        // execution lands in the FULLTRACE console log.
        std::lock_guard<std::mutex> lock(leandiag_trail_mutex);
        const auto it = leandiag_trails.find(tid);
        return it == leandiag_trails.end() || !it->second.full;
    }

    void windows_emulator::leandiag_record_block(const uint32_t tid, const uint64_t address)
    {
        std::lock_guard<std::mutex> lock(leandiag_trail_mutex);
        auto& trail = leandiag_trails[tid];
        trail.addresses[trail.next] = address;
        trail.next = (trail.next + 1) % leandiag_block_trail::k_depth;
        trail.full = trail.full || trail.next == 0;
    }

    void windows_emulator::leandiag_record_foreign_block(const uint32_t tid, const uint64_t address)
    {
        std::lock_guard<std::mutex> lock(leandiag_trail_mutex);
        auto& trail = leandiag_trails[tid];
        trail.foreign_addresses[trail.foreign_next] = address;
        trail.foreign_next = (trail.foreign_next + 1) % leandiag_block_trail::k_foreign_depth;
        trail.foreign_full = trail.foreign_full || trail.foreign_next == 0;
    }

    std::vector<uint64_t> windows_emulator::leandiag_last_foreign_blocks(const uint32_t tid)
    {
        std::lock_guard<std::mutex> lock(leandiag_trail_mutex);
        const auto it = leandiag_trails.find(tid);
        if (it == leandiag_trails.end())
        {
            return {};
        }
        const auto& trail = it->second;
        const auto depth = trail.foreign_full ? leandiag_block_trail::k_foreign_depth : trail.foreign_next;
        std::vector<uint64_t> out{};
        out.reserve(depth);
        for (size_t i = trail.foreign_full ? trail.foreign_next : 0; out.size() < depth;
             i = (i + 1) % leandiag_block_trail::k_foreign_depth)
        {
            out.push_back(trail.foreign_addresses[i]);
        }
        return out;
    }

    std::vector<uint64_t> windows_emulator::leandiag_last_blocks(const uint32_t tid)
    {
        std::lock_guard<std::mutex> lock(leandiag_trail_mutex);
        const auto it = leandiag_trails.find(tid);
        if (it == leandiag_trails.end())
        {
            return {};
        }
        const auto& trail = it->second;
        const auto depth = trail.full ? leandiag_block_trail::k_depth : trail.next;
        std::vector<uint64_t> out{};
        out.reserve(depth);
        for (size_t i = trail.full ? trail.next : 0; out.size() < depth; i = (i + 1) % leandiag_block_trail::k_depth)
        {
            out.push_back(trail.addresses[i]);
        }
        return out;
    }

    void windows_emulator::setup_hooks()
    {
        // LEANDIAG block trace: lean runs have no per-instruction observation, so a failing
        // DllMain(THREAD_ATTACH) - pure guest code, zero syscalls - is invisible. This optional
        // block-level breadcrumb (SOGEN_LEANDIAG_BLOCKTRACE=1) records the last few executed
        // block addresses per guest thread; the LEANDIAG raise dump prints the raiser's trail
        // with module names, naming the DLL whose init returned FALSE.
        const char* leandiag_blocktrace = std::getenv("SOGEN_LEANDIAG_BLOCKTRACE");
        const char* leandiag_fulltrace = std::getenv("SOGEN_LEANDIAG_FULLTRACE");
        if ((leandiag_blocktrace && *leandiag_blocktrace == '1') || (leandiag_fulltrace && *leandiag_fulltrace == '1'))
        {
            const bool full = leandiag_fulltrace && *leandiag_fulltrace == '1';
            this->emu().hook_basic_block([&](cpu_interface& cpu, const basic_block& block) {
                const std::scoped_lock lock(this->kernel_lock_);
                auto& vcpu = this->vcpu(cpu.index());
                if (!vcpu.active_thread)
                {
                    return;
                }
                const auto tid = vcpu.active_thread->id;
                windows_emulator::leandiag_record_block(tid, block.address);
                const auto* trail_owner = this->mod_manager.find_name(block.address);
                if (trail_owner && std::string_view(trail_owner) != "ntdll.dll")
                {
                    windows_emulator::leandiag_record_foreign_block(tid, block.address);
                }
                if (full && windows_emulator::leandiag_thread_is_young(tid))
                {
                    const auto* owner = this->mod_manager.find_name(block.address);
                    this->log.error("FULLTRACE tid=%u block=%llX (%s)\n", tid,
                                    (unsigned long long)block.address, owner ? owner : "no module");
                }
            });
        }

        this->callbacks.on_module_load.add([this](mapped_module& mod) {
            this->last_executed_section_ = {};
            for (size_t i = 0; i < mod.sections.size(); ++i)
            {
                this->install_section_first_execution_hook(mod, i);
            }
        });

        // Exact execution hooks only: no global instruction callback or default hot-path work.
        // The first few guest C++ throws identify the caller preceding a DXVK/d3d11 terminate.
        if (const auto* probe = std::getenv("SOGEN_GUEST_CXX_THROW_PROBE"); probe && *probe == '1')
        {
            auto samples = std::make_shared<uint32_t>(0);
            auto terminal_samples = std::make_shared<uint32_t>(0);
            auto load_records = std::make_shared<std::atomic<uint32_t>>(0);
            auto hooks = std::make_shared<std::unordered_map<uint64_t, std::array<emulator_hook*, 3>>>();
            // The captured x64 DXVK build links the CRT statically. Its matching PDB places
            // _CxxThrowException at RVA 0x4eeb68; require an explicit RVA plus image/signature
            // match so other d3d11.dll builds cannot accidentally receive this hook.
            constexpr uint64_t dxvk_image_size = 0x794000;
            constexpr uint64_t dxvk_terminate_rva = 0x5334f4;
            constexpr uint64_t dxvk_abort_rva = 0x536904;
            constexpr std::array<uint8_t, 8> dxvk_terminate_entry{
                0x48, 0x83, 0xec, 0x28, 0xe8, 0x43, 0xc4, 0x00};
            constexpr std::array<uint8_t, 8> dxvk_abort_entry{
                0x48, 0x83, 0xec, 0x28, 0xe8, 0x8f, 0x13, 0x01};
            constexpr std::array<uint8_t, 16> dxvk_throw_entry{
                0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74,
                0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48};
            constexpr uint64_t dxvk_join_rva = 0x325f10;
            constexpr uint64_t dxvk_join_throw_call_rva = 0x326004;
            constexpr std::array<uint8_t, 10> dxvk_join_entry{
                0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x70};
            constexpr std::array<uint8_t, 5> dxvk_join_throw_call{0xe8, 0x5f, 0x8b, 0x1c, 0x00};
            uint64_t dxvk_throw_rva = 0;
            if (const auto* rva_text = std::getenv("SOGEN_GUEST_CXX_THROW_RVA"))
            {
                char* end = nullptr;
                const auto parsed = std::strtoull(rva_text, &end, 0);
                if (end != rva_text && *end == '\0')
                {
                    dxvk_throw_rva = parsed;
                }
            }
            this->callbacks.on_module_load.add(
                [this, samples, terminal_samples, load_records, hooks, dxvk_throw_rva, dxvk_throw_entry,
                 dxvk_join_rva, dxvk_join_throw_call_rva, dxvk_join_entry, dxvk_join_throw_call,
                 dxvk_terminate_entry, dxvk_abort_entry](mapped_module& mod) {
                if (hooks->contains(mod.image_base))
                {
                    return;
                }
                uint64_t entry = 0;
                bool signature_read = false;
                bool signature_match = false;
                bool join_layout_match = false;
                const char* reason = "not_dxvk";
                if (is_vcruntime_throw_module(mod.name))
                {
                    entry = mod.find_export("_CxxThrowException");
                }
                else if (is_d3d11_throw_module(mod.name))
                {
                    reason = dxvk_throw_rva ? "image_size" : "rva_unset_or_invalid";
                    if (dxvk_throw_rva && mod.size_of_image == dxvk_image_size)
                    {
                        reason = "rva_range";
                        if (dxvk_throw_rva <= mod.size_of_image - dxvk_throw_entry.size() &&
                            mod.image_base <= UINT64_MAX - dxvk_throw_rva)
                        {
                            const auto candidate = mod.image_base + dxvk_throw_rva;
                            std::array<uint8_t, dxvk_throw_entry.size()> actual{};
                            signature_read = this->emu().try_read_memory(candidate, actual.data(), actual.size());
                            signature_match = signature_read && actual == dxvk_throw_entry;
                            reason = signature_match ? "ready" :
                                     signature_read ? "signature_mismatch" : "signature_unreadable";
                            if (signature_match)
                            {
                                entry = candidate;
                                if (dxvk_join_rva <= mod.size_of_image - dxvk_join_entry.size() &&
                                    dxvk_join_throw_call_rva <= mod.size_of_image - dxvk_join_throw_call.size() &&
                                    mod.image_base <= UINT64_MAX - dxvk_join_throw_call_rva)
                                {
                                    std::array<uint8_t, dxvk_join_entry.size()> actual_join{};
                                    std::array<uint8_t, dxvk_join_throw_call.size()> actual_call{};
                                    join_layout_match =
                                        this->emu().try_read_memory(mod.image_base + dxvk_join_rva,
                                                                    actual_join.data(), actual_join.size()) &&
                                        this->emu().try_read_memory(mod.image_base + dxvk_join_throw_call_rva,
                                                                    actual_call.data(), actual_call.size()) &&
                                        actual_join == dxvk_join_entry && actual_call == dxvk_join_throw_call;
                                }
                            }
                        }
                    }
                }
                const auto report_dxvk = [&](const char* result, const uint64_t installed,
                                             const uint64_t terminate, const uint64_t abort) {
                    if (is_d3d11_throw_module(mod.name) &&
                        load_records->fetch_add(1, std::memory_order_relaxed) < 4)
                    {
                        this->log.error(
                            "[GUESTCXXPROBE] module=%.*s base=%#llx requested_rva=%#llx image_size=%#llx "
                            "expected_size=%#llx signature_read=%u signature_match=%u installed=%#llx "
                            "reason=%s terminate_hook=%#llx abort_hook=%#llx\n",
                            static_cast<int>(std::min<size_t>(mod.name.size(), 32)), mod.name.c_str(),
                            static_cast<unsigned long long>(mod.image_base),
                            static_cast<unsigned long long>(dxvk_throw_rva),
                            static_cast<unsigned long long>(mod.size_of_image),
                            static_cast<unsigned long long>(dxvk_image_size),
                            static_cast<unsigned>(signature_read), static_cast<unsigned>(signature_match),
                            static_cast<unsigned long long>(installed), result,
                            static_cast<unsigned long long>(terminate), static_cast<unsigned long long>(abort));
                    }
                };
                if (!entry || !mod.contains(entry))
                {
                    report_dxvk(reason, 0, 0, 0);
                    return;
                }
                const auto runtime_name = mod.name;
                const auto runtime_image_base = mod.image_base;
                const auto runtime_image_size = mod.size_of_image;
                auto* hook = this->emu().hook_memory_execution(
                    entry, [this, samples, runtime_name, runtime_image_base, runtime_image_size,
                            join_layout_match](cpu_interface& cpu, const uint64_t rip) {
                        const std::scoped_lock lock(this->kernel_lock_);
                        if (*samples >= 8)
                        {
                            return;
                        }
                        const auto sample = ++*samples;
                        const auto& vcpu = this->vcpu(cpu.index());
                        auto& acting = vcpu.cpu;
                        const auto tid = vcpu.active_thread ? vcpu.active_thread->id : 0;
                        const auto rsp = acting.reg<uint64_t>(x86_register::rsp);
                        const auto object = acting.reg<uint64_t>(x86_register::rcx);
                        const auto throw_info = acting.reg<uint64_t>(x86_register::rdx);
                        uint64_t return_address{};
                        const bool return_ok = acting.try_read_memory(rsp, &return_address, sizeof(return_address));
                        const auto* caller = return_ok ? this->mod_manager.find_by_address(return_address) : nullptr;
                        const bool return_in_runtime = return_ok && return_address >= runtime_image_base &&
                            return_address - runtime_image_base < runtime_image_size;
                        const std::string_view caller_name = caller ? std::string_view{caller->name} :
                            return_in_runtime ? std::string_view{runtime_name} : "<unmapped>";
                        const auto caller_rva = caller ? return_address - caller->image_base :
                            return_in_runtime ? return_address - runtime_image_base : 0;
                        // This exact DXVK throw site is the WAIT_FAILED branch of dxvk::thread::join.
                        // Preserve its thread handle and Win32 error before stack unwinding destroys them.
                        const bool join_failure = is_d3d11_throw_module(caller_name) && caller_rva == 0x326009;
                        const auto join_this = acting.reg<uint64_t>(x86_register::rdi);
                        uint64_t join_data{};
                        uint64_t join_handle{};
                        const bool join_data_read = join_failure &&
                            acting.try_read_memory(join_this, &join_data, sizeof(join_data));
                        const bool join_handle_read = join_data_read &&
                            acting.try_read_memory(join_data, &join_handle, sizeof(join_handle));
                        const auto gs_base = acting.get_segment_base(x86_register::gs);
                        uint32_t last_error{};
                        const bool last_error_read = join_failure && gs_base <= UINT64_MAX - 0x68 &&
                            acting.try_read_memory(gs_base + 0x68, &last_error, sizeof(last_error));
                        uint64_t outer_return_address{};
                        const bool outer_return_read = join_failure && join_layout_match && rsp <= UINT64_MAX - 0x80 &&
                            acting.try_read_memory(rsp + 0x80, &outer_return_address, sizeof(outer_return_address));
                        const auto* outer_caller =
                            outer_return_read ? this->mod_manager.find_by_address(outer_return_address) : nullptr;
                        const bool outer_return_in_runtime = outer_return_read &&
                            outer_return_address >= runtime_image_base &&
                            outer_return_address - runtime_image_base < runtime_image_size;
                        const std::string_view outer_caller_name =
                            outer_caller ? std::string_view{outer_caller->name} :
                            outer_return_in_runtime ? std::string_view{runtime_name} : "<unmapped>";
                        const auto outer_caller_rva = outer_caller ? outer_return_address - outer_caller->image_base :
                            outer_return_in_runtime ? outer_return_address - runtime_image_base : 0;
                        std::array<char, 65> object_hex{};
                        std::array<char, 33> throw_info_hex{};
                        const auto object_readable = capture_guest_hex(acting, object, object_hex);
                        const auto info_readable = capture_guest_hex(acting, throw_info, throw_info_hex);
                        this->log.error(
                            "[GUESTCXXTHROW] n=%u tid=%u vcpu=%zu runtime=%.*s rip=%#llx rsp=%#llx object=%#llx "
                            "throw_info=%#llx caller_ret=%#llx ret_ok=%u caller=%.*s+%#llx "
                            "object_bytes=%s object_readable=%u throw_info_bytes=%s info_readable=%u\n",
                            sample, tid, cpu.index(), static_cast<int>(std::min<size_t>(runtime_name.size(), 32)),
                            runtime_name.c_str(), static_cast<unsigned long long>(rip),
                            static_cast<unsigned long long>(rsp), static_cast<unsigned long long>(object),
                            static_cast<unsigned long long>(throw_info),
                            static_cast<unsigned long long>(return_address), static_cast<unsigned>(return_ok),
                            static_cast<int>(std::min<size_t>(caller_name.size(), 48)), caller_name.data(),
                            static_cast<unsigned long long>(caller_rva), object_hex.data(), object_readable,
                            throw_info_hex.data(), info_readable);
                        if (join_failure)
                        {
                            this->log.error(
                                "[GUESTDXVKJOINFAIL] tid=%u vcpu=%zu this=%#llx data=%#llx data_read=%u "
                                "handle=%#llx handle_read=%u last_error=%#x last_error_read=%u "
                                "join_layout_match=%u outer_ret=%#llx outer_ret_read=%u outer_caller=%.*s+%#llx\n",
                                tid, cpu.index(), static_cast<unsigned long long>(join_this),
                                static_cast<unsigned long long>(join_data), static_cast<unsigned>(join_data_read),
                                static_cast<unsigned long long>(join_handle), static_cast<unsigned>(join_handle_read),
                                last_error, static_cast<unsigned>(last_error_read),
                                static_cast<unsigned>(join_layout_match),
                                static_cast<unsigned long long>(outer_return_address),
                                static_cast<unsigned>(outer_return_read),
                                static_cast<int>(std::min<size_t>(outer_caller_name.size(), 48)),
                                outer_caller_name.data(), static_cast<unsigned long long>(outer_caller_rva));
                        }
                    });
                std::array<emulator_hook*, 3> installed{hook, nullptr, nullptr};
                if (hook && is_d3d11_throw_module(mod.name))
                {
                    const auto install_terminal = [&](const uint64_t rva, const std::array<uint8_t, 8>& signature,
                                                      const char* kind) -> emulator_hook* {
                        if (rva > mod.size_of_image - signature.size() || mod.image_base > UINT64_MAX - rva)
                        {
                            return nullptr;
                        }
                        const auto address = mod.image_base + rva;
                        std::array<uint8_t, 8> actual{};
                        if (!this->emu().try_read_memory(address, actual.data(), actual.size()) || actual != signature)
                        {
                            return nullptr;
                        }
                        return this->emu().hook_memory_execution(
                            address, [this, terminal_samples, runtime_name, kind](cpu_interface& cpu, uint64_t rip) {
                                const std::scoped_lock lock(this->kernel_lock_);
                                if (*terminal_samples >= 4)
                                {
                                    return;
                                }
                                const auto sample = ++*terminal_samples;
                                const auto& vcpu = this->vcpu(cpu.index());
                                auto& acting = vcpu.cpu;
                                const auto tid = vcpu.active_thread ? vcpu.active_thread->id : 0;
                                const auto rsp = acting.reg<uint64_t>(x86_register::rsp);
                                uint64_t return_address{};
                                const bool return_ok = acting.try_read_memory(rsp, &return_address, sizeof(return_address));
                                const auto* caller = return_ok ? this->mod_manager.find_by_address(return_address) : nullptr;
                                const std::string_view caller_name = caller ? std::string_view{caller->name} : "<unmapped>";
                                const auto caller_rva = caller ? return_address - caller->image_base : 0;
                                std::array<char, 65> stack_hex{};
                                const auto stack_readable = capture_guest_hex(acting, rsp, stack_hex);
                                this->log.error(
                                    "[GUESTCXXTERM] n=%u kind=%s tid=%u vcpu=%zu runtime=%.*s rip=%#llx rsp=%#llx "
                                    "caller_ret=%#llx ret_ok=%u caller=%.*s+%#llx rcx=%#llx rdx=%#llx "
                                    "stack_bytes=%s stack_readable=%u\n",
                                    sample, kind, tid, cpu.index(),
                                    static_cast<int>(std::min<size_t>(runtime_name.size(), 32)), runtime_name.c_str(),
                                    static_cast<unsigned long long>(rip), static_cast<unsigned long long>(rsp),
                                    static_cast<unsigned long long>(return_address), static_cast<unsigned>(return_ok),
                                    static_cast<int>(std::min<size_t>(caller_name.size(), 48)), caller_name.data(),
                                    static_cast<unsigned long long>(caller_rva),
                                    static_cast<unsigned long long>(acting.reg<uint64_t>(x86_register::rcx)),
                                    static_cast<unsigned long long>(acting.reg<uint64_t>(x86_register::rdx)),
                                    stack_hex.data(), stack_readable);
                            });
                    };
                    installed[1] = install_terminal(dxvk_terminate_rva, dxvk_terminate_entry, "terminate");
                    installed[2] = install_terminal(dxvk_abort_rva, dxvk_abort_entry, "abort");
                }
                if (hook)
                {
                    hooks->emplace(mod.image_base, installed);
                }
                report_dxvk(hook ? "installed" : "hook_failed", hook ? entry : 0,
                            installed[1] ? mod.image_base + dxvk_terminate_rva : 0,
                            installed[2] ? mod.image_base + dxvk_abort_rva : 0);
            });
            this->callbacks.on_module_unload.add([this, hooks](mapped_module& mod) {
                if (auto entry = hooks->extract(mod.image_base); entry)
                {
                    for (auto* hook : entry.mapped())
                    {
                        if (hook)
                        {
                            this->emu().delete_hook(hook);
                        }
                    }
                }
            });
        }

        this->callbacks.on_module_unload.add([this](mapped_module& mod) {
            // LEANDIAG: the loader deregisters a failed DLL from the manager BEFORE the unmap
            // syscall, so unmap-time and raise-time lookups both miss it. Name it here - the
            // last module unloaded right before STATUS_DLL_INIT_FAILED is the failing DLL.
            this->log.error("LEANDIAG module-unload name=%s base=%#llx\n", mod.name.c_str(),
                            (unsigned long long)mod.image_base);
            this->last_executed_section_ = {};
            const auto hooks = this->section_first_execution_hooks_.extract(mod.image_base);
            if (hooks)
            {
                for (auto* hook : hooks.mapped())
                {
                    if (hook)
                    {
                        this->emu().delete_hook(hook);
                    }
                }
            }
        });

        this->emu().hook_instruction(x86_hookable_instructions::syscall, [&](cpu_interface& cpu, uint64_t) {
            const auto lock_detail = kernel_lock::attribution_enabled()
                                         ? (static_cast<uint64_t>(cpu.index()) << 32) | static_cast<uint32_t>(this->vcpu(cpu.index()).cpu.reg<uint64_t>(x86_register::rax))
                                         : static_cast<uint64_t>(cpu.index());
            const kernel_lock::attribution_scope lock_site("syscall", lock_detail);
            const std::scoped_lock lock(this->kernel_lock_);
            auto& vcpu = this->vcpu(cpu.index());
            const scoped_dispatch dispatch(*this, vcpu);
            this->dispatcher.dispatch(*this, vcpu);
            return instruction_hook_continuation::skip_instruction;
        });

        this->emu().hook_instruction(x86_hookable_instructions::rdtscp, [&](cpu_interface& cpu, uint64_t) {
            const kernel_lock::attribution_scope lock_site("rdtscp", cpu.index());
            const std::scoped_lock lock(this->kernel_lock_);
            auto& vcpu = this->vcpu(cpu.index());
            const scoped_dispatch dispatch(*this, vcpu);
            auto& acting = vcpu.cpu;
            this->callbacks.on_rdtscp();

            const auto ticks = this->timestamp_counter_for_guest();
            acting.reg(x86_register::rax, static_cast<uint32_t>(ticks));
            acting.reg(x86_register::rdx, static_cast<uint32_t>(ticks >> 32));

            // Return the IA32_TSC_AUX value in RCX (low 32 bits)
            auto tsc_aux = 0; // Need to replace this with proper CPUID later
            acting.reg(x86_register::rcx, tsc_aux);

            return instruction_hook_continuation::skip_instruction;
        });

        this->emu().hook_instruction(x86_hookable_instructions::rdtsc, [&](cpu_interface& cpu, uint64_t) {
            const kernel_lock::attribution_scope lock_site("rdtsc", cpu.index());
            const std::scoped_lock lock(this->kernel_lock_);
            auto& vcpu = this->vcpu(cpu.index());
            const scoped_dispatch dispatch(*this, vcpu);
            auto& acting = vcpu.cpu;
            this->callbacks.on_rdtsc();

            const auto ticks = this->timestamp_counter_for_guest();
            acting.reg(x86_register::rax, static_cast<uint32_t>(ticks));
            acting.reg(x86_register::rdx, static_cast<uint32_t>(ticks >> 32));

            return instruction_hook_continuation::skip_instruction;
        });

        // TODO: Unicorn needs this - This should be handled in the backend
        this->emu().hook_instruction(x86_hookable_instructions::invalid, [&](cpu_interface& cpu, uint64_t) {
            const kernel_lock::attribution_scope lock_site("invalid_instruction", cpu.index());
            const std::scoped_lock lock(this->kernel_lock_);
            // TODO: Unify icicle & unicorn handling
            dispatch_illegal_instruction_violation(*this, this->vcpu(cpu.index()));
            return instruction_hook_continuation::skip_instruction; //
        });

        this->emu().hook_interrupt([&](cpu_interface& cpu, const int interrupt) {
            const std::scoped_lock lock(this->kernel_lock_);
            auto& vcpu = this->vcpu(cpu.index());
            const scoped_dispatch dispatch(*this, vcpu);
            auto& acting = vcpu.cpu;
            this->callbacks.on_exception();
            const auto eflags = acting.reg<uint32_t>(x86_register::eflags);

            switch (interrupt)
            {
            case 0:
                dispatch_integer_division_by_zero(*this, vcpu);
                return;
            case 1:
                if ((eflags & 0x100) != 0)
                {
                    acting.reg(x86_register::eflags, eflags & ~0x100);
                }

                this->callbacks.on_suspicious_activity("Singlestep");
                dispatch_single_step(*this, vcpu);
                return;
            case 3:
                this->callbacks.on_suspicious_activity("Breakpoint");
                dispatch_breakpoint(*this, vcpu);
                return;
            case 6:
                this->callbacks.on_suspicious_activity("Illegal instruction");
                dispatch_illegal_instruction_violation(*this, vcpu);
                return;
            case 13:
                dispatch_access_violation(*this, vcpu, std::numeric_limits<uint64_t>::max(), memory_operation::read);
                return;
            case 19: {
                const auto mxcsr = acting.reg<uint32_t>(x86_register::mxcsr);
                const auto pending = mxcsr & ~(mxcsr >> 7) & 0x3F;
                if (pending == 0)
                {
                    return;
                }
                DWORD status = STATUS_FLOAT_INEXACT_RESULT;
                if (acting.reg<uint16_t>(x86_register::cs) == 0x23)
                {
                    status = pending & 7 ? STATUS_FLOAT_MULTIPLE_FAULTS : STATUS_FLOAT_MULTIPLE_TRAPS;
                }
                else if ((pending & 1) != 0)
                {
                    status = STATUS_FLOAT_INVALID_OPERATION;
                }
                else if ((pending & 4) != 0)
                {
                    status = STATUS_FLOAT_DIVIDE_BY_ZERO;
                }
                else if ((pending & 2) != 0)
                {
                    // Windows reports an unmasked SIMD denormal as invalid operation (KiXmmException).
                    status = STATUS_FLOAT_INVALID_OPERATION;
                }
                else if ((pending & 8) != 0)
                {
                    status = STATUS_FLOAT_OVERFLOW;
                }
                else if ((pending & 0x10) != 0)
                {
                    status = STATUS_FLOAT_UNDERFLOW;
                }
                dispatch_exception(*this, vcpu, status, {0, mxcsr});
                return;
            }
            case 41:
                this->callbacks.on_fast_fail(acting.reg<uint32_t>(x86_register::ecx));
                this->process.exit_status = STATUS_FAIL_FAST_EXCEPTION;
                this->log.error("EXITDIAG FailFast tid=%u\n", (unsigned)GetCurrentThreadId());

                this->stop();
                return;
            case 45:
                this->callbacks.on_suspicious_activity("DbgPrint");
                {
                    const auto cs_selector = acting.reg<uint16_t>(x86_register::cs);
                    const auto bitness = segment_utils::get_segment_bitness(acting, cs_selector);
                    const auto service = acting.reg<uint32_t>(x86_register::eax);
                    if (service == BREAKPOINT_PRINT && bitness)
                    {
                        const auto bits32 = *bitness == segment_utils::segment_bitness::bit32;
                        this->callbacks.on_debug_print(bits32 ? acting.reg<uint32_t>(x86_register::ecx) : acting.reg(x86_register::rcx),
                                                       acting.reg<uint16_t>(x86_register::dx),
                                                       acting.reg<uint32_t>(bits32 ? x86_register::ebx : x86_register::r8d),
                                                       acting.reg<uint32_t>(bits32 ? x86_register::edi : x86_register::r9d));
                    }

                    if (bitness && *bitness == segment_utils::segment_bitness::bit64 &&
                        (service == BREAKPOINT_PRINT || service == BREAKPOINT_LOAD_SYMBOLS || service == BREAKPOINT_UNLOAD_SYMBOLS ||
                         service == BREAKPOINT_COMMAND_STRING))
                    {
                        const auto ip = this->uses_instruction_precision() //
                                            ? vcpu.thread().current_ip
                                            : acting.read_instruction_pointer();
                        acting.reg(x86_register::rip, ip + 3);
                    }
                    else
                    {
                        dispatch_breakpoint(*this, vcpu);
                    }
                }
                return;
            default:
                if (this->callbacks.on_generic_activity)
                {
                    this->callbacks.on_generic_activity("Interrupt " + std::to_string(interrupt));
                }

                break;
            }
        });

        this->emu().hook_memory_violation([&](cpu_interface& cpu, const uint64_t address, const size_t size,
                                              const memory_operation operation, const memory_violation_type type) {
            const kernel_lock::attribution_scope lock_site("memory_violation", cpu.index());
            const std::scoped_lock lock(this->kernel_lock_);
            auto& vcpu = this->vcpu(cpu.index());
            const scoped_dispatch dispatch(*this, vcpu);
            auto& acting = vcpu.cpu;
            if (acting.reg<uint16_t>(x86_register::cs) == 0x33)
            {
                // loading gs selector only works in 64-bit mode
                const auto required_gs_base = vcpu.thread().gs_segment->get_base();
                const auto actual_gs_base = acting.get_segment_base(x86_register::gs);
                if (actual_gs_base != required_gs_base)
                {
                    acting.set_segment_base(x86_register::gs, required_gs_base);
                    return memory_violation_continuation::restart;
                }
            }

            // A near-null guest read after a mapped GS read can mean the active TEB has lost
            // its Self/PEB pointers. Capture the first few such faults before exception
            // dispatch changes the guest stack. This is diagnostic only: no repair or mapping.
            if (type == memory_violation_type::unmapped && operation == memory_operation::read &&
                address < 0x1000 && acting.reg<uint16_t>(x86_register::cs) == 0x33 &&
                this->emu().get_name() == "icicle-emu")
            {
                static std::atomic<unsigned> emitted{0};
                if (emitted.fetch_add(1, std::memory_order_relaxed) < 8)
                {
                    try
                    {
                        const auto& thread = vcpu.thread();
                        const auto gs_base = acting.get_segment_base(x86_register::gs);
                        const auto expected_gs = thread.gs_segment ? thread.gs_segment->get_base() : 0;
                        const auto teb_base = thread.teb64 ? thread.teb64->value() : 0;
                        struct teb_fields
                        {
                            uint64_t self{};
                            uint64_t peb{};
                            bool self_ok{};
                            bool peb_ok{};
                        };
                        const auto read_fields = [gs_base](const memory_interface& view) {
                            teb_fields fields{};
                            if (gs_base && gs_base <= UINT64_MAX - 0x68)
                            {
                                fields.self_ok = view.try_read_memory(gs_base + 0x30, &fields.self, sizeof(fields.self));
                                fields.peb_ok = view.try_read_memory(gs_base + 0x60, &fields.peb, sizeof(fields.peb));
                            }
                            return fields;
                        };
                        // The CPU delegates memory operations through the machine. Compare both
                        // public read surfaces even though Icicle normally routes them to the same
                        // acting VM while this vCPU is inside a hook.
                        const auto cpu_fields = read_fields(acting.memory());
                        const auto manager_fields = read_fields(this->memory);
                        const auto teb_region = this->memory.get_region_info(gs_base);
                        std::fprintf(
                            stderr,
                            "[GSFAULT] tid=%u vcpu=%zu fault=%#llx rip=%#llx gs=%#llx expected_gs=%#llx teb=%#llx "
                            "rax=%#llx r14=%#llx cpu_self_ok=%d cpu_self=%#llx cpu_peb_ok=%d cpu_peb=%#llx "
                            "manager_self_ok=%d manager_self=%#llx manager_peb_ok=%d manager_peb=%#llx "
                            "region_start=%#llx region_len=%zu alloc=%#llx alloc_len=%zu reserved=%d committed=%d "
                            "perm=%u guard=%d kind=%u\n",
                            thread.id, acting.index(), static_cast<unsigned long long>(address),
                            static_cast<unsigned long long>(acting.read_instruction_pointer()),
                            static_cast<unsigned long long>(gs_base), static_cast<unsigned long long>(expected_gs),
                            static_cast<unsigned long long>(teb_base),
                            static_cast<unsigned long long>(acting.reg(x86_register::rax)),
                            static_cast<unsigned long long>(acting.reg(x86_register::r14)),
                            static_cast<int>(cpu_fields.self_ok), static_cast<unsigned long long>(cpu_fields.self),
                            static_cast<int>(cpu_fields.peb_ok), static_cast<unsigned long long>(cpu_fields.peb),
                            static_cast<int>(manager_fields.self_ok), static_cast<unsigned long long>(manager_fields.self),
                            static_cast<int>(manager_fields.peb_ok), static_cast<unsigned long long>(manager_fields.peb),
                            static_cast<unsigned long long>(teb_region.start), teb_region.length,
                            static_cast<unsigned long long>(teb_region.allocation_base), teb_region.allocation_length,
                            static_cast<int>(teb_region.is_reserved), static_cast<int>(teb_region.is_committed),
                            static_cast<unsigned>(teb_region.permissions.common),
                            static_cast<int>(teb_region.permissions.is_guarded()), static_cast<unsigned>(teb_region.kind));
                    }
                    catch (...)
                    {
                        std::fprintf(stderr, "[GSFAULT] diagnostic unavailable tid=%u fault=%#llx\n",
                                     vcpu.thread().id, static_cast<unsigned long long>(address));
                    }
                }
            }

            auto region = this->memory.get_region_info(address);
            this->callbacks.on_memory_violate(address, size, operation, type);
            if (region.permissions.is_guarded())
            {
                // Unset the GUARD_PAGE flag and dispatch a STATUS_GUARD_PAGE_VIOLATION
                this->memory.protect_memory(region.allocation_base, region.length, region.permissions & ~memory_permission_ext::guard);
                dispatch_guard_page_violation(*this, vcpu, address, operation);
            }
            else
            {
                dispatch_access_violation(*this, vcpu, address, operation);
            }

            return memory_violation_continuation::resume;
        });

        if (this->uses_instruction_precision())
        {
            this->emu().hook_memory_execution([&](cpu_interface& cpu, const uint64_t address) {
                const std::scoped_lock lock(this->kernel_lock_);
                auto& vcpu = this->vcpu(cpu.index());
                const scoped_dispatch dispatch(*this, vcpu);
                this->on_instruction_execution(vcpu, address); //
            });
        }
        else if (!this->emu().is_stop_thread_safe())
        {
            // The backend cannot be stopped safely from another thread, so the interrupt thread in start()
            // is not available for time-slicing. Preempt cooperatively from the CPU thread via a basic-block
            // hook instead.
            this->emu().hook_basic_block([&](cpu_interface& cpu, const basic_block& block) {
                const std::scoped_lock lock(this->kernel_lock_);
                this->on_basic_block_execution(this->vcpu(cpu.index()), block); //
            });
        }
    }

    void windows_emulator::on_basic_block_execution(vcpu_context& vcpu, const basic_block&)
    {
        auto& thread = vcpu.thread();

        // This path deliberately trades instruction precision for speed (one callback per block instead of
        // one per instruction), so we cannot account for individual instructions. Time-slice on a fixed number
        // of executed blocks instead.
        ++this->executed_instructions_;

        if (++thread.executed_blocks % MAX_BASIC_BLOCKS_PER_TIME_SLICE == 0)
        {
            this->yield_thread(vcpu);
        }
    }

    void windows_emulator::start(size_t count)
    {
        this->should_stop = false;
        this->last_stop_reason_ = stop_reason::none;
        this->last_stop_detail_.clear();
        this->setup_process_if_necessary();

        if (count > 0 && this->vcpu_count_ > 1)
        {
            throw std::invalid_argument("Instruction-count budgets require a single vCPU");
        }

        const auto use_count = count > 0;
        const auto start_instructions = this->get_executed_instructions();
        const auto target_instructions = start_instructions + count;

        std::mutex interrupt_mutex{};
        std::condition_variable interrupt_cond{};
        std::thread interrupt_thread{};
        std::thread lock_owner_monitor{};
        std::vector<std::thread> workers{};
        std::atomic<uint32_t> active_workers{0};
        std::unique_ptr<worker_phase_state[]> worker_phases{};
        if (kernel_lock::attribution_enabled() && this->vcpu_count_ > 1)
        {
            worker_phases = std::make_unique<worker_phase_state[]>(this->vcpu_count_);
        }

        const auto _ = utils::finally([&] {
            {
                std::unique_lock lock{interrupt_mutex};
                this->should_stop = true;
            }

            interrupt_cond.notify_all();

            for (uint32_t i = 0; i < this->vcpu_count_; ++i)
            {
                this->vcpu(i).cpu.stop();
            }

            for (auto& worker : workers)
            {
                if (worker.joinable())
                {
                    worker.join();
                }
            }

            if (interrupt_thread.joinable())
            {
                interrupt_thread.join();
            }

            if (lock_owner_monitor.joinable())
            {
                lock_owner_monitor.join();
            }
        });

        if (kernel_lock::attribution_enabled())
        {
            lock_owner_monitor = std::thread([this, &worker_phases] {
                uint64_t last_reported_generation = 0;
                while (!this->should_stop)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    const auto owner = this->kernel_lock_.current_owner();
                    if (owner && owner.held_nanos >= 2'000'000'000ULL && owner.generation != last_reported_generation)
                    {
                        last_reported_generation = owner.generation;
                        if (std::strcmp(owner.site, "syscall") == 0)
                        {
                            std::fprintf(stderr,
                                         "[KERNELLOCKOWNER] site=syscall vcpu=%llu syscall_id=0x%08X host_thread=%llu held_ms=%llu "
                                         "generation=%llu\n",
                                         static_cast<unsigned long long>(owner.detail >> 32), static_cast<uint32_t>(owner.detail),
                                         static_cast<unsigned long long>(owner.host_thread),
                                         static_cast<unsigned long long>(owner.held_nanos / 1'000'000ULL),
                                         static_cast<unsigned long long>(owner.generation));
                        }
                        else if (std::strcmp(owner.site, "vcpu_worker") == 0 && worker_phases && owner.detail < this->vcpu_count_)
                        {
                            const auto packed = worker_phases[owner.detail].value.load(std::memory_order_relaxed);
                            const auto phase = static_cast<worker_lock_phase>(packed & 0xff);
                            const auto phase_age_ms = packed ? static_cast<uint64_t>(GetTickCount64()) - (packed >> 8) : 0;
                            std::fprintf(stderr,
                                         "[KERNELLOCKOWNER] site=vcpu_worker vcpu=%llu phase=%s phase_age_ms=%llu "
                                         "host_thread=%llu held_ms=%llu generation=%llu\n",
                                         static_cast<unsigned long long>(owner.detail), worker_lock_phase_name(phase),
                                         static_cast<unsigned long long>(phase_age_ms), static_cast<unsigned long long>(owner.host_thread),
                                         static_cast<unsigned long long>(owner.held_nanos / 1'000'000ULL),
                                         static_cast<unsigned long long>(owner.generation));
                        }
                        else
                        {
                            std::fprintf(stderr, "[KERNELLOCKOWNER] site=%s detail=%llu host_thread=%llu held_ms=%llu generation=%llu\n",
                                         owner.site, static_cast<unsigned long long>(owner.detail),
                                         static_cast<unsigned long long>(owner.host_thread),
                                         static_cast<unsigned long long>(owner.held_nanos / 1'000'000ULL),
                                         static_cast<unsigned long long>(owner.generation));
                        }
                    }
                }
            });
        }

        if (!this->uses_instruction_precision() && this->emu().is_stop_thread_safe())
        {
            // LEAN preemption tick. With per-instruction hooks on, the stop flag is observed
            // every instruction; lean paths rely on this timer thread alone, so the tick IS
            // the scheduling quantum for pure-compute stretches. Env-tunable for the
            // preemption-granularity experiments (default 20 ms).
            const int preempt_ms = [] {
                const char* configured = std::getenv("SOGEN_PREEMPT_MS");
                return configured ? std::max(1, atoi(configured)) : 20;
            }();
            interrupt_thread = std::thread([&] {
                const kernel_lock::attribution_scope lock_site("preemption_timer");
                constexpr auto heartbeat_interval = std::chrono::milliseconds(1000);
                auto last_preemption = std::chrono::steady_clock::now();
                while (!this->should_stop)
                {
                    std::unique_lock lock{interrupt_mutex};
                    interrupt_cond.wait_for(lock, std::min(std::chrono::milliseconds(preempt_ms), heartbeat_interval), [&] {
                        return this->should_stop.load(); //
                    });

                    if (!this->should_stop)
                    {
                        // Under the kernel lock so this switch_thread/stop() pair can't straddle a vCPU's
                        // own scheduling step: perform_thread_switch consumes switch_thread (exchange to
                        // false) under the lock, and a preemption whose switch request lands before that
                        // consume while its stop() only lands inside the next quantum surfaces there as a
                        // stop with no pending switch - the exact shape of a fatal wind-down, tearing the
                        // whole run off at a random parked rip. Serialized against the scheduler, the pair
                        // lands either fully before the consume (plain early switch) or fully inside the
                        // running quantum (ordinary preemption), never split across it.
                        const std::scoped_lock kernel_lock(this->kernel_lock_);
                        uint32_t running_vcpus = 0;
                        bool running_thread_needs_switch = false;
                        for (const auto& v : this->vcpus_)
                        {
                            if (v->running.load(std::memory_order_relaxed))
                            {
                                ++running_vcpus;
                                running_thread_needs_switch |= v->active_thread && !v->active_thread->is_thread_ready(*this);
                            }
                        }

                        uint32_t ready_threads = 0;
                        for (auto& thread : this->process.threads | std::views::values)
                        {
                            ready_threads += thread.is_thread_ready(*this);
                        }

                        const auto now = std::chrono::steady_clock::now();
                        if (running_vcpus == 0)
                        {
                            last_preemption = now;
                            continue;
                        }

                        const bool heartbeat_due = now - last_preemption >= heartbeat_interval;
                        if (ready_threads <= running_vcpus && !running_thread_needs_switch && !heartbeat_due)
                        {
                            continue;
                        }

                        bool preempted = false;
                        for (uint32_t i = 0; i < this->vcpu_count_; ++i)
                        {
                            auto& v = this->vcpu(i);
                            if (!v.running.load(std::memory_order_relaxed))
                            {
                                continue;
                            }
                            v.switch_thread = true;
                            v.cpu.stop();
                            if (scheduler_profiling_enabled())
                            {
                                ++v.scheduler_profile.timer_preempt_requests;
                            }
                            preempted = true;
                        }
                        if (preempted)
                        {
                            last_preemption = now;
                        }
                    }
                }
            });
        }

        if (this->vcpu_count_ > 1)
        {
            // One worker thread per vCPU; this thread pumps UI events until the run ends.
            active_workers = this->vcpu_count_;
            workers.reserve(this->vcpu_count_);

            for (uint32_t i = 0; i < this->vcpu_count_; ++i)
            {
                workers.emplace_back([this, i, &active_workers, &worker_phases] {
                    current_worker_phase = worker_phases ? &worker_phases[i] : nullptr;
                    const auto clear_worker_phase = utils::finally([] { current_worker_phase = nullptr; });
                    try
                    {
                        this->vcpu_worker(this->vcpu(i));
                    }
                    catch (const std::exception& e)
                    {
                        this->log.error("vCPU %u worker terminated: %s\n", i, e.what());
                        this->stop();
                    }

                    --active_workers;
                });
            }

            while (active_workers.load() > 0)
            {
                this->ui_backend_->pump_events();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            this->dump_exception_trace();
            this->dump_lock_profile();
            this->dump_scheduler_profile();
            return;
        }

        auto& vcpu = this->vcpu(0);

        const kernel_lock::attribution_scope lock_site("main_vcpu", vcpu.cpu.index());
        std::unique_lock lock(this->kernel_lock_);

        while (!this->should_stop)
        {
            lock.unlock();
            this->ui_backend_->pump_events();
            lock.lock();

            if (vcpu.switch_thread || !vcpu.thread().is_thread_ready(*this))
            {
                if (!this->perform_thread_switch(vcpu, lock))
                {
                    break;
                }
            }

            vcpu.running.store(true, std::memory_order_relaxed);
            lock.unlock();
            {
                const auto clear_running = utils::finally([&vcpu] { vcpu.running.store(false, std::memory_order_relaxed); });
                const kernel_lock::guest_execution_scope guest_scope(this->kernel_lock_, this->uses_instruction_precision());
                this->start_cpu(vcpu, count);
            }
            lock.lock();

            this->emu().sync_worker_context(vcpu.cpu.index());
            this->publish_activity_status();

            if (!vcpu.switch_thread && !vcpu.cpu.has_violation())
            {
                break;
            }

            if (use_count)
            {
                const auto current_instructions = this->get_executed_instructions();

                if (current_instructions >= target_instructions)
                {
                    break;
                }

                count = static_cast<size_t>(target_instructions - current_instructions);
            }
        }

        this->dump_exception_trace();
        this->dump_lock_profile();
        this->dump_scheduler_profile();
    }

    void windows_emulator::deliver_raw_input(const process_context::raw_input_payload& payload, const hwnd explicit_target)
    {
        // Resolve the destination at delivery time: an explicit hwndTarget, else the foreground window
        // (raw input registered with a NULL target follows keyboard focus). If neither resolves to a live
        // window right now there is nowhere to deliver to, so skip without dropping the registration.
        const auto target = explicit_target != 0 ? explicit_target : this->process.foreground_window;
        auto* raw_win = this->process.windows.get(target);
        if (!raw_win)
        {
            return;
        }

        auto* thread = get_thread_by_id(this->process, raw_win->thread_id);
        if (!thread)
        {
            return;
        }

        const auto token = this->process.next_raw_input_token++;
        this->process.raw_inputs[token] = payload;

        // Bound the pending set: WM_INPUT is normally consumed at once by GetRawInputData, but if the guest
        // stops pumping, drop the oldest tokens so the map can't grow without limit.
        constexpr size_t max_pending_raw_inputs = 256;
        while (this->process.raw_inputs.size() > max_pending_raw_inputs)
        {
            this->process.raw_inputs.erase(this->process.raw_inputs.begin());
        }

        msg m{};
        m.window = target;
        m.message = WM_INPUT;
        m.wParam = RIM_INPUT;
        m.lParam = token;
        thread->post_message(*this, m);
    }

    void windows_emulator::deliver_raw_mouse_input(const int32_t dx, const int32_t dy, const uint16_t button_flags,
                                                   const uint16_t button_data)
    {
        this->deliver_raw_input({.keyboard = false, .dx = dx, .dy = dy, .mouse_buttons = button_flags, .mouse_button_data = button_data},
                                this->process.raw_mouse_target);
    }

    void windows_emulator::deliver_raw_keyboard_input(const uint16_t vkey, const uint16_t scan_code, const uint32_t message,
                                                      const bool extended)
    {
        this->deliver_raw_input({.keyboard = true, .vkey = vkey, .scan_code = scan_code, .key_message = message, .key_extended = extended},
                                this->process.raw_keyboard_target);
    }

    void windows_emulator::handle_ui_event(const ui_event& event)
    {
        const std::scoped_lock lock(this->kernel_lock_);

        const auto* win = this->process.windows.get(event.window);
        if (!win)
        {
            return;
        }

        auto* thread = get_thread_by_id(this->process, win->thread_id);
        if (!thread)
        {
            return;
        }

        msg m{};
        m.window = event.window;
        m.message = event.message;
        m.wParam = event.wParam;
        m.lParam = event.lParam;

        if (is_pointer_message(event.message))
        {
            // Track the cursor in screen coordinates (top-level window origin + window-local position) so
            // GetCursorPos reflects it, and treat the window the user is pointing at as the foreground one.
            int32_t new_cursor_x = this->process.cursor_x;
            int32_t new_cursor_y = this->process.cursor_y;
            if (const auto origin = get_window_origin_relative_to_ancestor(this->process, event.window, 0))
            {
                new_cursor_x = origin->x + point_x(event.lParam);
                new_cursor_y = origin->y + point_y(event.lParam);
            }

            // A game using raw input for mouse-look reads relative deltas from WM_INPUT, not WM_MOUSEMOVE.
            // Synthesize that delta from the change in tracked cursor position before updating it. A
            // SetCursorPos recenter pre-updates cursor_x/y, so the warp-echo motion event yields a zero
            // delta -- no spurious look -- while genuine motion between recenters produces the real delta.
            if (this->process.raw_mouse_registered)
            {
                if (event.message == WM_MOUSEMOVE)
                {
                    const int32_t dx = new_cursor_x - this->process.cursor_x;
                    const int32_t dy = new_cursor_y - this->process.cursor_y;
                    if (dx != 0 || dy != 0)
                    {
                        this->deliver_raw_mouse_input(dx, dy, 0);
                    }
                }
                else if (const uint16_t buttons = raw_mouse_button_flags(event.message, event.wParam); buttons != 0)
                {
                    // Raw-input games read button transitions and wheel deltas from WM_INPUT, not only
                    // the corresponding window messages. For wheel messages the delta lives in usButtonData.
                    this->deliver_raw_mouse_input(0, 0, buttons, raw_mouse_button_data(event.message, event.wParam));
                }
            }

            this->process.cursor_x = new_cursor_x;
            this->process.cursor_y = new_cursor_y;
            this->process.foreground_window = event.window;

            const auto target = route_pointer(this->process, event.window, point_x(event.lParam), point_y(event.lParam));
            m.window = target.window;
            // WM_MOUSEWHEEL/WM_MOUSEHWHEEL carry screen coordinates in lParam, unlike button/move
            // messages which carry client coordinates. Keep the routed target but preserve screen coords.
            m.lParam = is_mouse_wheel_message(event.message) ? pack_point(new_cursor_x, new_cursor_y) : pack_point(target.x, target.y);
        }
        else if ((event.message == WM_ACTIVATE && event.wParam != 0) || event.message == WM_SETFOCUS)
        {
            // The window just became active/focused: make it the foreground window so polling APIs
            // (GetForegroundWindow/GetActiveWindow) agree with the activation the game just received.
            this->process.foreground_window = event.window;
        }
        else if ((event.message == WM_ACTIVATE && event.wParam == 0) && this->process.foreground_window == event.window)
        {
            this->process.foreground_window = 0;
        }

        // Mirror the foreground window into the shared SERVERINFO so the guest's client-side
        // GetForegroundWindow (which reads gpsi directly, never syscalling) returns the active window.
        // Fall back to the desktop window when no app window is active: real Windows always has a
        // foreground window, and code that needs a valid HWND (e.g. DirectSound's SetCooperativeLevel,
        // which Miles feeds from GetForegroundWindow) breaks on a null one.
        const auto foreground =
            this->process.foreground_window != 0 ? this->process.foreground_window : this->process.default_desktop_window_handle.bits;
        this->process.user_handles.get_server_info().access([&](USER_SERVERINFO& server_info) {
            server_info.foregroundWindow = foreground; //
        });

        // Maintain the polled key state from key and mouse-button transitions. GetKeyState reports the high
        // down bit; GetAsyncKeyState also reports a low edge bit that is set once when a key transitions from
        // up to down and cleared by the next GetAsyncKeyState query for that virtual key.
        switch (event.message)
        {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const auto virtual_key = static_cast<uint8_t>(event.wParam & 0xFF);
            if ((this->process.key_state[virtual_key] & 0x80) == 0)
            {
                this->process.async_key_state[virtual_key] = 1;
            }
            this->process.key_state[virtual_key] = 0x80;
            break;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            this->process.key_state[static_cast<uint8_t>(event.wParam & 0xFF)] = 0;
            break;
        default:
            if (const auto virtual_key = mouse_button_virtual_key(event.message, event.wParam); virtual_key != 0)
            {
                if (is_mouse_button_down_message(event.message))
                {
                    if ((this->process.key_state[virtual_key] & 0x80) == 0)
                    {
                        this->process.async_key_state[virtual_key] = 1;
                    }
                    this->process.key_state[virtual_key] = 0x80;
                }
                else if (is_mouse_button_up_message(event.message))
                {
                    this->process.key_state[virtual_key] = 0;
                }
            }
            break;
        }

        // Raw-input games (e.g. Skyrim) read the keyboard via WM_INPUT/GetRawInputData, not WM_KEYDOWN.
        if (this->process.raw_keyboard_registered && is_keyboard_message(event.message))
        {
            const auto vk = static_cast<uint16_t>(event.wParam & 0xFFFF);
            auto scan_code = static_cast<uint16_t>((event.lParam >> 16) & 0xFF);
            if (scan_code == 0)
            {
                scan_code = vk_to_scan_code(vk);
            }

            const bool extended = (event.lParam & (1ull << 24)) != 0;
            this->deliver_raw_keyboard_input(vk, scan_code, event.message, extended);
        }

        thread->post_message(*this, m, true);

        if (event.message == WM_CLOSE || event.message == WM_COMMAND || is_key_down_message(event.message) ||
            is_mouse_button_message(event.message) || is_mouse_wheel_message(event.message))
        {
            // Kick the vCPU currently running the target thread so it promptly re-checks its message
            // queue; if the thread is not running on any vCPU right now, fall back to vCPU 0 to force a
            // reschedule that can pick up the now-ready thread.
            auto* running_on = find_vcpu_running_thread(*this, *thread);
            auto& vcpu = running_on ? *running_on : this->vcpu(0);
            vcpu.switch_thread = true;
            vcpu.cpu.stop();
        }
    }

    void windows_emulator::dump_exception_trace()
    {
        // Opt-in post-mortem aid for multi-vCPU debugging (see docs/multi-vcpu-design.md).
        const auto* enabled = std::getenv("SOGEN_TRACE_EXCEPTIONS");
        if (!enabled || enabled[0] != '1')
        {
            return;
        }

        const auto count = std::min(this->exception_trace_index_, this->exception_trace_.size());
        if (count == 0)
        {
            return;
        }

        const auto total = this->exception_trace_index_;
        const auto start = total - count;
        this->log.error("--- exception trace (last %zu of %zu) ---\n", count, total);

        const auto describe = [this](const uint64_t address) -> std::string {
            const auto* mod = this->mod_manager.find_by_address(address);
            const auto region = this->memory.get_region_info(address);
            std::array<char, 256> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%s+0x%llx [%s]", mod ? mod->name.c_str() : "?",
                          mod ? static_cast<unsigned long long>(address - mod->image_base) : address,
                          region.is_committed ? "committed" : "FREE/reserved");
            return buffer.data();
        };

        for (size_t i = 0; i < count; ++i)
        {
            const auto& e = this->exception_trace_[(start + i) % this->exception_trace_.size()];
            this->log.error("  [%zu] status 0x%08x vcpu %u tid %u\n      rip  0x%llx %s\n      addr 0x%llx %s\n", start + i, e.status,
                            e.vcpu, e.tid, static_cast<unsigned long long>(e.rip), describe(e.rip).c_str(),
                            static_cast<unsigned long long>(e.info), describe(e.info).c_str());
        }
    }

    bool windows_emulator::try_signal_guest_event(const handle event_handle)
    {
        if (!this->kernel_lock_.try_lock())
        {
            return false;
        }

        const std::lock_guard<kernel_lock> lock{this->kernel_lock_, std::adopt_lock};

        auto* entry = this->process.events.get(event_handle);
        if (!entry)
        {
            return false;
        }

        entry->signaled = true;
        return true;
    }

    void windows_emulator::dump_lock_profile()
    {
        if (!kernel_lock::profiling_enabled())
        {
            return;
        }

        const auto stats = this->kernel_lock_.profile();
        const auto held_ms = static_cast<double>(stats.held_nanos) / 1e6;
        const auto wait_ms = static_cast<double>(stats.wait_nanos) / 1e6;
        const auto contended_pct =
            stats.acquisitions ? (100.0 * static_cast<double>(stats.contended) / static_cast<double>(stats.acquisitions)) : 0.0;

        this->log.print(color::cyan,
                        "--- kernel lock (BEL) profile ---\n"
                        "  acquisitions:   %llu\n"
                        "  contended:      %llu (%.1f%%)\n"
                        "  wait time:      %.1f ms (blocked on a busy BEL)\n"
                        "  held time:      %.1f ms (BEL busy across all threads)\n",
                        static_cast<unsigned long long>(stats.acquisitions), static_cast<unsigned long long>(stats.contended),
                        contended_pct, wait_ms, held_ms);
    }

    void windows_emulator::dump_scheduler_profile()
    {
        if (!scheduler_profiling_enabled())
        {
            return;
        }

        const auto print = [this] {
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                const auto& stats = this->vcpus_[i]->scheduler_profile;
                this->log.print(
                    color::cyan,
                    "SCHEDPROFILE vcpu=%zu switches=%llu switch_ms=%.3f device_calls=%llu device_ms=%.3f "
                    "idle_retries=%llu idle_yields=%llu idle_sleeps=%llu relative_ticks=%llu timer_preempts=%llu\n",
                    i, static_cast<unsigned long long>(stats.context_switch_calls), static_cast<double>(stats.context_switch_nanos) / 1e6,
                    static_cast<unsigned long long>(stats.device_work_calls), static_cast<double>(stats.device_work_nanos) / 1e6,
                    static_cast<unsigned long long>(stats.idle_retries), static_cast<unsigned long long>(stats.idle_host_yields),
                    static_cast<unsigned long long>(stats.idle_host_sleeps), static_cast<unsigned long long>(stats.idle_relative_ticks),
                    static_cast<unsigned long long>(stats.timer_preempt_requests));
            }
        };
        if (this->kernel_lock_.is_held_by_current_thread())
        {
            print();
        }
        else
        {
            const std::scoped_lock lock(this->kernel_lock_);
            print();
        }
    }

    void windows_emulator::stop()
    {
        this->should_stop = true;

        for (uint32_t i = 0; i < this->vcpu_count_; ++i)
        {
            this->vcpu(i).cpu.stop();
        }
    }

    void windows_emulator::register_factories(utils::buffer_deserializer& buffer)
    {
        buffer.register_factory<memory_manager_wrapper>([this] {
            return memory_manager_wrapper{this->memory}; //
        });

        buffer.register_factory<module_manager_wrapper>([this] {
            return module_manager_wrapper{this->mod_manager}; //
        });

        buffer.register_factory<x64_emulator_wrapper>([this] {
            return x64_emulator_wrapper{this->emu()}; //
        });

        buffer.register_factory<windows_emulator_wrapper>([this] {
            return windows_emulator_wrapper{*this}; //
        });

        buffer.register_factory<clock_wrapper>([this] {
            return clock_wrapper{this->clock()}; //
        });

        buffer.register_factory<socket_factory_wrapper>([this] {
            return socket_factory_wrapper{this->socket_factory()}; //
        });

        buffer.register_factory<window>([this] {
            return window{this->emu()}; //
        });
    }

    namespace
    {
        constexpr uint64_t steady_clock_snapshot_marker = 0x314B434F4C435453ULL;

        void restore_snapshot_deadlines(windows_emulator& emulator, utils::buffer_deserializer& buffer)
        {
            std::chrono::steady_clock::time_point saved_steady_time{};
            if (buffer.get_remaining_size())
            {
                if (buffer.read<uint64_t>() != steady_clock_snapshot_marker)
                {
                    throw std::runtime_error("Invalid steady clock snapshot extension");
                }
                saved_steady_time = std::chrono::steady_clock::time_point{
                    std::chrono::steady_clock::duration{buffer.read<std::chrono::steady_clock::duration::rep>()}};
            }
            else
            {
                // Older snapshots persisted absolute host-monotonic deadlines without an anchor. The saved
                // KUSER_SHARED_DATA InterruptTime is the last sampled value of the same guest steady clock.
                const auto ticks = emulator.process.kusd.access([](const KUSER_SHARED_DATA64& kusd) {
                    return (static_cast<uint64_t>(static_cast<uint32_t>(kusd.InterruptTime.High1Time)) << 32) |
                           kusd.InterruptTime.LowPart;
                });
                saved_steady_time = std::chrono::steady_clock::time_point{std::chrono::nanoseconds{ticks * 100}};
            }

            if (!emulator.uses_relative_time())
            {
                emulator.process.rebase_steady_deadlines(emulator.clock().steady_now() - saved_steady_time);
            }
        }
    }

    void windows_emulator::serialize(utils::buffer_serializer& buffer) const
    {
        const auto saved_steady_time = this->clock_->steady_now();
        buffer.write(this->application_settings_);
        buffer.write(this->setup_completed_);
        buffer.write(this->get_executed_instructions());
        buffer.write_atomic(this->vcpus_[0]->switch_thread);
        buffer.write(this->use_relative_time_);

        this->version.serialize(buffer);
        this->registry.serialize_runtime_state(buffer);

        // Backend snapshot mode is not used here; Unicorn's in-place snapshot path is broken.
        this->emu().serialize_state(buffer, false);
        this->memory.serialize_memory_state(buffer, false);
        this->mod_manager.serialize(buffer);
        this->dispatcher.serialize(buffer);
        this->process.serialize(buffer, this->vcpus_[0]->active_thread);
        this->memory.serialize_aslr_state(buffer);
        this->cng_changes.serialize(buffer);
        buffer.write(steady_clock_snapshot_marker);
        buffer.write(saved_steady_time.time_since_epoch().count());
    }

    void windows_emulator::deserialize(utils::buffer_deserializer& buffer)
    {
        if (this->ui().native_presentation_active())
        {
            throw std::runtime_error("Cannot restore over active native Vulkan presentation; use a fresh pre-GPU emulator");
        }

        this->register_factories(buffer);

        buffer.read(this->application_settings_);
        buffer.read(this->setup_completed_);
        buffer.read(this->executed_instructions_);
        buffer.read_atomic(this->vcpus_[0]->switch_thread);

        const auto old_relative_time = this->use_relative_time_;
        buffer.read(this->use_relative_time_);

        if (old_relative_time != this->use_relative_time_)
        {
            throw std::runtime_error("Can not deserialize emulator with different time dimensions");
        }

        this->version.deserialize(buffer);
        this->registry.deserialize_runtime_state(buffer);

        this->process.prepare_for_state_restore(*this);
        this->memory.unmap_all_memory();
        this->clear_section_first_execution_hooks();
        this->ui().reset();
        this->audio().stop();

        // Match raw serialize() above; do not use backend snapshot mode here.
        this->emu().deserialize_state(buffer, false);
        this->memory.deserialize_memory_state(buffer, false);
        this->mod_manager.deserialize(buffer);
        this->install_section_first_execution_hooks();
        this->dispatcher.deserialize(buffer);
        this->process.deserialize(buffer, this->vcpus_[0]->active_thread);
        this->configure_xstate();
        this->memory.deserialize_aslr_state(buffer,
                                            this->mod_manager.executable && (this->mod_manager.executable->dll_characteristics &
                                                                             IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA) != 0,
                                            this->process.is_wow64_process, this->uses_relative_time());
        this->cng_changes.deserialize(buffer, *this);
        restore_snapshot_deadlines(*this, buffer);
        this->process.restore_after_state_restore(*this);
    }

    void windows_emulator::configure_xstate()
    {
        if (this->emu().get_name() != "icicle-emu")
        {
            return;
        }
        this->process.kusd.access([](KUSER_SHARED_DATA64& kusd) {
            kusd.XState = {};
            kusd.XState.EnabledFeatures = 3;
            kusd.XState.EnabledVolatileFeatures = 3;
            kusd.XState.Size = 576;
            kusd.XState.Features[0] = {.Offset = 0, .Size = 160};
            kusd.XState.Features[1] = {.Offset = 160, .Size = 256};
        });
    }

    void windows_emulator::save_snapshot()
    {
        utils::buffer_serializer buffer{};
        const auto saved_steady_time = this->clock_->steady_now();

        buffer.write(this->setup_completed_);
        buffer.write(this->get_executed_instructions());
        buffer.write_atomic(this->vcpus_[0]->switch_thread);

        this->version.serialize(buffer);
        this->registry.serialize_runtime_state(buffer);

        // Snapshot path still uses regular backend state serialization.
        // Backend snapshot mode (is_snapshot=true) is not reliable yet.
        this->emu().serialize_state(buffer, false);
        this->memory.serialize_memory_state(buffer, false);
        this->mod_manager.serialize(buffer);
        this->dispatcher.serialize(buffer);
        this->process.serialize(buffer, this->vcpus_[0]->active_thread);
        this->memory.serialize_aslr_state(buffer);
        this->cng_changes.serialize(buffer);
        buffer.write(steady_clock_snapshot_marker);
        buffer.write(saved_steady_time.time_since_epoch().count());

        this->process_snapshot_ = buffer.move_buffer();
    }

    void windows_emulator::restore_snapshot()
    {
        if (this->ui().native_presentation_active())
        {
            throw std::runtime_error("Cannot restore over active native Vulkan presentation; use a fresh pre-GPU emulator");
        }

        if (this->process_snapshot_.empty())
        {
            throw std::runtime_error("No snapshot saved");
        }

        utils::buffer_deserializer buffer{this->process_snapshot_};

        this->register_factories(buffer);

        buffer.read(this->setup_completed_);
        buffer.read(this->executed_instructions_);
        buffer.read_atomic(this->vcpus_[0]->switch_thread);

        this->version.deserialize(buffer);
        this->registry.deserialize_runtime_state(buffer);

        this->process.prepare_for_state_restore(*this);
        this->memory.unmap_all_memory();
        this->clear_section_first_execution_hooks();
        this->ui().reset();
        this->audio().stop();

        this->emu().deserialize_state(buffer, false);
        this->memory.deserialize_memory_state(buffer, false);
        this->mod_manager.deserialize(buffer);
        this->install_section_first_execution_hooks();
        this->dispatcher.deserialize(buffer);
        this->process.deserialize(buffer, this->vcpus_[0]->active_thread);
        this->configure_xstate();
        this->memory.deserialize_aslr_state(buffer,
                                            this->mod_manager.executable && (this->mod_manager.executable->dll_characteristics &
                                                                             IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA) != 0,
                                            this->process.is_wow64_process, this->uses_relative_time());
        this->cng_changes.deserialize(buffer, *this);
        restore_snapshot_deadlines(*this, buffer);
        this->process.restore_after_state_restore(*this);
    }

} // namespace sogen
