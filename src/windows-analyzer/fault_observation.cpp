#include "std_include.hpp"
#include "fault_observation.hpp"
#include "analysis.hpp"
#include <windows_emulator.hpp>
#include <emulator_utils.hpp>
#include <utils/finally.hpp>
#include <limits>

namespace sogen
{
    namespace
    {
        bool passive_span(memory_manager& memory, const uint64_t address, const size_t size, std::string& error)
        {
            if (size > std::numeric_limits<uint64_t>::max() - address)
            {
                error = "address overflow";
                return false;
            }
            for (uint64_t cursor = address; cursor < address + size;)
            {
                const auto region = memory.get_region_info(cursor);
                if (!region.is_committed || region.kind == memory_region_kind::host_reserved)
                {
                    error = "uncommitted memory";
                    return false;
                }
                if (region.kind == memory_region_kind::mmio || region.permissions.is_guarded())
                {
                    error = region.kind == memory_region_kind::mmio ? "MMIO excluded" : "guarded memory excluded";
                    return false;
                }
                if (!is_readable(region.permissions.common))
                {
                    error = "memory is not readable";
                    return false;
                }
                cursor += std::min<uint64_t>(0x1000 - (cursor & 0xFFF), address + size - cursor);
            }
            return true;
        }

        bool passive_read(memory_manager& memory, const uint64_t address, void* output, const size_t size, std::string& error)
        {
            if (!passive_span(memory, address, size, error))
            {
                return false;
            }
            if (!memory.try_read_memory(address, output, size))
            {
                error = "backend read failed";
                return false;
            }
            return true;
        }

        std::string_view region_kind_name(const memory_region_kind kind)
        {
            switch (kind)
            {
            case memory_region_kind::free:
                return "free";
            case memory_region_kind::private_allocation:
                return "private";
            case memory_region_kind::file_section_view:
                return "file_section";
            case memory_region_kind::pagefile_section_view:
                return "pagefile_section";
            case memory_region_kind::section_image:
                return "image";
            case memory_region_kind::mmio:
                return "mmio";
            case memory_region_kind::host_reserved:
                return "host_reserved";
            default:
                return "unknown";
            }
        }

        fault_address_snapshot capture_address(windows_emulator& win, const uint64_t address)
        {
            fault_address_snapshot result{.address = address};
            if (const auto* module = win.mod_manager.find_by_address(address))
            {
                result.module = module->name;
                result.module_base = module->image_base;
                result.module_rva = address - module->image_base;
            }
            if (address > MAX_ALLOCATION_ADDRESS)
            {
                result.error = "outside managed address range";
                return result;
            }
            const auto region = win.memory.get_region_info(address);
            result.region = fault_region_snapshot{
                .start = region.start,
                .length = region.length,
                .allocation_base = region.allocation_base,
                .allocation_length = region.allocation_length,
                .permissions = get_permission_string(region.permissions.common),
                .kind = std::string(region_kind_name(region.kind)),
                .reserved = region.is_reserved,
                .committed = region.is_committed,
                .guarded = region.permissions.is_guarded(),
            };
            return result;
        }

        bool passive_descriptor(memory_manager& memory, const cpu_interface::descriptor_table_register& table, const uint16_t selector,
                                const size_t size, std::string& error)
        {
            const auto offset = static_cast<uint64_t>(selector & 0xFFF8);
            if (offset + size > static_cast<uint64_t>(table.limit) + 1 || table.base > std::numeric_limits<uint64_t>::max() - offset)
            {
                error = "descriptor outside table";
                return false;
            }
            return passive_span(memory, table.base + offset, size, error);
        }

        segment_utils::descriptor decode_descriptor(const segment_utils::raw_segment_descriptor& raw)
        {
            segment_utils::descriptor result{};
            result.base = raw.base_low | (static_cast<uint32_t>(raw.base_mid) << 16) | (static_cast<uint32_t>(raw.base_high) << 24);
            result.limit = raw.limit_low | (static_cast<uint32_t>(raw.limit_high_flags & 15) << 16);
            if (raw.limit_high_flags & 0x80)
            {
                result.limit = (result.limit << 12) | 0xFFF;
            }
            result.present = (raw.access & 0x80) != 0;
            result.system = (raw.access & 0x10) == 0;
            result.type = raw.access & 15;
            result.long_mode = (raw.limit_high_flags & 0x20) != 0;
            result.default_op_size = (raw.limit_high_flags & 0x40) != 0;
            return result;
        }

        bool capture_segment_descriptor(windows_emulator& win, x86_64_cpu& cpu, const uint16_t selector,
                                        segment_utils::raw_segment_descriptor& raw, std::string& error)
        {
            auto table = segment_utils::read_descriptor_table(cpu, x86_register::gdtr);
            if ((selector & 0xFFFC) == 0 || !table)
            {
                error = "segment descriptor unavailable";
                return false;
            }
            if (selector & 4)
            {
                const auto ldtr = segment_utils::read_selector(cpu, x86_register::ldtr);
                std::array<uint8_t, 16> bytes{};
                if (!ldtr || (*ldtr & 0xFFFC) == 0 || (*ldtr & 4) != 0 ||
                    !passive_descriptor(win.memory, *table, *ldtr, bytes.size(), error) ||
                    !passive_read(win.memory, table->base + (*ldtr & 0xFFF8), bytes.data(), bytes.size(), error))
                {
                    if (error.empty())
                    {
                        error = "LDT descriptor unavailable for passive capture";
                    }
                    return false;
                }
                segment_utils::raw_segment_descriptor ldt_raw{};
                std::memcpy(&ldt_raw, bytes.data(), sizeof(ldt_raw));
                auto ldt = decode_descriptor(ldt_raw);
                uint32_t upper_base{};
                std::memcpy(&upper_base, bytes.data() + 8, sizeof(upper_base));
                ldt.base |= static_cast<uint64_t>(upper_base) << 32;
                if (!ldt.present || !ldt.system || ldt.type != 2)
                {
                    error = "invalid LDT descriptor";
                    return false;
                }
                table = cpu_interface::descriptor_table_register{.base = ldt.base, .limit = ldt.limit};
            }
            return passive_descriptor(win.memory, *table, selector, sizeof(raw), error) &&
                   passive_read(win.memory, table->base + (selector & 0xFFF8), &raw, sizeof(raw), error);
        }

        std::optional<uint32_t> capture_code_bits(windows_emulator& win, x86_64_cpu& cpu, std::string& error)
        {
            segment_utils::raw_segment_descriptor raw{};
            if (!capture_segment_descriptor(win, cpu, cpu.reg<uint16_t>(x86_register::cs), raw, error))
            {
                return std::nullopt;
            }
            if ((raw.access & 0x98) != 0x98 || (raw.limit_high_flags & 0x60) == 0x60)
            {
                error = "invalid code descriptor";
                return std::nullopt;
            }
            const auto descriptor = decode_descriptor(raw);
            // The existing mode helper reads descriptors itself; all those reads must be passive too.
            // Decode the captured bytes directly instead, without re-reading the descriptor through that helper.
            if (descriptor.long_mode)
            {
                return 64U;
            }
            return descriptor.default_op_size ? 32U : 16U;
        }

        bool capture_stack_address(windows_emulator& win, x86_64_cpu& cpu, fault_stack_snapshot& slot, const uint32_t code_bits)
        {
            slot.pointer_bits = code_bits;
            slot.width_source = "fault_cs_default_near_return_operand";
            if (code_bits == 64)
            {
                slot.address_bits = 64;
                slot.segment_base = 0;
                slot.address_source = "64_bit_rsp";
                slot.address = cpu.reg<uint64_t>(x86_register::rsp);
                return true;
            }
            if (code_bits != 32)
            {
                slot.error = "16-bit code stack sampling is unsupported";
                return false;
            }
            // SS.B chooses SP/ESP independently of CS.D. These are current table bytes, not the
            // CPU's hidden descriptor cache; the source label preserves that diagnostic limitation.
            slot.address_source = "current_descriptor_table";
            const auto ss = cpu.reg<uint16_t>(x86_register::ss);
            const auto cpl = cpu.reg<uint16_t>(x86_register::cs) & 3;
            segment_utils::raw_segment_descriptor raw{};
            if (!capture_segment_descriptor(win, cpu, ss, raw, slot.error))
            {
                return false;
            }
            if ((raw.access & 0x9A) != 0x92 || (raw.limit_high_flags & 0x20) != 0 || ((raw.access >> 5) & 3) != cpl || (ss & 3) != cpl)
            {
                slot.error = "invalid current stack descriptor";
                return false;
            }
            const auto descriptor = decode_descriptor(raw);
            slot.address_bits = descriptor.default_op_size ? 32U : 16U;
            slot.segment_base = descriptor.base;
            const uint64_t offset = descriptor.default_op_size ? cpu.reg<uint32_t>(x86_register::esp)
                                                               : static_cast<uint16_t>(cpu.reg<uint64_t>(x86_register::rsp));
            const uint64_t last = offset + code_bits / 8 - 1;
            const uint64_t upper = descriptor.default_op_size ? UINT32_MAX : UINT16_MAX;
            if (last > upper)
            {
                slot.error = "stack sample crossing address-width boundary is unsupported";
                return false;
            }
            const bool expand_down = (descriptor.type & 4) != 0;
            if (expand_down ? offset <= descriptor.limit : last > descriptor.limit)
            {
                slot.error = "stack sample outside current descriptor limit";
                return false;
            }
            if (descriptor.base + last > UINT32_MAX)
            {
                slot.error = "stack sample with 32-bit linear-address wrap is unsupported";
                return false;
            }
            slot.address = descriptor.base + offset;
            return true;
        }

        std::string hex_bytes(const std::span<const uint8_t> bytes)
        {
            constexpr std::string_view digits = "0123456789abcdef";
            std::string result;
            result.reserve(bytes.size() * 2);
            for (const auto byte : bytes)
            {
                result.push_back(digits[byte >> 4]);
                result.push_back(digits[byte & 15]);
            }
            return result;
        }

        fault_instruction_snapshot capture_instruction(const analysis_context& context, const uint64_t address,
                                                       const std::optional<uint32_t> bits)
        {
            fault_instruction_snapshot result{.location = capture_address(*context.win_emu, address)};
            std::array<uint8_t, 15> bytes{};
            size_t count = 0;
            for (; count < bytes.size(); ++count)
            {
                if (address > std::numeric_limits<uint64_t>::max() - count ||
                    !passive_read(context.win_emu->memory, address + count, &bytes[count], 1, result.error))
                {
                    if (result.error.empty())
                    {
                        result.error = "address overflow";
                    }
                    break;
                }
            }
            result.bytes_hex = hex_bytes(std::span(bytes).first(count));
            if (!count)
            {
                return result;
            }
            if (!bits)
            {
                result.error = "decode mode unavailable";
                return result;
            }
            auto handle = context.d.get_handle_64();
            if (*bits == 32)
            {
                handle = context.d.get_handle_32();
            }
            else if (*bits == 16)
            {
                handle = context.d.get_handle_16();
            }
            cs_insn* decoded{};
            const auto decoded_count = cs_disasm(handle, bytes.data(), count, address, 1, &decoded);
            const auto release = utils::finally([&] { cs_free(decoded, decoded_count); });
            if (decoded_count == 0)
            {
                if (!result.error.empty())
                {
                    result.error += "; ";
                }
                result.error += "incomplete or invalid instruction";
                return result;
            }
            result.decoded_size = decoded[0].size;
            result.bytes_hex = hex_bytes(std::span(bytes).first(result.decoded_size));
            result.assembly = std::string(decoded[0].mnemonic) + (decoded[0].op_str[0] ? " "s + decoded[0].op_str : "");
            result.error.clear();
            return result;
        }

        template <typename T = uint64_t>
        void capture_register(memory_violation_event& event, x86_64_cpu& cpu, const char* name, const x86_register reg)
        {
            fault_register_snapshot value{.name = name};
            try
            {
                value.value = cpu.reg<T>(reg);
            }
            catch (const std::exception& error)
            {
                value.error = error.what();
            }
            event.registers.push_back(std::move(value));
        }
    }

    void capture_private_execute_memory(const analysis_context& context, memory_violation_event& event, const uint64_t rip)
    {
        constexpr size_t row_size = 16;
        constexpr size_t row_count = 8;
        constexpr std::string_view hex = "0123456789abcdef";
        const auto start = (rip >= 64 ? rip - 64 : 0) & ~uint64_t{0xF};
        event.private_execute_vcpu = context.win_emu->active_cpu().index();
        event.private_execute_memory.reserve(row_count);
        for (size_t row_index = 0; row_index < row_count; ++row_index)
        {
            auto& row = event.private_execute_memory.emplace_back();
            row.address = start + row_index * row_size;
            row.bytes_hex.reserve(row_size * 3 - 1);
            for (size_t byte_index = 0; byte_index < row_size; ++byte_index)
            {
                if (byte_index != 0)
                {
                    row.bytes_hex.push_back(' ');
                }
                uint8_t byte{};
                std::string error;
                if (row.address <= UINT64_MAX - byte_index &&
                    passive_read(context.win_emu->memory, row.address + byte_index, &byte, 1, error))
                {
                    row.bytes_hex.push_back(hex[byte >> 4]);
                    row.bytes_hex.push_back(hex[byte & 15]);
                    ++row.readable_bytes;
                }
                else
                {
                    row.bytes_hex += "??";
                }
            }
        }
    }

    void capture_memory_violation(const analysis_context& context, memory_violation_event& event, const uint64_t actual_ip)
    {
        auto& win = *context.win_emu;
        auto& cpu = win.active_cpu();
        event.fault_address.address = event.address;
        event.actual_instruction.location.address = actual_ip;
        constexpr std::array registers{
            std::pair{"rax", x86_register::rax}, std::pair{"rbx", x86_register::rbx}, std::pair{"rcx", x86_register::rcx},
            std::pair{"rdx", x86_register::rdx}, std::pair{"rsi", x86_register::rsi}, std::pair{"rdi", x86_register::rdi},
            std::pair{"rbp", x86_register::rbp}, std::pair{"rsp", x86_register::rsp}, std::pair{"r8", x86_register::r8},
            std::pair{"r9", x86_register::r9},   std::pair{"r10", x86_register::r10}, std::pair{"r11", x86_register::r11},
            std::pair{"r12", x86_register::r12}, std::pair{"r13", x86_register::r13}, std::pair{"r14", x86_register::r14},
            std::pair{"r15", x86_register::r15}, std::pair{"rip", x86_register::rip},
        };
        for (const auto& [name, reg] : registers)
        {
            capture_register(event, cpu, name, reg);
        }
        capture_register<uint32_t>(event, cpu, "eflags", x86_register::eflags);
        capture_register<uint16_t>(event, cpu, "cs", x86_register::cs);
        capture_register<uint16_t>(event, cpu, "ss", x86_register::ss);
        try
        {
            event.fault_address = capture_address(win, event.address);
            try
            {
                event.code_bits = capture_code_bits(win, cpu, event.capture_error);
            }
            catch (const std::exception& error)
            {
                event.capture_error = error.what();
            }
            event.actual_instruction = capture_instruction(context, actual_ip, event.code_bits);
            try
            {
                const auto& thread = win.current_thread();
                if (thread.executed_instructions != 0)
                {
                    event.last_tracked_instruction = capture_instruction(context, thread.current_ip, event.code_bits);
                }
            }
            catch (const std::exception& error)
            {
                event.capture_error = error.what();
            }
            if (!event.code_bits)
            {
                event.stack_slot.error = "32/64-bit stack width unavailable";
                return;
            }
            auto& slot = event.stack_slot;
            if (!capture_stack_address(win, cpu, slot, *event.code_bits))
            {
                return;
            }
            uint64_t value{};
            if (passive_read(win.memory, *slot.address, &value, *event.code_bits / 8, slot.error))
            {
                slot.value = value;
                slot.value_location = capture_address(win, value);
            }
        }
        catch (const std::exception& error)
        {
            event.capture_error = error.what();
        }
    }
}
