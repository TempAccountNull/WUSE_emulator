#pragma once

#include <windows_emulator.hpp>

namespace sogen
{

    namespace snapshot
    {
        constexpr bool is_resumable_checkpoint_stop(const stop_reason reason)
        {
            return reason == stop_reason::none || reason == stop_reason::explicit_stop || reason == stop_reason::breakpoint ||
                   reason == stop_reason::watchpoint || reason == stop_reason::instruction_limit;
        }

        std::vector<std::byte> create_emulator_snapshot(const windows_emulator& win_emu);
        std::filesystem::path write_emulator_snapshot(const windows_emulator& win_emu, bool log = true);
        std::filesystem::path write_emulator_snapshot(const windows_emulator& win_emu, const std::filesystem::path& snapshot_file,
                                                      bool log = true);

        void load_emulator_snapshot(windows_emulator& win_emu, std::span<const std::byte> snapshot);
        void load_emulator_snapshot(windows_emulator& win_emu, const std::filesystem::path& snapshot_file);
    }

} // namespace sogen
