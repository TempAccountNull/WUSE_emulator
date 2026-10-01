#pragma once

#include "native_marker_snapshot.hpp"
#include <bit>

namespace sogen::detail
{
    enum class native_child_kind : uint8_t
    {
        selection,
        activation,
        post_poll,
        pre_release,
        coverage,
        removed
    };

    enum class native_child_reason : uint8_t
    {
        available,
        selection_unavailable,
        profile_conflict,
        identity_changed,
        guard_unavailable,
        guard_mismatch,
        install_failure,
        lease_expired,
        clock_invalid,
        trap_cap,
        attempt_cap,
        collection_cap,
        callback_cap,
        read_unavailable,
        kernel_unavailable,
        link_mismatch,
        native_view_mismatch,
        continuity_changed,
        first_qualified_poll,
        first_selected_release,
        output_failure,
        module_unload,
        shutdown,
        removed
    };

    struct native_child_limits
    {
        static constexpr uint64_t poll_lease_ns = 2000000000;
        static constexpr uint64_t release_lease_ns = 120000000000;
        static constexpr uint64_t collection_ns = 8000000;
        static constexpr uint64_t callback_ns = 50000000;
        static constexpr uint64_t removal_acceptance_ns = 200000000;
        static constexpr uint32_t detail_records = 8;
        static constexpr uint32_t lifecycle_records = 16;
        static constexpr size_t detail_bytes = 48 * 1024;
        static constexpr size_t lifecycle_bytes = 16 * 1024;
        static constexpr std::array<uint64_t, 2> rvas{0x43C5E1, 0xB440BF};
        static constexpr std::array<uint64_t, 2> trap_caps{64, 16};
        static constexpr std::array<uint32_t, 2> read_caps{39, 35};
        static constexpr std::array<uint32_t, 2> byte_caps{841, 829};
        static constexpr uint32_t attempts_per_site = 4;
    };

    struct native_child_selection
    {
        native_marker_identity identity{};
        uint64_t marker_sequence{};
        uint64_t selected_ns{};
        uint32_t parent_handle{};
        uint32_t child_handle{};
        uint32_t tag{};
        uint64_t parent_view{};
        uint64_t child_view{};
        std::array<uint32_t, 2> counters{};
        std::array<uint32_t, 2> configs{};
    };

    inline bool native_child_chain(const native_marker_pool_sample& pool)
    {
        if (pool.reason != native_marker_pool_reason::available || pool.visited_nodes != 2 || !pool.roots_repeat_equal)
        {
            return false;
        }
        const auto& parent = pool.nodes[0];
        const auto& child = pool.nodes[1];
        return parent.best_effort_current_owner && child.best_effort_current_owner && parent.type == uint8_t{2} &&
               child.type == uint8_t{3} && parent.handle && child.handle && parent.related_handle == child.handle &&
               child.related_handle == parent.handle && parent.counter && child.counter && parent.config && child.config &&
               native_marker_pool_complete(parent.initial.header) && native_marker_pool_complete(child.initial.header);
    }

    inline std::optional<native_child_selection> select_native_child(const native_marker_snapshot& marker)
    {
        if (!marker.complete || !marker.core_complete || marker.reservation.event != native_marker_event::cleanup_entry ||
            !marker.identity.valid() || !marker.request_pool || !native_child_chain(*marker.request_pool))
        {
            return std::nullopt;
        }
        const auto slot = native_marker_decode_slot(marker.resources.resource3);
        if (!slot || slot->request_handle != marker.request_pool->nodes[0].handle)
        {
            return std::nullopt;
        }
        const auto& parent = marker.request_pool->nodes[0];
        const auto& child = marker.request_pool->nodes[1];
        return native_child_selection{.identity = marker.identity,
                                      .marker_sequence = marker.reservation.sequence,
                                      .selected_ns = marker.reservation.sample_steady_ns,
                                      .parent_handle = *parent.handle,
                                      .child_handle = *child.handle,
                                      .tag = slot->resource_handle,
                                      .parent_view = parent.initial.header.address,
                                      .child_view = child.initial.header.address,
                                      .counters = {*parent.counter, *child.counter},
                                      .configs = {*parent.config, *child.config}};
    }

    struct native_child_site_counts
    {
        uint64_t raw_traps{};
        uint64_t active_traps{};
        uint64_t retired_traps{};
        uint64_t filtered{};
        uint32_t attempts{};
        uint32_t accepted{};
        uint64_t installed_ns{};
        uint64_t retired_ns{};
        uint64_t removed_ns{};
        uint64_t removal_attempts{};
        uint64_t removal_deferrals{};
        native_child_reason retirement{native_child_reason::available};
    };

    struct native_child_sample
    {
        native_child_kind kind{native_child_kind::coverage};
        native_child_reason reason{native_child_reason::available};
        uint32_t site{UINT32_MAX};
        uint64_t sequence{};
        native_child_selection selection{};
        native_marker_register_snapshot registers{};
        uint64_t began_ns{};
        uint64_t ended_ns{};
        native_marker_read_budget budget{};
        native_marker_raw_value<12> resource3{};
        native_marker_pool_field<4> active_initial{};
        native_marker_pool_field<4> active_repeat{};
        native_marker_pool_field<2> context_initial{};
        native_marker_pool_field<2> context_repeat{};
        std::shared_ptr<const native_marker_pool_sample> pool{};
        std::optional<uint64_t> manager{};
        std::optional<uint64_t> context{};
        std::optional<uint16_t> context_word{};
        std::optional<uint32_t> raw_return{};
        std::optional<int32_t> effective_status{};
        std::optional<uint8_t> detail_return{};
        std::optional<uint8_t> slot_detail{};
        bool resource_identity{};
        bool child_qualified{};
        bool effective_terminal{};
        bool clock_accepted{};
        std::array<native_child_site_counts, 2> sites{};
        uint64_t collection_ns{};
        uint64_t callback_ns{};
        uint64_t output_bytes{};
        uint64_t output_rejections{};
        bool removal_accepted{true};
        std::array<native_marker_raw_value<69>, 1> resolver_guard{};
        native_marker_raw_value<30> poll_guard{};
        native_marker_raw_value<31> release_guard{};
    };

    inline constexpr std::array<uint8_t, 30> native_child_poll_guard{0x0F, 0xB7, 0x0B, 0xE8, 0x9F, 0xB7, 0xF1, 0xFF, 0x41, 0x8B,
                                                                     0x97, 0x68, 0x00, 0x22, 0x00, 0x44, 0x8B, 0xC0, 0x49, 0x8B,
                                                                     0xCF, 0x8B, 0xF0, 0xE8, 0xFB, 0xE7, 0xFE, 0xFF, 0x8B, 0xE8};
    inline constexpr std::array<uint8_t, 31> native_child_release_guard{0xE8, 0xA6, 0x85, 0x8E, 0xFF, 0x48, 0x8B, 0xCF, 0x8B, 0xD8, 0xE8,
                                                                        0xCC, 0x74, 0x8E, 0xFF, 0x41, 0x88, 0x47, 0x01, 0x48, 0x8B, 0xCE,
                                                                        0x41, 0x8B, 0x57, 0x04, 0xE8, 0x4C, 0x12, 0x8E, 0xFF};

    inline uint64_t native_child_add_count(const uint64_t total, const uint64_t value)
    {
        return value > UINT64_MAX - total ? UINT64_MAX : total + value;
    }

    inline bool native_child_prefilter(const native_child_selection& selected, const uint32_t site,
                                       const native_marker_register_snapshot& registers)
    {
        const auto& context = registers.context;
        constexpr uint64_t required = 0x3ffff;
        if (site >= 2 || (context.available[0] & required) != required || !registers.actual_tid ||
            context.rip != selected.identity.module_base + native_child_limits::rvas[site])
        {
            return false;
        }
        if (!site)
        {
            return context.gpr[5] == selected.child_view;
        }
        return context.gpr[6] == 3 && context.gpr[14] == selected.identity.module_base + 0x1FB5F20 &&
               context.gpr[15] == selected.identity.module_base + 0x1FB5F44 &&
               static_cast<uint32_t>(context.gpr[3]) == selected.parent_handle && context.gpr[5] == selected.parent_view;
    }

    template <typename Kernel, typename PassiveSpan, typename Reader, typename Clock>
    native_child_sample sample_native_child(const native_child_selection& selected, const uint32_t site,
                                            const native_marker_register_snapshot& registers, Kernel& kernel, PassiveSpan&& passive_span,
                                            Reader&& reader, Clock&& clock, const uint64_t began_ns = 0)
    {
        native_child_sample output{};
        output.kind = site ? native_child_kind::pre_release : native_child_kind::post_poll;
        output.site = site;
        output.selection = selected;
        output.registers = registers;
        output.reason = native_child_reason::read_unavailable;
        try
        {
            output.began_ns = began_ns ? began_ns : clock();
        }
        catch (...)
        {
            output.budget.clock_invalid = true;
            output.reason = native_child_reason::clock_invalid;
            return output;
        }
        output.budget.started_ns = output.began_ns;
        if (!native_child_prefilter(selected, site, registers))
        {
            output.reason = native_child_reason::native_view_mismatch;
            output.ended_ns = output.began_ns;
            return output;
        }
        try
        {
            if (!kernel.is_held_by_current_thread())
            {
                output.reason = native_child_reason::kernel_unavailable;
                output.ended_ns = output.began_ns;
                return output;
            }
        }
        catch (...)
        {
            output.reason = native_child_reason::kernel_unavailable;
            output.ended_ns = output.began_ns;
            return output;
        }
        output.budget.read_limit = native_child_limits::read_caps[site];
        output.budget.byte_limit = native_child_limits::byte_caps[site];
        const auto& gpr = registers.context.gpr;
        if (!site)
        {
            output.raw_return = static_cast<uint32_t>(gpr[0]);
            output.manager = gpr[15];
            output.context = gpr[1];
        }
        else
        {
            output.effective_status = std::bit_cast<int32_t>(static_cast<uint32_t>(gpr[1]));
            output.detail_return = static_cast<uint8_t>(gpr[0]);
            output.effective_terminal = *output.effective_status > 1;
            output.manager = gpr[2];
        }
        const auto finish = [&] {
            try
            {
                output.ended_ns = clock();
            }
            catch (...)
            {
                output.ended_ns = output.began_ns;
                output.budget.clock_invalid = true;
            }
            output.clock_accepted = !output.budget.expired(output.ended_ns);
            if (!output.clock_accepted)
            {
                output.child_qualified = false;
                output.resource_identity = false;
                output.context_word.reset();
                output.reason = output.budget.clock_invalid ? native_child_reason::clock_invalid : native_child_reason::collection_cap;
                if (output.pool)
                {
                    try
                    {
                        auto changed = std::make_shared<native_marker_pool_sample>(*output.pool);
                        native_marker_pool_unqualify(*changed, native_marker_pool_reason::time_or_clock_limit);
                        output.pool = std::move(changed);
                    }
                    catch (...)
                    {
                        output.pool.reset();
                    }
                }
            }
            return output;
        };
        const auto read = [&]<size_t Size>(const std::optional<uint64_t> address, native_marker_pool_field<Size>& field) {
            if (!address)
            {
                field.raw.status = native_marker_read_status::invalid_address;
                return;
            }
            field.address = *address;
            field.address_available = true;
            field.raw = native_marker_try_read<Size>(*address, output.budget, passive_span, reader, clock);
        };
        if (selected.identity.module_size < 0x1FB5F50)
        {
            output.reason = native_child_reason::identity_changed;
            return finish();
        }
        output.resource3 =
            native_marker_try_read<12>(selected.identity.module_base + 0x1FB5F44, output.budget, passive_span, reader, clock);
        const auto slot = native_marker_decode_slot(output.resource3);
        if (!slot || slot->request_handle != selected.parent_handle || slot->resource_handle != selected.tag)
        {
            return finish();
        }
        if (!site)
        {
            read(native_marker_pool_add(*output.manager, 0x220068), output.active_initial);
            read(output.context, output.context_initial);
            if (native_marker_decode_integer<uint32_t>(output.active_initial.raw) != selected.child_handle ||
                !native_marker_pool_complete(output.context_initial) ||
                native_marker_decode_integer<uint16_t>(output.context_initial.raw) == uint16_t{0xffff})
            {
                return finish();
            }
        }
        output.resource_identity = site != 0;
        output.slot_detail = slot->detail;
        native_marker_resource_sample resources{};
        resources.resource3 = output.resource3;
        auto pool = std::make_shared<native_marker_pool_sample>(
            sample_native_marker_request_pool(selected.identity.module_base, selected.identity.module_size, resources, kernel,
                                              output.budget, passive_span, reader, clock, 2));
        output.pool = pool;
        if (!site)
        {
            read(output.active_initial.address_available ? std::optional{output.active_initial.address} : std::nullopt,
                 output.active_repeat);
            read(output.context_initial.address_available ? std::optional{output.context_initial.address} : std::nullopt,
                 output.context_repeat);
            if (!native_marker_pool_equal(output.active_initial, output.active_repeat) ||
                !native_marker_pool_equal(output.context_initial, output.context_repeat))
            {
                output.reason = native_child_reason::continuity_changed;
                return finish();
            }
            output.context_word = native_marker_decode_integer<uint16_t>(output.context_initial.raw);
        }
        if (!native_child_chain(*pool))
        {
            output.reason = native_child_reason::link_mismatch;
            return finish();
        }
        const auto& parent = pool->nodes[0];
        const auto& child = pool->nodes[1];
        if (parent.handle != selected.parent_handle || child.handle != selected.child_handle ||
            parent.initial.header.address != selected.parent_view || child.initial.header.address != selected.child_view ||
            parent.counter != selected.counters[0] || child.counter != selected.counters[1] || parent.config != selected.configs[0] ||
            child.config != selected.configs[1])
        {
            output.reason = native_child_reason::identity_changed;
            return finish();
        }
        output.child_qualified = true;
        output.resource_identity = true;
        output.reason = native_child_reason::available;
        return finish();
    }
}
