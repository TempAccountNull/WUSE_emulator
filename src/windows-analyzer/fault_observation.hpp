#pragma once

#include "analysis_event.hpp"

namespace sogen
{
    struct analysis_context;
    void capture_memory_violation(const analysis_context& context, memory_violation_event& event, uint64_t actual_ip);
}
