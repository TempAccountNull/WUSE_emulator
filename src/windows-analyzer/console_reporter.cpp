#include "std_include.hpp"
#include "debug_print.hpp"

#include "analysis_reporter.hpp"
#include "analysis_reporter_common.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <logger.hpp>

namespace sogen
{

    using analysis_reporter_detail::make_overloaded;

    namespace
    {
        std::string fault_hex(const uint64_t value)
        {
            std::array<char, 24> buffer{};
            snprintf(buffer.data(), buffer.size(), "0x%" PRIx64, value);
            return buffer.data();
        }

        // NOLINTNEXTLINE(cert-dcl50-cpp)
        std::string format_text(const char* format, ...)
        {
            va_list args;
            va_start(args, format);
            va_list copy;
            va_copy(copy, args);
            const auto needed = vsnprintf(nullptr, 0, format, copy);
            va_end(copy);
            std::string text;
            if (needed > 0)
            {
                text.resize(static_cast<size_t>(needed));
                (void)vsnprintf(text.data(), text.size() + 1, format, args);
            }
            va_end(args);
            return text;
        }

        std::string fault_location(const fault_address_snapshot& location)
        {
            std::string result = location.module.value_or(location.region ? location.region->kind : "unknown");
            if (location.module_rva)
            {
                result += "+" + fault_hex(*location.module_rva);
            }
            if (location.region && location.region->reserved)
            {
                result += " alloc=" + fault_hex(location.region->allocation_base);
            }
            return result;
        }

        class console_analysis_reporter final : public analysis_reporter
        {
          public:
            console_analysis_reporter(logger& log, console_reporter_settings settings)
                : log_(log),
                  settings_(settings)
            {
            }

            void report(const analysis_event& event) override
            {
                if (this->settings_.silent)
                {
                    report_silent(event);
                    return;
                }
                if (this->settings_.dedupe && event_is_deduplicable(event) && !uses_repeat_key(event) &&
                    this->already_shown(event_content_hash(event)))
                {
                    return;
                }
                report_regular(event);
            }

            // Print the pending repeat summaries of every guest thread (run end, failure packets).
            void flush() override
            {
                {
                    const std::scoped_lock lock(this->repeat_mutex_);
                    for (auto& [tid, state] : this->repeats_)
                    {
                        this->flush_repeat(tid, state);
                    }
                }
                const std::scoped_lock lock(this->dedupe_mutex_);
                this->print_suppressed_summary();
            }

            // Routine classes build their console text anyway and pass it through absorb_repeat, which
            // applies the duplicate check to that text; hashing their records as well would cost a
            // serialization per event at the highest event rates.
            static bool uses_repeat_key(const analysis_event& event)
            {
                return std::holds_alternative<function_execution_event>(event) || std::holds_alternative<syscall_event>(event) ||
                       std::holds_alternative<object_access_event>(event) || std::holds_alternative<environment_access_event>(event) ||
                       std::holds_alternative<generic_access_event>(event) || std::holds_alternative<io_control_event>(event) ||
                       std::holds_alternative<foreign_code_transition_event>(event);
            }

            static void report_silent(const analysis_event& event)
            {
                std::visit(make_overloaded(
                               [&](const run_finished_event&) {
                                   fflush(stdout); //
                               },
                               [&](const stdout_chunk_event& e) {
                                   (void)fwrite(e.data.data(), 1, e.data.size(), stdout); //
                               },
                               [&](const auto&) {
                                   // Ignore all other events in silent mode.
                               }),
                           event);
            }

            std::string make_call_prefix(const uint64_t call_count) const
            {
                if (!this->settings_.prepend_call_count)
                {
                    return {};
                }

                return "[" + std::to_string(call_count) + "] ";
            }

            void report_regular(const analysis_event& event)
            {
                std::visit(
                    make_overloaded(
                        [&](const run_started_event& e) {
                            this->log_.force_print(color::gray, "Using emulator backend: %s\n", e.backend_name.c_str());
                        },
                        [&](const run_finished_event& e) {
                            if (e.checkpoint_saved)
                            {
                                this->log_.print(color::cyan, "Guest paused at: 0x%" PRIx64 " - checkpoint saved\n", e.rip);
                            }
                            else if (e.exit_status.has_value())
                            {
                                this->log_.print(e.success ? color::green : color::red, "Emulation terminated with status: %X\n",
                                                 *e.exit_status);
                            }
                        },
                        [&](const run_failed_event& e) {
                            if (e.phase == "snapshot_save")
                            {
                                this->log_.error("Snapshot save failed (host); guest paused at: 0x%" PRIx64 " - %s\n", e.rip,
                                                 e.message.c_str());
                            }
                            else
                            {
                                this->log_.error("Emulation failed at: 0x%" PRIx64 " - %s\n", e.rip, e.message.c_str());
                            }
                        },
                        [&](const instruction_summary_event& e) {
                            this->log_.print(color::white, "Instruction summary:\n");
                            for (const auto& entry : e.entries)
                            {
                                this->log_.print(color::white, "%s: %" PRIu64 "\n", entry.mnemonic.c_str(), entry.count);
                            }
                        },
                        [&](const buffered_stdout_event& e) {
                            if (this->settings_.buffer_stdout && !e.data.empty())
                            {
                                this->log_.info("%.*s%s", static_cast<int>(e.data.size()), e.data.data(),
                                                e.data.ends_with("\n") ? "" : "\n");
                            }
                        },
                        [&](const stdout_chunk_event& e) {
                            if (!this->settings_.buffer_stdout)
                            {
                                this->log_.info("%.*s%s", static_cast<int>(e.data.size()), e.data.data(),
                                                e.data.ends_with("\n") ? "" : "\n");
                            }
                        },
                        [&](const suspicious_activity_event& e) {
                            const auto addition = e.decoded_instruction.empty() ? std::string{} : " ("s + e.decoded_instruction + ")";
                            this->log_.print(color::pink, "Suspicious: %s%s at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n", e.details.c_str(),
                                             addition.c_str(), e.execution.rip, e.execution.previous_ip.value_or(0),
                                             e.execution.previous_ip_module.value_or("<N/A>").c_str());
                        },
                        [&](const debug_print_call_event& e) {
                            this->log_.info("-> Printed args: %s | call %" PRIu64 " | 0x%" PRIx64 " (%s) | caller 0x%" PRIx64 " (%s)\n",
                                            e.api.c_str(), e.call_id, e.execution.rip, e.execution.rip_module.c_str(), e.return_address,
                                            e.return_module.c_str());
                            if (!e.error.empty())
                            {
                                this->log_.error("   Capture error: %s\n", escape_debug_console(e.error).c_str());
                            }
                            for (const auto& arg : e.arguments)
                            {
                                this->log_.info("   %s=0x%" PRIx64 "%s%s%s%s\n", arg.name.c_str(), arg.raw,
                                                arg.encoding.empty() ? "" : " | ", escape_debug_console(arg.text).c_str(),
                                                arg.error.empty() ? "" : " | ", escape_debug_console(arg.error).c_str());
                            }
                        },
                        [&](const debug_string_event& e) {
                            if (!e.error.empty())
                            {
                                this->log_.error("-> Print capture error: %s\n", escape_debug_console(e.error).c_str());
                            }
                            this->log_.info("-> Printed: %s | %s | 0x%" PRIx64 " (%s)\n", escape_debug_console(e.details).c_str(),
                                            e.transport.c_str(), e.execution.rip, e.execution.rip_module.c_str());
                        },
                        [&](const generic_activity_event& e) { this->log_.print(color::dark_gray, "%s\n", e.details.c_str()); },
                        [&](const generic_access_event& e) {
                            auto text = format_text("--> %s: %s", e.type.c_str(), e.name.c_str());
                            if (this->absorb_repeat(e.execution.thread_id, "ga|" + text, 0, color::dark_gray, text))
                            {
                                return;
                            }
                            this->log_.print(color::dark_gray, "%s\n", text.c_str());
                        },
                        [&](const memory_allocate_event& e) {
                            this->log_.print(e.permissions.find('x') != std::string::npos ? color::gray : color::dark_gray,
                                             "--> %s 0x%" PRIx64 " - 0x%" PRIx64 " (%s)\n", e.commit ? "Committed" : "Allocating",
                                             e.address, e.address + e.length, e.permissions.c_str());
                        },
                        [&](const memory_protect_event& e) {
                            this->log_.print(color::dark_gray, "--> Changing protection at 0x%" PRIx64 "-0x%" PRIx64 " to %s\n", e.address,
                                             e.address + e.length, e.permissions.c_str());
                        },
                        [&](const memory_violation_event& e) {
                            const auto* label = e.violation_type == "protection" ? "Protection violation" : "Mapping violation";
                            this->log_.print(color::red, "%s: %s 0x%" PRIx64 " size=0x%" PRIx64 " tid=%u%s\n", label, e.operation.c_str(),
                                             e.address, e.size, e.execution.thread_id, e.near_null_execute ? " [near-null execute]" : "");
                            const auto instruction = [&](const char* name, const fault_instruction_snapshot& value) {
                                this->log_.print(color::cyan, "  %s: 0x%" PRIx64 " (%s) [%s] %s%s%s\n", name, value.location.address,
                                                 fault_location(value.location).c_str(), value.bytes_hex.c_str(), value.assembly.c_str(),
                                                 value.error.empty() ? "" : " | ", value.error.c_str());
                            };
                            instruction("CPU IP", e.actual_instruction);
                            if (e.last_tracked_instruction)
                            {
                                instruction("Last tracked (fault CS)", *e.last_tracked_instruction);
                            }
                            if (e.stack_slot.address)
                            {
                                this->log_.print(color::yellow, "  Stack sample: width=%s address=%s addressBits=%u base=%s\n",
                                                 e.stack_slot.width_source.c_str(), e.stack_slot.address_source.c_str(),
                                                 e.stack_slot.address_bits.value_or(0),
                                                 e.stack_slot.segment_base ? fault_hex(*e.stack_slot.segment_base).c_str()
                                                                           : "<unavailable>");
                                this->log_.print(color::yellow, "  Stack slot%u [%s]=%s%s%s\n", e.stack_slot.pointer_bits.value_or(0),
                                                 fault_hex(*e.stack_slot.address).c_str(),
                                                 e.stack_slot.value ? fault_hex(*e.stack_slot.value).c_str() : "<unreadable>",
                                                 e.stack_slot.value_location || !e.stack_slot.error.empty() ? " " : "",
                                                 e.stack_slot.value_location ? fault_location(*e.stack_slot.value_location).c_str()
                                                                             : e.stack_slot.error.c_str());
                            }
                            else if (!e.stack_slot.error.empty())
                            {
                                this->log_.print(color::yellow, "  Stack slot: %s\n", e.stack_slot.error.c_str());
                            }
                            std::string registers;
                            size_t count = 0;
                            for (const auto& reg : e.registers)
                            {
                                registers += " " + reg.name + "=" + (reg.value ? fault_hex(*reg.value) : "<unavailable>");
                                if (++count % 4 == 0)
                                {
                                    this->log_.print(color::gray, " %s\n", registers.c_str());
                                    registers.clear();
                                }
                            }
                            if (!registers.empty())
                            {
                                this->log_.print(color::gray, " %s\n", registers.c_str());
                            }
                            if (!e.capture_error.empty())
                            {
                                this->log_.error("  Capture: %s\n", e.capture_error.c_str());
                            }
                        },
                        [&](const io_control_event& e) {
                            auto text = format_text("--> %s: 0x%X", e.device_name.c_str(), e.code);
                            if (this->absorb_repeat(e.execution.thread_id, "io|" + text, 0, color::dark_gray, text))
                            {
                                return;
                            }
                            this->log_.print(color::dark_gray, "%s\n", text.c_str());
                        },
                        [&](const thread_create_event& e) {
                            std::string flags{};
                            for (const auto& flag : e.flags)
                            {
                                flags += ", ";
                                flags += flag;
                            }

                            this->log_.print(color::gray, "Thread created: tid %u, start address 0x%" PRIx64 " (param 0x%" PRIx64 ")%s\n",
                                             e.created_thread_id, e.start_address, e.argument, flags.c_str());
                        },
                        [&](const thread_terminated_event& e) {
                            if (e.exit_status.has_value())
                            {
                                this->log_.print(color::gray, "Thread terminated: tid %u | exit 0x%08" PRIX32 "\n", e.terminated_thread_id,
                                                 *e.exit_status);
                            }
                            else
                            {
                                this->log_.print(color::gray, "Thread terminated: tid %u\n", e.terminated_thread_id);
                            }
                        },
                        [&](const thread_set_name_event& e) {
                            this->log_.print(color::blue, "Setting thread (%u) name: %s\n", e.renamed_thread_id, e.name.c_str());
                        },
                        [&](const thread_switch_event&) {
                            // this->log_.print(color::dark_gray, "Performing thread switch: %X -> %X\n", e.previous_thread_id,
                            //                  e.next_thread_id);
                        },
                        [&](const module_load_event& e) { this->log_.log("Mapped %s at 0x%" PRIx64 "\n", e.path.c_str(), e.image_base); },
                        [&](const module_unload_event& e) {
                            this->log_.log("Unmapping %s (0x%" PRIx64 ")\n", e.path.c_str(), e.image_base);
                        },
                        [&](const import_read_event& e) {
                            this->log_.print(color::green, "Import read access: %s (%s) at 0x%" PRIx64 " (%s)\n", e.import_name.c_str(),
                                             e.import_module.c_str(), e.execution.rip, e.execution.rip_module.c_str());
                        },
                        [&](const import_write_event& e) {
                            this->log_.print(color::blue,
                                             "Import write access: %zd bytes with value 0x%" PRIX64 " to %s (%s) at 0x%" PRIx64 " (%s)\n",
                                             e.size, e.value, e.import_name.c_str(), e.import_module.c_str(), e.execution.rip,
                                             e.execution.rip_module.c_str());
                        },
                        [&](const object_access_event& e) {
                            if (this->settings_.interesting_only && !e.main_access)
                            {
                                return;
                            }
                            const auto line_color = e.main_access ? color::green : color::dark_gray;
                            auto text = format_text("Object access: %s - 0x%" PRIx64 " 0x%" PRIx64 " (%s) at 0x%" PRIx64 " (%s)",
                                                    e.type_name.c_str(), e.offset, e.size, e.member_name.value_or("<N/A>").c_str(),
                                                    e.execution.rip, e.execution.rip_module.c_str());
                            if (this->absorb_repeat(e.execution.thread_id, "oa|" + text, 0, line_color, text))
                            {
                                return;
                            }
                            this->log_.print(line_color, "%s\n", text.c_str());
                        },
                        [&](const environment_access_event& e) {
                            if (this->settings_.interesting_only && !e.main_access)
                            {
                                return;
                            }
                            const auto line_color = e.main_access ? color::green : color::dark_gray;
                            auto text = format_text("Environment access: 0x%" PRIx64 " (0x%zX) at 0x%" PRIx64 " (%s)", e.offset,
                                                    static_cast<size_t>(e.size), e.execution.rip, e.execution.rip_module.c_str());
                            if (this->absorb_repeat(e.execution.thread_id, "ea|" + text, 0, line_color, text))
                            {
                                return;
                            }
                            this->log_.print(line_color, "%s\n", text.c_str());
                        },
                        [&](const function_execution_event& e) {
                            if (this->settings_.interesting_only && !e.interesting)
                            {
                                return;
                            }
                            const auto line_color = e.interesting ? color::yellow : color::dark_gray;
                            auto text = format_text("Executing function: %s (%s) (0x%" PRIx64 ") via 0x%" PRIx64 " (%s)",
                                                    e.function_name.c_str(), e.execution.rip_module.c_str(), e.execution.rip,
                                                    e.execution.previous_ip.value_or(0), e.execution.previous_ip_module.value_or("<N/A>").c_str());
                            std::string key = "fn|" + text;
                            for (const auto& detail : e.details)
                            {
                                key += "|" + detail.label + "=" + detail.value;
                            }
                            if (this->absorb_repeat(e.execution.thread_id, std::move(key), e.call_count, line_color, text))
                            {
                                return;
                            }
                            const auto prefix = this->make_call_prefix(e.call_count);
                            this->log_.print(line_color, "%s%s\n", prefix.c_str(), text.c_str());
                            for (const auto& detail : e.details)
                            {
                                if (detail.label.empty())
                                {
                                    this->log_.print(color::dark_gray, "--> %s\n", detail.value.c_str());
                                }
                                else
                                {
                                    this->log_.print(color::dark_gray, "--> %s: %s\n", detail.label.c_str(), detail.value.c_str());
                                }
                            }
                        },
                        [&](const entry_point_execution_event& e) {
                            this->log_.print(e.interesting ? color::yellow : color::gray, "Executing entry point: %s (0x%" PRIx64 ")\n",
                                             e.execution.rip_module.c_str(), e.execution.rip);
                        },
                        [&](const foreign_code_transition_event& e) {
                            if (this->settings_.interesting_only && !e.interesting)
                            {
                                return;
                            }
                            const auto line_color = e.interesting ? color::yellow : color::dark_gray;
                            auto text = format_text("Transition to foreign code: %s+0x%" PRIx64 " (%s) (0x%" PRIx64 ") via 0x%" PRIx64 " (%s)",
                                                    e.function_name.c_str(), e.function_offset, e.execution.rip_module.c_str(), e.execution.rip,
                                                    e.execution.previous_ip.value_or(0), e.execution.previous_ip_module.value_or("<N/A>").c_str());
                            if (this->absorb_repeat(e.execution.thread_id, "ft|" + text, 0, line_color, text))
                            {
                                return;
                            }
                            this->log_.print(line_color, "%s\n", text.c_str());
                        },
                        [&](const section_first_execute_event& e) {
                            this->log_.print(
                                color::green, "Section %s (%s) first execute at 0x%" PRIx64 " 0x%" PRIx64 " (tid: %" PRIu32 ")\n",
                                e.module_name.c_str(), e.section_name.c_str(), e.execution.rip, e.file_address, e.execution.thread_id);
                        },
                        [&](const execution_progress_event& e) {
                            this->log_.print(color::green, "Progress: tid %" PRIu32 " | RIP 0x%" PRIx64 " (%s", e.execution.thread_id,
                                             e.execution.rip, e.execution.rip_module.c_str());
                            if (e.module_rva)
                            {
                                this->log_.print(color::green, "+0x%" PRIx64, *e.module_rva);
                            }
                            this->log_.print(color::green, ")\n");
                        },
                        [&](const rdtsc_event& e) {
                            this->log_.print(color::blue, "Executing RDTSC instruction at 0x%" PRIx64 " (%s)\n", e.execution.rip,
                                             e.execution.rip_module.c_str());
                        },
                        [&](const rdtscp_event& e) {
                            this->log_.print(color::blue, "Executing RDTSCP instruction at 0x%" PRIx64 " (%s)\n", e.execution.rip,
                                             e.execution.rip_module.c_str());
                        },
                        [&](const cpuid_event& e) {
                            this->log_.print(color::blue, "Executing CPUID instruction with leaf 0x%X at 0x%" PRIx64 " (%s)\n", e.leaf,
                                             e.execution.rip, e.execution.rip_module.c_str());
                        },
                        [&](const syscall_event& e) {
                            std::string text;
                            auto line_color = color::blue;
                            switch (e.classification)
                            {
                            case syscall_classification::inline_syscall:
                                text = format_text("Executing inline syscall: %s (0x%X) at 0x%" PRIx64 " (%s)", e.syscall_name.c_str(),
                                                   e.syscall_id, e.execution.rip, e.execution.rip_module.c_str());
                                break;
                            case syscall_classification::crafted_out_of_line:
                                text = format_text("Crafted out-of-line syscall: %s (0x%X) at 0x%" PRIx64 " (%s) via 0x%" PRIx64 " (%s)",
                                                   e.syscall_name.c_str(), e.syscall_id, e.execution.rip, e.execution.rip_module.c_str(),
                                                   e.execution.previous_ip.value_or(0), e.execution.previous_ip_module.value_or("<N/A>").c_str());
                                break;
                            case syscall_classification::regular:
                            default:
                                if (this->settings_.interesting_only)
                                {
                                    return;
                                }
                                line_color = color::dark_gray;
                                text = format_text("Executing syscall: %s (0x%X) at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)", e.syscall_name.c_str(),
                                                   e.syscall_id, e.execution.rip, e.caller_rip.value_or(0),
                                                   e.caller_module.value_or("<N/A>").c_str());
                                break;
                            }
                            if (this->absorb_repeat(e.execution.thread_id, "sc|" + text, e.call_count, line_color, text))
                            {
                                return;
                            }
                            const auto prefix = this->make_call_prefix(e.call_count);
                            this->log_.print(line_color, "%s%s\n", prefix.c_str(), text.c_str());
                        },
                        [&](const foreign_module_read_event& e) {
                            this->log_.print(color::pink, "Reading %zd bytes from module %s at 0x%" PRIx64 " (%s) via 0x%" PRIx64 " (%s)\n",
                                             e.size, e.module_name.c_str(), e.address, e.region_name.c_str(), e.execution.rip,
                                             e.execution.rip_module.c_str());
                        },
                        [&](const executable_read_event& e) {
                            this->log_.print(color::green,
                                             "Reading %zd bytes from executable section %s at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n",
                                             e.size, e.section_name.c_str(), e.address, e.execution.rip, e.execution.rip_module.c_str());
                        },
                        [&](const executable_write_event& e) {
                            this->log_.print(color::blue,
                                             "Writing %zd bytes with value 0x%" PRIX64 " to executable section %s at 0x%" PRIx64
                                             " via 0x%" PRIx64 " (%s)\n",
                                             e.size, e.value, e.section_name.c_str(), e.address, e.execution.rip,
                                             e.execution.rip_module.c_str());
                        },
                        [&](const fast_fail_event& e) {
                            this->log_.print(color::red, "Process requested fast fail with code %d\n", e.fail_code);
                        }),
                    event);
            }

          private:
            // Console-only repeat coalescing, one state per guest thread. The structured reporters
            // still receive every event; only the human console folds consecutive identical lines.
            struct repeat_state
            {
                std::string key{};
                std::string text{};
                color line_color{color::gray};
                uint64_t repeats{};            // identical events absorbed after the printed line
                uint64_t reported{};           // repeats already covered by a summary line
                uint64_t pending_first_call{}; // traced-call count of the first unreported repeat
                uint64_t last_call{};
                std::chrono::steady_clock::time_point last_summary{};
            };

            // Returns true when the event repeats the previous line of the same thread and was folded
            // into its repeat run. Otherwise the pending run is summarized and the caller prints the line.
            bool absorb_repeat(const uint32_t tid, std::string key, const uint64_t call_count, const color line_color,
                               const std::string& text)
            {
                if (!this->settings_.coalesce_repeats)
                {
                    return this->settings_.dedupe && this->already_shown(record_content_hash(key));
                }

                const std::scoped_lock lock(this->repeat_mutex_);
                auto& state = this->repeats_[tid];
                const auto now = std::chrono::steady_clock::now();
                if (!state.key.empty() && state.key == key)
                {
                    ++state.repeats;
                    state.last_call = call_count;
                    if (state.repeats - state.reported == 1)
                    {
                        state.pending_first_call = call_count;
                    }
                    const auto pending = state.repeats - state.reported;
                    if ((this->settings_.repeat_summary_every && pending >= this->settings_.repeat_summary_every) ||
                        now - state.last_summary >= this->settings_.repeat_summary_interval)
                    {
                        this->print_repeat_summary(tid, state);
                        state.last_summary = now;
                    }
                    return true;
                }

                // A new line for this thread that any thread already printed is a duplicate, not a run.
                if (this->settings_.dedupe && this->already_shown(record_content_hash(key)))
                {
                    return true;
                }

                this->flush_repeat(tid, state);
                state.key = std::move(key);
                state.text = text;
                state.line_color = line_color;
                state.repeats = 0;
                state.reported = 0;
                state.pending_first_call = call_count;
                state.last_call = call_count;
                state.last_summary = now;
                return false;
            }

            void print_repeat_summary(const uint32_t tid, repeat_state& state)
            {
                const auto pending = state.repeats - state.reported;
                if (pending == 0)
                {
                    return;
                }
                if (state.last_call != 0)
                {
                    this->log_.print(state.line_color, "~ tid %" PRIu32 " repeated %" PRIu64 " more times [calls %" PRIu64 "..%" PRIu64 "]: %s\n",
                                     tid, pending, state.pending_first_call, state.last_call, state.text.c_str());
                }
                else
                {
                    this->log_.print(state.line_color, "~ tid %" PRIu32 " repeated %" PRIu64 " more times: %s\n", tid, pending,
                                     state.text.c_str());
                }
                state.reported = state.repeats;
            }

            void flush_repeat(const uint32_t tid, repeat_state& state)
            {
                this->print_repeat_summary(tid, state);
                state.key.clear();
                state.repeats = 0;
                state.reported = 0;
            }

            // Returns true when a line with this data was already printed; duplicates are counted and
            // summarized at most once per repeat_summary_interval and at flush.
            bool already_shown(const uint64_t hash)
            {
                const std::scoped_lock lock(this->dedupe_mutex_);
                if (this->shown_.contains(hash))
                {
                    ++this->suppressed_;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - this->last_suppressed_summary_ >= this->settings_.repeat_summary_interval)
                    {
                        this->print_suppressed_summary();
                        this->last_suppressed_summary_ = now;
                    }
                    return true;
                }
                if (this->shown_.size() < max_dedupe_entries)
                {
                    this->shown_.insert(hash);
                }
                return false;
            }

            void print_suppressed_summary()
            {
                const auto pending = this->suppressed_ - this->suppressed_reported_;
                if (pending == 0)
                {
                    return;
                }
                this->log_.print(color::dark_gray,
                                 "~ suppressed %" PRIu64 " duplicate lines (identical data already shown; %" PRIu64 " so far)\n", pending,
                                 this->suppressed_);
                this->suppressed_reported_ = this->suppressed_;
            }

            static constexpr size_t max_dedupe_entries = size_t{1} << 20;

            logger& log_;
            console_reporter_settings settings_{};
            std::mutex repeat_mutex_{};
            std::unordered_map<uint32_t, repeat_state> repeats_{};
            std::mutex dedupe_mutex_{};
            std::unordered_set<uint64_t> shown_{};
            uint64_t suppressed_{};
            uint64_t suppressed_reported_{};
            std::chrono::steady_clock::time_point last_suppressed_summary_{std::chrono::steady_clock::now()};
        };
    }

    std::unique_ptr<analysis_reporter> create_console_reporter(logger& log, const console_reporter_settings settings)
    {
        return std::make_unique<console_analysis_reporter>(log, settings);
    }

} // namespace sogen
