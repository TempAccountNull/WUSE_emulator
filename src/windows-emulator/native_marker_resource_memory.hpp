#pragma once

#include "guest_inspection_query.hpp"
#include "native_marker_resources.hpp"

namespace sogen::detail
{
    template <typename Kernel, typename Clock>
    native_marker_resource_sample sample_native_marker_resource_memory(memory_manager& memory, const uint64_t base,
                                                                       const uint64_t image_size, Kernel& kernel,
                                                                       native_marker_read_budget& budget, Clock&& clock)
    {
        return sample_native_marker_resources(
            base, image_size, kernel, budget,
            [&memory](const uint64_t address, const size_t size) { return inspection_passive_span(memory, address, size); },
            [&memory](const uint64_t address, void* destination, const size_t size) {
                return memory.try_read_memory(address, destination, size);
            },
            std::forward<Clock>(clock));
    }
}
