#pragma once

#include <WinHvPlatform.h>

#include <cstdint>
#include <stdexcept>

namespace sogen::whp::detail
{
    inline uint64_t require_timestamp_instruction_length(const WHV_RUN_VP_EXIT_CONTEXT& exit_context)
    {
        const auto length = static_cast<uint64_t>(exit_context.VpContext.InstructionLength);
        const auto minimum_length = exit_context.ReadTsc.RdtscInfo.IsRdtscp ? 3U : 2U;
        if (length < minimum_length)
        {
            throw std::runtime_error("WHP timestamp exit reported an invalid instruction length");
        }

        return length;
    }
}
