#include <gtest/gtest.h>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201) // Existing NT structure declarations use anonymous unions.
#endif
#include <platform/ui_backend.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifdef _WIN32
#include <windows.h>

namespace sogen::test
{
    TEST(SdlUiBackend, KeepsNativeTitleBarInsideRequestedOuterPosition)
    {
        auto backend = create_sdl_ui_backend();
        ui_window_desc desc{};
        desc.handle = 0x53444c;
        desc.rect = RECT{0, 0, 320, 240};
        desc.title = u"SDL outer position test";
        desc.style = WS_OVERLAPPEDWINDOW;
        desc.top_level = true;
        desc.visible = false;

        backend->create_window(desc);
        backend->pump_events();

        const auto host = FindWindowW(nullptr, L"Sogen - SDL outer position test");
        ASSERT_NE(host, nullptr);
        RECT actual{};
        ASSERT_TRUE(GetWindowRect(host, &actual));
        EXPECT_EQ(actual.left, 0);
        EXPECT_EQ(actual.top, 0);

        backend->set_window_rect(desc.handle, RECT{30, 40, 350, 280});
        backend->pump_events();
        ASSERT_TRUE(GetWindowRect(host, &actual));
        EXPECT_EQ(actual.left, 30);
        EXPECT_EQ(actual.top, 40);

        backend->destroy_window(desc.handle);
        backend->pump_events();
        backend->drain_native_shutdown();
    }
}
#endif