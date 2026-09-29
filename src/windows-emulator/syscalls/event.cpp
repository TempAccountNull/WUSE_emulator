#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../kusd_mmio.hpp"
#include "../syscall_utils.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <utils/string.hpp>

namespace sogen
{

    namespace syscalls
    {

        namespace
        {
            constexpr ULONG object_case_insensitive = 0x40;
            constexpr std::u16string_view maximum_commit_event = u"\\KernelObjects\\MaximumCommitCondition";

            std::string_view guest_clock_probe_marker(const std::string_view message)
            {
                if (message.find("Entering state 'bootflow:package_registration'") != std::string_view::npos)
                {
                    return "package_enter";
                }
                if (message.find("Total time spent:") != std::string_view::npos &&
                    message.find("name: [bootflow:package_registration]") != std::string_view::npos)
                {
                    return "package_exit";
                }
                if (message.find("Entering state 'bootflow:bap_signin'") != std::string_view::npos)
                {
                    return "bap_enter";
                }
                if (message.find("_channel_starting") != std::string_view::npos && message.find("Message '0'") != std::string_view::npos &&
                    message.find("has timed out") != std::string_view::npos)
                {
                    return "bap_first_message_timeout";
                }
                if (message.find("networking:server:bap:") != std::string_view::npos &&
                    message.find("has timed out") != std::string_view::npos)
                {
                    return "bap_message_timeout";
                }
                if (message.find("_channel_starting") != std::string_view::npos &&
                    message.find("Fatal error '_connection_failure_timed_out'") != std::string_view::npos)
                {
                    return "bap_channel_fatal";
                }
                if (message.find("BAP connection failed due to hitting timeout") != std::string_view::npos)
                {
                    return "bap_signin_timeout";
                }
                return {};
            }

            void emit_guest_clock_probe(const syscall_context& c, const std::string_view message)
            {
                static const bool enabled = [] {
                    const char* const value = std::getenv("SOGEN_GUEST_CLOCK_PROBE");
                    return value != nullptr && std::string_view{value} == "1";
                }();
                if (!enabled || !c.win_emu.callbacks.on_debug_string)
                {
                    return;
                }

                const auto marker = guest_clock_probe_marker(message);
                if (marker.empty())
                {
                    return;
                }

                static std::atomic<uint32_t> records{};
                if (records.fetch_add(1, std::memory_order_relaxed) >= 32)
                {
                    return;
                }

                const auto host_monotonic = std::chrono::steady_clock::now().time_since_epoch();
                const auto host_wall = std::chrono::system_clock::now().time_since_epoch();
                const auto guest_qpc = c.win_emu.clock().steady_now().time_since_epoch().count();
                const auto guest_tsc = c.win_emu.clock().timestamp_counter();
                KUSER_SHARED_DATA64 shared{};
                if (!c.win_emu.memory.try_read_memory(kusd_mmio::address(), &shared, sizeof(shared)))
                {
                    return;
                }
                const auto interrupt_100ns =
                    (static_cast<uint64_t>(static_cast<uint32_t>(shared.InterruptTime.High1Time)) << 32) | shared.InterruptTime.LowPart;
                const auto tick_ms = (shared.TickCount.TickCountQuad * shared.TickCountMultiplier) >> 24;
                const auto interrupt_ms = interrupt_100ns / 10000;
                const auto host_monotonic_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(host_monotonic).count();
                const auto host_tick_ms = GetTickCount64();
                const auto host_unix_us = std::chrono::duration_cast<std::chrono::microseconds>(host_wall).count();
                std::array<char, 512> line{};
                const int length =
                    std::snprintf(line.data(), line.size(),
                                  "guest_clock_probe marker=%.*s guest_qpc=%lld qpc_frequency=%lld kusd_tick_raw=%llu kusd_tick_ms=%llu "
                                  "kusd_tick_ms_low32=%u kusd_interrupt_100ns=%llu kusd_interrupt_ms=%llu kusd_interrupt_ms_low32=%u "
                                  "guest_tsc=%llu host_monotonic_ns=%lld host_unix_us=%lld "
                                  "host_tick_ms=%llu",
                                  static_cast<int>(marker.size()), marker.data(), static_cast<long long>(guest_qpc),
                                  static_cast<long long>(shared.QpcFrequency),
                                  static_cast<unsigned long long>(shared.TickCount.TickCountQuad), static_cast<unsigned long long>(tick_ms),
                                  static_cast<uint32_t>(tick_ms), static_cast<unsigned long long>(interrupt_100ns),
                                  static_cast<unsigned long long>(interrupt_ms), static_cast<uint32_t>(interrupt_ms),
                                  static_cast<unsigned long long>(guest_tsc), static_cast<long long>(host_monotonic_ns),
                                  static_cast<long long>(host_unix_us), static_cast<unsigned long long>(host_tick_ms));
                if (length > 0 && static_cast<size_t>(length) < line.size())
                {
                    c.win_emu.callbacks.on_debug_string({line.data(), static_cast<size_t>(length)});
                }
            }

            void emit_bap_fatal_stack_probe(const syscall_context& c, const std::string_view message)
            {
                static const bool enabled = [] {
                    const char* const value = std::getenv("SOGEN_BAP_FATAL_STACK_PROBE");
                    return value != nullptr && std::string_view{value} == "1";
                }();
                if (!enabled || message.find("Fatal error '_connection_failure_timed_out' raised.") == std::string_view::npos)
                {
                    return;
                }

                static std::atomic_flag captured = ATOMIC_FLAG_INIT;
                if (captured.test_and_set(std::memory_order_relaxed))
                {
                    return;
                }

                const uint64_t rip = c.emu.reg<uint64_t>(x86_register::rip);
                const uint64_t rsp = c.emu.reg<uint64_t>(x86_register::rsp);
                const uint32_t tid = c.vcpu.active_thread ? c.vcpu.active_thread->id : 0;
                uint64_t first_word{};
                bool first_read{};
                size_t readable{};
                size_t candidates_logged{};
                std::string candidates;
                candidates.reserve(960);
                constexpr size_t max_stack_words = 64;
                constexpr size_t max_candidates = 12;
                for (size_t index = 0; index < max_stack_words; ++index)
                {
                    const uint64_t offset = index * sizeof(uint64_t);
                    uint64_t word{};
                    if (!rsp || rsp > UINT64_MAX - offset - sizeof(word) || !c.emu.try_read_memory(rsp + offset, &word, sizeof(word)))
                    {
                        continue;
                    }
                    ++readable;
                    if (index == 0)
                    {
                        first_word = word;
                        first_read = true;
                    }
                    if (candidates_logged == max_candidates)
                    {
                        continue;
                    }
                    const auto* module = c.win_emu.mod_manager.find_by_address(word);
                    if (!module)
                    {
                        continue;
                    }
                    std::array<char, 96> candidate{};
                    const int length = std::snprintf(candidate.data(), candidate.size(), "%ss%zu:%.*s+%#llx", candidates.empty() ? "" : ",",
                                                     index, static_cast<int>(std::min<size_t>(module->name.size(), 40)),
                                                     module->name.c_str(), static_cast<unsigned long long>(word - module->image_base));
                    if (length > 0 && static_cast<size_t>(length) < candidate.size())
                    {
                        candidates.append(candidate.data(), static_cast<size_t>(length));
                        ++candidates_logged;
                    }
                }

                const auto* rip_module = c.win_emu.mod_manager.find_by_address(rip);
                std::string rip_module_name = "<unmapped>";
                if (rip_module)
                {
                    rip_module_name.clear();
                    for (const char character : rip_module->name.substr(0, 40))
                    {
                        const auto byte = static_cast<unsigned char>(character);
                        rip_module_name.push_back(byte >= 0x20 && byte <= 0x7E ? character : '?');
                    }
                }
                c.win_emu.log.warn("BAPFATALSTACK kind=raw_stack_candidates tid=%u vcpu=%zu rip=%#llx rip_module=%s rip_rva=%#llx "
                                   "rsp=%#llx first_word=%#llx first_read=%u readable=%zu scanned=%zu candidates=[%s]\n",
                                   tid, c.emu.index(), static_cast<unsigned long long>(rip), rip_module_name.c_str(),
                                   rip_module ? static_cast<unsigned long long>(rip - rip_module->image_base) : 0ull,
                                   static_cast<unsigned long long>(rsp), static_cast<unsigned long long>(first_word),
                                   static_cast<unsigned>(first_read), readable, max_stack_words, candidates.c_str());
            }

            std::optional<handle> open_named_event(process_context& process, const std::u16string_view name, const bool case_insensitive)
            {
                const auto matches = [&](const std::u16string_view candidate) {
                    return case_insensitive ? utils::string::equals_ignore_case(name, candidate) : name == candidate;
                };

                for (auto& [id, entry] : process.events)
                {
                    if (matches(entry.name))
                    {
                        ++entry.ref_count;
                        return process.events.make_handle(id);
                    }
                }

                if (!matches(maximum_commit_event))
                {
                    return std::nullopt;
                }

                event entry{};
                entry.name = maximum_commit_event;
                entry.type = NotificationEvent;
                // The kernel owns a reference independently of open user handles. System commit-pressure
                // transitions are not modeled; the event starts nonsignaled, as in MiCreateMemoryEvent.
                entry.ref_count = 2;
                return process.events.store(std::move(entry));
            }
        }

        NTSTATUS handle_NtSetEvent(const syscall_context& c, const uint64_t handle, const emulator_object<LONG> previous_state)
        {
            if (handle == DBWIN_DATA_READY)
            {
                if (c.proc.dbwin_buffer && (c.win_emu.callbacks.on_debug_string || c.win_emu.package_reads_trace.enabled()))
                {
                    constexpr auto pid_length = 4;
                    std::array<char, 4096 - pid_length> buffer{};
                    if (!c.win_emu.memory.try_read_memory(c.proc.dbwin_buffer + pid_length, buffer.data(), buffer.size()))
                    {
                        if (c.win_emu.callbacks.on_debug_string_error)
                        {
                            c.win_emu.callbacks.on_debug_string_error(c.proc.dbwin_buffer + pid_length, "DBWIN buffer unreadable");
                        }
                        return STATUS_SUCCESS;
                    }
                    const auto end = std::ranges::find(buffer, '\0');
                    const std::string_view message(buffer.data(), static_cast<size_t>(end - buffer.begin()));
                    if (c.win_emu.callbacks.on_debug_string)
                    {
                        c.win_emu.callbacks.on_debug_string(message);
                        emit_guest_clock_probe(c, message);
                        emit_bap_fatal_stack_probe(c, message);
                    }
                    if (c.win_emu.package_reads_trace.trigger_on_oodle(message) && c.win_emu.callbacks.on_debug_string)
                    {
                        const auto snapshot = c.win_emu.package_reads_trace.snapshot_json();
                        c.win_emu.callbacks.on_debug_string("Package read snapshot at first OODLE ERROR: " + snapshot);
                    }
                }

                return STATUS_SUCCESS;
            }

            auto* entry = c.proc.events.get(handle);
            if (!entry)
            {
                return STATUS_INVALID_HANDLE;
            }

            if (previous_state.value())
            {
                previous_state.write(entry->signaled ? 1ULL : 0ULL);
            }

            entry->signaled = true;
            c.proc.process_graphics_commands();
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtPulseEvent(const syscall_context& c, const uint64_t handle, const emulator_object<LONG> previous_state)
        {
            auto* entry = c.proc.events.get(handle);
            if (!entry)
            {
                return STATUS_INVALID_HANDLE;
            }

            if (previous_state.value())
            {
                previous_state.write(entry->signaled ? 1ULL : 0ULL);
            }

            // Pulse: momentarily signal the event so threads already blocked on it wake, then return it to
            // the non-signaled state. Threads that are not currently waiting miss the pulse, matching the
            // lossy NtPulseEvent semantics. The cooperative scheduler only re-evaluates readiness at context
            // switches, so wake the current waiters explicitly while the event is signaled.
            entry->signaled = true;

            const auto event_handle = make_handle(handle);
            for (auto& thread : c.proc.threads | std::views::values)
            {
                if (std::ranges::find(thread.await_objects, event_handle) != thread.await_objects.end())
                {
                    (void)thread.is_thread_ready(c.win_emu);
                }
            }

            entry->signaled = false;
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtTraceEvent()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtQueryEvent(const syscall_context& c, const handle event_handle, const uint32_t event_information_class,
                                     const emulator_object<EVENT_BASIC_INFORMATION> event_information,
                                     const uint32_t event_information_length, const emulator_object<uint32_t> return_length)
        {
            if (event_information_class != 0) // EventBasicInformation
            {
                return STATUS_INVALID_INFO_CLASS;
            }

            if (event_information_length < sizeof(EVENT_BASIC_INFORMATION))
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            EVENT_TYPE type = NotificationEvent;
            bool is_signaled = event_handle == LSA_AUTHENTICATION_INITIALIZED;

            if (auto* entry = c.proc.events.get(event_handle))
            {
                type = entry->type;
                is_signaled = entry->signaled;
            }

            event_information.access([&](EVENT_BASIC_INFORMATION& info) {
                info.EventType = type;
                info.EventState = is_signaled ? 1 : 0;
            });

            if (return_length)
            {
                return_length.write(sizeof(EVENT_BASIC_INFORMATION));
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtClearEvent(const syscall_context& c, const handle event_handle)
        {
            auto* e = c.proc.events.get(event_handle);
            if (!e)
            {
                return STATUS_INVALID_HANDLE;
            }

            e->signaled = false;
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtCreateEvent(const syscall_context& c, const emulator_object<handle> event_handle,
                                      const ACCESS_MASK /*desired_access*/,
                                      const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                      const EVENT_TYPE event_type, const BOOLEAN initial_state)
        {
            std::u16string name{};
            bool case_insensitive{};
            if (object_attributes)
            {
                const auto attributes = object_attributes.read();
                case_insensitive = (attributes.Attributes & object_case_insensitive) != 0;
                if (attributes.ObjectName)
                {
                    name = read_unicode_string(c.emu, attributes.ObjectName);
                    c.win_emu.callbacks.on_generic_access("Opening event", name);
                }
            }

            if (!name.empty())
            {
                if (const auto existing = open_named_event(c.proc, name, case_insensitive))
                {
                    event_handle.write(*existing);
                    return STATUS_OBJECT_NAME_EXISTS;
                }
            }

            event e{};
            e.type = event_type;
            e.signaled = initial_state != FALSE;
            e.name = std::move(name);

            const auto handle = c.proc.events.store(std::move(e));
            event_handle.write(handle);

            static_assert(sizeof(EVENT_TYPE) == sizeof(uint32_t));
            static_assert(sizeof(ACCESS_MASK) == sizeof(uint32_t));

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtOpenEvent(const syscall_context& c, const emulator_object<uint64_t> event_handle,
                                    const ACCESS_MASK /*desired_access*/,
                                    const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
        {
            const auto attributes = object_attributes.read();
            const auto name = read_unicode_string(c.emu, attributes.ObjectName);
            c.win_emu.callbacks.on_generic_access("Opening event", name);

            if (name == u"\\KernelObjects\\SystemErrorPortReady")
            {
                event_handle.write(WER_PORT_READY.bits);
                return STATUS_SUCCESS;
            }

            if (name == u"Global\\SvcctrlStartEvent_A3752DX")
            {
                event_handle.write(SVCCTRL_START_EVENT.bits);
                return STATUS_SUCCESS;
            }

            if (name == u"\\SECURITY\\LSA_AUTHENTICATION_INITIALIZED")
            {
                event_handle.write(LSA_AUTHENTICATION_INITIALIZED.bits);
                return STATUS_SUCCESS;
            }

            if (name == u"DBWIN_DATA_READY")
            {
                event_handle.write(DBWIN_DATA_READY.bits);
                return STATUS_SUCCESS;
            }

            if (name == u"DBWIN_BUFFER_READY")
            {
                event_handle.write(DBWIN_BUFFER_READY.bits);
                return STATUS_SUCCESS;
            }

            if (const auto existing = open_named_event(c.proc, name, (attributes.Attributes & object_case_insensitive) != 0))
            {
                event_handle.write(existing->bits);
                return STATUS_SUCCESS;
            }

            return STATUS_NOT_FOUND;
        }
    }

} // namespace sogen
