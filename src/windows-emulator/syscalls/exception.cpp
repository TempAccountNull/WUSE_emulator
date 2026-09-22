#include <array>
#include <cstdio>
#include <string>
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
            c.win_emu.log.error("EXITDIAG NtRaiseHardError status=%#x tid=%u\n", (unsigned)error_status,
                                (unsigned)GetCurrentThreadId());

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
                static_cast<unsigned long long>(c.emu.reg(x86_register::rip)),
                c.vcpu.cpu.index());
            // SMP-6.6 FINAL-ITEM DIAG: dump 32 bytes at the faulting target so the ntdll self-read
            // AV is decodable offline, and whether the target is readable on the FAULTING vCPU.
            if (record.NumberParameters > 1 && record.ExceptionInformation[1] != 0)
            {
                const auto fault_addr = record.ExceptionInformation[1];
                std::array<uint8_t, 32> bytes{};
                if (c.emu.try_read_memory(fault_addr, bytes.data(), bytes.size()))
                {
                    std::string hex{};
                    for (const auto b : bytes)
                    {
                        char buf[4];
                        std::snprintf(buf, sizeof(buf), "%02X ", b);
                        hex += buf;
                    }
                    c.win_emu.log.error("SMPDIAG fault-target bytes @0x%llX: %s\n",
                                        static_cast<unsigned long long>(fault_addr), hex.c_str());
                }
                else
                {
                    c.win_emu.log.error("SMPDIAG fault-target @0x%llX UNREADABLE on vCPU %zu (peer-view fault confirmed)\n",
                                        static_cast<unsigned long long>(fault_addr), c.vcpu.cpu.index());
                }
            }
            c.proc.exit_status = record.ExceptionCode;
            c.win_emu.log.error("EXITDIAG NtRaiseException status=%#x tid=%u\n", (unsigned)record.ExceptionCode,
                                (unsigned)GetCurrentThreadId());
            // 6.6/6.7 DIAG: dump bytes at the ExceptionAddress + the raiser's registers, so the
            // terminal 0x43@0x2000 raise is decodable (which structure, which vCPU, what code).
            {
                std::array<uint8_t, 32> target{};
                if (c.emu.try_read_memory(record.ExceptionAddress, target.data(), target.size()))
                {
                    std::string hex{};
                    for (const auto b : target)
                    {
                        char buf[4];
                        std::snprintf(buf, sizeof(buf), "%02X ", b);
                        hex += buf;
                    }
                    c.win_emu.log.error("RAISECTX target@0x%llX: %s\n",
                                        (unsigned long long)record.ExceptionAddress, hex.c_str());
                }
                c.win_emu.log.error(
                    "RAISECTX raiser rip=0x%llX rsp=0x%llX rax=0x%llX rcx=0x%llX rdx=0x%llX rbx=0x%llX vcpu=%zu\n",
                    (unsigned long long)c.emu.reg(x86_register::rip), (unsigned long long)c.emu.reg(x86_register::rsp),
                    (unsigned long long)c.emu.reg(x86_register::rax), (unsigned long long)c.emu.reg(x86_register::rcx),
                    (unsigned long long)c.emu.reg(x86_register::rdx), (unsigned long long)c.emu.reg(x86_register::rbx),
                    c.vcpu.cpu.index());
                // SMP 6.7: walk the raiser's GUEST STACK (ntdll-base return addresses at rsp) to
                // name the exact raising path. Raw RVAs vs ntdll base 0x180000000, decoded offline.
                {
                    const auto rsp = c.emu.reg(x86_register::rsp);
                    std::array<uint64_t, 24> stack{};
                    if (rsp && c.emu.try_read_memory(rsp, stack.data(), stack.size() * sizeof(uint64_t)))
                    {
                        std::string chain{};
                        char buf[24];
                        for (const auto q : stack)
                        {
                            if (q >= 0x180000000 && q < 0x180400000)
                            {
                                std::snprintf(buf, sizeof(buf), "ntdll+%llX ",
                                              (unsigned long long)(q - 0x180000000));
                                chain += buf;
                            }
                        }
                        c.win_emu.log.error("RAISECTX stack-chain: %s\n", chain.c_str());
                    }
                }
            }

            c.win_emu.callbacks.on_exception();
            c.emu.stop();

            return STATUS_SUCCESS;
        }
    }

} // namespace sogen
