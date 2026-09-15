#pragma once

#include "cng_protocol.hpp"
#include "../registry/registry_manager.hpp"

namespace sogen::cng
{
    struct resolution_result
    {
        NTSTATUS status{};
        std::vector<provider_reference> providers;
    };

    resolution_result resolve(registry_manager& registry, const resolution_request& request);
}
