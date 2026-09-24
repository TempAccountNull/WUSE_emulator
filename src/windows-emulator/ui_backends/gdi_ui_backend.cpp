#include "../std_include.hpp"
#include "gdi_surface_math.hpp"
#include <platform/ui_backend.hpp>

#ifdef OS_WINDOWS
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr wchar_t class_name[] = L"SogenGdiSoftwareSurface";
        constexpr UINT destroy_after_backend = WM_APP + 0x451;

        std::wstring wide(const std::u16string_view value)
        {
            return {value.begin(), value.end()};
        }

        uint64_t pack_point(const int x, const int y)
        {
            return static_cast<uint16_t>(x) | (static_cast<uint64_t>(static_cast<uint16_t>(y)) << 16);
        }

        class gdi_ui_backend final : public ui_backend
        {
          private:
            // The HWND owns a separate reference until WM_NCDESTROY. Its window procedure
            // never follows a backend pointer after the destructor detaches it.
            struct dispatch_state
            {
                std::recursive_mutex mutex;
                gdi_ui_backend* backend{};
            };

          public:
            ~gdi_ui_backend() override
            {
                std::vector<HWND> hosts;
                {
                    std::lock_guard lifecycle_lock(lifecycle_mutex_);
                    shutting_down_ = true;
                    for (const auto& [guest, state] : windows_)
                    {
                        (void)guest;
                        if (state.dispatch)
                        {
                            std::lock_guard dispatch_lock(state.dispatch->mutex);
                            state.dispatch->backend = nullptr;
                        }
                        if (state.host) hosts.push_back(state.host);
                    }
                }
                for (const auto host : hosts)
                {
                    if (owner_thread_ == std::this_thread::get_id())
                    {
                        DestroyWindow(host);
                    }
                    else
                    {
                        DWORD_PTR ignored{};
                        if (!SendMessageTimeoutW(host, destroy_after_backend, 0, 0,
                                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored))
                        {
                            // The owner may have stopped pumping. The detached dispatch state
                            // remains HWND-owned, so delayed messages cannot access this backend.
                            PostMessageW(host, destroy_after_backend, 0, 0);
                        }
                    }
                }
            }

            void set_event_sink(event_sink sink) override
            {
                enqueue([this, sink = std::move(sink)]() mutable { sink_ = std::move(sink); });
            }

            void pump_events() override
            {
                std::lock_guard lifecycle_lock(lifecycle_mutex_);
                if (owner_thread_ == std::thread::id{})
                {
                    owner_thread_ = std::this_thread::get_id();
                }
                if (owner_thread_ != std::this_thread::get_id())
                {
                    return;
                }
                if (!shutting_down_)
                {
                    std::vector<std::function<void()>> commands;
                    {
                        std::lock_guard lock(command_mutex_);
                        commands.swap(commands_);
                    }
                    for (auto& command : commands)
                    {
                        command();
                    }
                }
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (shutting_down_) return;
                auto events = std::move(events_);
                events_.clear();
                for (const auto& event : events)
                {
                    if (sink_)
                    {
                        sink_(event);
                    }
                }
            }

            // This backend presents software bitmaps only. The inherited native-presentation
            // methods explicitly report that Vulkan/DXVK host-window leases are unavailable.
            std::shared_ptr<ui_completion_queue> activate_native_presentation() override { return {}; }
            native_window_acquisition acquire_native_window_on_ui(uint64_t) override
            {
                return {.status = native_window_acquire_status::native_target_unavailable, .lease = {}};
            }

            void reset() override
            {
                enqueue([this] {
                    std::vector<hwnd> guests = order_;
                    for (const auto guest : guests)
                    {
                        destroy_impl(guest);
                    }
                    events_.clear();
                });
            }

            void create_window(const ui_window_desc& desc) override
            {
                enqueue([this, desc] { create_impl(desc); });
            }

            void destroy_window(const hwnd guest) override
            {
                enqueue([this, guest] { destroy_impl(guest); });
            }

            void set_window_rect(const hwnd guest, const RECT& rect) override
            {
                enqueue([this, guest, rect] {
                    if (auto* state = find(guest))
                    {
                        state->desc.rect = rect;
                        if (state->host)
                        {
                            resize_host(*state);
                        }
                        invalidate_top(guest);
                    }
                });
            }

            void set_window_visible(const hwnd guest, const bool visible) override
            {
                enqueue([this, guest, visible] {
                    if (auto* state = find(guest))
                    {
                        state->desc.visible = visible;
                        if (state->host) ShowWindow(state->host, visible ? SW_SHOW : SW_HIDE);
                        invalidate_top(guest);
                    }
                });
            }

            void set_window_enabled(const hwnd guest, const bool enabled) override
            {
                enqueue([this, guest, enabled] {
                    if (auto* state = find(guest))
                    {
                        state->desc.enabled = enabled;
                        if (state->host) EnableWindow(state->host, enabled);
                    }
                });
            }

            void set_window_title(const hwnd guest, std::u16string_view title) override
            {
                enqueue([this, guest, title = std::u16string{title}] {
                    if (auto* state = find(guest))
                    {
                        state->desc.title = title;
                        if (state->host) SetWindowTextW(state->host, wide(title).c_str());
                    }
                });
            }

            void invalidate(const hwnd guest, const std::optional<RECT>&) override
            {
                enqueue([this, guest] { invalidate_top(guest); });
            }

            void present_surface(const hwnd guest, const ui_surface_desc& source) override
            {
                // The caller owns source.pixels only for this call. Keep an owned, tightly packed
                // top-down BGRA copy before the queued UI-thread command can outlive it.
                auto frame = gdi_detail::copy_surface(source);
                if (!frame)
                {
                    return;
                }
                enqueue([this, guest, frame = std::make_shared<gdi_detail::surface>(std::move(*frame))] {
                    if (auto* state = find(guest))
                    {
                        state->frame = frame;
                        invalidate_top(guest);
                    }
                });
            }

            void set_cursor_position(const hwnd guest, const int32_t x, const int32_t y) override
            {
                enqueue([this, guest, x, y] {
                    const auto top = top_level(guest);
                    const auto* state = find(top);
                    if (!state || !state->host) return;
                    RECT client{};
                    GetClientRect(state->host, &client);
                    const auto width = state->frame ? state->frame->width : client.right;
                    const auto height = state->frame ? state->frame->height : client.bottom;
                    const auto fit = gdi_detail::letterbox(client.right, client.bottom, width, height);
                    POINT point{fit.x + static_cast<int>(static_cast<int64_t>(x - state->desc.rect.left) * fit.width / std::max<int>(width, 1)),
                                fit.y + static_cast<int>(static_cast<int64_t>(y - state->desc.rect.top) * fit.height / std::max<int>(height, 1))};
                    ClientToScreen(state->host, &point);
                    SetCursorPos(point.x, point.y);
                });
            }

            void set_cursor_visibility(const bool visible) override
            {
                enqueue([this, visible] { cursor_visible_ = visible; });
            }

          private:
            struct window_state
            {
                ui_window_desc desc{};
                HWND host{};
                std::shared_ptr<dispatch_state> dispatch{};
                std::shared_ptr<gdi_detail::surface> frame{};
            };

            void enqueue(std::function<void()> command)
            {
                std::lock_guard lock(command_mutex_);
                commands_.push_back(std::move(command));
            }

            window_state* find(const hwnd guest)
            {
                const auto it = windows_.find(guest);
                return it == windows_.end() ? nullptr : &it->second;
            }

            const window_state* find(const hwnd guest) const
            {
                const auto it = windows_.find(guest);
                return it == windows_.end() ? nullptr : &it->second;
            }

            hwnd top_level(hwnd guest) const
            {
                for (int depth = 0; depth < 32 && guest; ++depth)
                {
                    const auto* state = find(guest);
                    if (!state) return 0;
                    if (state->desc.top_level) return guest;
                    guest = state->desc.parent;
                }
                return 0;
            }

            std::optional<POINT> child_origin(hwnd guest, const hwnd top) const
            {
                POINT origin{};
                for (int depth = 0; depth < 32 && guest && guest != top; ++depth)
                {
                    const auto* state = find(guest);
                    if (!state) return std::nullopt;
                    origin.x += state->desc.rect.left;
                    origin.y += state->desc.rect.top;
                    guest = state->desc.parent;
                }
                return guest == top ? std::optional<POINT>{origin} : std::nullopt;
            }

            void invalidate_top(const hwnd guest)
            {
                if (const auto* top = find(top_level(guest)); top && top->host)
                {
                    InvalidateRect(top->host, nullptr, FALSE);
                }
            }

            void resize_host(const window_state& state)
            {
                const auto& d = state.desc;
                const auto width = std::max(1, static_cast<int>(d.rect.right - d.rect.left) - d.client_insets.left - d.client_insets.right);
                const auto height = std::max(1, static_cast<int>(d.rect.bottom - d.rect.top) - d.client_insets.top - d.client_insets.bottom);
                RECT outer{0, 0, width, height};
                const DWORD style = (d.style & WS_CAPTION) == WS_CAPTION ? WS_OVERLAPPEDWINDOW : WS_POPUP;
                AdjustWindowRectEx(&outer, style, FALSE, 0);
                SetWindowPos(state.host, nullptr, d.rect.left, d.rect.top, outer.right - outer.left, outer.bottom - outer.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }

            void create_impl(const ui_window_desc& desc)
            {
                destroy_impl(desc.handle);
                auto& state = windows_[desc.handle];
                state.desc = desc;
                order_.push_back(desc.handle);
                if (!desc.top_level) return;
                static std::once_flag registered;
                std::call_once(registered, [] {
                    WNDCLASSEXW cls{};
                    cls.cbSize = sizeof(cls);
                    cls.lpfnWndProc = &gdi_ui_backend::window_proc;
                    cls.hInstance = GetModuleHandleW(nullptr);
                    cls.lpszClassName = class_name;
                    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
                    RegisterClassExW(&cls);
                });
                const DWORD style = (desc.style & WS_CAPTION) == WS_CAPTION ? WS_OVERLAPPEDWINDOW : WS_POPUP;
                const auto title = wide(desc.title);
                const auto width = std::max(1, static_cast<int>(desc.rect.right - desc.rect.left) - desc.client_insets.left - desc.client_insets.right);
                const auto height = std::max(1, static_cast<int>(desc.rect.bottom - desc.rect.top) - desc.client_insets.top - desc.client_insets.bottom);
                RECT outer{0, 0, width, height};
                AdjustWindowRectEx(&outer, style, FALSE, 0);
                state.dispatch = std::make_shared<dispatch_state>();
                state.dispatch->backend = this;
                state.host = CreateWindowExW(0, class_name, title.c_str(), style, desc.rect.left, desc.rect.top,
                                             outer.right - outer.left, outer.bottom - outer.top, nullptr, nullptr,
                                             GetModuleHandleW(nullptr), &state.dispatch);
                if (state.host)
                {
                    guest_by_host_[state.host] = desc.handle;
                    EnableWindow(state.host, desc.enabled);
                    if (desc.visible) ShowWindow(state.host, SW_SHOW);
                }
            }

            void destroy_impl(const hwnd guest)
            {
                auto it = windows_.find(guest);
                if (it == windows_.end()) return;
                const auto top = top_level(guest);
                const auto host = it->second.host;
                order_.erase(std::remove(order_.begin(), order_.end(), guest), order_.end());
                windows_.erase(it);
                if (host)
                {
                    guest_by_host_.erase(host);
                    DestroyWindow(host);
                }
                else if (const auto* root = find(top); root && root->host) InvalidateRect(root->host, nullptr, FALSE);
            }

            static void blit(HDC dc, const gdi_detail::surface& frame, const RECT& dest)
            {
                BITMAPINFO bitmap{};
                bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bitmap.bmiHeader.biWidth = frame.width;
                bitmap.bmiHeader.biHeight = -frame.height;
                bitmap.bmiHeader.biPlanes = 1;
                bitmap.bmiHeader.biBitCount = 32;
                bitmap.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(dc, COLORONCOLOR);
                StretchDIBits(dc, dest.left, dest.top, dest.right - dest.left, dest.bottom - dest.top,
                              0, 0, frame.width, frame.height, frame.bgra.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
            }

            void paint(const HWND host)
            {
                PAINTSTRUCT ps{};
                const auto dc = BeginPaint(host, &ps);
                if (!dc) return;
                RECT client{};
                GetClientRect(host, &client);
                FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                const auto hit = guest_by_host_.find(host);
                const auto top = hit == guest_by_host_.end() ? 0 : hit->second;
                const auto* root = find(top);
                if (root)
                {
                    const auto width = root->frame ? root->frame->width : std::max(1L, client.right);
                    const auto height = root->frame ? root->frame->height : std::max(1L, client.bottom);
                    const auto fit = gdi_detail::letterbox(client.right, client.bottom, width, height);
                    if (root->frame)
                    {
                        const RECT destination{fit.x, fit.y, fit.x + fit.width, fit.y + fit.height};
                        blit(dc, *root->frame, destination);
                    }
                    const auto saved = SaveDC(dc);
                    IntersectClipRect(dc, fit.x, fit.y, fit.x + fit.width, fit.y + fit.height);
                    for (const auto guest : order_)
                    {
                        const auto* child = find(guest);
                        if (!child || !child->frame || !child->desc.visible || guest == top || top_level(guest) != top) continue;
                        const auto origin = child_origin(guest, top);
                        if (!origin) continue;
                        const auto child_width = child->desc.rect.right - child->desc.rect.left;
                        const auto child_height = child->desc.rect.bottom - child->desc.rect.top;
                        const RECT destination{
                            fit.x + static_cast<int>(static_cast<int64_t>(origin->x) * fit.width / width),
                            fit.y + static_cast<int>(static_cast<int64_t>(origin->y) * fit.height / height),
                            fit.x + static_cast<int>(static_cast<int64_t>(origin->x + child_width) * fit.width / width),
                            fit.y + static_cast<int>(static_cast<int64_t>(origin->y + child_height) * fit.height / height)};
                        blit(dc, *child->frame, destination);
                    }
                    RestoreDC(dc, saved);
                }
                EndPaint(host, &ps);
            }

            void post(const hwnd guest, const UINT message, const WPARAM w, const LPARAM l)
            {
                if (guest) events_.push_back({guest, message, static_cast<uint64_t>(w), static_cast<uint64_t>(l)});
            }

            void mouse_event(HWND host, const UINT message, WPARAM w, LPARAM l)
            {
                const auto hit = guest_by_host_.find(host);
                if (hit == guest_by_host_.end()) return;
                const auto top = hit->second;
                const auto* root = find(top);
                if (!root) return;
                POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) ScreenToClient(host, &point);
                RECT client{};
                GetClientRect(host, &client);
                const auto width = root->frame ? root->frame->width : std::max(1L, client.right);
                const auto height = root->frame ? root->frame->height : std::max(1L, client.bottom);
                const auto fit = gdi_detail::letterbox(client.right, client.bottom, width, height);
                int x = gdi_detail::map_coordinate(point.x, fit.x, fit.width, width);
                int y = gdi_detail::map_coordinate(point.y, fit.y, fit.height, height);
                hwnd target = top;
                for (auto it = order_.rbegin(); it != order_.rend(); ++it)
                {
                    const auto* child = find(*it);
                    if (!child || !child->desc.visible || !child->desc.enabled || *it == top || top_level(*it) != top) continue;
                    const auto origin = child_origin(*it, top);
                    if (!origin) continue;
                    if (x >= origin->x && y >= origin->y && x < origin->x + child->desc.rect.right - child->desc.rect.left &&
                        y < origin->y + child->desc.rect.bottom - child->desc.rect.top)
                    {
                        target = *it;
                        x -= origin->x;
                        y -= origin->y;
                        break;
                    }
                }
                post(target, message, w, static_cast<LPARAM>(pack_point(x, y)));
            }

            static LRESULT CALLBACK window_proc(HWND host, UINT message, WPARAM w, LPARAM l)
            {
                if (message == WM_NCCREATE)
                {
                    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l);
                    const auto* dispatch = static_cast<const std::shared_ptr<dispatch_state>*>(create->lpCreateParams);
                    SetWindowLongPtrW(host, GWLP_USERDATA,
                                      reinterpret_cast<LONG_PTR>(new std::shared_ptr<dispatch_state>(*dispatch)));
                }
                auto* holder = reinterpret_cast<std::shared_ptr<dispatch_state>*>(GetWindowLongPtrW(host, GWLP_USERDATA));
                if (message == WM_NCDESTROY)
                {
                    SetWindowLongPtrW(host, GWLP_USERDATA, 0);
                    const auto result = DefWindowProcW(host, message, w, l);
                    delete holder;
                    return result;
                }
                if (message == destroy_after_backend)
                {
                    DestroyWindow(host);
                    return 0;
                }
                if (!holder) return DefWindowProcW(host, message, w, l);
                const auto dispatch = *holder;
                std::lock_guard dispatch_lock(dispatch->mutex);
                auto* self = dispatch->backend;
                if (!self) return DefWindowProcW(host, message, w, l);
                const auto it = self->guest_by_host_.find(host);
                const auto guest = it == self->guest_by_host_.end() ? 0 : it->second;
                switch (message)
                {
                case WM_PAINT: self->paint(host); return 0;
                case WM_ERASEBKGND: return 1;
                case WM_SIZE: InvalidateRect(host, nullptr, FALSE); return 0;
                case WM_CLOSE: self->post(guest, WM_CLOSE, 0, 0); return 0;
                case WM_SETFOCUS: self->post(guest, WM_SETFOCUS, 0, 0); self->post(guest, WM_ACTIVATE, WA_ACTIVE, 0); return 0;
                case WM_KILLFOCUS: self->post(guest, WM_ACTIVATE, WA_INACTIVE, 0); self->post(guest, WM_KILLFOCUS, 0, 0); return 0;
                case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
                    self->post(guest, message, w, l); return 0;
                case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                    self->mouse_event(host, message, w, l); return 0;
                case WM_SETCURSOR:
                    SetCursor(self->cursor_visible_ ? LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) : nullptr);
                    return TRUE;
                default: return DefWindowProcW(host, message, w, l);
                }
            }

            std::mutex lifecycle_mutex_;
            bool shutting_down_{};
            std::mutex command_mutex_;
            std::vector<std::function<void()>> commands_;
            std::vector<ui_event> events_;
            std::unordered_map<hwnd, window_state> windows_;
            std::unordered_map<HWND, hwnd> guest_by_host_;
            std::vector<hwnd> order_;
            event_sink sink_;
            std::thread::id owner_thread_{};
            bool cursor_visible_{true};
        };
    }

    std::unique_ptr<ui_backend> create_gdi_ui_backend()
    {
        return std::make_unique<gdi_ui_backend>();
    }
}
#endif
