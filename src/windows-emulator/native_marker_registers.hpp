#pragma once

#include "guest_inspection_query.hpp"
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace sogen::detail
{
    inline constexpr uint32_t native_marker_stack_words = 16;

    struct native_marker_register_snapshot
    {
        uint32_t actual_cpu{UINT32_MAX};
        uint32_t actual_tid{};
        inspection_context context{};
    };

    struct native_marker_register_view
    {
        std::string_view name{};
        uint32_t index{};
        size_t width{};
        bool available{};
        bool read_error{};
        std::span<const std::byte> raw{};
    };

    struct native_marker_stack_view
    {
        uint32_t index{};
        bool address_available{};
        uint64_t address{};
        bool available{};
        std::span<const std::byte> raw{};
    };

    namespace marker_register_detail
    {
        struct register_description
        {
            std::string_view name{};
            size_t offset{};
            size_t width{};
        };

        inline constexpr auto descriptions = [] {
            std::array<register_description, 78> result{};
            size_t index = 0;
            const auto group = [&](const auto& names, const size_t offset, const size_t width) {
                for (size_t item = 0; item < names.size(); ++item)
                {
                    result[index++] = {.name = names[item], .offset = offset + item * width, .width = width};
                }
            };
            constexpr std::array<std::string_view, 16> gpr{"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                                           "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
            constexpr std::array<std::string_view, 4> execution{"rip", "rflags", "fs_base", "gs_base"};
            constexpr std::array<std::string_view, 4> cr{"cr0", "cr2", "cr3", "cr4"};
            constexpr std::array<std::string_view, 6> dr{"dr0", "dr1", "dr2", "dr3", "dr6", "dr7"};
            constexpr std::array<std::string_view, 6> segments{"cs", "ss", "ds", "es", "fs", "gs"};
            constexpr std::array<std::string_view, 1> mxcsr{"mxcsr"};
            constexpr std::array<std::string_view, 1> efer{"efer"};
            constexpr std::array<std::string_view, 16> xmm{"xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
                                                           "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};
            constexpr std::array<std::string_view, 8> st{"st0", "st1", "st2", "st3", "st4", "st5", "st6", "st7"};
            constexpr std::array<std::string_view, 3> control{"fpcw", "fpsw", "fptag"};
            constexpr std::array<std::string_view, 5> fp_address{"fcs", "fip", "fds", "fdp", "fop"};
            constexpr std::array<std::string_view, 8> mm{"mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7"};
            group(gpr, offsetof(inspection_context, gpr), 8);
            group(execution, offsetof(inspection_context, rip), 8);
            group(cr, offsetof(inspection_context, cr), 8);
            group(dr, offsetof(inspection_context, dr), 8);
            group(segments, offsetof(inspection_context, segments), 2);
            group(mxcsr, offsetof(inspection_context, mxcsr), 4);
            group(efer, offsetof(inspection_context, efer), 8);
            group(xmm, offsetof(inspection_context, xmm), 16);
            group(st, offsetof(inspection_context, st), 10);
            group(control, offsetof(inspection_context, fp_control), 2);
            group(fp_address, offsetof(inspection_context, fp_address), 4);
            group(mm, offsetof(inspection_context, mmx), 8);
            return result;
        }();

        static_assert(descriptions.front().name == "rax");
        static_assert(descriptions.back().name == "mm7");
        static_assert(descriptions[38].offset == offsetof(inspection_context, xmm));
        static_assert(descriptions[54].offset == offsetof(inspection_context, st));
    }

    template <typename RegisterReader, typename MemoryReader, typename StackAdmission>
    native_marker_register_snapshot capture_native_marker_context(memory_manager& memory, const uint32_t actual_cpu,
                                                                  const uint32_t actual_tid, RegisterReader&& register_reader,
                                                                  MemoryReader&& memory_reader, StackAdmission&& admission)
    {
        native_marker_register_snapshot result{.actual_cpu = actual_cpu, .actual_tid = actual_tid};
        inspection_registers(std::forward<RegisterReader>(register_reader), result.context);
        auto& context = result.context;
        context.stack_requested_words = native_marker_stack_words;
        context.stack_status = 2;
        if (!(context.available[0] & (uint64_t{1} << 7)))
        {
            return result;
        }
        context.stack_base = context.gpr[7];
        uint32_t successes = 0;
        for (uint32_t index = 0; index < native_marker_stack_words; ++index)
        {
            const auto offset = uint64_t{index} * 8;
            if (context.stack_base > UINT64_MAX - offset)
            {
                continue;
            }
            const auto address = context.stack_base + offset;
            uint64_t value{};
            try
            {
                if (!admission(address, sizeof(value)) || !inspection_passive_span(memory, address, sizeof(value)) ||
                    !admission(address, sizeof(value)) || !memory_reader(address, &value, sizeof(value)))
                {
                    continue;
                }
            }
            catch (...)
            {
                continue;
            }
            context.stack[index] = value;
            context.stack_success[index / 64] |= uint64_t{1} << (index % 64);
            ++successes;
        }
        if (successes == native_marker_stack_words)
        {
            context.stack_status = 0;
        }
        else if (successes)
        {
            context.stack_status = 1;
        }
        return result;
    }

    template <typename RegisterReader, typename MemoryReader>
    native_marker_register_snapshot capture_native_marker_context(memory_manager& memory, const uint32_t actual_cpu,
                                                                  const uint32_t actual_tid, RegisterReader&& register_reader,
                                                                  MemoryReader&& memory_reader)
    {
        return capture_native_marker_context(memory, actual_cpu, actual_tid, std::forward<RegisterReader>(register_reader),
                                             std::forward<MemoryReader>(memory_reader), [](uint64_t, size_t) { return true; });
    }

    template <typename Visitor>
    void for_each_native_marker_register(const native_marker_register_snapshot& snapshot, Visitor&& visitor)
    {
        const auto bytes = std::as_bytes(std::span{&snapshot.context, size_t{1}});
        for (uint32_t index = 0; index < marker_register_detail::descriptions.size(); ++index)
        {
            const auto& description = marker_register_detail::descriptions[index];
            const auto mask = uint64_t{1} << (index % 64);
            const bool available = (snapshot.context.available[index / 64] & mask) != 0;
            const bool read_error = (snapshot.context.read_errors[index / 64] & mask) != 0;
            const auto raw = available ? bytes.subspan(description.offset, description.width) : std::span<const std::byte>{};
            visitor(native_marker_register_view{.name = description.name,
                                                .index = index,
                                                .width = description.width,
                                                .available = available,
                                                .read_error = read_error,
                                                .raw = raw});
        }
    }

    template <typename Visitor>
    void for_each_native_marker_stack_word(const native_marker_register_snapshot& snapshot, Visitor&& visitor)
    {
        const auto& context = snapshot.context;
        const auto count = (std::min)(context.stack_requested_words, native_marker_stack_words);
        const bool rsp_available = (context.available[0] & (uint64_t{1} << 7)) != 0;
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto offset = uint64_t{index} * 8;
            const bool address_available = rsp_available && context.stack_base <= UINT64_MAX - offset;
            const auto address = address_available ? context.stack_base + offset : 0;
            const bool available = address_available && (context.stack_success[index / 64] & (uint64_t{1} << (index % 64))) != 0;
            const auto raw = available ? std::as_bytes(std::span{&context.stack[index], size_t{1}}) : std::span<const std::byte>{};
            visitor(native_marker_stack_view{
                .index = index, .address_available = address_available, .address = address, .available = available, .raw = raw});
        }
    }

    std::string native_marker_raw_hex(std::span<const std::byte> bytes);
}
