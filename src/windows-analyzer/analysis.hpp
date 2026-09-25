#pragma once

#include <type_traits>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string_view>
#include <utility>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "analysis_event.hpp"
#include "analysis_hook_profile.hpp"
#include "disassembler.hpp"

namespace sogen
{

    struct mapped_module;
    class module_manager;
    class windows_emulator;
    class analysis_reporter;

    using string_set = std::set<std::string, std::less<>>;

    struct analysis_settings
    {
        bool concise_logging{false};
        bool verbose_logging{false};
        bool silent{false};
        bool buffer_stdout{false};
        bool instruction_summary{false};
        bool skip_syscalls{false};
        bool skip_generic_activity{false};
        bool reproducible{false};
        bool log_first_section_execution{false};

        string_set modules{};
        string_set ignored_functions{};
    };

    struct accessed_import
    {
        uint64_t address{};
        execution_context access_context{};
        uint64_t access_inst_count{};
        std::string import_name{};
        std::string import_module{};
    };

    struct debug_print_frame
    {
        uint64_t call_id{};
        uint64_t stack{};
        uint64_t return_address{};
    };

    // Diagnostic only: observe concurrent reporter entry without serializing reporters.
    class analysis_event_overlap_probe
    {
      public:
        struct snapshot
        {
            uint64_t overlaps{};
            uint64_t lines_emitted{};
            uint32_t max_active{};
            uint64_t first_host_tid{};
            uint64_t second_host_tid{};
            std::string_view first_type{};
            std::string_view second_type{};
        };

        class scope
        {
          public:
            scope(analysis_event_overlap_probe& probe, const analysis_event& event) noexcept;
            ~scope();
            scope(const scope&) = delete;
            scope& operator=(const scope&) = delete;

          private:
            analysis_event_overlap_probe* probe_{};
            uint64_t token_{};
        };

        analysis_event_overlap_probe() noexcept;
        void set_enabled(bool enabled) noexcept;
        snapshot read_snapshot() const noexcept;

      private:
        friend class scope;

        struct active_entry
        {
            uint64_t token{};
            uint64_t host_tid{};
            std::string_view type{};
        };

        static constexpr size_t max_entries = 16;
        static constexpr uint64_t max_lines = 4;
        std::atomic<bool> enabled_{};
        mutable std::mutex mutex_{};
        std::array<active_entry, max_entries> active_{};
        snapshot stats_{};
        uint64_t next_token_{1};
        uint32_t active_count_{};
    };

    struct analysis_context
    {
        const analysis_settings* settings{};
        windows_emulator* win_emu{};
        std::vector<analysis_reporter*> reporters{};
        mutable analysis_hook_profile hook_profile{};
        mutable analysis_event_overlap_probe event_overlap_probe{};

        std::unordered_map<uint32_t, std::vector<debug_print_frame>> debug_print_calls{};
        std::string output{};
        bool has_reached_main{false};

        disassembler d{};
        std::unordered_map<uint32_t, uint64_t> instructions{};
        std::vector<accessed_import> accessed_imports{};
        std::set<uint64_t> rdtsc_cache{};
        std::set<uint64_t> rdtscp_cache{};
        std::set<std::pair<uint64_t, uint32_t>> cpuid_cache{};
        uint64_t traced_call_count{};
        std::chrono::steady_clock::time_point progress_started{std::chrono::steady_clock::now()};
        std::chrono::steady_clock::time_point progress_last{progress_started};
        uint64_t progress_instructions{};
        std::vector<uint8_t> main_pages_seen; // one byte per page of the main image; execution coverage for Progress %
        uint64_t main_pages_covered{};
        std::optional<uint64_t> auto_break_before_call{};
        std::optional<uint64_t> syscall_to_resume_after_break{};

        mutable std::pair<uint64_t, uint64_t> mapping_violation{0, 0};
        mutable bool private_execute_dump_captured{false};
        mutable uint64_t next_event_sequence{1};

        event_header make_event_header() const;
        execution_context make_execution_context() const;
        execution_context make_execution_context(uint64_t rip, std::string rip_module) const;
        void emit_event(const analysis_event& event) const;

        template <typename Event, typename Initializer>
        void emit_observation(Initializer&& initialize) const
        {
            this->emit_observation<Event>(this->make_execution_context(), std::forward<Initializer>(initialize));
        }

        template <typename Event, typename Initializer>
        void emit_observation(execution_context context, Initializer&& initialize) const
        {
            static_assert(std::is_base_of_v<observation_event, Event>);

            Event event{};
            initialize(event);
            event.header = this->make_event_header();
            event.execution = std::move(context);
            this->emit_event(event);
        }

        template <typename Event>
        void emit_observation() const
        {
            this->emit_observation<Event>([](Event&) {});
        }

        template <typename Event>
        void emit_observation(execution_context context) const
        {
            this->emit_observation<Event>(std::move(context), [](Event&) {});
        }

        template <typename Event, typename Initializer>
        void emit_summary(Initializer&& initialize) const
        {
            static_assert(std::is_base_of_v<summary_event, Event>);

            Event event{};
            initialize(event);
            event.header = this->make_event_header();
            this->emit_event(event);
        }

        template <typename Event>
        void emit_summary() const
        {
            this->emit_summary<Event>([](Event&) {});
        }
    };

    void register_analysis_callbacks(analysis_context& c);
    std::optional<mapped_module*> get_module_if_interesting(module_manager& manager, const string_set& modules, uint64_t address);

} // namespace sogen
