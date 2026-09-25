#pragma once

#include <cstdint>

namespace sogen::detail
{
    template <typename Threads, typename Thread, typename TrySwitch>
    bool select_next_guest_thread(Threads&& threads, Thread* active_thread, const uint32_t vcpu_index,
                                  const bool prefer_previous_vcpu, TrySwitch&& try_switch)
    {
        const auto scan = [&](auto&& try_candidate) {
            bool after_active = false;
            for (auto& candidate : threads)
            {
                if (after_active && try_candidate(candidate))
                {
                    return true;
                }
                if (&candidate == active_thread)
                {
                    after_active = true;
                }
            }
            for (auto& candidate : threads)
            {
                if (try_candidate(candidate))
                {
                    return true;
                }
            }
            return false;
        };

        if (!prefer_previous_vcpu)
        {
            return scan(try_switch);
        }

        if (scan([&](Thread& candidate) {
                if (candidate.last_vcpu && *candidate.last_vcpu != vcpu_index &&
                    !candidate.affinity_deferred_once)
                {
                    candidate.affinity_deferred_once = true;
                    return false;
                }
                return try_switch(candidate);
            }))
        {
            return true;
        }
        return scan(try_switch);
    }
}
