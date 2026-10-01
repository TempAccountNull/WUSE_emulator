#pragma once

#include "native_marker_resources.hpp"
#include <bit>
#include <limits>

namespace sogen::detail
{
    template <size_t Size>
    struct native_marker_pool_field
    {
        uint64_t address{};
        bool address_available{};
        native_marker_raw_value<Size> raw{};
    };

    struct native_marker_pool_node_bytes
    {
        native_marker_pool_field<56> descriptor{};
        native_marker_pool_field<56> allocator{};
        native_marker_pool_field<32> inner{};
        native_marker_pool_field<4> counter{};
        native_marker_pool_field<4> free_link{};
        native_marker_pool_field<8> encoded{};
        native_marker_pool_field<16> header{};
    };

    enum class native_marker_pool_reason : uint8_t
    {
        not_selected,
        available,
        kernel_unavailable,
        invalid_image,
        resource_unavailable,
        invalid_handle,
        resolver_unavailable,
        resolver_mismatch,
        directory_unavailable,
        node_unavailable,
        invalid_geometry,
        unsupported_lifetime_mode,
        generation_mismatch,
        released,
        free_slot,
        continuity_unavailable,
        continuity_changed,
        child_unavailable,
        traversal_required,
        cycle,
        time_or_clock_limit,
        capture_rejected
    };

    struct native_marker_pool_node
    {
        std::optional<uint32_t> handle{};
        uint32_t group{};
        uint32_t index{};
        native_marker_pool_node_bytes initial{};
        native_marker_pool_node_bytes repeat{};
        std::optional<uint32_t> counter{};
        std::optional<uint32_t> counter_mask{};
        std::optional<uint32_t> config{};
        std::optional<uint32_t> reconstructed_handle{};
        std::optional<bool> counter_within_mask{};
        std::optional<bool> generation_bits_match{};
        std::optional<bool> released{};
        std::optional<bool> free_tag{};
        std::optional<uint16_t> bitmap_range_start{};
        std::optional<uint16_t> bitmap_range_end{};
        std::optional<bool> linked_mode{};
        std::optional<uint8_t> type{};
        std::optional<int8_t> stored_status{};
        std::optional<int8_t> stored_detail{};
        std::optional<uint32_t> related_handle{};
        bool repeat_complete{};
        bool repeat_equal{};
        bool best_effort_current_owner{};
        native_marker_pool_reason reason{native_marker_pool_reason::not_selected};
    };

    struct native_marker_pool_sample
    {
        static constexpr uint32_t max_nodes = 4;
        static constexpr uint32_t max_reads = 62;
        static constexpr uint32_t max_bytes = 1521;
        static constexpr bool atomic = false;
        static constexpr bool strong_lifetime = false;
        bool selected{};
        native_marker_pool_field<69> resolver{};
        bool resolver_matched{};
        native_marker_pool_field<8> directory{};
        native_marker_pool_field<8> table{};
        native_marker_pool_field<8> directory_repeat{};
        native_marker_pool_field<8> table_repeat{};
        native_marker_raw_value<12> resource3_initial{};
        native_marker_pool_field<12> resource3_repeat{};
        std::array<native_marker_pool_node, max_nodes> nodes{};
        uint32_t visited_nodes{};
        native_marker_pool_reason traversal_reason{native_marker_pool_reason::not_selected};
        bool roots_repeat_complete{};
        bool roots_repeat_equal{};
        std::optional<int8_t> conditional_stored_status{};
        std::optional<int8_t> conditional_stored_detail{};
        std::optional<uint32_t> status_node{};
        std::optional<uint32_t> detail_node{};
        native_marker_pool_reason status_reason{native_marker_pool_reason::not_selected};
        native_marker_pool_reason detail_reason{native_marker_pool_reason::not_selected};
        native_marker_pool_reason reason{native_marker_pool_reason::not_selected};
        uint32_t reads{};
        uint32_t bytes{};
    };

    inline constexpr std::array<uint8_t, 69> native_marker_pool_resolver_bytes{
        0x41, 0x8B, 0xC0, 0x41, 0x81, 0xE0, 0xFF, 0x1F, 0x00, 0x00, 0xC1, 0xF8, 0x0D, 0x8B, 0xD0, 0x48, 0x81, 0xCA,
        0x00, 0x00, 0xFC, 0x0F, 0x0F, 0xB7, 0xC0, 0x48, 0xC1, 0xEA, 0x12, 0x48, 0x23, 0xD0, 0x48, 0x8B, 0x05, 0xE0,
        0xD5, 0x00, 0x02, 0x48, 0xC1, 0xE2, 0x06, 0x48, 0x03, 0x10, 0x44, 0x0F, 0xAF, 0x42, 0x30, 0x48, 0x63, 0x42,
        0x34, 0x41, 0x8B, 0xC8, 0x48, 0x03, 0x4A, 0x08, 0x48, 0x23, 0x41, 0x08, 0x48, 0x2B, 0xC8};

    inline void native_marker_pool_unqualify(native_marker_pool_sample& sample, const native_marker_pool_reason reason)
    {
        sample.reason = reason;
        sample.conditional_stored_status.reset();
        sample.conditional_stored_detail.reset();
        sample.status_node.reset();
        sample.detail_node.reset();
        sample.status_reason = reason;
        sample.detail_reason = reason;
        for (auto& node : sample.nodes)
        {
            node.best_effort_current_owner = false;
        }
    }

    inline std::optional<uint64_t> native_marker_pool_add(const uint64_t first, const uint64_t second)
    {
        if (second > (std::numeric_limits<uint64_t>::max)() - first)
        {
            return std::nullopt;
        }
        return first + second;
    }

    inline uint32_t native_marker_pool_group(const uint32_t handle)
    {
        const auto shifted = (handle >> 13) | ((handle & 0x80000000U) ? 0xfff80000U : 0U);
        return static_cast<uint32_t>((shifted & 0xffffU) & ((uint64_t{shifted} | 0x0ffc0000ULL) >> 18));
    }

    inline uint32_t native_marker_pool_handle(const uint32_t counter, const uint32_t config, const uint32_t index)
    {
        if (!(config & 0x40000000U))
        {
            return (((config & 0x3ffU) | ((counter & 0xffU) << 10)) << 13) | (index & 0x1fffU);
        }
        return (((((counter & 7U) | 0xfffffff0U) << 14) | (config & 0x3fffU)) << 13) | (index & 0x1fffU);
    }

    template <size_t Size>
    bool native_marker_pool_complete(const native_marker_pool_field<Size>& field)
    {
        return field.address_available && field.raw.status == native_marker_read_status::complete && field.raw.bytes.has_value();
    }

    template <size_t Size>
    bool native_marker_pool_equal(const native_marker_pool_field<Size>& first, const native_marker_pool_field<Size>& repeat)
    {
        return native_marker_pool_complete(first) && native_marker_pool_complete(repeat) && first.address == repeat.address &&
               first.raw.bytes == repeat.raw.bytes;
    }

    inline bool native_marker_pool_complete(const native_marker_pool_node_bytes& node, const bool require_link = true)
    {
        return native_marker_pool_complete(node.descriptor) && native_marker_pool_complete(node.allocator) &&
               native_marker_pool_complete(node.inner) && native_marker_pool_complete(node.counter) &&
               (!require_link || native_marker_pool_complete(node.free_link)) && native_marker_pool_complete(node.encoded) &&
               native_marker_pool_complete(node.header);
    }

    inline bool native_marker_pool_equal(const native_marker_pool_node_bytes& first, const native_marker_pool_node_bytes& repeat,
                                         const bool require_link = true)
    {
        return native_marker_pool_equal(first.descriptor, repeat.descriptor) &&
               native_marker_pool_equal(first.allocator, repeat.allocator) && native_marker_pool_equal(first.inner, repeat.inner) &&
               native_marker_pool_equal(first.counter, repeat.counter) &&
               (!require_link || native_marker_pool_equal(first.free_link, repeat.free_link)) &&
               native_marker_pool_equal(first.encoded, repeat.encoded) && native_marker_pool_equal(first.header, repeat.header);
    }

    template <typename Kernel, typename PassiveSpan, typename Reader, typename Clock>
    native_marker_pool_sample sample_native_marker_request_pool(const uint64_t base, const uint64_t image_size,
                                                                const native_marker_resource_sample& resources, Kernel& kernel,
                                                                native_marker_read_budget& budget, PassiveSpan&& passive_span,
                                                                Reader&& reader, Clock&& clock)
    {
        native_marker_pool_sample output{};
        output.selected = true;
        output.resource3_initial = resources.resource3;
        const auto finish = [&](const native_marker_pool_reason reason) {
            output.reason = reason;
            try
            {
                budget.expired(clock());
            }
            catch (...)
            {
                budget.clock_invalid = true;
            }
            if (budget.time_exhausted || budget.clock_invalid)
            {
                native_marker_pool_unqualify(output, native_marker_pool_reason::time_or_clock_limit);
            }
            return output;
        };
        try
        {
            if (!kernel.is_held_by_current_thread())
            {
                return finish(native_marker_pool_reason::kernel_unavailable);
            }
        }
        catch (...)
        {
            return finish(native_marker_pool_reason::kernel_unavailable);
        }
        constexpr uint64_t user_end = 0x7fffffff0000ULL;
        if (base < 0x10000 || base >= user_end || !image_size || image_size > user_end - base)
        {
            return finish(native_marker_pool_reason::invalid_image);
        }
        const auto slot = native_marker_decode_slot(resources.resource3);
        if (!slot)
        {
            return finish(native_marker_pool_reason::resource_unavailable);
        }
        output.nodes[0].handle = slot->request_handle;
        if (slot->request_handle == 0xffffffffU)
        {
            return finish(native_marker_pool_reason::invalid_handle);
        }
        const auto read = [&]<size_t Size>(const std::optional<uint64_t> address, native_marker_pool_field<Size>& field) {
            if (!address)
            {
                field.raw.status = native_marker_read_status::invalid_address;
                return;
            }
            field.address = *address;
            field.address_available = true;
            if (output.reads >= native_marker_pool_sample::max_reads)
            {
                field.raw.status = native_marker_read_status::read_limit;
                return;
            }
            if (output.bytes > native_marker_pool_sample::max_bytes || Size > native_marker_pool_sample::max_bytes - output.bytes)
            {
                field.raw.status = native_marker_read_status::byte_limit;
                return;
            }
            const auto reads_before = budget.reads;
            const auto bytes_before = budget.bytes;
            field.raw = native_marker_try_read<Size>(*address, budget, passive_span, reader, clock);
            output.reads += budget.reads - reads_before;
            output.bytes += budget.bytes - bytes_before;
        };
        const auto module_read = [&]<size_t Size>(const uint64_t rva, native_marker_pool_field<Size>& field) {
            if (rva > image_size || Size > image_size - rva)
            {
                field.raw.status = native_marker_read_status::invalid_image;
                return;
            }
            read(base + rva, field);
        };
        module_read(0x42C669, output.resolver);
        if (!native_marker_pool_complete(output.resolver))
        {
            return finish(native_marker_pool_reason::resolver_unavailable);
        }
        output.resolver_matched = *output.resolver.raw.bytes == native_marker_pool_resolver_bytes;
        if (!output.resolver_matched)
        {
            return finish(native_marker_pool_reason::resolver_mismatch);
        }
        module_read(0x2439C70, output.directory);
        if (!native_marker_pool_complete(output.directory))
        {
            return finish(native_marker_pool_reason::directory_unavailable);
        }
        read(native_marker_decode_integer<uint64_t>(output.directory.raw), output.table);
        const auto table = native_marker_decode_integer<uint64_t>(output.table.raw);
        if (!table)
        {
            return finish(native_marker_pool_reason::directory_unavailable);
        }
        const auto collect = [&](native_marker_pool_node& node) {
            node.group = native_marker_pool_group(*node.handle);
            node.index = native_marker_handle_index(*node.handle);
            auto& raw = node.initial;
            node.reason = native_marker_pool_reason::node_unavailable;
            read(native_marker_pool_add(*table, uint64_t{node.group} * 0x40), raw.descriptor);
            if (!native_marker_pool_complete(raw.descriptor))
            {
                return;
            }
            const auto& descriptor = *raw.descriptor.raw.bytes;
            const auto data = native_marker_little_integer<uint64_t>(descriptor, 8);
            const auto allocator = native_marker_little_integer<uint64_t>(descriptor, 0x10);
            const auto stride = native_marker_little_integer<uint32_t>(descriptor, 0x30);
            const auto mask = native_marker_little_integer<uint32_t>(descriptor, 0x34);
            const auto slot_address = native_marker_pool_add(data, uint32_t{node.index * stride});
            if (!slot_address)
            {
                node.reason = native_marker_pool_reason::invalid_geometry;
                return;
            }
            read(allocator, raw.allocator);
            if (native_marker_pool_complete(raw.allocator))
            {
                const auto& allocation = *raw.allocator.raw.bytes;
                read(native_marker_little_integer<uint64_t>(allocation), raw.inner);
                const auto storage = native_marker_little_integer<uint64_t>(allocation, 8);
                const auto offset = native_marker_little_integer<uint32_t>(allocation, 0x1c);
                const auto counter_stride = native_marker_little_integer<uint32_t>(allocation, 0x20);
                const auto counter_address = native_marker_pool_add(storage, uint64_t{node.index} * counter_stride);
                read(counter_address ? native_marker_pool_add(*counter_address, offset) : std::nullopt, raw.counter);
                node.counter = native_marker_decode_integer<uint32_t>(raw.counter.raw);
                node.counter_mask = native_marker_little_integer<uint32_t>(allocation, 0x24);
                node.config = native_marker_little_integer<uint32_t>(allocation, 0x34);
                if (node.counter)
                {
                    node.reconstructed_handle = native_marker_pool_handle(*node.counter, *node.config, node.index);
                    node.counter_within_mask = (*node.counter & *node.counter_mask) == *node.counter;
                    node.generation_bits_match = *node.reconstructed_handle == *node.handle;
                }
                if (native_marker_pool_complete(raw.inner))
                {
                    const auto& inner = *raw.inner.raw.bytes;
                    node.bitmap_range_start = native_marker_little_integer<uint16_t>(inner, 0x1a);
                    node.bitmap_range_end = native_marker_little_integer<uint16_t>(inner, 0x1c);
                    node.linked_mode = node.index < *node.bitmap_range_start || node.index >= *node.bitmap_range_end;
                    const auto inner_data = native_marker_little_integer<uint64_t>(inner);
                    const auto inner_stride = native_marker_little_integer<uint32_t>(inner, 0x10);
                    const auto metadata_offset = native_marker_little_integer<uint32_t>(inner, 0x14);
                    const auto link = native_marker_pool_add(inner_data, uint32_t{node.index * inner_stride});
                    if (*node.linked_mode)
                    {
                        read(link ? native_marker_pool_add(*link, metadata_offset) : std::nullopt, raw.free_link);
                        if (native_marker_pool_complete(raw.free_link))
                        {
                            node.free_tag = native_marker_little_integer<uint16_t>(*raw.free_link.raw.bytes, 2) == 0xfefeU;
                        }
                    }
                    else
                    {
                        raw.free_link.raw.status = native_marker_read_status::dependency_unavailable;
                    }
                }
            }
            read(native_marker_pool_add(*slot_address, 8), raw.encoded);
            const auto encoded = native_marker_decode_integer<uint64_t>(raw.encoded.raw);
            if (encoded)
            {
                const auto wide_mask = (mask & 0x80000000U) ? (0xffffffff00000000ULL | mask) : uint64_t{mask};
                const auto displacement = *encoded & wide_mask;
                if (displacement > *slot_address)
                {
                    node.reason = native_marker_pool_reason::invalid_geometry;
                    return;
                }
                read(*slot_address - displacement, raw.header);
            }
            if (!native_marker_pool_complete(raw.header))
            {
                return;
            }
            const auto& header = *raw.header.raw.bytes;
            node.type = header[6];
            node.stored_status = std::bit_cast<int8_t>(header[7]);
            node.stored_detail = std::bit_cast<int8_t>(header[0xa]);
            node.related_handle = native_marker_little_integer<uint32_t>(header, 0xc);
            node.released = (header[8] & 1U) != 0;
            if (!node.linked_mode || !native_marker_pool_complete(raw, *node.linked_mode) || !node.generation_bits_match)
            {
                return;
            }
            node.reason = !*node.linked_mode             ? native_marker_pool_reason::unsupported_lifetime_mode
                          : !*node.counter_within_mask   ? native_marker_pool_reason::invalid_geometry
                          : !*node.generation_bits_match ? native_marker_pool_reason::generation_mismatch
                          : *node.released               ? native_marker_pool_reason::released
                          : *node.free_tag               ? native_marker_pool_reason::free_slot
                                                         : native_marker_pool_reason::available;
        };
        for (uint32_t index = 0; index < native_marker_pool_sample::max_nodes; ++index)
        {
            auto& node = output.nodes[index];
            ++output.visited_nodes;
            collect(node);
            if (node.reason != native_marker_pool_reason::available)
            {
                output.traversal_reason = node.reason;
                break;
            }
            if (node.type != uint8_t{2} || *node.related_handle == 0xffffffffU)
            {
                output.traversal_reason = native_marker_pool_reason::available;
                break;
            }
            bool cycle{};
            for (uint32_t previous = 0; previous <= index; ++previous)
            {
                cycle = cycle || output.nodes[previous].handle == node.related_handle;
            }
            if (cycle)
            {
                output.traversal_reason = native_marker_pool_reason::cycle;
                break;
            }
            if (index + 1 == native_marker_pool_sample::max_nodes)
            {
                output.traversal_reason = native_marker_pool_reason::traversal_required;
                break;
            }
            output.nodes[index + 1].handle = node.related_handle;
        }
        const auto repeat = [&](native_marker_pool_node& node) {
            if (!node.handle || !node.linked_mode || !native_marker_pool_complete(node.initial, *node.linked_mode))
            {
                return;
            }
            const auto reread = [&]<size_t Size>(const native_marker_pool_field<Size>& initial, native_marker_pool_field<Size>& final) {
                read(initial.address, final);
            };
            reread(node.initial.header, node.repeat.header);
            reread(node.initial.encoded, node.repeat.encoded);
            if (*node.linked_mode)
            {
                reread(node.initial.free_link, node.repeat.free_link);
            }
            reread(node.initial.counter, node.repeat.counter);
            reread(node.initial.inner, node.repeat.inner);
            reread(node.initial.allocator, node.repeat.allocator);
            reread(node.initial.descriptor, node.repeat.descriptor);
            node.repeat_complete = native_marker_pool_complete(node.repeat, *node.linked_mode);
            node.repeat_equal = native_marker_pool_equal(node.initial, node.repeat, *node.linked_mode);
            if (node.reason == native_marker_pool_reason::available)
            {
                node.reason = !node.repeat_complete ? native_marker_pool_reason::continuity_unavailable
                              : !node.repeat_equal  ? native_marker_pool_reason::continuity_changed
                                                    : native_marker_pool_reason::available;
            }
        };
        for (uint32_t remaining = output.visited_nodes; remaining != 0; --remaining)
        {
            repeat(output.nodes[remaining - 1]);
        }
        read(output.table.address, output.table_repeat);
        module_read(0x2439C70, output.directory_repeat);
        module_read(0x1FB5F44, output.resource3_repeat);
        output.roots_repeat_complete = native_marker_pool_complete(output.directory_repeat) &&
                                       native_marker_pool_complete(output.table_repeat) &&
                                       native_marker_pool_complete(output.resource3_repeat);
        output.roots_repeat_equal = output.roots_repeat_complete && native_marker_pool_equal(output.directory, output.directory_repeat) &&
                                    native_marker_pool_equal(output.table, output.table_repeat) &&
                                    output.resource3_initial.bytes == output.resource3_repeat.raw.bytes;
        const auto qualify = [&](native_marker_pool_node& node) {
            node.best_effort_current_owner = node.reason == native_marker_pool_reason::available && output.roots_repeat_equal;
        };
        for (uint32_t index = 0; index < output.visited_nodes; ++index)
        {
            qualify(output.nodes[index]);
        }
        if (!output.roots_repeat_complete)
        {
            return finish(native_marker_pool_reason::continuity_unavailable);
        }
        if (!output.roots_repeat_equal)
        {
            return finish(native_marker_pool_reason::continuity_changed);
        }
        if (!output.nodes[0].best_effort_current_owner)
        {
            return finish(output.nodes[0].reason);
        }
        if (output.traversal_reason == native_marker_pool_reason::cycle ||
            output.traversal_reason == native_marker_pool_reason::traversal_required)
        {
            output.status_reason = output.traversal_reason;
            output.detail_reason = output.traversal_reason;
            return finish(output.traversal_reason);
        }
        const auto calculate = [&](const bool detail) {
            auto& value = detail ? output.conditional_stored_detail : output.conditional_stored_status;
            auto& source = detail ? output.detail_node : output.status_node;
            auto& reason = detail ? output.detail_reason : output.status_reason;
            for (uint32_t index = 0; index < output.visited_nodes; ++index)
            {
                const auto& node = output.nodes[index];
                if (!node.best_effort_current_owner)
                {
                    reason = node.reason;
                    return;
                }
                const bool follows =
                    node.type == uint8_t{2} && *node.related_handle != 0xffffffffU && (detail || node.stored_status == int8_t{1});
                if (!follows)
                {
                    value = detail ? node.stored_detail : node.stored_status;
                    source = index;
                    reason = native_marker_pool_reason::available;
                    return;
                }
            }
            reason = output.traversal_reason;
        };
        calculate(false);
        calculate(true);
        for (uint32_t index = 0; index < output.visited_nodes; ++index)
        {
            if (output.nodes[index].reason != native_marker_pool_reason::available)
            {
                return finish(output.nodes[index].reason);
            }
        }
        if (output.traversal_reason != native_marker_pool_reason::available)
        {
            return finish(output.traversal_reason);
        }
        return finish(native_marker_pool_reason::available);
    }
}
