#pragma once

#include <native_marker_snapshot.hpp>
#include <optional>
#include <string_view>

namespace sogen
{
    class windows_emulator;

    class native_marker_observer
    {
      public:
        native_marker_observer();
        explicit native_marker_observer(std::string_view opt_in);
        std::optional<detail::native_marker_snapshot> capture(windows_emulator& win, std::string_view message) noexcept;

      private:
        bool enabled_{};
        bool arm_attempted_{};
        bool coverage_sent_{};
        uint64_t generation_{};
        detail::native_marker_capture_state state_{};
    };
} // namespace sogen
