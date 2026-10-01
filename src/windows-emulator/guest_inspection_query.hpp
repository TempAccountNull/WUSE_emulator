#pragma once

#include "guest_inspection_protocol.hpp"
#include "memory_manager.hpp"
#include "module/module_manager.hpp"
#include <x86_register.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>

namespace sogen::detail
{
    inline bool inspection_region_at(memory_manager& memory, const uint64_t address, inspection_region& output)
    {
        if (address >= MAX_ALLOCATION_END_EXCL)
        {
            return false;
        }
        const auto& reservations = memory.get_reserved_regions();
        auto next = reservations.upper_bound(address);
        uint64_t base = 0;
        if (next != reservations.begin())
        {
            const auto previous = std::prev(next);
            if (previous->first >= MAX_ALLOCATION_END_EXCL || !previous->second.length ||
                previous->second.length > MAX_ALLOCATION_END_EXCL - previous->first)
            {
                return false;
            }
            const auto end = previous->first + previous->second.length;
            if (address < end)
            {
                base = previous->first;
                const auto& commits = previous->second.committed_regions;
                const auto after = commits.upper_bound(address);
                if (after != commits.begin())
                {
                    const auto before = std::prev(after);
                    if (before->first < previous->first || before->first >= end || !before->second.length ||
                        before->second.length > end - before->first)
                    {
                        return false;
                    }
                    const auto commit_end = before->first + before->second.length;
                    base = address < commit_end ? before->first : commit_end;
                }
            }
            else
            {
                base = end;
            }
        }
        const auto info = memory.get_region_info(address);
        if (info.start > address || info.start >= MAX_ALLOCATION_END_EXCL || !info.length ||
            info.length > MAX_ALLOCATION_END_EXCL - info.start || base > info.start || info.start + info.length <= address)
        {
            return false;
        }
        output.base = base;
        output.length = info.start + info.length - base;
        output.allocation_base = info.allocation_base;
        output.allocation_length = info.is_reserved ? info.allocation_length : 0;
        output.queried_page_base = info.start;
        output.forward_length = info.length;
        if (info.is_committed)
        {
            output.state = 2;
        }
        else if (info.is_reserved)
        {
            output.state = 1;
        }
        else
        {
            output.state = 0;
        }
        output.kind = static_cast<uint8_t>(info.kind);
        output.permissions = static_cast<uint8_t>(info.permissions.common);
        output.initial_permissions = static_cast<uint8_t>(info.initial_permissions.common);
        output.guarded = info.permissions.is_guarded() ? 1 : 0;
        output.dep_enabled = memory.is_dep_enabled() ? 1 : 0;
        return output.length != 0;
    }

    inline bool inspection_address_at(memory_manager& memory, module_manager& modules, const uint64_t address, inspection_address& output)
    {
        if (!inspection_region_at(memory, address, output.region))
        {
            return false;
        }
        if (const auto* module = modules.find_by_address(address))
        {
            output.provenance_flags |= 1;
            output.module_base = module->image_base;
            output.module_size = module->size_of_image;
            std::memcpy(output.module_name.data(), module->name.data(), (std::min)(module->name.size(), sizeof(output.module_name) - 1));
            size_t examined = 0;
            for (const auto& section : module->sections)
            {
                if (examined++ >= 64)
                {
                    output.provenance_flags |= 8;
                    break;
                }
                if (address >= section.region.start && address - section.region.start < section.region.length)
                {
                    output.provenance_flags |= 2;
                    output.section_base = section.region.start;
                    output.section_length = section.region.length;
                    std::memcpy(output.section_name.data(), section.name.data(),
                                (std::min)(section.name.size(), sizeof(output.section_name) - 1));
                    break;
                }
            }
        }
        const auto& reservations = memory.get_reserved_regions();
        const auto found = reservations.find(output.region.allocation_base);
        if (output.region.state && found != reservations.end())
        {
            const auto& filename = found->second.mapped_filename;
            const auto count = (std::min)(filename.size(), size_t{255});
            output.mapped_filename_units = static_cast<uint32_t>(count);
            std::memcpy(output.mapped_filename.data(), filename.data(), count * sizeof(char16_t));
            if (filename.size() > count)
            {
                output.provenance_flags |= 4;
            }
        }
        return true;
    }

    inline void inspection_regions(memory_manager& memory, const uint64_t start, const uint32_t limit, inspection_region_page& output,
                                   uint64_t& next_cursor)
    {
        const auto began = std::chrono::steady_clock::now();
        next_cursor = start;
        const auto capped_limit = (std::min)(limit, uint32_t{64});
        while (output.count < capped_limit && next_cursor < MAX_ALLOCATION_END_EXCL)
        {
            if (output.count && std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(2))
            {
                output.stop_reason = 2;
                break;
            }
            auto& row = output.regions[output.count];
            if (!inspection_region_at(memory, next_cursor, row))
            {
                output = {};
                output.stop_reason = 3;
                next_cursor = 0;
                return;
            }
            const auto end = row.base + row.length;
            if (end <= next_cursor || end > MAX_ALLOCATION_END_EXCL)
            {
                output = {};
                output.stop_reason = 3;
                next_cursor = 0;
                return;
            }
            next_cursor = end;
            ++output.count;
        }
        if (output.stop_reason != 2 && next_cursor < MAX_ALLOCATION_END_EXCL)
        {
            output.stop_reason = 1;
        }
        output.elapsed_ns =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count());
    }

    inline bool inspection_passive_span(memory_manager& memory, const uint64_t address, const size_t size)
    {
        if (!size || address >= MAX_ALLOCATION_END_EXCL || size > MAX_ALLOCATION_END_EXCL - address)
        {
            return false;
        }
        auto cursor = address;
        const auto end = address + size;
        while (cursor < end)
        {
            inspection_region info{};
            if (!inspection_region_at(memory, cursor, info) || info.state != 2 ||
                info.kind == static_cast<uint8_t>(memory_region_kind::mmio) ||
                info.kind == static_cast<uint8_t>(memory_region_kind::host_reserved) || info.guarded ||
                !(info.permissions & static_cast<uint8_t>(memory_permission::read)))
            {
                return false;
            }
            const auto forward_end = info.queried_page_base + info.forward_length;
            if (forward_end <= cursor)
            {
                return false;
            }
            cursor = (std::min)(end, forward_end);
        }
        return true;
    }

    template <typename Reader>
    void inspection_registers(Reader&& reader, inspection_context& output)
    {
        const auto read = [&](const uint32_t index, const x86_register reg, void* destination, const size_t width) {
            std::array<uint8_t, 16> value{};
            try
            {
                if (reader(reg, value.data(), width) == width)
                {
                    std::memcpy(destination, value.data(), width);
                    output.available[index / 64] |= uint64_t{1} << (index % 64);
                    return;
                }
            }
            catch (...)
            {
            }
            output.read_errors[index / 64] |= uint64_t{1} << (index % 64);
        };
        constexpr std::array gpr = {x86_register::rax, x86_register::rbx, x86_register::rcx, x86_register::rdx,
                                    x86_register::rsi, x86_register::rdi, x86_register::rbp, x86_register::rsp,
                                    x86_register::r8,  x86_register::r9,  x86_register::r10, x86_register::r11,
                                    x86_register::r12, x86_register::r13, x86_register::r14, x86_register::r15};
        for (uint32_t index = 0; index < 16; ++index)
        {
            read(index, gpr[index], &output.gpr[index], 8);
        }
        read(16, x86_register::rip, &output.rip, 8);
        read(17, x86_register::rflags, &output.rflags, 8);
        read(18, x86_register::fs_base, &output.fs_base, 8);
        read(19, x86_register::gs_base, &output.gs_base, 8);
        constexpr std::array cr = {x86_register::cr0, x86_register::cr2, x86_register::cr3, x86_register::cr4};
        for (uint32_t index = 0; index < 4; ++index)
        {
            read(20 + index, cr[index], &output.cr[index], 8);
        }
        constexpr std::array dr = {x86_register::dr0, x86_register::dr1, x86_register::dr2,
                                   x86_register::dr3, x86_register::dr6, x86_register::dr7};
        for (uint32_t index = 0; index < 6; ++index)
        {
            read(24 + index, dr[index], &output.dr[index], 8);
        }
        constexpr std::array segments = {x86_register::cs, x86_register::ss, x86_register::ds,
                                         x86_register::es, x86_register::fs, x86_register::gs};
        for (uint32_t index = 0; index < 6; ++index)
        {
            read(30 + index, segments[index], &output.segments[index], 2);
        }
        read(36, x86_register::mxcsr, &output.mxcsr, 4);
        read(37, x86_register::msr, &output.efer, 8);
        for (uint32_t index = 0; index < 16; ++index)
        {
            read(38 + index, static_cast<x86_register>(static_cast<int>(x86_register::xmm0) + index), output.xmm[index].data(), 16);
        }
        for (uint32_t index = 0; index < 8; ++index)
        {
            read(54 + index, static_cast<x86_register>(static_cast<int>(x86_register::st0) + index), output.st[index].data(), 10);
        }
        constexpr std::array control = {x86_register::fpcw, x86_register::fpsw, x86_register::fptag};
        for (uint32_t index = 0; index < 3; ++index)
        {
            read(62 + index, control[index], &output.fp_control[index], 2);
        }
        constexpr std::array address = {x86_register::fcs, x86_register::fip, x86_register::fds, x86_register::fdp, x86_register::fop};
        for (uint32_t index = 0; index < 5; ++index)
        {
            read(65 + index, address[index], &output.fp_address[index], 4);
        }
        for (uint32_t index = 0; index < 8; ++index)
        {
            read(70 + index, static_cast<x86_register>(static_cast<int>(x86_register::mm0) + index), &output.mmx[index], 8);
        }
    }

    template <typename Reader>
    void inspection_stack(memory_manager& memory, Reader&& reader, const uint32_t words, inspection_context& output)
    {
        const auto capped_words = (std::min)(words, uint32_t{128});
        output.stack_requested_words = capped_words;
        if (!words)
        {
            return;
        }
        output.stack_status = 2;
        if (!(output.available[0] & (uint64_t{1} << 7)))
        {
            return;
        }
        output.stack_base = output.gpr[7];
        uint32_t successes = 0;
        for (uint32_t index = 0; index < capped_words; ++index)
        {
            const auto offset = uint64_t{index} * 8;
            if (output.stack_base > UINT64_MAX - offset)
            {
                continue;
            }
            const auto address = output.stack_base + offset;
            uint64_t value{};
            if (inspection_passive_span(memory, address, sizeof(value)) && reader(address, &value, sizeof(value)))
            {
                output.stack[index] = value;
                output.stack_success[index / 64] |= uint64_t{1} << (index % 64);
                ++successes;
            }
        }
        if (successes == capped_words)
        {
            output.stack_status = 0;
        }
        else if (successes)
        {
            output.stack_status = 1;
        }
    }
}
