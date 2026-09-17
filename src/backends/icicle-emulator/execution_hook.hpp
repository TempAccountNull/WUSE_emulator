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

    class hook_scope
    {
      public:
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
        execution_hook(cpu_interface& cpu, memory_execution_hook_callback callback, bool& state, hook_exception_sink* sink = nullptr)
            : cpu_(&cpu),
              callback_(std::move(callback)),
              state_(&state),
              sink_(sink)
        {
        }

        void operator()(const uint64_t address) const
        {
            const hook_scope scope(this->state_);
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
