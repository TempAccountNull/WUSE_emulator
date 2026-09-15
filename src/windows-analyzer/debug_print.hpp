#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sogen
{
    struct analysis_context;
    void observe_debug_print_call(analysis_context& c, std::string_view name);
    void prune_debug_print_calls(analysis_context& c, uint64_t address);
    void observe_debug_string(analysis_context& c, std::string_view bytes);
    void observe_debug_print_interrupt(analysis_context& c, uint64_t address, uint16_t length, uint32_t component, uint32_t level);
    std::string escape_debug_console(std::string_view text);
}
