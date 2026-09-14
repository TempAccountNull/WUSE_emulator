#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include <utils/string.hpp>

namespace sogen
{

    namespace syscalls
    {

        namespace
        {
            constexpr ULONG object_case_insensitive = 0x40;
            constexpr std::u16string_view maximum_commit_event = u"\\KernelObjects\\MaximumCommitCondition";

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
                if (c.proc.dbwin_buffer && c.win_emu.callbacks.on_debug_string)
                {
                    constexpr auto pid_length = 4;
                    const auto debug_data = read_string<char>(c.win_emu.memory, c.proc.dbwin_buffer + pid_length);
                    c.win_emu.callbacks.on_debug_string(debug_data);
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
            bool is_signaled = false;

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
