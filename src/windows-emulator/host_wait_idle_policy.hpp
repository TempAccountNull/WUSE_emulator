#pragma once

#include <chrono>
#include <thread>

namespace sogen::detail
{
    // Idle vCPUs must release the kernel lock before polling a host completion again.
    // A bounded sleep avoids an N-vCPU yield storm while keeping completion latency low.
    inline void sleep_for_pending_host_wait()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}