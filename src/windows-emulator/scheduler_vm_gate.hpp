#pragma once

#include <arch_emulator.hpp>

#include <cassert>
#include <mutex>

namespace sogen
{
    // Acquire both scheduler locks without ever blocking on one while holding the other.
    // An external pause can hold the parked gate while a hook callback's captured object
    // is destroyed; that destructor is allowed to acquire the kernel lock. Conversely an
    // external caller may already hold the kernel lock before beginning a pause.
    template <typename KernelLock>
    void acquire_scheduler_vm_parked(x86_64_emulator& emu, const size_t index, std::unique_lock<KernelLock>& kernel)
    {
        assert(!kernel.owns_lock());
        for (;;)
        {
            kernel.lock();
            if (emu.try_set_scheduler_vm_parked(index))
            {
                return;
            }
            kernel.unlock();
            // Wait for the external owner without the kernel lock. Release the
            // gate again before retrying, so neither lock is held while waiting
            // for the other even when another pause wins the next race.
            emu.set_scheduler_vm_parked(index, true);
            emu.set_scheduler_vm_parked(index, false);
        }
    }
}
