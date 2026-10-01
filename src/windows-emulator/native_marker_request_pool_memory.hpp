#pragma once

#include "guest_inspection_query.hpp"
#include "native_marker_request_pool.hpp"

namespace sogen::detail
{
    template <typename Kernel, typename Clock>
    native_marker_pool_sample sample_native_marker_request_pool_memory(memory_manager& memory, const uint64_t base,
                                                                       const uint64_t image_size,
                                                                       const native_marker_resource_sample& resources, Kernel& kernel,
                                                                       native_marker_read_budget& budget, Clock&& clock)
    {
        return sample_native_marker_request_pool(
            base, image_size, resources, kernel, budget,
            [&memory](const uint64_t address, const size_t size) { return inspection_passive_span(memory, address, size); },
            [&memory](const uint64_t address, void* destination, const size_t size) {
                return memory.try_read_memory(address, destination, size);
            },
            std::forward<Clock>(clock));
    }
}
