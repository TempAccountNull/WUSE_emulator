#include "std_include.hpp"
#include "memory_manager.hpp"

#include "memory_region.hpp"
#include "address_utils.hpp"
#include "memory_permission_ext.hpp"

#include <vector>
#include <optional>
#include <stdexcept>
#include <cassert>
#include <random>

namespace sogen
{
    uint64_t memory_manager::next_aslr_random()
    {
        auto& value = this->aslr_random_state_;
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
        return value;
    }

    void memory_manager::initialize_aslr_policy(const bool high_entropy, const bool is_32bit, const bool deterministic)
    {
        this->aslr_policy_ = 1u | (high_entropy && !is_32bit ? 4u : 0u);
        this->aslr_random_state_ = 0x9e3779b97f4a7c15ULL;
        if (!deterministic)
        {
            std::random_device random{};
            this->aslr_random_state_ = (static_cast<uint64_t>(random()) << 32) ^ random();
        }
        if (!this->aslr_random_state_)
        {
            this->aslr_random_state_ = 1;
        }
        const auto mask = (this->aslr_policy_ & 4) ? 0xffffffULL : 0xffULL;
        const auto origin = is_32bit ? DEFAULT_ALLOCATION_ADDRESS_32BIT : DEFAULT_ALLOCATION_ADDRESS_64BIT;
        this->default_allocation_address_ = origin + ((this->next_aslr_random() & mask) + 1) * ALLOCATION_GRANULARITY;
    }

    NTSTATUS memory_manager::set_aslr_policy(const uint32_t flags)
    {
        if (flags & ~0xfu)
        {
            return STATUS_INVALID_PARAMETER;
        }
        if ((this->aslr_policy_ & 0xbu) & ~flags)
        {
            return STATUS_ACCESS_DENIED;
        }
        if ((flags & 8) && !(flags & 2))
        {
            return STATUS_INVALID_PARAMETER_MIX;
        }
        // High-entropy eligibility is fixed at process creation, not by this syscall.
        this->aslr_policy_ = (flags & 0xbu) | (this->aslr_policy_ & 4u);
        return STATUS_SUCCESS;
    }

    uint64_t memory_manager::find_randomized_image_base(const size_t size, const bool is_32bit)
    {
        const auto mask = (this->aslr_policy_ & 4) && !is_32bit ? 0xffffffULL : 0xffULL;
        const auto origin = is_32bit ? DEFAULT_ALLOCATION_ADDRESS_32BIT : DEFAULT_ALLOCATION_ADDRESS_64BIT;
        const auto start = origin + ((this->next_aslr_random() & mask) + 1) * ALLOCATION_GRANULARITY;
        return this->find_free_host_allocation_base(size, start, is_32bit ? UINT32_MAX : MAX_ALLOCATION_ADDRESS);
    }

    void memory_manager::serialize_aslr_state(utils::buffer_serializer& buffer) const
    {
        buffer.write<uint64_t>(0x31594c4f50524c41);
        buffer.write(this->aslr_policy_);
        buffer.write(this->aslr_random_state_);
        buffer.write<uint64_t>(0x3157454956444853);
        buffer.write_map(this->shared_views_);
    }

    void memory_manager::deserialize_aslr_state(utils::buffer_deserializer& buffer, const bool high_entropy, const bool is_32bit,
                                                const bool deterministic)
    {
        if (!buffer.get_remaining_size())
        {
            this->initialize_aslr_policy(high_entropy, is_32bit, deterministic);
            return;
        }
        if (buffer.read<uint64_t>() != 0x31594c4f50524c41)
        {
            throw std::runtime_error("Invalid ASLR snapshot extension");
        }
        buffer.read(this->aslr_policy_);
        buffer.read(this->aslr_random_state_);
        this->shared_views_.clear();
        if (buffer.get_remaining_size())
        {
            if (buffer.read<uint64_t>() != 0x3157454956444853)
            {
                throw std::runtime_error("Invalid shared view snapshot extension");
            }
            buffer.read_map(this->shared_views_);
            this->restore_shared_views();
        }
        if ((this->aslr_policy_ & ~0xfu) || ((this->aslr_policy_ & 8) && !(this->aslr_policy_ & 2)))
        {
            throw std::runtime_error("Invalid saved ASLR policy");
        }
    }

    namespace
    {
        void split_regions(memory_manager::committed_region_map& regions, const std::vector<uint64_t>& split_points)
        {
            for (auto i = regions.begin(); i != regions.end(); ++i)
            {
                for (const auto split_point : split_points)
                {
                    if (is_within_start_and_length(split_point, i->first, i->second.length) && i->first != split_point)
                    {
                        const auto first_length = split_point - i->first;
                        const auto second_length = i->second.length - first_length;

                        i->second.length = static_cast<size_t>(first_length);

                        regions[split_point] = memory_manager::committed_region{
                            .length = static_cast<size_t>(second_length),
                            .permissions = i->second.permissions,
                        };
                    }
                }
            }
        }

        void merge_regions(memory_manager::committed_region_map& regions)
        {
            for (auto i = regions.begin(); i != regions.end();)
            {
                assert(i->second.length > 0);

                auto next = i;
                std::advance(next, 1);

                if (next == regions.end())
                {
                    break;
                }

                assert(next->second.length > 0);

                const auto end = i->first + i->second.length;
                assert(end <= next->first);

                if (end != next->first || i->second.permissions != next->second.permissions)
                {
                    ++i;
                    continue;
                }

                i->second.length += next->second.length;
                regions.erase(next);
            }
        }
    }

    namespace utils
    {
        static void serialize(buffer_serializer& buffer, const memory_manager::committed_region& region)
        {
            buffer.write<uint64_t>(region.length);
            buffer.write(region.permissions);
        }

        static void deserialize(buffer_deserializer& buffer, memory_manager::committed_region& region)
        {
            region.length = static_cast<size_t>(buffer.read<uint64_t>());
            region.permissions = buffer.read<nt_memory_permission>();
        }

        static void serialize(buffer_serializer& buffer, const memory_manager::reserved_region& region)
        {
            // Host ownership is runtime state. Keep the existing region format and let the device restore its backing.
            buffer.write(region.kind);
            buffer.write(region.mapped_filename);
            buffer.write(region.initial_permission);
            buffer.write<uint64_t>(region.length);
            buffer.write_map(region.committed_regions);
        }

        static void deserialize(buffer_deserializer& buffer, memory_manager::reserved_region& region)
        {
            buffer.read(region.kind);
            buffer.read(region.mapped_filename);
            buffer.read(region.initial_permission);
            region.length = static_cast<size_t>(buffer.read<uint64_t>());
            buffer.read_map(region.committed_regions);
            region.host_backing.reset();
        }
    }

    void memory_manager::update_layout_version()
    {
#if SOGEN_REFLECTION_LEVEL > 0
        this->layout_version_.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    memory_stats memory_manager::compute_memory_stats() const
    {
        memory_stats stats{};
        stats.reserved_memory = 0;
        stats.committed_memory = 0;

        for (const auto& reserved_region : this->reserved_regions_ | std::views::values)
        {
            stats.reserved_memory += reserved_region.length;

            for (const auto& committed_region : reserved_region.committed_regions | std::views::values)
            {
                stats.committed_memory += committed_region.length;
            }
        }

        return stats;
    }

    memory_permission memory_manager::get_effective_permissions(const nt_memory_permission permissions) const
    {
        if (permissions.is_guarded())
        {
            return memory_permission::none;
        }

        auto effective = permissions.common;
        if (!this->dep_enabled_ && effective != memory_permission::none)
        {
            effective |= memory_permission::exec;
        }

        return effective;
    }

    void memory_manager::set_dep_enabled(const bool enabled)
    {
        if (this->dep_enabled_ == enabled)
        {
            return;
        }

        this->dep_enabled_ = enabled;
        for (const auto& reserved : this->reserved_regions_ | std::views::values)
        {
            if (reserved.kind == memory_region_kind::mmio)
            {
                continue;
            }

            for (const auto& [address, region] : reserved.committed_regions)
            {
                this->apply_memory_protection(address, region.length, this->get_effective_permissions(region.permissions));
            }
        }
    }

    void memory_manager::serialize_memory_state(utils::buffer_serializer& buffer, const bool is_snapshot) const
    {
        for (const auto& [address, region] : this->reserved_regions_)
        {
            if (region.host_backing && !region.host_backing->snapshot_reconstructible)
            {
                throw std::runtime_error("Cannot serialize host-backed memory without a reconstruction owner");
            }
        }
        buffer.write_atomic(this->layout_version_);
        buffer.write(this->default_allocation_address_);
        buffer.write(this->dep_enabled_);
        buffer.write_map(this->reserved_regions_);

        if (is_snapshot)
        {
            return;
        }

        for (const auto& reserved_region : this->reserved_regions_)
        {
            if (reserved_region.second.kind == memory_region_kind::mmio)
            {
                continue;
            }

            for (const auto& region : reserved_region.second.committed_regions)
            {
                buffer.write_chunked(region.second.length, [&](const size_t offset, const std::span<std::byte> chunk) {
                    this->read_memory(region.first + offset, chunk.data(), chunk.size());
                });
            }
        }
    }

    void memory_manager::deserialize_memory_state(utils::buffer_deserializer& buffer, const bool is_snapshot)
    {
        if (!is_snapshot)
        {
            assert(this->reserved_regions_.empty());
        }

        buffer.read_atomic(this->layout_version_);
        buffer.read(this->default_allocation_address_);
        buffer.read(this->dep_enabled_);
        buffer.read_map(this->reserved_regions_);

        if (is_snapshot)
        {
            return;
        }

        for (auto i = this->reserved_regions_.begin(); i != this->reserved_regions_.end();)
        {
            auto& reserved_region = i->second;
            if (reserved_region.kind == memory_region_kind::mmio)
            {
                i = this->reserved_regions_.erase(i);
                continue;
            }

            ++i;

            for (const auto& region : reserved_region.committed_regions)
            {
                const auto data = buffer.read_data(region.second.length);

                const auto effective_permission = this->get_effective_permissions(region.second.permissions);
                this->map_memory(region.first, region.second.length, effective_permission);
                this->write_memory(region.first, data.data(), region.second.length);
            }
        }
    }

    bool memory_manager::protect_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions,
                                        nt_memory_permission* old_permissions)
    {
        const auto entry = this->find_reserved_region(address);
        if (entry == this->reserved_regions_.end())
        {
            return false;
        }

        const auto end = address + size;
        const auto region_end = entry->first + entry->second.length;

        if (region_end < end)
        {
            throw std::runtime_error("Cross region protect not supported yet!");
        }

        std::optional<memory_permission> old_first_permissions{};

        auto& committed_regions = entry->second.committed_regions;
        split_regions(committed_regions, {address, end});

        const auto effective_permission = this->get_effective_permissions(permissions);

        for (auto& sub_region : committed_regions)
        {
            if (sub_region.first >= end)
            {
                break;
            }

            const auto sub_region_end = sub_region.first + sub_region.second.length;
            if (sub_region.first >= address && sub_region_end <= end)
            {
                if (!old_first_permissions.has_value())
                {
                    old_first_permissions = sub_region.second.permissions;
                }

                this->apply_memory_protection(sub_region.first, sub_region.second.length, effective_permission);
                sub_region.second.permissions = permissions;
            }
        }

        if (old_permissions)
        {
            *old_permissions = old_first_permissions.value_or(memory_permission::none);
        }

        merge_regions(committed_regions);

        this->update_layout_version();

        return true;
    }

    bool memory_manager::allocate_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb)
    {
        if (this->overlaps_reserved_region(address, size, true))
        {
            return false;
        }

        this->carve_host_reserved_hole(address, size);
        this->map_mmio(address, size, std::move(read_cb), std::move(write_cb));

        const auto entry = this->reserved_regions_
                               .try_emplace(address,
                                            reserved_region{
                                                .length = size,
                                                .kind = memory_region_kind::mmio,
                                            })
                               .first;

        entry->second.committed_regions[address] = committed_region{
            .length = size,
            .permissions = memory_permission::read_write,
        };

        this->update_layout_version();

        return true;
    }

    bool memory_manager::allocate_host_memory_at(const uint64_t address, const size_t size, void* host_pointer,
                                                 const nt_memory_permission permissions, std::shared_ptr<void> storage)
    {
        if (!size || !host_pointer || address > MAX_ALLOCATION_END_EXCL || size > MAX_ALLOCATION_END_EXCL - address ||
            this->overlaps_reserved_region(address, size))
        {
            return false;
        }

        reserved_region region{.length = size, .kind = memory_region_kind::mmio};
        region.host_backing = std::make_shared<host_memory_backing>();
        region.host_backing->storage = std::move(storage);
        region.committed_regions.emplace(address, committed_region{.length = size, .permissions = permissions});
        const auto entry = this->reserved_regions_.emplace(address, std::move(region)).first;
        try
        {
            this->map_host_memory(address, size, host_pointer, this->get_effective_permissions(permissions));
        }
        catch (...)
        {
            this->reserved_regions_.erase(entry);
            throw;
        }
        this->update_layout_version();
        return true;
    }

    memory_manager::host_memory_token memory_manager::host_memory_backing_at(const uint64_t address) const
    {
        auto region = this->reserved_regions_.upper_bound(address);
        if (region == this->reserved_regions_.begin())
        {
            return {};
        }
        --region;
        return address - region->first < region->second.length ? region->second.host_backing : nullptr;
    }

    void memory_manager::revoke_host_memory(const host_memory_token& backing)
    {
        const auto retained = backing;
        if (!retained)
        {
            return;
        }
        for (auto entry = this->reserved_regions_.begin(); entry != this->reserved_regions_.end();)
        {
            if (entry->second.host_backing == retained)
            {
                const auto address = entry->first;
                ++entry;
                this->release_memory(address, 0);
            }
            else
            {
                ++entry;
            }
        }
    }

    uint64_t memory_manager::allocate_host_memory(const size_t size, void* host_pointer, const nt_memory_permission permissions,
                                                  std::shared_ptr<void> storage)
    {
        if (size == 0 || host_pointer == nullptr)
        {
            return 0;
        }

        const bool uses_existing_host_mapping = this->memory_->host_memory_mapping_requires_identity();
        const uint64_t address =
            uses_existing_host_mapping ? get_untagged_pointer_address(host_pointer) : this->find_free_host_allocation_base(size, 0);

        if (address < MIN_ALLOCATION_ADDRESS || address >= MAX_ALLOCATION_END_EXCL || size > MAX_ALLOCATION_END_EXCL - address)
        {
            return 0;
        }

        if (this->overlaps_reserved_region(address, size, uses_existing_host_mapping))
        {
            return 0;
        }

        if (uses_existing_host_mapping)
        {
            this->carve_host_reserved_hole(address, size);
        }
        else
        {
            this->memory_->reserve_guest_address_range(address, size);
        }

        try
        {
            if (this->allocate_host_memory_at(address, size, host_pointer, permissions, std::move(storage)))
            {
                return address;
            }
        }
        catch (...)
        {
            if (!uses_existing_host_mapping)
            {
                this->release_host_claims(address + size);
            }
            throw;
        }
        if (!uses_existing_host_mapping)
        {
            this->release_host_claims(address + size);
        }
        return 0;
    }

    void memory_manager::reserve_host_memory_ranges()
    {
        for (const auto& range : this->memory_->reserved_host_ranges())
        {
            this->reserve_host_range_gaps(range.address, range.size);
        }
    }

    void memory_manager::reserve_host_memory_ranges_in(const uint64_t address, const size_t size)
    {
        for (const auto& range : this->memory_->reserved_host_ranges_in(address, size))
        {
            this->reserve_host_range_gaps(range.address, range.size);
        }
    }

    void memory_manager::reserve_host_range_gaps(const uint64_t address, const size_t size)
    {
        // A backend-reported range can partially overlap existing entries - its own earlier records,
        // guest allocations made since, or an MMIO hole carved by allocate_mmio - so each still
        // uncovered gap is recorded individually instead of failing the whole range. Tagged
        // host_reserved rather than private_allocation so allocate_mmio can still claim addresses in
        // here.
        const uint64_t end = address + size;
        uint64_t cursor = address;

        while (cursor < end)
        {
            const auto next = this->reserved_regions_.upper_bound(cursor);
            if (next != this->reserved_regions_.begin())
            {
                const auto& prev = *std::prev(next);
                const auto prev_end = prev.first + prev.second.length;
                if (prev_end > cursor)
                {
                    cursor = prev_end;
                    continue;
                }
            }

            const auto gap_end = next == this->reserved_regions_.end() ? end : std::min<uint64_t>(end, next->first);
            if (gap_end > cursor && this->allocate_memory_raw(cursor, static_cast<size_t>(gap_end - cursor), nt_memory_permission{}, true,
                                                              memory_region_kind::host_reserved))
            {
                this->host_reserved_addresses_.push_back(cursor);
            }

            cursor = gap_end;
        }
    }

    void memory_manager::carve_host_reserved_hole(const uint64_t address, const size_t size)
    {
        // allocate_mmio may claim addresses inside a host_reserved range (see
        // overlaps_reserved_region's ignore_host_reserved). Nesting the MMIO entry inside it would
        // break the pairwise-non-overlap invariant overlaps_reserved_region's fast path depends on,
        // so the surrounding host_reserved coverage is split around the hole instead.
        const uint64_t end = address + size;

        auto it = this->reserved_regions_.upper_bound(address);
        if (it != this->reserved_regions_.begin())
        {
            --it;
        }

        while (it != this->reserved_regions_.end() && it->first < end)
        {
            const auto region_start = it->first;
            const auto region_end = region_start + it->second.length;

            if (region_end <= address || it->second.kind != memory_region_kind::host_reserved)
            {
                ++it;
                continue;
            }

            assert(it->second.committed_regions.empty());
            const bool was_tracked = std::erase(this->host_reserved_addresses_, region_start) != 0;
            it = this->reserved_regions_.erase(it);

            if (region_start < address)
            {
                this->reserved_regions_.try_emplace(region_start, reserved_region{
                                                                      .length = static_cast<size_t>(address - region_start),
                                                                      .kind = memory_region_kind::host_reserved,
                                                                  });
                if (was_tracked)
                {
                    this->host_reserved_addresses_.push_back(region_start);
                }
            }

            if (region_end > end)
            {
                it = this->reserved_regions_
                         .try_emplace(end,
                                      reserved_region{
                                          .length = static_cast<size_t>(region_end - end),
                                          .kind = memory_region_kind::host_reserved,
                                      })
                         .first;
                if (was_tracked)
                {
                    this->host_reserved_addresses_.push_back(end);
                }
                ++it;
            }
        }
    }

    bool memory_manager::host_window_is_free(const uint64_t address, const size_t size) const
    {
        return this->memory_->reserved_host_ranges_in(address, size).empty();
    }

    void memory_manager::reset_host_memory_ranges()
    {
        for (const auto addr : this->host_reserved_addresses_)
        {
            this->release_memory(addr, 0);
        }
        this->host_reserved_addresses_.clear();

        this->reserve_host_memory_ranges();
    }

    bool memory_manager::allocate_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions,
                                         const bool reserve_only, const memory_region_kind kind)
    {
        // On a backend sharing the address space with the guest (FEX on Apple), a host framework can
        // claim a guest-visible VA at any time - e.g. AppKit/SkyLight lazily vm_allocate'ing a
        // window-tag shared page into a gap freed by an earlier guest DLL unload - and a fixed-address
        // mmap over it would silently destroy that mapping, so the target window is confirmed against
        // the live host layout, not sogen's bookkeeping as of the last rescan. Module preferred-base
        // loads hit this on every DLL map/remap; a collision surfaces as an overlaps_reserved_region
        // hit, routing relocatable modules through the existing find_free_allocation_base fallback.
        // Only [address, size) is rescanned - the sole window this call could clobber - since a
        // full-address-space rescan grows costlier as the host map does, for no added safety.
        this->reserve_host_memory_ranges_in(address, size);

        if (!this->allocate_memory_raw(address, size, permissions, reserve_only, kind))
        {
            return false;
        }

        // A reserve-only range never reaches map_memory, so nothing else claims it at the host level:
        // the host allocator would stay free to hand the address out until the guest commits, and the
        // commit's MAP_FIXED would then clobber whatever landed there.
        if (reserve_only)
        {
            this->memory_->reserve_guest_address_range(address, size);
        }

        return true;
    }

    bool memory_manager::allocate_memory_raw(const uint64_t address, const size_t size, const nt_memory_permission permissions,
                                             const bool reserve_only, const memory_region_kind kind)
    {
        if (this->overlaps_reserved_region(address, size))
        {
            return false;
        }

        const auto entry = this->reserved_regions_
                               .try_emplace(address,
                                            reserved_region{
                                                .length = size,
                                                .initial_permission = permissions,
                                                .kind = kind,
                                            })
                               .first;

        if (!reserve_only)
        {
            this->map_memory(address, size, this->get_effective_permissions(permissions));
            entry->second.committed_regions[address] = committed_region{
                .length = size,
                .permissions = permissions,
            };
        }

        this->update_layout_version();

        return true;
    }

    bool memory_manager::allocate_shared_view(const uint64_t address, const uint64_t source, const size_t size,
                                              const nt_memory_permission permissions)
    {
        const auto backing = this->find_reserved_region(source);
        if (backing == this->reserved_regions_.end() || !size || source + size < source ||
            source + size > backing->first + backing->second.length ||
            !this->allocate_memory(address, size, permissions, true, memory_region_kind::pagefile_section_view))
        {
            return false;
        }
        auto& view = this->reserved_regions_.at(address);
        view.host_backing = backing->second.host_backing;
        try
        {
            for (const auto& [start, region] : backing->second.committed_regions)
            {
                const auto first = std::max(source, start);
                const auto end = std::min(source + size, start + region.length);
                if (first >= end)
                {
                    continue;
                }
                const auto target = address + first - source;
                const auto length = static_cast<size_t>(end - first);
                const auto committed =
                    view.committed_regions.emplace(target, committed_region{.length = length, .permissions = permissions}).first;
                bool mapped{};
                try
                {
                    mapped = this->memory_->map_shared_memory(target, first, length, this->get_effective_permissions(permissions));
                }
                catch (...)
                {
                    view.committed_regions.erase(committed);
                    throw;
                }
                if (!mapped)
                {
                    view.committed_regions.erase(committed);
                    this->release_memory(address, 0);
                    return false;
                }
            }
            this->shared_views_.emplace(address, source);
        }
        catch (...)
        {
            this->release_memory(address, 0);
            throw;
        }
        this->update_layout_version();
        return true;
    }

    uint64_t memory_manager::shared_view_source(const uint64_t address) const
    {
        const auto entry = this->shared_views_.find(address);
        return entry == this->shared_views_.end() ? 0 : entry->second;
    }

    bool memory_manager::has_shared_views(const uint64_t backing) const
    {
        const auto region = this->reserved_regions_.find(backing);
        if (region == this->reserved_regions_.end())
        {
            return false;
        }
        return std::ranges::any_of(this->shared_views_, [&](const auto& entry) {
            return entry.second >= backing && entry.second - backing < region->second.length;
        });
    }

    void memory_manager::restore_shared_views()
    {
        for (const auto& [address, source] : this->shared_views_)
        {
            const auto view = this->reserved_regions_.find(address);
            const auto backing = this->find_reserved_region(source);
            if (view == this->reserved_regions_.end() || view->second.kind != memory_region_kind::pagefile_section_view ||
                source + view->second.length < source)
            {
                throw std::runtime_error("Invalid saved shared view");
            }
            if (backing == this->reserved_regions_.end())
            {
                throw std::runtime_error("Invalid saved shared view backing");
            }
            if (source + view->second.length > backing->first + backing->second.length)
            {
                throw std::runtime_error("Invalid saved shared view extent");
            }
            view->second.host_backing = backing->second.host_backing;
            for (const auto& [start, region] : view->second.committed_regions)
            {
                this->unmap_memory(start, region.length);
                if (!this->memory_->map_shared_memory(start, source + start - address, region.length,
                                                      this->get_effective_permissions(region.permissions)))
                {
                    throw std::runtime_error("Unable to restore shared view");
                }
            }
        }
    }

    bool memory_manager::commit_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions)
    {
        const auto view = this->find_reserved_region(address);
        if (view == this->reserved_regions_.end() || view->second.kind != memory_region_kind::pagefile_section_view)
        {
            return this->commit_memory(address, size, permissions, false);
        }
        if (!size || address + size < address || address + size > view->first + view->second.length)
        {
            return false;
        }
        const auto source = this->shared_view_source(view->first);
        const auto target = source ? source + address - view->first : address;
        const auto backing = this->find_reserved_region(target);
        if (backing == this->reserved_regions_.end() || !this->commit_memory(target, size, backing->second.initial_permission, true))
        {
            return false;
        }
        for (const auto& [base, origin] : this->shared_views_)
        {
            auto& region = this->reserved_regions_.at(base);
            const auto first = std::max(target, origin);
            const auto end = std::min(target + size, origin + region.length);
            if (first >= end)
            {
                continue;
            }
            const auto begin = base + first - origin;
            const auto finish = base + end - origin;
            auto cursor = begin;
            split_regions(region.committed_regions, {begin, finish});
            auto existing = region.committed_regions.lower_bound(begin);
            while (cursor < finish)
            {
                if (existing != region.committed_regions.end() && existing->first == cursor)
                {
                    cursor += existing->second.length;
                    ++existing;
                    continue;
                }
                const auto gap_end = existing == region.committed_regions.end() ? finish : std::min(finish, existing->first);
                const auto length = static_cast<size_t>(gap_end - cursor);
                if (!this->memory_->map_shared_memory(cursor, origin + cursor - base, length,
                                                      this->get_effective_permissions(region.initial_permission)))
                {
                    throw std::runtime_error("Unable to commit shared view");
                }
                region.committed_regions.emplace(cursor, committed_region{.length = length, .permissions = region.initial_permission});
                cursor = gap_end;
            }
            merge_regions(region.committed_regions);
        }
        this->update_layout_version();
        return source ? this->protect_memory(address, size, permissions) : true;
    }

    bool memory_manager::commit_image_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions)
    {
        const auto entry = this->find_reserved_region(address);
        if (entry == this->reserved_regions_.end() || entry->second.kind != memory_region_kind::section_image)
        {
            return false;
        }

        return this->commit_memory(address, size, permissions, true);
    }

    bool memory_manager::commit_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions,
                                       const bool allow_image_section)
    {
        const auto entry = this->find_reserved_region(address);
        if (entry == this->reserved_regions_.end())
        {
            return false;
        }

        if (entry->second.kind == memory_region_kind::host_reserved)
        {
            return false;
        }

        if (memory_region_policy::is_section_kind(entry->second.kind) &&
            !(allow_image_section &&
              (entry->second.kind == memory_region_kind::section_image || entry->second.kind == memory_region_kind::pagefile_section_view)))
        {
            return false;
        }

        const auto end = address + size;
        const auto region_end = entry->first + entry->second.length;

        if (region_end < end)
        {
            throw std::runtime_error("Cross region commit not supported yet!");
        }

        auto& committed_regions = entry->second.committed_regions;
        split_regions(committed_regions, {address, end});

        uint64_t last_region_start{};
        const committed_region* last_region{nullptr};

        const auto effective_permission = this->get_effective_permissions(permissions);

        for (auto& sub_region : committed_regions)
        {
            if (sub_region.first >= end)
            {
                break;
            }

            const auto sub_region_end = sub_region.first + sub_region.second.length;
            if (sub_region.first >= address && sub_region_end <= end)
            {
                const auto map_start = last_region ? (last_region_start + last_region->length) : address;
                const auto map_length = sub_region.first - map_start;

                if (map_length > 0)
                {
                    this->map_memory(map_start, static_cast<size_t>(map_length), effective_permission);
                    committed_regions[map_start] = committed_region{
                        .length = static_cast<size_t>(map_length),
                        .permissions = permissions,
                    };
                }

                // Update protection for existing committed region when re-committing
                this->apply_memory_protection(sub_region.first, sub_region.second.length, effective_permission);
                sub_region.second.permissions = permissions;

                last_region_start = sub_region.first;
                last_region = &sub_region.second;
            }
        }

        if (!last_region || (last_region_start + last_region->length) < end)
        {
            const auto map_start = last_region ? (last_region_start + last_region->length) : address;
            const auto map_length = end - map_start;

            this->map_memory(map_start, static_cast<size_t>(map_length), effective_permission);
            committed_regions[map_start] = committed_region{
                .length = static_cast<size_t>(map_length),
                .permissions = permissions,
            };
        }

        merge_regions(committed_regions);

        this->update_layout_version();

        return true;
    }

    bool memory_manager::decommit_memory(const uint64_t address, const size_t size)
    {
        const auto entry = this->find_reserved_region(address);
        if (entry == this->reserved_regions_.end())
        {
            return false;
        }

        if (entry->second.kind == memory_region_kind::mmio)
        {
            throw std::runtime_error("Not allowed to decommit MMIO!");
        }

        const auto end = address + size;
        const auto region_end = entry->first + entry->second.length;

        if (region_end < end)
        {
            throw std::runtime_error("Cross region decommit not supported yet!");
        }

        auto& committed_regions = entry->second.committed_regions;

        split_regions(committed_regions, {address, end});

        for (auto i = committed_regions.begin(); i != committed_regions.end();)
        {
            if (i->first >= end)
            {
                break;
            }

            const auto sub_region_end = i->first + i->second.length;
            if (i->first >= address && sub_region_end <= end)
            {
                this->unmap_memory(i->first, i->second.length);
                i = committed_regions.erase(i);
                continue;
            }

            ++i;
        }

        this->update_layout_version();

        return true;
    }

    void memory_manager::release_host_claims(const uint64_t released_end)
    {
        // Since reserved_regions_ entries are pairwise non-overlapping, the space between the last
        // region starting below released_end and the first starting at or above it holds no
        // reservations. That whole gap is handed back rather than just the released range, so the
        // backend can also drop claim pages straddling the released range's unaligned edges.
        uint64_t gap_start = 0;
        uint64_t gap_end = MAX_ALLOCATION_END_EXCL;

        const auto next = this->reserved_regions_.lower_bound(released_end);
        if (next != this->reserved_regions_.end())
        {
            gap_end = next->first;
        }

        if (next != this->reserved_regions_.begin())
        {
            const auto& prev = *std::prev(next);
            gap_start = prev.first + prev.second.length;
        }

        if (gap_start >= gap_end)
        {
            return;
        }

        this->memory_->release_guest_address_range(gap_start, static_cast<size_t>(gap_end - gap_start));
    }

    bool memory_manager::release_memory(const uint64_t address, size_t size)
    {
        if (!size)
        {
            const auto entry = this->reserved_regions_.find(address);
            if (entry == this->reserved_regions_.end())
            {
                return false;
            }

            const auto entry_length = entry->second.length;

            auto& committed_regions = entry->second.committed_regions;
            for (auto i = committed_regions.begin(); i != committed_regions.end();)
            {
                this->unmap_memory(i->first, i->second.length);
                i = committed_regions.erase(i);
                this->update_layout_version();
            }

            this->reserved_regions_.erase(entry);
            this->shared_views_.erase(address);
            this->release_host_claims(address + entry_length);
            this->update_layout_version();
            return true;
        }

        const auto aligned_start = page_align_down(address);
        const auto aligned_end = page_align_up(address + size);
        size = static_cast<size_t>(aligned_end - aligned_start);

        const auto entry = this->find_reserved_region(aligned_start);
        if (entry == this->reserved_regions_.end())
        {
            return false;
        }

        const auto reserved_start = entry->first;
        const auto reserved_end = entry->first + entry->second.length;

        if (reserved_end < aligned_end)
        {
            throw std::runtime_error("Cross region release not supported yet!");
        }

        reserved_region region = entry->second;
        const auto shared_source = this->shared_view_source(reserved_start);

        auto& committed_regions = region.committed_regions;
        split_regions(committed_regions, {aligned_start, aligned_end});
        auto pending_committed_regions = committed_regions;
        std::vector<std::pair<uint64_t, size_t>> removed_ranges;

        for (auto i = committed_regions.begin(); i != committed_regions.end();)
        {
            if (i->first >= aligned_end)
            {
                break;
            }

            const auto sub_region_end = i->first + i->second.length;
            if (i->first >= aligned_start && sub_region_end <= aligned_end)
            {
                removed_ranges.emplace_back(i->first, i->second.length);
                i = committed_regions.erase(i);
            }
            else
            {
                ++i;
            }
        }

        reserved_region_map surviving_regions;
        std::map<uint64_t, uint64_t> surviving_views;
        // Allocate replacement bookkeeping before unmapping, so allocation failure cannot orphan surviving host aliases.
        committed_region_map left_committed{};
        committed_region_map right_committed{};

        for (const auto& sub_region : committed_regions)
        {
            if (sub_region.first < aligned_start)
            {
                left_committed.emplace(sub_region.first, sub_region.second);
            }
            else if (sub_region.first >= aligned_end)
            {
                right_committed.emplace(sub_region.first, sub_region.second);
            }
        }

        if (reserved_start < aligned_start)
        {
            reserved_region left_region{};
            left_region.length = static_cast<size_t>(aligned_start - reserved_start);
            left_region.initial_permission = region.initial_permission;
            left_region.kind = region.kind;
            left_region.mapped_filename = region.mapped_filename;
            left_region.host_backing = region.host_backing;
            left_region.committed_regions = std::move(left_committed);
            surviving_regions.try_emplace(reserved_start, std::move(left_region));
            if (shared_source)
            {
                surviving_views.emplace(reserved_start, shared_source);
            }
        }

        if (aligned_end < reserved_end)
        {
            reserved_region right_region{};
            right_region.length = static_cast<size_t>(reserved_end - aligned_end);
            right_region.initial_permission = region.initial_permission;
            right_region.kind = region.kind;
            right_region.mapped_filename = region.mapped_filename;
            right_region.host_backing = region.host_backing;
            right_region.committed_regions = std::move(right_committed);
            surviving_regions.try_emplace(aligned_end, std::move(right_region));
            if (shared_source)
            {
                surviving_views.emplace(aligned_end, shared_source + aligned_end - reserved_start);
            }
        }

        // Keep the reservation and its owner until every unmap succeeds, but record each completed
        // subrange immediately. Retrying after a later failure must not unmap an already removed range.
        entry->second.committed_regions.swap(pending_committed_regions);
        for (const auto& [removed_address, removed_size] : removed_ranges)
        {
            this->unmap_memory(removed_address, removed_size);
            entry->second.committed_regions.erase(removed_address);
            this->update_layout_version();
        }
        this->reserved_regions_.erase(entry);
        this->shared_views_.erase(reserved_start);
        this->reserved_regions_.merge(surviving_regions);
        this->shared_views_.merge(surviving_views);
        this->release_host_claims(aligned_end);
        this->update_layout_version();
        return true;
    }

    void memory_manager::unmap_all_memory()
    {
        this->shared_views_.clear();
        for (const auto& reserved_region : this->reserved_regions_)
        {
            for (const auto& region : reserved_region.second.committed_regions)
            {
                this->unmap_memory(region.first, region.second.length);
            }
        }

        this->reserved_regions_.clear();
    }

    namespace
    {
        // Backstop only: every non-settling iteration of the pick/confirm loop below records at least
        // one more foreign range, so find_free_allocation_base is guaranteed to make progress.
        constexpr int max_host_reserved_retries = 8;
    }

    uint64_t memory_manager::allocate_memory(const size_t size, const nt_memory_permission permissions, const bool reserve_only,
                                             uint64_t start, const memory_region_kind kind)
    {
        // Backends sharing the address space with the guest (e.g. FEX) allocate their own host memory
        // (JIT code buffers, thread and GCD worker stacks, a framework's lazy vm_allocate) at any point
        // during execution, so a base picked purely from sogen's bookkeeping may already be claimed and
        // the reserve_guest_address_range below would mmap(MAP_FIXED) over it. find_free_host_allocation_base
        // confirms the pick against the host instead of paying for an unconditional full rescan, which
        // costs O(host VM regions) Mach IPC syscalls and grows without bound over a session.
        const uint64_t allocation_base = this->find_free_host_allocation_base(size, start);
        if (!allocation_base)
        {
            return 0;
        }

        // Claimed at the host level immediately, even when the guest range is only reserved and not yet
        // backed by map_memory - see reserve_guest_address_range's doc comment.
        this->memory_->reserve_guest_address_range(allocation_base, size);

        // Not the public allocate_memory(address, ...): its rescan would re-discover the
        // reserve_guest_address_range mmap just made as a foreign host allocation - reserved_regions_
        // does not know about it yet - and mark allocation_base host_reserved, making the call below
        // self-conflict. The confirm inside find_free_host_allocation_base already covers this window.
        if (!this->allocate_memory_raw(allocation_base, size, permissions, reserve_only, kind))
        {
            this->release_host_claims(allocation_base + size);
            return 0;
        }

        return allocation_base;
    }

    uint64_t memory_manager::find_free_host_allocation_base(const size_t size, const uint64_t start)
    {
        return this->find_free_host_allocation_base(size, start, MAX_ALLOCATION_END_EXCL - 1);
    }

    uint64_t memory_manager::find_free_host_allocation_base(const size_t size, const uint64_t start, const uint64_t highest_address)
    {
        // The confirm uses the pure host_window_is_free probe rather than reserve_host_memory_ranges_in,
        // which would record a clamped window slice and stop the rescan below from recording an
        // intruder's full extent.
        //
        // Both rescan forms are needed on collision: the full scan records every visible foreign range
        // at once, so a pick at the low edge of a large occupied region skips the whole region in one
        // step; the windowed record retires exactly the flagged window, covering backends whose full
        // reserved_host_ranges() omits ranges their windowed query still reports occupied - otherwise
        // the same pick would be rejected every iteration while the full rescan recorded nothing new.
        for (int attempt = 0;; ++attempt)
        {
            const uint64_t allocation_base =
                this->find_free_allocation_base(size, start, ALLOCATION_GRANULARITY, MIN_ALLOCATION_ADDRESS, highest_address);
            if (!allocation_base)
            {
                return 0;
            }

            if (this->host_window_is_free(allocation_base, size))
            {
                return allocation_base;
            }

            if (attempt >= max_host_reserved_retries)
            {
                return 0;
            }

            this->reserve_host_memory_ranges();
            this->reserve_host_memory_ranges_in(allocation_base, size);
        }
    }

    uint64_t memory_manager::find_free_allocation_base(const size_t size, const uint64_t start) const
    {
        return this->find_free_allocation_base(size, start, ALLOCATION_GRANULARITY, MIN_ALLOCATION_ADDRESS, MAX_ALLOCATION_END_EXCL - 1);
    }

    namespace
    {
        bool is_power_of_two(const uint64_t value)
        {
            return value != 0 && (value & (value - 1)) == 0;
        }

        std::optional<uint64_t> checked_align_up(const uint64_t value, const uint64_t alignment)
        {
            if (!is_power_of_two(alignment) || value > UINT64_MAX - (alignment - 1))
            {
                return std::nullopt;
            }

            return align_up(value, alignment);
        }
    }

    uint64_t memory_manager::find_free_allocation_base(const size_t size, const uint64_t start, uint64_t alignment, uint64_t lowest_address,
                                                       uint64_t highest_address) const
    {
        if (!is_power_of_two(alignment) || alignment < ALLOCATION_GRANULARITY || size == 0)
        {
            return 0;
        }

        lowest_address = std::max<uint64_t>(lowest_address, MIN_ALLOCATION_ADDRESS);
        highest_address = std::min<uint64_t>(highest_address ? highest_address : MAX_ALLOCATION_END_EXCL - 1, MAX_ALLOCATION_END_EXCL - 1);
        if (lowest_address > highest_address)
        {
            return 0;
        }

        auto candidate = start ? start : this->default_allocation_address_;
        if (candidate < lowest_address || candidate > highest_address)
        {
            candidate = lowest_address;
        }

        auto aligned_start = checked_align_up(candidate, alignment);
        if (!aligned_start.has_value())
        {
            return 0;
        }

        uint64_t start_address = *aligned_start;

        // Since reserved_regions_ is a sorted map, start at the region immediately
        // before or containing start_address and advance through it only once.
        auto region = this->reserved_regions_.upper_bound(start_address);
        if (region != this->reserved_regions_.begin())
        {
            --region;
        }

        while (start_address <= highest_address)
        {
            const auto end_address = start_address + size;
            if (end_address < start_address || end_address > MAX_ALLOCATION_END_EXCL || end_address - 1 > highest_address)
            {
                return 0;
            }

            while (region != this->reserved_regions_.end())
            {
                const auto region_end = region->first + region->second.length;
                if (region_end < region->first)
                {
                    return 0;
                }

                // This region ends before our candidate, so it can never conflict
                // with this or any later candidate.
                if (region_end <= start_address)
                {
                    ++region;
                    continue;
                }

                // The next reserved region starts after our candidate range, so
                // the entire range [start_address, end_address) is free.
                if (region->first >= end_address)
                {
                    return start_address;
                }

                // Otherwise the candidate overlaps this region. Move the candidate
                // past it and continue scanning from the following region.
                aligned_start = checked_align_up(region_end, alignment);
                if (!aligned_start.has_value())
                {
                    return 0;
                }

                start_address = *aligned_start;
                ++region;
                break;
            }

            // No reserved regions remain, so only the address-range bounds need
            // to be checked for the candidate we may have just advanced to.
            if (region == this->reserved_regions_.end())
            {
                const auto final_end_address = start_address + size;
                if (final_end_address < start_address || final_end_address > MAX_ALLOCATION_END_EXCL ||
                    final_end_address - 1 > highest_address)
                {
                    return 0;
                }

                return start_address;
            }
        }

        return 0;
    }

    region_info memory_manager::get_region_info(const uint64_t address)
    {
        region_info result{};
        result.start = page_align_down(address);
        result.length = static_cast<size_t>(MAX_ALLOCATION_END_EXCL - result.start);
        result.permissions = nt_memory_permission();
        result.initial_permissions = nt_memory_permission();
        result.allocation_base = {};
        result.allocation_length = result.length;
        result.is_committed = false;
        result.is_reserved = false;

        if (this->reserved_regions_.empty())
        {
            return result;
        }

        auto upper_bound = this->reserved_regions_.upper_bound(address);
        if (upper_bound == this->reserved_regions_.begin())
        {
            result.length = static_cast<size_t>(upper_bound->first - result.start);
            return result;
        }

        const auto entry = --upper_bound;
        const auto reserved_end = entry->first + entry->second.length;

        if (reserved_end <= address)
        {
            auto next = std::next(entry);

            result.start = page_align_down(address);
            result.length = next == this->reserved_regions_.end() ? static_cast<size_t>(MAX_ALLOCATION_END_EXCL - result.start)
                                                                  : static_cast<size_t>(next->first - result.start);

            return result;
        }

        const auto& reserved_region = entry->second;
        const auto& committed_regions = reserved_region.committed_regions;

        result.is_reserved = true;
        result.allocation_base = entry->first;
        result.allocation_length = reserved_region.length;
        result.initial_permissions = reserved_region.initial_permission;
        result.kind = reserved_region.kind;
        result.start = page_align_down(address);
        result.length = static_cast<size_t>(reserved_end - result.start);

        if (committed_regions.empty())
        {
            return result;
        }

        auto committed_bound = committed_regions.upper_bound(address);
        if (committed_bound == committed_regions.begin())
        {
            result.length = static_cast<size_t>(committed_bound->first - result.start);
            return result;
        }

        const auto committed_entry = --committed_bound;
        const auto committed_end = committed_entry->first + committed_entry->second.length;

        if (committed_end <= address)
        {
            auto next_committed = std::next(committed_entry);

            result.length = next_committed == committed_regions.end() ? static_cast<size_t>(reserved_end - result.start)
                                                                      : static_cast<size_t>(next_committed->first - result.start);

            return result;
        }

        result.is_committed = true;
        result.permissions = committed_entry->second.permissions;
        result.length = static_cast<size_t>(committed_end - result.start);

        return result;
    }

    std::optional<std::u16string> memory_manager::get_region_mapped_filename(const uint64_t address) const
    {
        if (this->reserved_regions_.empty())
        {
            return std::nullopt;
        }

        auto upper_bound = this->reserved_regions_.upper_bound(address);
        if (upper_bound == this->reserved_regions_.begin())
        {
            return std::nullopt;
        }

        const auto entry = --upper_bound;
        if (entry->first + entry->second.length <= address || entry->second.mapped_filename.empty())
        {
            return std::nullopt;
        }

        return entry->second.mapped_filename;
    }

    void memory_manager::set_region_mapped_filename(const uint64_t address, std::u16string filename)
    {
        const auto entry = this->find_reserved_region(address);
        if (entry == this->reserved_regions_.end())
        {
            return;
        }

        entry->second.mapped_filename = std::move(filename);
    }

    memory_manager::reserved_region_map::iterator memory_manager::find_reserved_region(const uint64_t address)
    {
        if (this->reserved_regions_.empty())
        {
            return this->reserved_regions_.end();
        }

        auto upper_bound = this->reserved_regions_.upper_bound(address);
        if (upper_bound == this->reserved_regions_.begin())
        {
            return this->reserved_regions_.end();
        }

        const auto entry = --upper_bound;
        if (entry->first + entry->second.length <= address)
        {
            return this->reserved_regions_.end();
        }

        return entry;
    }

    bool memory_manager::overlaps_reserved_region(const uint64_t address, const size_t size, const bool ignore_host_reserved) const
    {
        // reserved_regions_ entries are pairwise non-overlapping - every insertion path checks this
        // function first, and allocate_mmio splits host_reserved coverage around its claim (see
        // carve_host_reserved_hole) - so only the region starting immediately before `address` and
        // those starting within [address, address + size) can intersect. The full scan this replaces
        // mattered: reserve_host_memory_ranges calls this once per host-reported range on every
        // fixed-address module map, so module remap churn cost O(n) per remap as reserved_regions_ grew.
        auto it = this->reserved_regions_.upper_bound(address);

        if (it != this->reserved_regions_.begin())
        {
            const auto& prev = *std::prev(it);
            if (!(ignore_host_reserved && prev.second.kind == memory_region_kind::host_reserved) &&
                regions_with_length_intersect(address, size, prev.first, prev.second.length))
            {
                return true;
            }
        }

        for (; it != this->reserved_regions_.end() && it->first < address + size; ++it)
        {
            if (ignore_host_reserved && it->second.kind == memory_region_kind::host_reserved)
            {
                continue;
            }

            if (regions_with_length_intersect(address, size, it->first, it->second.length))
            {
                return true;
            }
        }

        return false;
    }

    memory_region_kind memory_manager::get_region_kind(const uint64_t address) const
    {
        if (this->reserved_regions_.empty())
        {
            return memory_region_kind::free;
        }

        auto upper_bound = this->reserved_regions_.upper_bound(address);
        if (upper_bound == this->reserved_regions_.begin())
        {
            return memory_region_kind::free;
        }

        const auto entry = --upper_bound;
        if (entry->first + entry->second.length <= address)
        {
            return memory_region_kind::free;
        }

        return entry->second.kind;
    }

    void memory_manager::read_memory(const uint64_t address, void* data, const size_t size) const
    {
        this->memory_->read_memory(address, data, size);
    }

    bool memory_manager::try_read_memory(const uint64_t address, void* data, const size_t size) const
    {
        try
        {
            return this->memory_->try_read_memory(address, data, size);
        }
        catch (...)
        {
            return false;
        }
    }

    void memory_manager::write_memory(const uint64_t address, const void* data, const size_t size)
    {
        this->memory_->write_memory(address, data, size);
    }

    bool memory_manager::try_write_memory(const uint64_t address, const void* data, const size_t size)
    {
        try
        {
            return this->memory_->try_write_memory(address, data, size);
        }
        catch (...)
        {
            return false;
        }
    }

    void memory_manager::map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb)
    {
        this->memory_->map_mmio(address, size, std::move(read_cb), std::move(write_cb));
    }

    void memory_manager::map_memory(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        this->memory_->map_memory(address, size, permissions);
    }

    void memory_manager::map_host_memory(const uint64_t address, const size_t size, void* host_pointer, const memory_permission permissions)
    {
        this->memory_->map_host_memory(address, size, host_pointer, permissions);
    }

    bool memory_manager::host_memory_aliasing_is_coherent() const
    {
        return this->memory_->host_memory_aliasing_is_coherent();
    }

    void memory_manager::flush_host_memory_cache(const void* host_pointer, const size_t size)
    {
        this->memory_->flush_host_memory_cache(host_pointer, size);
    }

    void memory_manager::unmap_memory(const uint64_t address, const size_t size)
    {
        this->memory_->unmap_memory(address, size);
    }

    void memory_manager::apply_memory_protection(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        this->memory_->apply_memory_protection(address, size, permissions);
    }

} // namespace sogen
