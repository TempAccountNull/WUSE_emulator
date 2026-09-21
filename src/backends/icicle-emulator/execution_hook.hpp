#pragma once

#include <cstdint>
#include <exception>
#include <utility>
#include <hook_interface.hpp>
#include <utils/object.hpp>

namespace sogen::icicle::detail
{
    // Host C++ exceptions raised inside a hook must not unwind through icicle's Rust frames: Rust
    // marks its extern "C" entry points nounwind and aborts the whole process when an exception
    // crosses them ("panic in a function that cannot unwind", analyzer.exe.40620.dmp). Hooks hand
    // the exception to the emulator, which stops icicle and rethrows it once icicle_start has
    // returned to C++ so the analyzer reports it like any other emulation failure.
    struct hook_exception_sink
    {
        virtual ~hook_exception_sink() = default;
        virtual void defer_hook_exception(std::exception_ptr exception) noexcept = 0;
    };

    // Per-thread "inside a hook callback" flag. Thread-local: with N vCPU worker threads invoking hooks
    // concurrently, one shared bool would let one thread's scope-restore clear another thread's flag,
    // routing a BEL-held mutation down the unsafe pause path (6.5).
    inline bool& in_hook_flag() noexcept
    {
        static thread_local bool flag = false;
        return flag;
    }

    // Per-thread "inside an EXECUTION-family hook callback" flag (exact/ranged/generic/block hooks and
    // the run_on_next_instruction one-shot — everything living in icicle's execution_hooks RefCell).
    // Installing/removing an execution hook while inside one panics icicle ("RefCell already borrowed"),
    // so those requests must defer to a quantum boundary; other hook families (syscall/read/write) are
    // separate RefCells and can install immediately.
    inline bool& in_execution_hook_flag() noexcept
    {
        static thread_local bool flag = false;
        return flag;
    }

    class hook_scope
    {
      public:
        // SMP: scopes the CALLING thread's in-hook flag. Resolved at invocation time (the wrapper runs on
        // the VM's worker thread), never at construction time, so a machine-global flag can't be clobbered
        // by another vCPU thread's concurrent scope-restore.
        hook_scope() noexcept
            : state_(&in_hook_flag()),
              previous_(std::exchange(in_hook_flag(), true))
        {
        }

        explicit hook_scope(bool* state) noexcept
            : state_(state),
              previous_(state ? std::exchange(*state, true) : false)
        {
        }

        ~hook_scope()
        {
            if (this->state_)
            {
                *this->state_ = this->previous_;
            }
        }

        hook_scope(const hook_scope&) = delete;
        hook_scope& operator=(const hook_scope&) = delete;

      private:
        bool* state_;
        bool previous_;
    };

    class execution_hook final : public utils::object
    {
      public:
        // `sink` is the emulator that owns the icicle boundary; unit tests that call the hook
        // object directly from C++ may omit it, in which case exceptions propagate normally.
        // Uses the invoking thread's in-hook flag (see hook_scope).
        execution_hook(cpu_interface& cpu, memory_execution_hook_callback callback, hook_exception_sink* sink = nullptr)
            : cpu_(&cpu),
              callback_(std::move(callback)),
              sink_(sink)
        {
        }

        // Test/direct-construction form with an explicit state flag (nesting-semantics tests).
        execution_hook(cpu_interface& cpu, memory_execution_hook_callback callback, bool& state,
                       hook_exception_sink* sink = nullptr)
            : cpu_(&cpu),
              callback_(std::move(callback)),
              state_(&state),
              sink_(sink)
        {
        }

        void operator()(const uint64_t address) const
        {
            const hook_scope scope(this->state_ ? this->state_ : &in_hook_flag());
            const hook_scope exec_scope(&in_execution_hook_flag());
            if (!this->sink_)
            {
                this->callback_(*this->cpu_, address);
                return;
            }
            try
            {
                this->callback_(*this->cpu_, address);
            }
            catch (...)
            {
                this->sink_->defer_hook_exception(std::current_exception());
            }
        }

      private:
        cpu_interface* cpu_;
        memory_execution_hook_callback callback_;
        bool* state_;
        hook_exception_sink* sink_;
    };
}
