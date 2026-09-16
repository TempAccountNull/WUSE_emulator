#pragma once

#include <cstdint>
#include <utility>
#include <hook_interface.hpp>
#include <utils/object.hpp>

namespace sogen::icicle::detail
{
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
        execution_hook(cpu_interface& cpu, memory_execution_hook_callback callback, bool& state)
            : cpu_(&cpu),
              callback_(std::move(callback)),
              state_(&state)
        {
        }

        void operator()(const uint64_t address) const
        {
            const hook_scope scope(this->state_);
            this->callback_(*this->cpu_, address);
        }

      private:
        cpu_interface* cpu_;
        memory_execution_hook_callback callback_;
        bool* state_;
    };
}
