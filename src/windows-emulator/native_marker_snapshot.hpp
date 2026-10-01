#pragma once

#include "native_marker_capture_state.hpp"
#include "native_marker_registers.hpp"
#include "native_marker_resources.hpp"
#include <array>
#include <string>

namespace sogen::detail
{
    struct native_marker_snapshot
    {
        native_marker_reservation reservation{};
        native_marker_identity identity{};
        native_marker_register_snapshot registers{};
        native_marker_resource_sample resources{};
        native_marker_read_budget budget{};
        std::array<uint8_t, 8> layout_signature{};
        uint32_t qualification_reads{};
        uint32_t qualification_bytes{};
        uint8_t qualification_available{};
        bool layout_qualified{};
        uint64_t ended_steady_ns{};
        bool complete{};
        std::string error{};
        native_marker_completion completion{};
        native_marker_counts counts{};
    };
} // namespace sogen::detail
