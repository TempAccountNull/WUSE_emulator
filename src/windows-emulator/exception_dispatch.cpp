#include "std_include.hpp"
#include "exception_dispatch.hpp"
#include "process_context.hpp"
#include "cpu_context.hpp"
#include "windows_emulator.hpp"

#include "segment_utils.hpp"
#include "wow64_heaven_gate.hpp"

namespace sogen
{

    namespace
    {
        using exception_record = EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>;
        using exception_record_map = std::unordered_map<const exception_record*, emulator_object<exception_record>>;

        emulator_object<exception_record> save_exception_record(emulator_allocator& allocator, const exception_record& record,
                                                                exception_record_map& record_mapping)
        {
            const auto record_obj = allocator.reserve<exception_record>();
            record_obj.write(record);

            if (record.ExceptionRecord)
            {
                record_mapping.emplace(&record, record_obj);

                emulator_object<exception_record> nested_record_obj{allocator.get_memory()};
                const auto nested_record = record_mapping.find(reinterpret_cast<exception_record*>(record.ExceptionRecord));

                if (nested_record != record_mapping.end())
                {
                    nested_record_obj = nested_record->second;
                }
                else
                {
                    nested_record_obj =
                        save_exception_record(allocator, *reinterpret_cast<exception_record*>(record.ExceptionRecord), record_mapping);
                }

                record_obj.access([&](exception_record& r) {
                    r.ExceptionRecord = nested_record_obj.value(); //
                });
            }

            return record_obj;
        }

        emulator_object<exception_record> save_exception_record(emulator_allocator& allocator, const exception_record& record)
        {
            exception_record_map record_mapping{};
            return save_exception_record(allocator, record, record_mapping);
        }

        uint32_t map_violation_operation_to_parameter(const memory_operation operation)
        {
            switch (operation)
            {
            default:
            case memory_operation::read:
                return 0;
            case memory_operation::write:
            case memory_operation::exec:
                return 1;
            }
        }

        size_t calculate_exception_record_size(const exception_record& record)
        {
            std::unordered_set<const exception_record*> records{};
            size_t total_size = 0;

            const exception_record* current_record = &record;
            while (current_record)
            {
                if (!records.insert(current_record).second)
                {
                    break;
                }

                total_size += sizeof(*current_record);
                current_record = reinterpret_cast<exception_record*>(record.ExceptionRecord);
            }

            return total_size;
        }

        struct machine_frame
        {
            uint64_t rip;
            uint64_t cs;
            uint64_t eflags;
            uint64_t rsp;
            uint64_t ss;
        };

        void dispatch_exception_pointers(x86_64_cpu& emu, const uint64_t dispatcher,
                                         const EMU_EXCEPTION_POINTERS<EmulatorTraits<Emu64>> pointers)
        {
            constexpr auto mach_frame_size = 0x40;
            constexpr auto context_record_size = 0x4F0;
            const auto exception_record_size =
                calculate_exception_record_size(*reinterpret_cast<exception_record*>(pointers.ExceptionRecord));
            const auto combined_size = align_up(exception_record_size + context_record_size, 0x10);

            assert(combined_size == 0x590);

            const auto allocation_size = combined_size + mach_frame_size;

            const auto initial_sp = emu.reg(x86_register::rsp);
            const auto new_sp = align_down(initial_sp - allocation_size, 0x100);

            const auto total_size = initial_sp - new_sp;
            assert(total_size >= allocation_size);

            std::vector<uint8_t> zero_memory{};
            zero_memory.resize(static_cast<size_t>(total_size), 0);

            emu.write_memory(new_sp, zero_memory.data(), zero_memory.size());

            const emulator_object<CONTEXT64> context_record_obj{emu, new_sp};
            context_record_obj.write(*reinterpret_cast<CONTEXT64*>(pointers.ContextRecord));

            emulator_allocator allocator{emu, new_sp + context_record_size, exception_record_size};
            const auto exception_record_obj =
                save_exception_record(allocator, *reinterpret_cast<exception_record*>(pointers.ExceptionRecord));

            if (exception_record_obj.value() != allocator.get_base())
            {
                throw std::runtime_error("Bad exception record position on stack");
            }

            const emulator_object<machine_frame> machine_frame_obj{emu, new_sp + combined_size};
            machine_frame_obj.access([&](machine_frame& frame) {
                const auto& record = *reinterpret_cast<CONTEXT64*>(pointers.ContextRecord);
                frame.rip = record.Rip;
                frame.rsp = record.Rsp;
                frame.ss = record.SegSs;
                frame.cs = record.SegCs;
                frame.eflags = record.EFlags;
            });

            const auto cs_selector = emu.reg<uint16_t>(x86_register::cs);
            const auto bitness = segment_utils::get_segment_bitness(emu, cs_selector);

            if (!bitness || *bitness != segment_utils::segment_bitness::bit32)
            {
                emu.reg(x86_register::rsp, new_sp);
                emu.reg(x86_register::rip, dispatcher);
                return;
            }

            emu.reg(x86_register::rax, dispatcher);
            emu.reg(x86_register::rbx, new_sp);
            emu.reg(x86_register::rcx, static_cast<uint64_t>(wow64::heaven_gate::kUserCodeSelector));
            emu.reg(x86_register::rdx, static_cast<uint64_t>(wow64::heaven_gate::kUserStackSelector));
            emu.reg(x86_register::rsp, wow64::heaven_gate::kStackTop);
            emu.reg(x86_register::rip, wow64::heaven_gate::kCodeBase);
        }

        // ARM64 KiUserExceptionDispatcher finds the CONTEXT at SP and the EXCEPTION_RECORD at SP + 0x3B0,
        // as its RtlDispatchException(record, context) call sequence shows.
        constexpr uint64_t arm64_exception_record_offset = 0x3B0;

        void dispatch_arm64_exception(windows_emulator& win_emu, arm64_cpu& cpu, const exception_record& record, const arm64_context& ctx)
        {
            static_assert(sizeof(arm64_context) <= arm64_exception_record_offset);

            const auto exception_record_size = calculate_exception_record_size(record);
            const auto allocation_size = arm64_exception_record_offset + exception_record_size;
            const auto initial_sp = cpu.read_stack_pointer();
            const auto new_sp = align_down(initial_sp - allocation_size, 0x10);

            std::vector<uint8_t> zero_memory(static_cast<size_t>(initial_sp - new_sp), 0);
            cpu.write_memory(new_sp, zero_memory.data(), zero_memory.size());
            cpu.write_memory(new_sp, ctx);

            emulator_allocator allocator{cpu, new_sp + arm64_exception_record_offset, exception_record_size};
            const auto exception_record_obj = save_exception_record(allocator, record);
            if (exception_record_obj.value() != allocator.get_base())
            {
                throw std::runtime_error("Bad exception record position on stack");
            }

            cpu.reg(arm64_register::sp, new_sp);
            cpu.reg(arm64_register::pc, win_emu.process.ki_user_exception_dispatcher);
        }

        WOW64_CONTEXT make_wow64_context(const CONTEXT64& ctx)
        {
            WOW64_CONTEXT result{};
            result.ContextFlags = CONTEXT32_ALL;
            result.Dr0 = static_cast<DWORD>(ctx.Dr0);
            result.Dr1 = static_cast<DWORD>(ctx.Dr1);
            result.Dr2 = static_cast<DWORD>(ctx.Dr2);
            result.Dr3 = static_cast<DWORD>(ctx.Dr3);
            result.Dr6 = static_cast<DWORD>(ctx.Dr6);
            result.Dr7 = static_cast<DWORD>(ctx.Dr7);
            result.SegGs = ctx.SegGs;
            result.SegFs = ctx.SegFs;
            result.SegEs = ctx.SegEs;
            result.SegDs = ctx.SegDs;
            result.Edi = static_cast<DWORD>(ctx.Rdi);
            result.Esi = static_cast<DWORD>(ctx.Rsi);
            result.Ebx = static_cast<DWORD>(ctx.Rbx);
            result.Edx = static_cast<DWORD>(ctx.Rdx);
            result.Ecx = static_cast<DWORD>(ctx.Rcx);
            result.Eax = static_cast<DWORD>(ctx.Rax);
            result.Ebp = static_cast<DWORD>(ctx.Rbp);
            result.Eip = static_cast<DWORD>(ctx.Rip);
            result.SegCs = ctx.SegCs;
            result.EFlags = ctx.EFlags;
            result.Esp = static_cast<DWORD>(ctx.Rsp);
            result.SegSs = ctx.SegSs;
            result.FloatSave.ControlWord = 0x037F;
            result.FloatSave.TagWord = 0xFFFF;

            XMM_SAVE_AREA32 xmm_state{};
            xmm_state.ControlWord = 0x037F;
            // FXSAVE abridged x87 tag: 0x00 = empty stack (fresh FPU). 0xFF (all valid) makes the next x87
            // FLD overflow the stack and yield the x87 indefinite (NaN).
            xmm_state.TagWord = 0x00;
            xmm_state.MxCsr = 0x1F80;
            xmm_state.MxCsr_Mask = 0xFFFFFFFF;
            static_assert(sizeof(xmm_state) <= sizeof(result.ExtendedRegisters));
            memcpy(result.ExtendedRegisters, &xmm_state, sizeof(xmm_state));
            return result;
        }

        void sync_wow64_cpu_reserved_context(windows_emulator& win_emu, x86_64_cpu& emu, emulator_thread& thread, const CONTEXT64& ctx)
        {
            if (!win_emu.process.is_wow64_process)
            {
                return;
            }

            const auto bitness = segment_utils::get_segment_bitness(emu, ctx.SegCs);
            if (!bitness || *bitness != segment_utils::segment_bitness::bit32)
            {
                return;
            }

            // Wow64PassExceptionToGuest rebuilds the 32-bit context from WOW64_CPURESERVED
            // (TEB64 TLS slot 1), not from the native exception ContextRecord below.
            thread.wow64_cpu_reserved->access([&](WOW64_CPURESERVED& cpu) {
                cpu.Flags |= WOW64_CPURESERVED_FLAG_RESET_STATE;
                cpu.Context = make_wow64_context(ctx);
            });
        }

    }

    bool dispatch_debug_exception(windows_emulator& win_emu, CONTEXT64& ctx, EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>& record)
    {
        std::array<uint8_t, 2> ins = {0};

        // CD 2D int 2dh
        if (win_emu.memory.try_read_memory(ctx.Rip, &ins, sizeof(ins)) && ins[0] == 0xCD && ins[1] == 0x2D)
        {
            // skip 2 bytes int 2dh
            ctx.Rip += 2;

            record.NumberParameters = 3;

            record.ExceptionInformation[0] = ctx.Rax;
            record.ExceptionInformation[1] = ctx.Rcx;
            record.ExceptionInformation[2] = ctx.Rdx;

            return true;
        }

        return false;
    }

    void dispatch_exception(windows_emulator& win_emu, vcpu_context& vcpu, const DWORD status,
                            const std::vector<EmulatorTraits<Emu64>::ULONG_PTR>& parameters)
    {
        auto& thread = vcpu.thread();

        win_emu.record_exception_trace({
            .status = static_cast<uint32_t>(status),
            .tid = thread.id,
            .vcpu = static_cast<uint32_t>(vcpu.cpu.index()),
            .rip = vcpu.cpu.read_instruction_pointer(),
            .info = parameters.size() > 1 ? static_cast<uint64_t>(parameters[1]) : 0,
        });

        if (auto* arm64 = dynamic_cast<arm64_cpu*>(&vcpu.cpu))
        {
            arm64_context ctx{};
            ctx.ContextFlags = arm64_context_all;
            cpu_context::save(*arm64, ctx);
            ctx.Pc = win_emu.uses_instruction_precision() ? thread.current_ip : arm64->read_instruction_pointer();

            if (parameters.size() > 15)
            {
                throw std::runtime_error("Too many exception parameters");
            }

            exception_record record{};
            record.ExceptionCode = status;
            record.NumberParameters = static_cast<DWORD>(parameters.size());
            std::ranges::copy(parameters, record.ExceptionInformation);
            record.ExceptionAddress = ctx.Pc;

            dispatch_arm64_exception(win_emu, *arm64, record, ctx);
            return;
        }

        auto& cpu = vcpu.x64_cpu();
        CONTEXT64 ctx{};
        ctx.ContextFlags = CONTEXT64_ALL;
        cpu_context::save(cpu, ctx);
        ctx.Rip = win_emu.uses_instruction_precision() //
                      ? thread.current_ip
                      : vcpu.cpu.read_instruction_pointer();

        exception_record record{};
        memset(&record, 0, sizeof(record));
        record.ExceptionCode = status;
        record.ExceptionFlags = 0;
        record.ExceptionRecord = 0;
        record.NumberParameters = 0;

        bool is_debug_exception = false;
        if (status == STATUS_BREAKPOINT)
        {
            is_debug_exception = dispatch_debug_exception(win_emu, ctx, record);
        }

        if (!is_debug_exception)
        {
            record.NumberParameters = static_cast<DWORD>(parameters.size());

            if (parameters.size() > 15)
            {
                throw std::runtime_error("Too many exception parameters");
            }

            for (size_t i = 0; i < parameters.size(); ++i)
            {
                record.ExceptionInformation[i] = parameters[i];
            }
        }

        record.ExceptionAddress = ctx.Rip;

        sync_wow64_cpu_reserved_context(win_emu, cpu, thread, ctx);

        EMU_EXCEPTION_POINTERS<EmulatorTraits<Emu64>> pointers{};
        pointers.ContextRecord = reinterpret_cast<EmulatorTraits<Emu64>::PVOID>(&ctx);
        pointers.ExceptionRecord = reinterpret_cast<EmulatorTraits<Emu64>::PVOID>(&record);
        dispatch_exception_pointers(cpu, win_emu.process.ki_user_exception_dispatcher, pointers);
    }

    void dispatch_access_violation(windows_emulator& win_emu, vcpu_context& vcpu, const uint64_t address, const memory_operation operation)
    {
        dispatch_exception(win_emu, vcpu, STATUS_ACCESS_VIOLATION,
                           {
                               map_violation_operation_to_parameter(operation),
                               address,
                           });
    }

    void dispatch_guard_page_violation(windows_emulator& win_emu, vcpu_context& vcpu, const uint64_t address,
                                       const memory_operation operation)
    {
        dispatch_exception(win_emu, vcpu, STATUS_GUARD_PAGE_VIOLATION,
                           {
                               map_violation_operation_to_parameter(operation),
                               address,
                           });
    }

    void dispatch_illegal_instruction_violation(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        dispatch_exception(win_emu, vcpu, STATUS_ILLEGAL_INSTRUCTION, {});
    }

    namespace
    {
        // The KiOpOneByteTable and KiOpTwoByteTable entries whose KiOp_Priv handler makes a user-mode #GP raise
        // STATUS_PRIVILEGED_INSTRUCTION. The remaining entries serve #UD and #DE, or only act for flagged threads or with
        // CR4.UMIP set, which sogen's guests never have, so KiPreprocessFault leaves those faults as access violations.
        struct privileged_opcode
        {
            uint8_t first{};
            uint8_t count{1};
            uint8_t modrm_reg{0xFF};
            uint8_t modrm_mod{0xFF}; // 3 matches the register form only, any other value the memory form only
            uint8_t modrm_rm{0xFF};
        };

        constexpr std::array<privileged_opcode, 6> one_byte_privileged_opcodes{{
            {.first = 0xFA},             // CLI
            {.first = 0xFB},             // STI
            {.first = 0xE4, .count = 4}, // IN/OUT imm8
            {.first = 0xEC, .count = 4}, // IN/OUT DX
            {.first = 0x6C, .count = 4}, // INS/OUTS
            {.first = 0xF4},             // HLT
        }};

        constexpr std::array<privileged_opcode, 14> two_byte_privileged_opcodes{{
            {.first = 0x00, .modrm_reg = 2},                                // LLDT
            {.first = 0x00, .modrm_reg = 3},                                // LTR
            {.first = 0x01, .modrm_reg = 2, .modrm_mod = 0},                // LGDT
            {.first = 0x01, .modrm_reg = 3, .modrm_mod = 0},                // LIDT
            {.first = 0x01, .modrm_reg = 6},                                // LMSW
            {.first = 0x01, .modrm_reg = 7, .modrm_mod = 0},                // INVLPG
            {.first = 0x01, .modrm_reg = 7, .modrm_mod = 3, .modrm_rm = 0}, // SWAPGS
            {.first = 0x06},                                                // CLTS
            {.first = 0x07},                                                // SYSRET
            {.first = 0x08},                                                // INVD
            {.first = 0x09},                                                // WBINVD
            {.first = 0x20, .count = 4},                                    // MOV to and from CR/DR
            {.first = 0x30, .count = 4},                                    // WRMSR, RDTSC, RDMSR, RDPMC
            {.first = 0x35},                                                // SYSEXIT
        }};

        // KiOpPrefixTable. KiOpIsPrefix fails the decode on a repeated prefix or a second one from the same group.
        struct instruction_prefix
        {
            uint8_t value{};
            uint16_t flag{};
            uint8_t group{};
        };

        constexpr std::array<instruction_prefix, 11> instruction_prefixes{{
            {0x26, 0x001, 1},
            {0x2E, 0x002, 1},
            {0x36, 0x004, 1},
            {0x3E, 0x008, 1},
            {0x64, 0x010, 1},
            {0x65, 0x020, 1},
            {0x66, 0x040, 0},
            {0x67, 0x080, 0},
            {0xF0, 0x100, 0},
            {0xF2, 0x200, 2},
            {0xF3, 0x400, 2},
        }};

        constexpr uint16_t rex_prefix_flag = 0x800;

        bool matches_modrm(const privileged_opcode& entry, const uint8_t modrm)
        {
            const auto is_register_form = (modrm & 0xC0) == 0xC0;
            return (entry.modrm_reg == 0xFF || entry.modrm_reg == ((modrm >> 3) & 7)) &&
                   (entry.modrm_rm == 0xFF || entry.modrm_rm == (modrm & 7)) &&
                   (entry.modrm_mod == 0xFF || (entry.modrm_mod == 3) == is_register_form);
        }
    }

    NTSTATUS get_general_protection_fault_status(const std::span<const uint8_t> instruction, const bool is_32bit)
    {
        size_t offset = 0;
        uint16_t prefix_flags = 0;
        uint8_t prefix_groups = 0;

        for (; offset < instruction.size(); ++offset)
        {
            const auto value = instruction[offset];
            uint16_t flag = 0;
            uint8_t group = 0;

            if (!is_32bit && (value & 0xF0) == 0x40)
            {
                flag = rex_prefix_flag;
            }
            else if (const auto prefix = std::ranges::find(instruction_prefixes, value, &instruction_prefix::value);
                     prefix != instruction_prefixes.end())
            {
                flag = prefix->flag;
                group = prefix->group;
            }
            else
            {
                break;
            }

            if ((prefix_flags & flag) != 0 || (prefix_groups & group) != 0)
            {
                return STATUS_ACCESS_VIOLATION;
            }

            prefix_flags |= flag;
            prefix_groups |= group;
        }

        const auto byte_at = [&](const size_t index) -> std::optional<uint8_t> {
            return index < instruction.size() ? std::optional{instruction[index]} : std::nullopt;
        };

        auto opcode = byte_at(offset);
        std::span<const privileged_opcode> table = one_byte_privileged_opcodes;
        if (opcode == 0x0F)
        {
            opcode = byte_at(++offset);
            table = two_byte_privileged_opcodes;
        }

        if (!opcode)
        {
            return STATUS_ACCESS_VIOLATION;
        }

        const auto modrm = byte_at(offset + 1);
        for (const auto& entry : table)
        {
            if (*opcode < entry.first || *opcode >= entry.first + entry.count)
            {
                continue;
            }

            if (entry.modrm_reg == 0xFF && entry.modrm_mod == 0xFF && entry.modrm_rm == 0xFF)
            {
                return STATUS_PRIVILEGED_INSTRUCTION;
            }

            if (!modrm)
            {
                return STATUS_ACCESS_VIOLATION;
            }

            if (matches_modrm(entry, *modrm))
            {
                return STATUS_PRIVILEGED_INSTRUCTION;
            }
        }

        return STATUS_ACCESS_VIOLATION;
    }

    void dispatch_general_protection_fault(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        auto& cpu = vcpu.x64_cpu();
        const auto rip = win_emu.uses_instruction_precision() ? vcpu.thread().current_ip : cpu.read_instruction_pointer();

        // KiOpFetchNextByte decodes at most 16 bytes and stops at the first unreadable one.
        std::array<uint8_t, 16> instruction{};
        size_t length = 0;
        while (length < instruction.size() && win_emu.memory.try_read_memory(rip + length, &instruction[length], 1))
        {
            ++length;
        }

        // KiOpDecode only treats 0x40-0x4F as REX outside the 32-bit user code segment.
        const auto is_32bit = cpu.reg<uint16_t>(x86_register::cs) == 0x23;
        const auto status = get_general_protection_fault_status({instruction.data(), length}, is_32bit);
        if (status == STATUS_ACCESS_VIOLATION)
        {
            // KiPreprocessFault reports any other #GP as a read of the all-ones address.
            dispatch_access_violation(win_emu, vcpu, ~uint64_t{0}, memory_operation::read);
            return;
        }

        dispatch_exception(win_emu, vcpu, status, {});
    }

    void dispatch_integer_division_by_zero(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        dispatch_exception(win_emu, vcpu, STATUS_INTEGER_DIVIDE_BY_ZERO, {});
    }

    void dispatch_single_step(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        dispatch_exception(win_emu, vcpu, STATUS_SINGLE_STEP, {});
    }

    void dispatch_breakpoint(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        dispatch_exception(win_emu, vcpu, STATUS_BREAKPOINT, {});
    }

} // namespace sogen
