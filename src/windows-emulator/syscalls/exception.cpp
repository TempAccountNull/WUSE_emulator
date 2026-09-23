#include <array>
#include <cstdio>
#include <cstring>
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
            // LEANDIAG: name the failing component. A hard error raised by ntdll's loader
            // (e.g. STATUS_DLL_INIT_FAILED) carries the failing DllMain's frames on the guest
            // stack - walk it and annotate every code pointer with its module so the DLL whose
            // init failed is named directly from the crash.
            {
                const auto rsp = c.emu.reg(x86_register::rsp);
                std::array<uint64_t, 48> stack{};
                if (rsp && c.emu.try_read_memory(rsp, stack.data(), stack.size() * sizeof(uint64_t)))
                {
                    c.win_emu.log.error(
                        "LEANDIAG registers: rip=0x%llX rsp=0x%llX rax=0x%llX rbx=0x%llX rcx=0x%llX rdx=0x%llX rsi=0x%llX rdi=0x%llX r8=0x%llX r9=0x%llX r10=0x%llX r11=0x%llX\n",
                        (unsigned long long)c.emu.reg(x86_register::rip), (unsigned long long)c.emu.reg(x86_register::rsp),
                        (unsigned long long)c.emu.reg(x86_register::rax), (unsigned long long)c.emu.reg(x86_register::rbx),
                        (unsigned long long)c.emu.reg(x86_register::rcx), (unsigned long long)c.emu.reg(x86_register::rdx),
                        (unsigned long long)c.emu.reg(x86_register::rsi), (unsigned long long)c.emu.reg(x86_register::rdi),
                        (unsigned long long)c.emu.reg(x86_register::r8), (unsigned long long)c.emu.reg(x86_register::r9),
                        (unsigned long long)c.emu.reg(x86_register::r10), (unsigned long long)c.emu.reg(x86_register::r11));
                    c.win_emu.log.error("LEANDIAG stack-chain (rsp+%zu .. rsp+%zu, one qword per line):\n", (size_t)0,
                                        stack.size() * sizeof(uint64_t));
                    if (c.vcpu.active_thread)
                    {
                        const auto trail = windows_emulator::leandiag_last_blocks(c.vcpu.active_thread->id);
                        if (!trail.empty())
                        {
                            const auto foreign = windows_emulator::leandiag_last_foreign_blocks(c.vcpu.active_thread->id);
                            if (!foreign.empty())
                            {
                                c.win_emu.log.error("LEANDIAG raiser tid=%u last NON-ntdll blocks (oldest -> newest):\n",
                                                    c.vcpu.active_thread->id);
                                for (const auto faddress : foreign)
                                {
                                    const auto* fowner = c.win_emu.mod_manager.find_name(faddress);
                                    char fbuf[128];
                                    std::snprintf(fbuf, sizeof(fbuf), "  %llX (%s)\n",
                                                  (unsigned long long)faddress, fowner ? fowner : "no module");
                                    c.win_emu.log.error("%s", fbuf);
                                }
                            }
                            c.win_emu.log.error("LEANDIAG raiser tid=%u last blocks (oldest -> newest):\n",
                                                c.vcpu.active_thread->id);
                            for (const auto address : trail)
                            {
                                const auto* owner = c.win_emu.mod_manager.find_name(address);
                                char trail_buf[128];
                                std::snprintf(trail_buf, sizeof(trail_buf), "  %llX (%s)\n",
                                              (unsigned long long)address, owner ? owner : "no module");
                                c.win_emu.log.error("%s", trail_buf);
                            }
                        }
                    }
                    c.win_emu.log.error("LEANDIAG registered modules (name @ base):\n");                    for (const auto& mod : c.win_emu.mod_manager.modules() | std::views::values)
                    {
                        char mod_buf[160];
                        std::snprintf(mod_buf, sizeof(mod_buf), "  %s @ %#llx\n", mod.name.c_str(),
                                      (unsigned long long)mod.image_base);
                        c.win_emu.log.error("%s", mod_buf);
                    }
                    size_t entry_index = 0;
                    for (const auto q : stack)
                    {
                        if (q < 0x1000 || q >= 0x7F0000000000ull)
                        {
                            ++entry_index;
                            continue;
                        }
                        const auto* module_name = c.win_emu.mod_manager.find_name(q);
                        char buf[128];
                        std::snprintf(buf, sizeof(buf), "  [%02zu] %llX (%s)\n", entry_index, (unsigned long long)q,
                                      module_name ? module_name : "no module");
                        c.win_emu.log.error("%s", buf);
                        // Sniff guest-readable pointers as UTF-16 text: the loader work item on
                        // this stack carries the failing DLL's name. Printed as its own line.
                        if (!module_name && (q & 1) == 0)
                        {
                            std::array<char16_t, 24> text{};
                            if (c.emu.try_read_memory(q, text.data(), text.size() * sizeof(char16_t)))
                            {
                                size_t n = 0;
                                while (n < text.size() && text[n] >= 0x20 && text[n] < 0x80)
                                {
                                    ++n;
                                }
                                if (n >= 6)
                                {
                                    std::string ascii{};
                                    ascii.reserve(n);
                                    for (size_t i = 0; i < n; ++i)
                                    {
                                        ascii.push_back(static_cast<char>(text[i]));
                                    }
                                    char text_buf[96];
                                    std::snprintf(text_buf, sizeof(text_buf), "        text: \"%s\"\n", ascii.c_str());
                                    c.win_emu.log.error("%s", text_buf);
                                }
                            }
                        }
                        // Probe for a PE image at this pointer's page-aligned base (walking down a
                        // few pages for alignment slack): read MZ -> e_lfanew -> export data
                        // directory -> module name. Names ANY mapped image from memory alone,
                        // independent of mod_manager (the failing DLL is untracked there).
                        if (!module_name)
                        {
                            // 64K-aligned images can sit hundreds of KB below a data pointer
                            // into them (e.g. a loader work item at +0x30050) - walk up to
                            // 1 MB down in 4K steps, not just a few pages.
                            for (uint64_t page = q & ~0xFFFull; (q - page) < 0x100000; page -= 0x1000)
                            {
                                uint16_t dos_magic = 0;
                                if (!c.emu.try_read_memory(page, &dos_magic, sizeof(dos_magic)) || dos_magic != 0x5A4D)
                                {
                                    continue;
                                }
                                int32_t lfanew = 0;
                                if (!c.emu.try_read_memory(page + 0x3C, &lfanew, sizeof(lfanew)) || lfanew <= 0 ||
                                    lfanew > 0x400)
                                {
                                    continue;
                                }
                                // Optional header is pe+0x18; data directories at +0x70 (PE32+)
                                // with export dir as entry 0: {rva, size}.
                                uint32_t export_rva = 0;
                                if (!c.emu.try_read_memory(page + lfanew + 0x18 + 0x70, &export_rva, sizeof(export_rva)) ||
                                    export_rva == 0)
                                {
                                    continue;
                                }
                                // Name RVA is at export_dir+12 (after characteristics, timestamp,
                                // forwarder, nameRVA... actually +12 IS the name RVA).
                                uint32_t name_rva = 0;
                                if (!c.emu.try_read_memory(page + export_rva + 12, &name_rva, sizeof(name_rva)) ||
                                    name_rva == 0)
                                {
                                    continue;
                                }
                                std::array<char, 40> image_name{};
                                if (!c.emu.try_read_memory(page + name_rva, image_name.data(), image_name.size() - 1))
                                {
                                    continue;
                                }
                                image_name.back() = '\0';
                                const auto len = strnlen(image_name.data(), image_name.size() - 1);
                                if (len < 3)
                                {
                                    continue;
                                }
                                char mz_buf[128];
                                std::snprintf(mz_buf, sizeof(mz_buf), "        image@%llX: %s\n",
                                              (unsigned long long)page, image_name.data());
                                c.win_emu.log.error("%s", mz_buf);
                                break;
                            }
                        }
                        ++entry_index;
                    }
                    for (const auto q : stack)
                    {
                        if (q < 0x10000 || (q & 1) != 0)
                        {
                            continue;
                        }
                        struct unicode_string_like
                        {
                            uint16_t length;
                            uint16_t maximum;
                            uint32_t _pad;
                            uint64_t buffer;
                        } us{};
                        if (!c.emu.try_read_memory(q, &us, sizeof(us)) || us.length == 0 || us.length > 1040 ||
                            (us.length & 1) != 0 || us.length + 2 > us.maximum || us.maximum > 1042 ||
                            us.buffer < 0x10000 || us.buffer >= 0x7F0000000000ull)
                        {
                            continue;
                        }
                        std::array<char16_t, 520> name{};
                        if (!c.emu.try_read_memory(us.buffer, name.data(), us.length))
                        {
                            continue;
                        }
                        const auto chars = us.length / 2;
                        size_t n = 0;
                        while (n < chars && name[n] >= 0x20 && name[n] < 0x80)
                        {
                            ++n;
                        }
                        if (n + 2 >= chars && n >= 4)
                        {
                            std::string ascii{};
                            ascii.reserve(n);
                            for (size_t i = 0; i < n; ++i)
                            {
                                ascii.push_back(static_cast<char>(name[i]));
                            }
                            char buf2[96];
                            std::snprintf(buf2, sizeof(buf2), "LEANDIAG string@%llX: \"%s\"\n",
                                          (unsigned long long)q, ascii.c_str());
                            c.win_emu.log.error("%s", buf2);
                        }
                    }
                }
            }

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
                // SMP 6.7 ROOT-CAUSE #2: at the raise, dump what gs:[0x60] actually READS - the
                // TEB at the raiser vCPU's CURRENT GS base, field +0x60 (ProcessEnvironmentBlock)
                // vs +0x08 (Self). Compare with [TEBDIAG w] writes from initial_setup_thread.
                {
                    const auto gs_base = c.emu.reg(x86_register::gs_base);
                    std::array<uint8_t, 16> teb_head{};
                    if (gs_base && c.emu.try_read_memory(gs_base + 0x58, teb_head.data(), teb_head.size()))
                    {
                        uint64_t peb_field = 0;
                        std::memcpy(&peb_field, teb_head.data() + 8, sizeof(peb_field)); // +0x60
                        c.win_emu.log.error("TEBDIAG r gs_base=0x%llX PEBfield=0x%llX\n",
                                            (unsigned long long)gs_base, (unsigned long long)peb_field);
                    }
                    else
                    {
                        c.win_emu.log.error("TEBDIAG r gs_base=0x%llX UNREADABLE\n", (unsigned long long)gs_base);
                    }
                }
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
