#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace sogen
{

    namespace syscalls
    {
        NTSTATUS handle_NtRaiseHardError(const syscall_context& c, const NTSTATUS error_status, const ULONG number_of_parameters,
                                         const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> /*unicode_string_parameter_mask*/,
                                         const uint64_t parameters, const HARDERROR_RESPONSE_OPTION /*valid_response_option*/,
                                         const emulator_object<HARDERROR_RESPONSE> response)
        {
            if (response)
            {
                response.try_write(ResponseAbort);
            }

            if (error_status & STATUS_SERVICE_NOTIFICATION && number_of_parameters >= 3)
            {
                std::array<uint64_t, 3> params = {0, 0, 0};

                try
                {
                    if (c.emu.try_read_memory(parameters, &params, sizeof(params)))
                    {
                        const auto message =
                            read_unicode_string(c.emu, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{c.emu, params[0]});
                        c.win_emu.log.error("Error Message: %s\n", u16_to_u8(message).c_str());
                    }
                }
                catch (...)
                {
                    // ignore
                }
            }

            c.proc.exit_status = error_status;
            c.win_emu.callbacks.on_exception();
            c.emu.stop();

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtRaiseException(const syscall_context& c,
                                         const emulator_object<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>> exception_record,
                                         const emulator_object<CONTEXT64> /*thread_context*/, const BOOLEAN handle_exception)
        {
            if (handle_exception)
            {
                c.win_emu.log.error("Unhandled exceptions not supported yet!\n");
                c.emu.stop();
                return STATUS_NOT_SUPPORTED;
            }

            // SMP-6.6 DIAGNOSTIC: print the full crash context for an unhandled guest exception so the
            // N>1 probe's guest AV is traceable (code, faulting address, info words, TID, RIP).
            const auto record = exception_record.read();
            c.win_emu.log.error(
                "SMPDIAG unhandled guest exception: code=0x%08X addr=0x%llX params=%u info=[%llu,%llu,%llu] tid=%u rip=0x%llX\n",
                record.ExceptionCode, static_cast<unsigned long long>(record.ExceptionAddress),
                record.NumberParameters,
                record.NumberParameters > 0 ? static_cast<unsigned long long>(record.ExceptionInformation[0]) : 0ull,
                record.NumberParameters > 1 ? static_cast<unsigned long long>(record.ExceptionInformation[1]) : 0ull,
                record.NumberParameters > 2 ? static_cast<unsigned long long>(record.ExceptionInformation[2]) : 0ull,
                c.vcpu.active_thread ? c.vcpu.active_thread->id : 0,
                static_cast<unsigned long long>(c.emu.reg(x86_register::rip)));
            c.proc.exit_status = record.ExceptionCode;
            c.win_emu.callbacks.on_exception();
            c.emu.stop();

            return STATUS_SUCCESS;
        }
    }

} // namespace sogen
