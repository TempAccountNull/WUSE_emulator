#include "std_include.hpp"
#include "debug_print.hpp"
#include "analysis.hpp"
#include <windows_emulator.hpp>
#include <emulator_utils.hpp>
#include <utils/string.hpp>
#include <limits>

namespace sogen
{
    namespace
    {
        constexpr size_t capture_limit = 16 * 1024 * 1024;

        bool passive_read(analysis_context& c, const uint64_t address, void* output, const size_t size)
        {
            if (size > std::numeric_limits<uint64_t>::max() - address)
            {
                return false;
            }
            for (uint64_t cursor = address; cursor < address + size;)
            {
                const auto region = c.win_emu->memory.get_region_info(cursor);
                if (!region.is_committed || region.kind == memory_region_kind::mmio)
                {
                    return false;
                }
                cursor += std::min<uint64_t>(4096 - (cursor & 4095), address + size - cursor);
            }
            return c.win_emu->memory.try_read_memory(address, output, size);
        }

        template <typename T>
        T passive_value(analysis_context& c, const uint64_t address)
        {
            T value{};
            if (!passive_read(c, address, &value, sizeof(value)))
            {
                throw std::runtime_error("unreadable argument memory");
            }
            return value;
        }

        uint64_t function_argument(analysis_context& c, const size_t index)
        {
            auto& cpu = c.win_emu->active_cpu();
            if (is_32bit_code_segment(cpu))
            {
                return passive_value<uint32_t>(c, static_cast<uint64_t>(cpu.reg<uint32_t>(x86_register::esp)) + 4 + index * 4);
            }
            constexpr std::array registers{x86_register::rcx, x86_register::rdx, x86_register::r8, x86_register::r9};
            return index < registers.size() ? cpu.reg(registers[index])
                                            : passive_value<uint64_t>(c, cpu.reg(x86_register::rsp) + 8 + index * 8);
        }

        std::string hex_bytes(const std::string_view bytes)
        {
            constexpr std::string_view digits = "0123456789abcdef";
            std::string result;
            result.reserve(bytes.size() * 2);
            for (const unsigned char byte : bytes)
            {
                result.push_back(digits[byte >> 4]);
                result.push_back(digits[byte & 15]);
            }
            return result;
        }

        std::string display_bytes(const std::string_view bytes, const bool wide)
        {
            if (!wide)
            {
                return u16_to_u8(cp1252_to_u16(std::string(bytes)));
            }
            std::u16string value(bytes.size() / 2, u'\0');
            memcpy(value.data(), bytes.data(), value.size() * 2);
            for (size_t index = 0; index < value.size(); ++index)
            {
                if (value[index] >= 0xd800 && value[index] <= 0xdbff)
                {
                    if (index + 1 < value.size() && value[index + 1] >= 0xdc00 && value[index + 1] <= 0xdfff)
                    {
                        ++index;
                    }
                    else
                    {
                        value[index] = 0xfffd;
                    }
                }
                else if (value[index] >= 0xdc00 && value[index] <= 0xdfff)
                {
                    value[index] = 0xfffd;
                }
            }
            return u16_to_u8(value);
        }

        std::string read_bytes(analysis_context& c, const uint64_t address, const std::optional<size_t> length, const bool wide,
                               std::string& error, const bool terminated = false)
        {
            std::string bytes;
            if (length && !*length)
            {
                return bytes;
            }
            if (!address)
            {
                error = "null pointer";
                return bytes;
            }
            const auto limit = std::min(length.value_or(capture_limit), capture_limit);
            const size_t unit = wide ? 2 : 1;
            while (bytes.size() < limit)
            {
                if (address > std::numeric_limits<uint64_t>::max() - bytes.size())
                {
                    error = "address overflow";
                    break;
                }
                const auto next = address + bytes.size();
                const auto region = c.win_emu->memory.get_region_info(next);
                if (!region.is_committed || region.kind == memory_region_kind::mmio)
                {
                    error = "unreadable memory";
                    break;
                }
                std::array<char, 4096> chunk{};
                auto count = std::min({chunk.size(), limit - bytes.size(), static_cast<size_t>(4096 - (next & 4095))});
                count -= count % unit;
                if (!count)
                {
                    count = std::min(unit, limit - bytes.size());
                }
                if (!passive_read(c, next, chunk.data(), count))
                {
                    error = "unreadable memory";
                    break;
                }
                if (!length || terminated)
                {
                    for (size_t i = 0; i + unit <= count; i += unit)
                    {
                        if (chunk[i] == 0 && (!wide || chunk[i + 1] == 0))
                        {
                            bytes.append(chunk.data(), i);
                            return bytes;
                        }
                    }
                }
                bytes.append(chunk.data(), count);
            }
            if (error.empty() && (!length || *length > capture_limit))
            {
                error = "capture safety limit reached (16 MiB)";
            }
            return bytes;
        }

        void read_text(analysis_context& c, debug_print_argument& arg, const bool wide, const std::optional<size_t> length = std::nullopt,
                       const bool terminated = false)
        {
            const auto bytes = read_bytes(c, arg.raw, length, wide, arg.error, terminated);
            arg.encoding = wide ? "utf-16le" : "windows-1252";
            arg.bytes_hex = hex_bytes(bytes);
            arg.text = display_bytes(bytes, wide);
        }

        std::vector<uint64_t> origins(analysis_context& c)
        {
            prune_debug_print_calls(c, c.win_emu->active_cpu().read_instruction_pointer());
            std::vector<uint64_t> ids;
            const auto found = c.debug_print_calls.find(c.win_emu->current_thread().id);
            if (found != c.debug_print_calls.end())
            {
                for (const auto& entry : found->second)
                {
                    ids.push_back(entry.call_id);
                }
            }
            return ids;
        }

        void emit_output(analysis_context& c, const std::string_view transport, const uint64_t address, const size_t length,
                         const bool wide, const bool remove_terminator, const std::optional<std::string_view> supplied = std::nullopt,
                         const uint32_t component = 0, const uint32_t level = 0, const uint64_t fallback_address = 0,
                         const size_t fallback_length = 0)
        {
            std::string error;
            auto bytes = supplied ? std::string(*supplied) : read_bytes(c, address, length, wide, error);
            auto text_bytes = bytes;
            if (remove_terminator)
            {
                const auto unit = wide ? 2U : 1U;
                if (text_bytes.size() >= unit && text_bytes.back() == 0 && (!wide || text_bytes[text_bytes.size() - 2] == 0))
                {
                    text_bytes.resize(text_bytes.size() - unit);
                }
            }
            c.emit_observation<debug_string_event>([&](auto& event) {
                event.details = display_bytes(text_bytes, wide);
                event.transport = transport;
                event.origin_calls = origins(c);
                if (transport == "dbwin" && text_bytes.starts_with("OODLE ERROR"))
                {
                    auto& snapshot = event.cpu_snapshot.emplace();
                    auto& cpu = c.win_emu->active_cpu();
                    const bool bits32 = is_32bit_code_segment(cpu);
                    snapshot.pointer_bits = bits32 ? 32 : 64;
                    snapshot.instruction_pointer = cpu.read_instruction_pointer();
                    snapshot.stack_pointer = bits32 ? cpu.reg<uint32_t>(x86_register::esp) : cpu.reg(x86_register::rsp);
                    constexpr std::array registers{x86_register::rax, x86_register::rbx, x86_register::rcx, x86_register::rdx,
                                                   x86_register::rsi, x86_register::rdi, x86_register::rbp, x86_register::rsp,
                                                   x86_register::r8,  x86_register::r9,  x86_register::r10, x86_register::r11,
                                                   x86_register::r12, x86_register::r13, x86_register::r14, x86_register::r15};
                    for (size_t i = 0; i < registers.size(); ++i)
                    {
                        if (bits32 && i >= 8)
                        {
                            break;
                        }
                        snapshot.gprs[i] = bits32 ? cpu.reg<uint32_t>(registers[i]) : cpu.reg(registers[i]);
                    }
                    for (size_t i = 0; i < 64; ++i)
                    {
                        const auto width = bits32 ? sizeof(uint32_t) : sizeof(uint64_t);
                        if (snapshot.stack_pointer > std::numeric_limits<uint64_t>::max() - i * width)
                        {
                            break;
                        }
                        const auto word_address = snapshot.stack_pointer + i * width;
                        uint64_t word{};
                        if (bits32)
                        {
                            uint32_t value{};
                            if (!passive_read(c, word_address, &value, sizeof(value)))
                            {
                                break;
                            }
                            word = value;
                        }
                        else if (!passive_read(c, word_address, &word, sizeof(word)))
                        {
                            break;
                        }
                        snapshot.stack_words.push_back(word);
                    }
                }
                event.data_address = address;
                event.byte_length = length;
                event.encoding = wide ? "utf-16le" : "windows-1252";
                event.bytes_hex = hex_bytes(bytes);
                event.error = error;
                event.component = component;
                event.level = level;
                if (fallback_address)
                {
                    event.ansi_fallback = debug_print_argument{.name = "AnsiFallback", .raw = fallback_address};
                    read_text(c, *event.ansi_fallback, false, fallback_length);
                    if (!event.ansi_fallback->text.empty() && event.ansi_fallback->text.back() == 0)
                    {
                        event.ansi_fallback->text.pop_back();
                    }
                }
            });
        }

        void observe_exception(analysis_context& c, const bool bits32)
        {
            const auto pointer = function_argument(c, 0);
            uint32_t code{};
            uint32_t count{};
            const auto count_offset = bits32 ? 16U : 24U;
            const auto info_offset = bits32 ? 20U : 32U;
            if (!passive_read(c, pointer, &code, sizeof(code)) || (code != 0x40010006 && code != 0x4001000a) ||
                !passive_read(c, pointer + count_offset, &count, sizeof(count)) || count < (code == 0x4001000a ? 4U : 2U))
            {
                return;
            }
            const auto info = [&](const size_t index) -> uint64_t {
                return bits32 ? passive_value<uint32_t>(c, pointer + info_offset + index * 4)
                              : passive_value<uint64_t>(c, pointer + info_offset + index * 8);
            };
            const auto length = info(0);
            if (length > std::numeric_limits<size_t>::max() / 2)
            {
                return;
            }
            emit_output(c, "debug_exception", info(1), length * (code == 0x4001000a ? 2 : 1), code == 0x4001000a, true, std::nullopt, 0, 0,
                        code == 0x4001000a ? info(3) : 0, code == 0x4001000a ? info(2) : 0);
        }

        void read_variadic(analysis_context& c, debug_print_call_event& event, const std::string_view format, const size_t first,
                           const std::optional<uint64_t> list)
        {
            const bool bits32 = event.pointer_bits == 32;
            size_t slot = first;
            uint64_t offset = 0;
            const auto next = [&](std::string label, const size_t size) -> debug_print_argument& {
                debug_print_argument arg{.name = std::move(label)};
                const auto current_slot = slot;
                const auto current_offset = offset;
                slot += bits32 && size == 8 ? 2 : 1;
                offset += bits32 ? size : 8;
                try
                {
                    if (list)
                    {
                        arg.raw = size == 8 ? passive_value<uint64_t>(c, *list + current_offset)
                                            : passive_value<uint32_t>(c, *list + current_offset);
                    }
                    else if (bits32 && size == 8)
                    {
                        arg.raw = function_argument(c, current_slot) | (function_argument(c, current_slot + 1) << 32);
                    }
                    else
                    {
                        arg.raw = function_argument(c, current_slot);
                        if (size == 4)
                        {
                            arg.raw = static_cast<uint32_t>(arg.raw);
                        }
                    }
                }
                catch (const std::exception&)
                {
                    arg.error = "unreadable argument";
                }
                event.arguments.push_back(std::move(arg));
                return event.arguments.back();
            };
            size_t argument = 0;
            size_t captured = 0;
            for (const auto& arg : event.arguments)
            {
                captured += arg.bytes_hex.size() / 2;
            }
            for (size_t i = 0; i < format.size(); ++i)
            {
                if (event.arguments.size() >= 4096 || captured >= capture_limit * 4)
                {
                    event.error = "call capture safety limit reached (4096 arguments or 64 MiB)";
                    return;
                }
                if (format[i] != '%')
                {
                    continue;
                }
                ++i;
                if (i >= format.size())
                {
                    break;
                }
                if (format[i] == '%')
                {
                    continue;
                }
                const auto start = i - 1;
                while (i < format.size() && std::string_view("-+ #0").find(format[i]) != std::string_view::npos)
                {
                    ++i;
                }
                if (i < format.size() && format[i] == '*')
                {
                    next("width", 4);
                    ++i;
                }
                while (i < format.size() && format[i] >= '0' && format[i] <= '9')
                {
                    ++i;
                }
                std::optional<size_t> precision;
                if (i < format.size() && format[i] == '.')
                {
                    ++i;
                    if (i < format.size() && format[i] == '*')
                    {
                        const auto value = static_cast<int32_t>(next("precision", 4).raw);
                        if (value >= 0)
                        {
                            precision = value;
                        }
                        ++i;
                    }
                    else
                    {
                        size_t value = 0;
                        while (i < format.size() && format[i] >= '0' && format[i] <= '9')
                        {
                            value = std::min(capture_limit, value * 10 + static_cast<size_t>(format[i++] - '0'));
                        }
                        precision = value;
                    }
                }
                std::string modifier;
                for (const auto candidate : {"I64"sv, "I32"sv, "ll"sv, "hh"sv, "h"sv, "l"sv, "w"sv, "I"sv, "z"sv, "t"sv, "j"sv, "L"sv})
                {
                    if (format.substr(i).starts_with(candidate))
                    {
                        modifier = candidate;
                        i += candidate.size();
                        break;
                    }
                }
                if (i == format.size() || std::string_view("diouxXfFeEgGaAcCsSpnZ").find(format[i]) == std::string_view::npos)
                {
                    event.error = "unrecognized format directive at byte " + std::to_string(start);
                    return;
                }
                const auto type = format[i];
                const bool pointer = std::string_view("sSpnZ").find(type) != std::string_view::npos;
                const bool floating = std::string_view("fFeEgGaA").find(type) != std::string_view::npos;
                const bool wide_integer = modifier == "ll" || modifier == "I64" || modifier == "j" ||
                                          (!bits32 && (modifier == "I" || modifier == "z" || modifier == "t"));
                const size_t pointer_width = bits32 ? 4 : 8;
                const size_t value_width = floating || wide_integer ? 8 : 4;
                auto& arg = next("vararg[" + std::to_string(argument++) + "] " + std::string(format.substr(start, i - start + 1)),
                                 pointer ? pointer_width : value_width);
                if (arg.error.empty() && (type == 's' || type == 'S'))
                {
                    const bool wide = modifier == "l" || modifier == "w" || (type == 'S' && modifier != "h");
                    if (precision)
                    {
                        read_text(c, arg, wide, *precision * (wide ? 2 : 1), true);
                    }
                    else
                    {
                        read_text(c, arg, wide);
                    }
                }
                if (arg.error.empty() && type == 'Z' && arg.raw)
                {
                    try
                    {
                        const auto length = passive_value<uint16_t>(c, arg.raw);
                        const auto buffer = bits32 ? passive_value<uint32_t>(c, arg.raw + 4) : passive_value<uint64_t>(c, arg.raw + 8);
                        const auto structure = arg.raw;
                        arg.raw = buffer;
                        read_text(c, arg, modifier == "w" || modifier == "l", length);
                        arg.raw = structure;
                    }
                    catch (const std::exception&)
                    {
                        arg.error = "unreadable counted string";
                    }
                }
                captured += arg.bytes_hex.size() / 2;
            }
        }
    }

    void prune_debug_print_calls(analysis_context& c, const uint64_t address)
    {
        if (c.debug_print_calls.empty())
        {
            return;
        }
        const auto found = c.debug_print_calls.find(c.win_emu->current_thread().id);
        if (found == c.debug_print_calls.end())
        {
            return;
        }
        const auto sp = c.win_emu->active_cpu().reg(x86_register::rsp);
        auto& calls = found->second;
        while (!calls.empty() && (sp > calls.back().stack || address == calls.back().return_address))
        {
            calls.pop_back();
        }
        if (calls.empty())
        {
            c.debug_print_calls.erase(found);
        }
    }

    void observe_debug_print_call(analysis_context& c, std::string_view name)
    {
        if (name.starts_with('_'))
        {
            name.remove_prefix(1);
        }
        name = name.substr(0, name.find('@'));
        const bool output = name == "OutputDebugStringA" || name == "OutputDebugStringW";
        const bool simple = name == "DbgPrint" || name == "DbgPrintReturnControlC";
        const bool prefixed = name == "vDbgPrintExWithPrefix";
        const bool va = prefixed || name == "vDbgPrintEx";
        if (!output && !simple && !va && name != "DbgPrintEx" && name != "RtlRaiseException" && name != "NtWow64DebuggerCall")
        {
            return;
        }
        auto& cpu = c.win_emu->active_cpu();
        const bool bits32 = is_32bit_code_segment(cpu);
        try
        {
            if (name == "RtlRaiseException")
            {
                observe_exception(c, bits32);
                return;
            }
            if (name == "NtWow64DebuggerCall")
            {
                if (function_argument(c, 0) == 1)
                {
                    emit_output(c, "wow64_debugger_call", function_argument(c, 1), static_cast<uint16_t>(function_argument(c, 2)), false,
                                false, std::nullopt, static_cast<uint32_t>(function_argument(c, 3)),
                                static_cast<uint32_t>(function_argument(c, 4)));
                }
                return;
            }
            debug_print_call_event event;
            event.header = c.make_event_header();
            event.execution = c.make_execution_context();
            event.call_id = event.header.sequence;
            event.api = name;
            event.pointer_bits = bits32 ? 32 : 64;
            event.stack_pointer = cpu.reg(x86_register::rsp);
            event.return_address =
                bits32 ? passive_value<uint32_t>(c, event.stack_pointer) : passive_value<uint64_t>(c, event.stack_pointer);
            if (const auto* module = c.win_emu->mod_manager.find_name(event.return_address))
            {
                event.return_module = module;
            }
            const auto add = [&](const size_t index, const std::string_view label, const bool text, const bool wide = false) {
                debug_print_argument arg{.name = std::string(label), .raw = function_argument(c, index)};
                if (text)
                {
                    read_text(c, arg, wide);
                }
                event.arguments.push_back(std::move(arg));
            };
            if (output)
            {
                add(0, "lpOutputString", true, name == "OutputDebugStringW");
            }
            else
            {
                if (prefixed)
                {
                    add(0, "Prefix", true);
                }
                if (!simple)
                {
                    add(prefixed ? 1 : 0, "ComponentId", false);
                    add(prefixed ? 2 : 1, "Level", false);
                }
                const size_t extended_format_index = prefixed ? 3 : 2;
                const size_t format_index = simple ? 0 : extended_format_index;
                add(format_index, "Format", true);
                const auto format = event.arguments.back().text;
                std::optional<uint64_t> list;
                if (va)
                {
                    add(format_index + 1, "ArgList", false);
                    list = event.arguments.back().raw;
                }
                read_variadic(c, event, format, format_index + 1, list);
            }
            c.debug_print_calls[event.execution.thread_id].push_back(
                {.call_id = event.call_id, .stack = event.stack_pointer, .return_address = event.return_address});
            c.emit_event(event);
        }
        catch (const std::exception& error)
        {
            c.emit_observation<debug_print_call_event>([&](auto& event) {
                event.api = name;
                event.call_id = c.next_event_sequence;
                event.pointer_bits = bits32 ? 32 : 64;
                event.error = error.what();
            });
        }
    }

    void observe_debug_string(analysis_context& c, const std::string_view bytes)
    {
        emit_output(c, "dbwin", c.win_emu->process.dbwin_buffer ? c.win_emu->process.dbwin_buffer + 4 : 0, bytes.size(), false, false,
                    bytes);
    }

    void observe_debug_print_interrupt(analysis_context& c, const uint64_t address, const uint16_t length, const uint32_t component,
                                       const uint32_t level)
    {
        emit_output(c, "int2d", address, length, false, false, std::nullopt, component, level);
    }

    std::string escape_debug_console(const std::string_view text)
    {
        std::string output;
        constexpr std::string_view digits = "0123456789ABCDEF";
        for (const unsigned char ch : text)
        {
            switch (ch)
            {
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '"':
                output += "\\\"";
                break;
            default:
                if (ch < 32 || ch >= 127)
                {
                    output += "\\x";
                    output.push_back(digits[ch >> 4]);
                    output.push_back(digits[ch & 15]);
                }
                else
                {
                    output.push_back(static_cast<char>(ch));
                }
            }
        }
        return output;
    }
}
