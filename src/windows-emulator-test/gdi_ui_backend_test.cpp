#include <gtest/gtest.h>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201) // Existing NT structure declarations use anonymous unions.
#endif
#include <platform/ui_backend.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../windows-emulator/ui_backends/gdi_surface_math.hpp"

#ifdef OS_WINDOWS
#include <windows.h>
#include <algorithm>
#include <thread>
#include <vector>

namespace sogen::test
{
    TEST(GdiUiBackend, CopiesPaddedBgraAndRgbaRows)
    {
        const uint8_t bgra[]{1, 2, 3, 4, 9, 9, 9, 9, 5, 6, 7, 8, 9, 9, 9, 9};
        const auto copied = gdi_detail::copy_surface({1, 2, 8, ui_surface_format::bgra8, bgra});
        ASSERT_TRUE(copied);
        EXPECT_EQ(copied->bgra, (std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));
        const uint8_t rgba[]{3, 2, 1, 4};
        const auto converted = gdi_detail::copy_surface({1, 1, 4, ui_surface_format::rgba8, rgba});
        ASSERT_TRUE(converted);
        EXPECT_EQ(converted->bgra, (std::vector<uint8_t>{1, 2, 3, 4}));
        EXPECT_FALSE(gdi_detail::copy_surface({2, 1, 4, ui_surface_format::bgra8, bgra}));
    }

    TEST(GdiUiBackend, LetterboxesAndMapsCoordinates)
    {
        const auto fit = gdi_detail::letterbox(200, 100, 100, 100);
        EXPECT_EQ(fit.x, 50);
        EXPECT_EQ(fit.y, 0);
        EXPECT_EQ(fit.width, 100);
        EXPECT_EQ(fit.height, 100);
        EXPECT_EQ(gdi_detail::map_coordinate(75, fit.x, fit.width, 100), 25);
    }

    TEST(GdiUiBackend, RoutesChildMouseAndCloseWithoutNativePresentation)
    {
        auto backend = create_gdi_ui_backend();
        std::vector<ui_event> events;
        backend->set_event_sink([&](const ui_event& event) { events.push_back(event); });
        ui_window_desc top{};
        top.handle = 0x111;
        top.rect = RECT{0, 0, 100, 100};
        top.title = u"Sogen GDI test surface";
        top.top_level = true;
        top.visible = false;
        backend->create_window(top);
        ui_window_desc child{};
        child.handle = 0x222;
        child.parent = top.handle;
        child.rect = RECT{10, 10, 40, 40};
        child.visible = true;
        backend->create_window(child);
        backend->pump_events();
        const auto host = FindWindowW(L"SogenGdiSoftwareSurface", L"Sogen GDI test surface");
        ASSERT_NE(host, nullptr);
        backend->set_window_rect(top.handle, RECT{0, 0, 180, 120});
        backend->pump_events();
        RECT resized{};
        ASSERT_TRUE(GetClientRect(host, &resized));
        EXPECT_EQ(resized.right, 180);
        EXPECT_EQ(resized.bottom, 120);
        PostMessageW(host, WM_MOUSEMOVE, 0, MAKELPARAM(15, 20));
        PostMessageW(host, WM_CLOSE, 0, 0);
        backend->pump_events();
        const auto child_mouse = std::find_if(events.begin(), events.end(), [](const ui_event& e) {
            return e.window == 0x222 && e.message == WM_MOUSEMOVE && e.lParam == MAKELPARAM(5, 10);
        });
        EXPECT_NE(child_mouse, events.end());
        const auto close = std::find_if(events.begin(), events.end(), [](const ui_event& e) {
            return e.window == 0x111 && e.message == WM_CLOSE;
        });
        EXPECT_NE(close, events.end());
        EXPECT_FALSE(backend->activate_native_presentation());
        EXPECT_FALSE(backend->native_presentation_active());
        EXPECT_EQ(backend->acquire_native_window_on_ui(top.handle).status,
                  native_window_acquire_status::native_target_unavailable);
    }

    TEST(GdiUiBackend, OffThreadDestroyDetachesQueuedWindowMessages)
    {
        auto backend = create_gdi_ui_backend();
        ui_window_desc top{};
        top.handle = 0x333;
        top.rect = RECT{0, 0, 100, 100};
        top.title = u"Sogen GDI teardown test";
        top.top_level = true;
        backend->create_window(top);
        backend->pump_events();
        const auto host = FindWindowW(L"SogenGdiSoftwareSurface", L"Sogen GDI teardown test");
        ASSERT_NE(host, nullptr);

        // This command retains a backend pointer, and this window message precedes the
        // asynchronous destroy fallback. Neither may use the backend after destruction.
        backend->set_window_title(top.handle, u"Unexecuted queued command");
        ASSERT_TRUE(PostMessageW(host, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10)));
        std::thread worker([backend = std::move(backend)]() mutable { backend.reset(); });
        worker.join();

        wchar_t title[64]{};
        GetWindowTextW(host, title, static_cast<int>(std::size(title)));
        EXPECT_STREQ(title, L"Sogen GDI teardown test");
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        EXPECT_FALSE(IsWindow(host));
    }
}
#endif
