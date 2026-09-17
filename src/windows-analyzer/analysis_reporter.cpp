#include "std_include.hpp"

#include "analysis_reporter.hpp"
#include "analysis_reporter_common.hpp"
#include "jsonl_reporter.hpp"
#include <utils/async_file_writer.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sogen
{

    namespace
    {
        using analysis_reporter_detail::make_overloaded;

        void append_escaped_json(std::string& output, const std::string_view value)
        {
            for (const auto ch : value)
            {
                switch (ch)
                {
                case '\\':
                    output += "\\\\";
                    break;
                case '"':
                    output += "\\\"";
                    break;
                case '\b':
                    output += "\\b";
                    break;
                case '\f':
                    output += "\\f";
                    break;
                case '\n':
                    output += "\\n";
                    break;
                case '\r':
                    output += "\\r";
                    break;
                case '\t':
                    output += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20)
                    {
                        std::array<char, 8> buffer{};
                        snprintf(buffer.data(), buffer.size(), "\\u%04x", static_cast<unsigned char>(ch));
                        output += buffer.data();
                    }
                    else
                    {
                        output.push_back(ch);
                    }
                    break;
                }
            }
        }

        void append_unsigned(std::string& output, const uint64_t value, const int base = 10)
        {
            std::array<char, 32> buffer{};
            const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, base);
            if (result.ec != std::errc{})
            {
                throw std::runtime_error("Failed to serialize integer");
            }
            output.append(buffer.data(), result.ptr);
        }

        class json_object_builder
        {
          public:
            explicit json_object_builder(std::string& output)
                : output_(output)
            {
                this->output_ += '{';
            }

            ~json_object_builder()
            {
                this->output_ += '}';
            }

            void field(std::string_view key, std::string_view value)
            {
                this->key(key);
                this->output_ += '"';
                append_escaped_json(this->output_, value);
                this->output_ += '"';
            }

            void field(std::string_view key, const char* value)
            {
                this->field(key, std::string_view(value ? value : ""));
            }

            void field(std::string_view key, const std::string& value)
            {
                this->field(key, std::string_view(value));
            }

            void field(std::string_view key, const bool value)
            {
                this->key(key);
                this->output_ += value ? "true" : "false";
            }

            void field(std::string_view key, const uint32_t value)
            {
                this->key(key);
                append_unsigned(this->output_, value);
            }

            void field(std::string_view key, const uint64_t value)
            {
                this->key(key);
                this->output_ += '"';
                append_unsigned(this->output_, value);
                this->output_ += '"';
            }

            void hex_field(std::string_view key, const uint64_t value)
            {
                this->key(key);
                this->output_ += "\"0x";
                append_unsigned(this->output_, value, 16);
                this->output_ += '"';
            }

            void optional_hex_field(std::string_view key, const std::optional<uint64_t>& value)
            {
                if (value.has_value())
                {
                    this->hex_field(key, *value);
                }
            }

            void optional_string_field(std::string_view key, const std::optional<std::string>& value)
            {
                if (value.has_value())
                {
                    this->field(key, *value);
                }
            }

            template <typename Callback>
            void object_field(std::string_view key, Callback&& callback)
            {
                this->key(key);
                json_object_builder object{this->output_};
                callback(object);
            }

            template <typename Callback>
            void array_field(std::string_view key, Callback&& callback)
            {
                this->key(key);
                this->output_ += '[';
                bool first = true;
                callback([&](const auto& writer) {
                    if (!first)
                    {
                        this->output_ += ',';
                    }

                    first = false;
                    writer(this->output_);
                });
                this->output_ += ']';
            }

          private:
            std::string& output_;
            bool first_{true};

            void key(std::string_view key)
            {
                if (!this->first_)
                {
                    this->output_ += ',';
                }

                this->first_ = false;
                this->output_ += '"';
                append_escaped_json(this->output_, key);
                this->output_ += "\":";
            }
        };

        std::string_view syscall_classification_name(const syscall_classification classification)
        {
            switch (classification)
            {
            case syscall_classification::regular:
                return "regular";
            case syscall_classification::inline_syscall:
                return "inline";
            case syscall_classification::crafted_out_of_line:
                return "crafted_out_of_line";
            default:
                return "unknown";
            }
        }

        class jsonl_analysis_reporter final : public analysis_reporter
        {
          public:
            explicit jsonl_analysis_reporter(const std::filesystem::path& path, jsonl_report_settings settings)
                : file_(path),
                  settings_(std::move(settings)),
                  last_aggregate_(std::chrono::steady_clock::now()),
                  last_status_(last_aggregate_)
            {
            }

            void report(const analysis_event& event) override
            {
                this->observe_location(event);

                if ((!this->settings_.hidden_modules.empty() && event_from_hidden_module(event, this->settings_.hidden_modules)) ||
                    (!this->settings_.hidden_event_types.empty() && event_of_hidden_type(event, this->settings_.hidden_event_types)))
                {
                    ++this->hidden_events_;
                    this->maybe_publish_status(false);
                    return;
                }

                if (this->settings_.mode == jsonl_report_mode::audit && this->summarize(event))
                {
                    this->maybe_publish_status(false);
                    return;
                }

                thread_local std::string line;
                line.clear();
                if (line.capacity() < 768)
                {
                    line.reserve(768);
                }

                {
                    json_object_builder object{line};
                    std::visit(make_overloaded([&](const auto& e) { write_record(object, e); }), event);
                }

                if (this->settings_.dedupe && event_is_deduplicable(event))
                {
                    const auto hash = record_content_hash(line);
                    if (this->seen_content_.contains(hash))
                    {
                        ++this->deduplicated_events_;
                        this->maybe_publish_status(false);
                        return;
                    }
                    if (this->seen_content_.size() < max_dedupe_entries)
                    {
                        this->seen_content_.insert(hash);
                    }
                }

                line.push_back('\n');
                this->file_.write(line);
                ++this->retained_events_;
                this->maybe_publish_status(false);
            }

            void flush() override
            {
                this->emit_aggregate(true);
                this->file_.flush();
                this->maybe_publish_status(true);
            }

            static std::string_view type_name(const analysis_event& event)
            {
                return std::visit([](const auto& e) { return event_name(e); }, event);
            }

            static uint64_t content_hash(const analysis_event& event)
            {
                thread_local std::string record;
                record.clear();
                {
                    json_object_builder object{record};
                    std::visit(make_overloaded([&](const auto& e) { write_record(object, e); }), event);
                }
                return record_content_hash(record);
            }

          private:
            static constexpr size_t max_window_keys = 4096;
            static constexpr size_t max_retained_keys = 65536;
            static constexpr size_t max_dedupe_entries = size_t{1} << 20;

            utils::async_file_writer file_;
            jsonl_report_settings settings_{};
            std::chrono::steady_clock::time_point last_aggregate_{};
            std::chrono::steady_clock::time_point last_status_{};
            uint64_t retained_events_{};
            uint64_t deduplicated_events_{};
            uint64_t hidden_events_{};
            std::unordered_set<uint64_t> seen_content_{};
            uint64_t summarized_events_{};
            uint64_t window_events_{};
            uint64_t clock_check_counter_{};
            uint64_t aggregates_written_{};
            uint64_t last_instruction_count_{};
            uint32_t last_tid_{};
            uint64_t last_rip_{};
            std::string last_module_{};
            std::string last_event_type_{};
            std::map<std::string, uint64_t, std::less<>> summarized_by_type_{}; // cumulative per event type
            std::unordered_map<std::string, uint64_t> window_counts_{};         // "type|key" -> count in the current window
            std::unordered_map<std::string, uint32_t> retained_seen_{};         // "type|key" -> retained occurrences

            void observe_location(const analysis_event& event)
            {
                std::visit(
                    [&](const auto& e) {
                        using event_type = std::decay_t<decltype(e)>;
                        this->last_instruction_count_ = e.header.instruction_count;
                        const std::string_view name = event_name(e);
                        if (this->last_event_type_ != name)
                        {
                            this->last_event_type_.assign(name);
                        }
                        if constexpr (std::is_base_of_v<observation_event, event_type>)
                        {
                            this->last_tid_ = e.execution.thread_id;
                            this->last_rip_ = e.execution.rip;
                            if (this->last_module_ != e.execution.rip_module)
                            {
                                this->last_module_ = e.execution.rip_module;
                            }
                        }
                    },
                    event);
            }

            static std::string_view event_name(const run_started_event&)
            {
                return "header";
            }

            static std::string_view event_name(const run_finished_event&)
            {
                return "footer";
            }

            static std::string_view event_name(const run_failed_event&)
            {
                return "footer";
            }

            static void append_hex(std::string& key, const uint64_t value)
            {
                key += "0x";
                append_unsigned(key, value, 16);
            }

            // Returns true when the event is routine in audit mode and was counted instead of written.
            bool summarize(const analysis_event& event)
            {
                thread_local std::string key;
                key.clear();
                return std::visit(make_overloaded(
                                      [&](const function_execution_event& e) {
                                          key.append(e.function_name).append(" (").append(e.execution.rip_module).append(")");
                                          if (!e.interesting)
                                          {
                                              // Library-to-library traffic: counted only.
                                              return this->count("function_execution", key);
                                          }
                                          key.append(" via ").append(e.execution.previous_ip_module.value_or("<N/A>")).append("+");
                                          append_hex(key, e.execution.previous_ip.value_or(0));
                                          return this->count_after_retained("function_execution", key);
                                      },
                                      [&](const object_access_event& e) {
                                          key.append(e.type_name).append("+");
                                          append_hex(key, e.offset);
                                          key.append(" (")
                                              .append(e.member_name.value_or("<N/A>"))
                                              .append(") at ")
                                              .append(e.execution.rip_module)
                                              .append("+");
                                          append_hex(key, e.execution.rip);
                                          return e.main_access ? this->count_after_retained("object_access", key)
                                                               : this->count("object_access", key);
                                      },
                                      [&](const environment_access_event& e) {
                                          append_hex(key, e.offset);
                                          key.append(" at ").append(e.execution.rip_module).append("+");
                                          append_hex(key, e.execution.rip);
                                          return e.main_access ? this->count_after_retained("environment_access", key)
                                                               : this->count("environment_access", key);
                                      },
                                      [&](const syscall_event& e) {
                                          if (e.classification != syscall_classification::regular)
                                          {
                                              // Inline and crafted syscalls are anti-analysis signals; always retained.
                                              return false;
                                          }
                                          key.append(e.syscall_name);
                                          return this->count("syscall", key);
                                      },
                                      [&](const foreign_code_transition_event& e) {
                                          key.append(e.function_name).append("+");
                                          append_hex(key, e.function_offset);
                                          key.append(" (")
                                              .append(e.execution.rip_module)
                                              .append(") via ")
                                              .append(e.execution.previous_ip_module.value_or("<N/A>"));
                                          return e.interesting ? this->count_after_retained("foreign_code_transition", key)
                                                               : this->count("foreign_code_transition", key);
                                      },
                                      [&](const thread_switch_event&) { return this->count("thread_switch", key); },
                                      [&](const generic_access_event& e) {
                                          key.append(e.type).append(": ").append(e.name);
                                          return this->count_after_retained("generic_access", key);
                                      },
                                      [&](const io_control_event& e) {
                                          key.append(e.device_name).append(" ");
                                          append_hex(key, e.code);
                                          return this->count_after_retained("io_control", key);
                                      },
                                      [&](const auto&) { return false; }),
                                  event);
            }

            bool count(const std::string_view type, const std::string& key)
            {
                thread_local std::string full;
                full.assign(type).append("|").append(key);
                auto entry = this->window_counts_.find(full);
                if (entry == this->window_counts_.end())
                {
                    if (this->window_counts_.size() >= max_window_keys)
                    {
                        // Bounded memory: further distinct keys of this window share one overflow bucket.
                        full.assign(type).append("|<other>");
                    }
                    entry = this->window_counts_.try_emplace(full, 0).first;
                }
                ++entry->second;
                auto by_type = this->summarized_by_type_.find(type);
                if (by_type == this->summarized_by_type_.end())
                {
                    by_type = this->summarized_by_type_.emplace(std::string(type), 0).first;
                }
                ++by_type->second;
                ++this->summarized_events_;
                ++this->window_events_;
                this->maybe_emit_aggregate();
                return true;
            }

            // Keep the first `retained_per_key` occurrences of a distinct key in the report, count the rest.
            bool count_after_retained(const std::string_view type, const std::string& key)
            {
                thread_local std::string full;
                full.assign(type).append("|").append(key);
                auto entry = this->retained_seen_.find(full);
                if (entry == this->retained_seen_.end())
                {
                    if (this->retained_seen_.size() >= max_retained_keys)
                    {
                        return this->count(type, key);
                    }
                    entry = this->retained_seen_.try_emplace(full, 0).first;
                }
                if (entry->second < this->settings_.retained_per_key)
                {
                    ++entry->second;
                    return false;
                }
                return this->count(type, key);
            }

            void maybe_emit_aggregate()
            {
                if (this->window_events_ >= this->settings_.aggregate_interval_events)
                {
                    this->emit_aggregate(false);
                    return;
                }
                if ((++this->clock_check_counter_ & 1023) == 0 &&
                    std::chrono::steady_clock::now() - this->last_aggregate_ >= this->settings_.aggregate_interval)
                {
                    this->emit_aggregate(false);
                }
            }

            void emit_aggregate(const bool final)
            {
                if (this->window_events_ == 0)
                {
                    return;
                }
                std::vector<std::pair<std::string_view, uint64_t>> top;
                top.reserve(this->window_counts_.size());
                for (const auto& [name, value] : this->window_counts_)
                {
                    top.emplace_back(name, value);
                }
                std::partial_sort(top.begin(), top.begin() + std::min<size_t>(top.size(), 24), top.end(),
                                  [](const auto& a, const auto& b) { return a.second > b.second; });
                top.resize(std::min<size_t>(top.size(), 24));

                std::string line;
                line.reserve(4096);
                {
                    json_object_builder object{line};
                    object.field("type", "event_aggregate");
                    object.field("ic", this->last_instruction_count_);
                    object.field("tid", this->last_tid_);
                    object.field("windowEvents", this->window_events_);
                    object.field("summarizedEvents", this->summarized_events_);
                    object.field("retainedEvents", this->retained_events_);
                    object.field("distinctKeys", static_cast<uint64_t>(this->window_counts_.size()));
                    object.field("final", final);
                    object.object_field("byType", [&](json_object_builder& by_type) {
                        for (const auto& [name, value] : this->summarized_by_type_)
                        {
                            by_type.field(name, value);
                        }
                    });
                    object.array_field("top", [&](const auto& emit) {
                        for (const auto& [name, value] : top)
                        {
                            emit([&](std::string& out) {
                                json_object_builder row{out};
                                const auto split = name.find('|');
                                row.field("type", name.substr(0, split));
                                row.field("key", split == std::string_view::npos ? std::string_view{} : name.substr(split + 1));
                                row.field("count", value);
                            });
                        }
                    });
                }
                line.push_back('\n');
                this->file_.write(line);
                ++this->retained_events_;
                ++this->aggregates_written_;
                this->window_counts_.clear();
                this->window_events_ = 0;
                this->last_aggregate_ = std::chrono::steady_clock::now();
            }

            // Small sidecar for panels/MCP readers; best effort and never throws.
            void maybe_publish_status(const bool force)
            {
                if (this->settings_.status_path.empty())
                {
                    return;
                }
                if (!force)
                {
                    if ((++this->clock_check_counter_ & 1023) != 0)
                    {
                        return;
                    }
                    if (std::chrono::steady_clock::now() - this->last_status_ < this->settings_.status_interval)
                    {
                        return;
                    }
                }
                this->last_status_ = std::chrono::steady_clock::now();
                try
                {
                    std::string text;
                    text.reserve(1024);
                    {
                        json_object_builder object{text};
                        object.field("schema_version", 1U);
                        object.field("mode", this->settings_.mode == jsonl_report_mode::audit ? "audit" : "full");
                        object.field("retained_events", this->retained_events_);
                        object.field("summarized_events", this->summarized_events_);
                        object.field("aggregates_written", this->aggregates_written_);
                        object.field("dedupe", this->settings_.dedupe);
                        object.field("deduplicated_events", this->deduplicated_events_);
                        object.field("dedupe_keys", static_cast<uint64_t>(this->seen_content_.size()));
                        object.field("hidden_events", this->hidden_events_);
                        object.field("last_event_type", this->last_event_type_);
                        object.field("updated_unix_ms", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                                  std::chrono::system_clock::now().time_since_epoch())
                                                                                  .count()));
                        object.object_field("summarized_by_type", [&](json_object_builder& by_type) {
                            for (const auto& [name, value] : this->summarized_by_type_)
                            {
                                by_type.field(name, value);
                            }
                        });
                        object.object_field("last_location", [&](json_object_builder& location) {
                            location.hex_field("rip", this->last_rip_);
                            location.field("module", this->last_module_);
                            location.field("tid", this->last_tid_);
                            location.field("ic", this->last_instruction_count_);
                        });
                    }
                    text.push_back('\n');
                    const auto temporary = this->settings_.status_path.string() + ".tmp";
                    {
                        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
                    }
                    std::filesystem::rename(temporary, this->settings_.status_path);
                }
                catch (...)
                {
                    // Status is advisory; the report stream and the run must not depend on it.
                }
            }

            static void write_record(json_object_builder& object, const run_started_event& event)
            {
                object.field("type", "header");
                object.field("schema", 1U);
                write_fields(object, event);
            }

            static void write_record(json_object_builder& object, const run_finished_event& event)
            {
                object.field("type", "footer");
                write_fields(object, event);
            }

            static void write_record(json_object_builder& object, const run_failed_event& event)
            {
                object.field("type", "footer");
                object.field("success", false);
                write_fields(object, event);
            }

            template <typename Event>
            static void write_record(json_object_builder& object, const Event& event)
            {
                static_assert(std::is_base_of_v<observation_event, Event> || std::is_base_of_v<summary_event, Event>);

                object.field("type", event_name(event));

                if constexpr (std::is_base_of_v<observation_event, Event>)
                {
                    object.field("ic", event.header.instruction_count);
                    object.field("tid", event.execution.thread_id);
                    object.hex_field("rip", event.execution.rip);
                    object.field("mod", event.execution.rip_module);
                    object.optional_hex_field("prev", event.execution.previous_ip);
                    object.optional_string_field("prevMod", event.execution.previous_ip_module);
                }

                write_fields(object, event);
            }

#define EVENT_NAME(TYPE, NAME)                      \
    static std::string_view event_name(const TYPE&) \
    {                                               \
        return NAME;                                \
    }

            EVENT_NAME(instruction_summary_event, "instruction_summary");
            EVENT_NAME(buffered_stdout_event, "buffered_stdout");
            EVENT_NAME(stdout_chunk_event, "stdout_chunk");
            EVENT_NAME(suspicious_activity_event, "suspicious_activity");
            EVENT_NAME(debug_print_call_event, "debug_print_call");
            EVENT_NAME(debug_string_event, "debug_string");
            EVENT_NAME(generic_activity_event, "generic_activity");
            EVENT_NAME(generic_access_event, "generic_access");
            EVENT_NAME(memory_allocate_event, "memory_allocate");
            EVENT_NAME(memory_protect_event, "memory_protect");
            EVENT_NAME(memory_violation_event, "memory_violation");
            EVENT_NAME(io_control_event, "io_control");
            EVENT_NAME(thread_create_event, "thread_create");
            EVENT_NAME(thread_terminated_event, "thread_terminated");
            EVENT_NAME(thread_set_name_event, "thread_set_name");
            EVENT_NAME(thread_switch_event, "thread_switch");
            EVENT_NAME(module_load_event, "module_load");
            EVENT_NAME(module_unload_event, "module_unload");
            EVENT_NAME(import_read_event, "import_read");
            EVENT_NAME(import_write_event, "import_write");
            EVENT_NAME(object_access_event, "object_access");
            EVENT_NAME(environment_access_event, "environment_access");
            EVENT_NAME(function_execution_event, "function_execution");
            EVENT_NAME(entry_point_execution_event, "entry_point_execution");
            EVENT_NAME(foreign_code_transition_event, "foreign_code_transition");
            EVENT_NAME(section_first_execute_event, "section_first_execute");
            EVENT_NAME(execution_progress_event, "execution_progress");
            EVENT_NAME(rdtsc_event, "rdtsc");
            EVENT_NAME(rdtscp_event, "rdtscp");
            EVENT_NAME(cpuid_event, "cpuid");
            EVENT_NAME(syscall_event, "syscall");
            EVENT_NAME(foreign_module_read_event, "foreign_module_read");
            EVENT_NAME(executable_read_event, "executable_read");
            EVENT_NAME(executable_write_event, "executable_write");
            EVENT_NAME(fast_fail_event, "fast_fail");

#undef EVENT_NAME

            static void write_fields(json_object_builder& object, const run_started_event& event)
            {
                object.field("backend", event.backend_name);
                object.field("mode", event.mode);
                if (!event.application.empty())
                {
                    object.field("app", event.application);
                }

                object.array_field("args", [&](const auto& emit) {
                    for (const auto& arg : event.arguments)
                    {
                        emit([&](std::string& out) {
                            out += '"';
                            append_escaped_json(out, arg);
                            out += '"';
                        });
                    }
                });
            }

            static void write_fields(json_object_builder& object, const run_finished_event& event)
            {
                object.field("success", event.success);
                if (event.checkpoint_saved)
                {
                    object.field("state", "paused");
                    object.field("checkpoint_saved", true);
                    object.hex_field("rip", event.rip);
                }
                if (event.exit_status.has_value())
                {
                    object.field("exit", *event.exit_status);
                }
            }

            static void write_fields(json_object_builder& object, const run_failed_event& event)
            {
                object.hex_field("rip", event.rip);
                object.field("error", event.message);
                object.field("phase", event.phase);
            }

            static void write_fields(json_object_builder& object, const instruction_summary_event& event)
            {
                object.array_field("entries", [&](const auto& emit) {
                    for (const auto& entry : event.entries)
                    {
                        emit([&](std::string& out) {
                            json_object_builder entry_object{out};
                            entry_object.field("mnemonic", entry.mnemonic);
                            entry_object.field("count", entry.count);
                        });
                    }
                });
            }

            static void write_fields(json_object_builder& object, const buffered_stdout_event& event)
            {
                object.field("data", event.data);
            }

            static void write_fields(json_object_builder& object, const stdout_chunk_event& event)
            {
                object.field("data", event.data);
            }

            static void write_fields(json_object_builder& object, const suspicious_activity_event& event)
            {
                object.field("details", event.details);
                if (!event.decoded_instruction.empty())
                {
                    object.field("inst", event.decoded_instruction);
                }
            }

            static void write_fields(json_object_builder& object, const debug_print_call_event& event)
            {
                object.field("call_id", event.call_id);
                object.field("api", event.api);
                object.field("pointer_bits", event.pointer_bits);
                object.hex_field("stack_pointer", event.stack_pointer);
                object.hex_field("return_address", event.return_address);
                object.field("return_module", event.return_module);
                object.field("error", event.error);
                object.array_field("arguments", [&](const auto& append) {
                    for (const auto& arg : event.arguments)
                    {
                        append([&](std::string& output) {
                            json_object_builder item(output);
                            item.field("name", arg.name);
                            item.hex_field("raw", arg.raw);
                            if (!arg.encoding.empty())
                            {
                                item.field("text", arg.text);
                                item.field("encoding", arg.encoding);
                                item.field("bytes_hex", arg.bytes_hex);
                            }
                            if (!arg.error.empty())
                            {
                                item.field("error", arg.error);
                            }
                        });
                    }
                });
            }

            static void write_fields(json_object_builder& object, const debug_string_event& event)
            {
                object.field("details", event.details);
                object.field("transport", event.transport);
                object.hex_field("data_address", event.data_address);
                object.field("byte_length", event.byte_length);
                object.field("encoding", event.encoding);
                object.field("bytes_hex", event.bytes_hex);
                object.field("error", event.error);
                if (event.ansi_fallback)
                {
                    object.field("ansi_fallback_text", event.ansi_fallback->text);
                    object.field("ansi_fallback_bytes_hex", event.ansi_fallback->bytes_hex);
                    object.hex_field("ansi_fallback_address", event.ansi_fallback->raw);
                    object.field("ansi_fallback_error", event.ansi_fallback->error);
                }
                object.field("component", event.component);
                object.field("level", event.level);
                object.array_field("origin_calls", [&](const auto& append) {
                    for (const auto call : event.origin_calls)
                    {
                        append([&](std::string& output) { output += std::to_string(call); });
                    }
                });
            }

            static void write_fields(json_object_builder& object, const generic_activity_event& event)
            {
                object.field("details", event.details);
            }

            static void write_fields(json_object_builder& object, const generic_access_event& event)
            {
                object.field("accessType", event.type);
                object.field("name", event.name);
            }

            static void write_fields(json_object_builder& object, const memory_allocate_event& event)
            {
                object.hex_field("addr", event.address);
                object.field("len", event.length);
                object.field("perms", event.permissions);
                object.field("commit", event.commit);
            }

            static void write_fields(json_object_builder& object, const memory_protect_event& event)
            {
                object.hex_field("addr", event.address);
                object.field("len", event.length);
                object.field("perms", event.permissions);
            }

            static void write_fault_address(json_object_builder& object, const fault_address_snapshot& location)
            {
                object.hex_field("address", location.address);
                object.optional_string_field("module", location.module);
                object.optional_hex_field("moduleBase", location.module_base);
                object.optional_hex_field("moduleRva", location.module_rva);
                object.field("locationError", location.error);
                if (location.region)
                {
                    object.object_field("region", [&](auto& region) {
                        const auto& value = *location.region;
                        region.hex_field("start", value.start);
                        region.hex_field("length", value.length);
                        region.hex_field("allocationBase", value.allocation_base);
                        region.hex_field("allocationLength", value.allocation_length);
                        region.field("permissions", value.permissions);
                        region.field("kind", value.kind);
                        region.field("reserved", value.reserved);
                        region.field("committed", value.committed);
                        region.field("guarded", value.guarded);
                    });
                }
            }

            static void write_fault_instruction(json_object_builder& object, const fault_instruction_snapshot& instruction)
            {
                write_fault_address(object, instruction.location);
                object.field("bytes", instruction.bytes_hex);
                object.field("asm", instruction.assembly);
                object.field("decodedSize", instruction.decoded_size);
                object.field("error", instruction.error.empty() ? instruction.location.error : instruction.error);
            }

            static void write_fields(json_object_builder& object, const memory_violation_event& event)
            {
                object.hex_field("addr", event.address);
                object.field("size", event.size);
                object.field("op", event.operation);
                object.field("violation", event.violation_type);
                object.field("nearNullExecute", event.near_null_execute);
                object.field("captureError", event.capture_error);
                if (event.code_bits)
                {
                    object.field("codeBits", *event.code_bits);
                }
                object.object_field("faultAddress", [&](auto& value) { write_fault_address(value, event.fault_address); });
                object.object_field("actualInstruction", [&](auto& value) { write_fault_instruction(value, event.actual_instruction); });
                if (event.last_tracked_instruction)
                {
                    object.object_field("lastTrackedInstruction", [&](auto& value) {
                        write_fault_instruction(value, *event.last_tracked_instruction);
                        value.field("decodeModeSource", "fault_cs");
                    });
                }
                object.array_field("registers", [&](const auto& emit) {
                    for (const auto& reg : event.registers)
                    {
                        emit([&](std::string& output) {
                            json_object_builder value{output};
                            value.field("name", reg.name);
                            value.optional_hex_field("value", reg.value);
                            value.field("error", reg.error);
                        });
                    }
                });
                object.object_field("stackSlot", [&](auto& value) {
                    if (event.stack_slot.pointer_bits)
                    {
                        value.field("pointerBits", *event.stack_slot.pointer_bits);
                    }
                    if (event.stack_slot.address_bits)
                    {
                        value.field("addressBits", *event.stack_slot.address_bits);
                    }
                    value.optional_hex_field("segmentBase", event.stack_slot.segment_base);
                    value.field("widthSource", event.stack_slot.width_source);
                    value.field("addressSource", event.stack_slot.address_source);
                    value.optional_hex_field("address", event.stack_slot.address);
                    value.optional_hex_field("value", event.stack_slot.value);
                    value.field("error", event.stack_slot.error);
                    if (event.stack_slot.value_location)
                    {
                        value.object_field("valueLocation",
                                           [&](auto& location) { write_fault_address(location, *event.stack_slot.value_location); });
                    }
                });
            }

            static void write_fields(json_object_builder& object, const io_control_event& event)
            {
                object.field("device", event.device_name);
                object.field("code", event.code);
            }

            static void write_fields(json_object_builder& object, const thread_create_event& event)
            {
                object.field("createdTid", event.created_thread_id);
                object.hex_field("start", event.start_address);
                object.hex_field("arg", event.argument);
                object.array_field("flags", [&](const auto& emit) {
                    for (const auto& flag : event.flags)
                    {
                        emit([&](std::string& out) {
                            out += '"';
                            append_escaped_json(out, flag);
                            out += '"';
                        });
                    }
                });
            }

            static void write_fields(json_object_builder& object, const thread_terminated_event& event)
            {
                object.field("terminatedTid", event.terminated_thread_id);
                if (event.exit_status.has_value())
                {
                    object.field("exit", *event.exit_status);
                }
            }

            static void write_fields(json_object_builder& object, const thread_set_name_event& event)
            {
                object.field("namedTid", event.renamed_thread_id);
                object.field("name", event.name);
            }

            static void write_fields(json_object_builder& object, const thread_switch_event& event)
            {
                object.field("fromTid", event.previous_thread_id);
                object.field("toTid", event.next_thread_id);
            }

            static void write_fields(json_object_builder& object, const module_load_event& event)
            {
                object.field("path", event.path);
                object.hex_field("base", event.image_base);
            }

            static void write_fields(json_object_builder& object, const module_unload_event& event)
            {
                object.field("path", event.path);
                object.hex_field("base", event.image_base);
            }

            static void write_fields(json_object_builder& object, const import_read_event& event)
            {
                object.hex_field("target", event.resolved_address);
                object.field("import", event.import_name);
                object.field("importMod", event.import_module);
            }

            static void write_fields(json_object_builder& object, const import_write_event& event)
            {
                object.field("size", static_cast<uint64_t>(event.size));
                object.hex_field("value", event.value);
                object.field("import", event.import_name);
                object.field("importMod", event.import_module);
            }

            static void write_fields(json_object_builder& object, const object_access_event& event)
            {
                object.field("mainAccess", event.main_access);
                object.field("typeName", event.type_name);
                object.hex_field("offset", event.offset);
                object.hex_field("size", event.size);
                if (event.member_name.has_value())
                {
                    object.field("member", *event.member_name);
                }
            }

            static void write_fields(json_object_builder& object, const environment_access_event& event)
            {
                object.field("mainAccess", event.main_access);
                object.hex_field("offset", event.offset);
                object.hex_field("size", event.size);
            }

            static void write_fields(json_object_builder& object, const function_execution_event& event)
            {
                object.field("callCount", event.call_count);
                object.field("fn", event.function_name);
                object.field("interesting", event.interesting);
                object.array_field("details", [&](const auto& emit) {
                    for (const auto& detail : event.details)
                    {
                        emit([&](std::string& out) {
                            json_object_builder detail_object{out};
                            detail_object.field("label", detail.label);
                            detail_object.field("value", detail.value);
                        });
                    }
                });
            }

            static void write_fields(json_object_builder& object, const entry_point_execution_event& event)
            {
                object.field("interesting", event.interesting);
            }

            static void write_fields(json_object_builder& object, const foreign_code_transition_event& event)
            {
                object.field("fn", event.function_name);
                object.hex_field("off", event.function_offset);
                object.field("interesting", event.interesting);
            }

            static void write_fields(json_object_builder& object, const section_first_execute_event& event)
            {
                object.field("moduleName", event.module_name);
                object.field("section", event.section_name);
                object.hex_field("fileAddr", event.file_address);
            }

            static void write_fields(json_object_builder& object, const execution_progress_event& event)
            {
                object.field("elapsedMs", event.elapsed_milliseconds);
                object.field("instructionsPerSecond", event.instructions_per_second);
                if (event.module_rva)
                {
                    object.hex_field("moduleRva", *event.module_rva);
                }
            }

            static void write_fields(json_object_builder&, const rdtsc_event&)
            {
            }

            static void write_fields(json_object_builder&, const rdtscp_event&)
            {
            }

            static void write_fields(json_object_builder& object, const cpuid_event& event)
            {
                object.field("leaf", event.leaf);
            }

            static void write_fields(json_object_builder& object, const syscall_event& event)
            {
                object.field("callCount", event.call_count);
                object.field("class", syscall_classification_name(event.classification));
                object.field("id", event.syscall_id);
                object.field("name", event.syscall_name);
                object.optional_hex_field("callerRip", event.caller_rip);
                object.optional_string_field("callerMod", event.caller_module);
            }

            static void write_fields(json_object_builder& object, const foreign_module_read_event& event)
            {
                object.hex_field("addr", event.address);
                object.field("size", static_cast<uint64_t>(event.size));
                object.field("moduleName", event.module_name);
                object.field("region", event.region_name);
            }

            static void write_fields(json_object_builder& object, const executable_read_event& event)
            {
                object.hex_field("addr", event.address);
                object.field("size", static_cast<uint64_t>(event.size));
                object.field("section", event.section_name);
            }

            static void write_fields(json_object_builder& object, const executable_write_event& event)
            {
                object.hex_field("addr", event.address);
                object.field("size", static_cast<uint64_t>(event.size));
                object.hex_field("value", event.value);
                object.field("section", event.section_name);
            }

            static void write_fields(json_object_builder& object, const fast_fail_event& event)
            {
                object.field("fail_code", static_cast<uint64_t>(event.fail_code));
            }
        };
    }

    std::unique_ptr<analysis_reporter> create_jsonl_reporter(const std::filesystem::path& path, jsonl_report_settings settings)
    {
        return std::make_unique<jsonl_analysis_reporter>(path, std::move(settings));
    }

    bool event_is_deduplicable(const analysis_event& event)
    {
        return std::visit(
            make_overloaded([](const run_started_event&) { return false; }, [](const run_finished_event&) { return false; },
                            [](const run_failed_event&) { return false; }, [](const stdout_chunk_event&) { return false; },
                            [](const buffered_stdout_event&) { return false; }, [](const instruction_summary_event&) { return false; },
                            [](const execution_progress_event&) { return false; }, [](const thread_switch_event&) { return false; },
                            [](const memory_violation_event&) { return false; }, [](const fast_fail_event&) { return false; },
                            [](const entry_point_execution_event&) { return false; }, [](const auto&) { return true; }),
            event);
    }

    namespace
    {
        bool equals_ignoring_case(const std::string_view left, const std::string_view right)
        {
            return std::ranges::equal(left, right, [](const char a, const char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            });
        }

        bool module_is_hidden(const std::string_view module, const std::vector<std::string>& hidden)
        {
            for (const auto& name : hidden)
            {
                if (equals_ignoring_case(module, name))
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool event_from_hidden_module(const analysis_event& event, const std::vector<std::string>& hidden)
    {
        if (hidden.empty())
        {
            return false;
        }
        return std::visit(make_overloaded([](const run_started_event&) { return false; }, [](const run_finished_event&) { return false; },
                                          [](const run_failed_event&) { return false; },
                                          [](const memory_violation_event&) { return false; }, [](const fast_fail_event&) { return false; },
                                          [&](const auto& e) {
                                              using event_type = std::decay_t<decltype(e)>;
                                              if constexpr (std::is_base_of_v<observation_event, event_type>)
                                              {
                                                  if (module_is_hidden(e.execution.rip_module, hidden))
                                                  {
                                                      return true;
                                                  }
                                                  return e.execution.previous_ip_module.has_value() &&
                                                         module_is_hidden(*e.execution.previous_ip_module, hidden);
                                              }
                                              else
                                              {
                                                  return false;
                                              }
                                          }),
                          event);
    }

    uint64_t record_content_hash(const std::string_view record)
    {
        // Values are skipped in the three shapes json_object_builder writes them: "digits", digits, "0x...".
        static constexpr std::array<std::string_view, 6> volatile_keys{
            "\"ic\":", "\"callCount\":", "\"call_id\":", "\"stack_pointer\":", "\"raw\":", "\"data_address\":"};
        uint64_t hash = 14695981039346656037ULL;
        auto rest = record;
        while (!rest.empty())
        {
            const auto key =
                std::ranges::find_if(volatile_keys, [&](const std::string_view candidate) { return rest.starts_with(candidate); });
            if (key != volatile_keys.end())
            {
                rest.remove_prefix(key->size());
                if (rest.starts_with('"'))
                {
                    const auto end = rest.find('"', 1);
                    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
                }
                else
                {
                    while (!rest.empty() && rest.front() >= '0' && rest.front() <= '9')
                    {
                        rest.remove_prefix(1);
                    }
                }
                continue;
            }
            hash ^= static_cast<unsigned char>(rest.front());
            hash *= 1099511628211ULL;
            rest.remove_prefix(1);
        }
        return hash;
    }

    uint64_t event_content_hash(const analysis_event& event)
    {
        return jsonl_analysis_reporter::content_hash(event);
    }

    std::string_view event_type_name(const analysis_event& event)
    {
        return jsonl_analysis_reporter::type_name(event);
    }

    bool event_of_hidden_type(const analysis_event& event, const std::vector<std::string>& hidden_types)
    {
        if (hidden_types.empty() || std::holds_alternative<run_started_event>(event) || std::holds_alternative<run_finished_event>(event) ||
            std::holds_alternative<run_failed_event>(event) || std::holds_alternative<memory_violation_event>(event) ||
            std::holds_alternative<fast_fail_event>(event))
        {
            return false;
        }
        return module_is_hidden(event_type_name(event), hidden_types);
    }
} // namespace sogen
