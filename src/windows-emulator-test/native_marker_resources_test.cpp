#include <native_marker_resources.hpp>

#include <algorithm>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace sogen::detail;

namespace
{
    void require(const bool condition, const char* expression)
    {
        if (!condition)
        {
            throw std::runtime_error(expression);
        }
    }

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression)

    struct kernel_policy
    {
        bool held{true};
        bool throws{};

        bool is_held_by_current_thread() const
        {
            if (this->throws)
            {
                throw std::runtime_error("kernel policy");
            }
            return this->held;
        }
    };

    struct request
    {
        uint64_t address{};
        size_t size{};
        bool operator==(const request&) const = default;
    };

    enum class rejected_span
    {
        none,
        uncommitted,
        guard,
        unreadable,
        mmio,
        host_reserved
    };

    struct fixture
    {
        static constexpr uint64_t base = 0x140000000;
        static constexpr uint64_t image_size = 0x3000000;
        static constexpr uint64_t object = 0x20000000;

        kernel_policy kernel{};
        native_marker_read_budget budget{1000};
        uint64_t now{1000};
        uint64_t clock_advance_after_guard{};
        uint64_t clock_advance_after_read{};
        bool clock_throws{};
        bool metadata_throws{};
        bool reader_throws{};
        bool mutate_header_vtable{};
        uint64_t failed_address{};
        size_t failed_size{};
        uint64_t rejected_address{};
        size_t rejected_size{};
        rejected_span reject{rejected_span::none};
        std::map<uint64_t, std::vector<uint8_t>> memory{};
        std::vector<request> guarded{};
        std::vector<request> reads{};

        fixture()
        {
            this->memory[base + 0x1FB5F44].resize(12);
            this->memory[base + 0x1FB5F68].resize(12);
            this->memory[base + 0x1FB5F80].resize(4);
            this->memory[base + 0x2742FA0].resize(8);
            this->memory[base + 0x2742FB0].resize(8);
            this->memory[object].resize(16);
            this->set<uint64_t>(base + 0x2742FA0, 0, object);
            this->set<uint64_t>(base + 0x2742FB0, 0, object);
            this->set<uint64_t>(object, 0, base + 0x1BEB568);
            this->set<uint32_t>(object, 8, 21);
            this->set<uint32_t>(object, 12, 0x12345fff);
        }

        template <typename Integer>
        void set(const uint64_t address, const size_t offset, const Integer value)
        {
            auto& bytes = this->memory.at(address);
            REQUIRE(offset <= bytes.size() && sizeof(Integer) <= bytes.size() - offset);
            for (size_t index = 0; index < sizeof(Integer); ++index)
            {
                bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
            }
        }

        native_marker_resource_sample sample(const uint64_t module_base = base, const uint64_t module_size = image_size)
        {
            auto guard = [&](const uint64_t address, const size_t size) {
                this->guarded.push_back({address, size});
                this->now += this->clock_advance_after_guard;
                if (this->metadata_throws)
                {
                    throw std::runtime_error("metadata policy");
                }
                return this->reject == rejected_span::none &&
                       !(address == this->rejected_address && (!this->rejected_size || size == this->rejected_size));
            };
            auto reader = [&](const uint64_t address, void* destination, const size_t size) {
                const request call{address, size};
                REQUIRE(!this->guarded.empty() && this->guarded.back() == call);
                this->reads.push_back(call);
                this->now += this->clock_advance_after_read;
                auto* bytes = static_cast<uint8_t*>(destination);
                if (this->reader_throws)
                {
                    bytes[0] = 0xee;
                    throw std::runtime_error("reader policy after prefix write");
                }
                if (address == this->failed_address && (!this->failed_size || size == this->failed_size))
                {
                    std::fill_n(bytes, size / 2, uint8_t{0xee});
                    return false;
                }
                const auto found = this->memory.find(address);
                if (found == this->memory.end() || found->second.size() < size)
                {
                    return false;
                }
                if (this->mutate_header_vtable && address == object && size == 16)
                {
                    this->set<uint64_t>(object, 0, base + 0x1BEB569);
                }
                std::copy_n(found->second.data(), size, bytes);
                return true;
            };
            auto clock = [&] {
                if (this->clock_throws)
                {
                    throw std::runtime_error("clock policy");
                }
                return this->now;
            };
            return sample_native_marker_resources(module_base, module_size, this->kernel, this->budget, guard, reader, clock);
        }
    };

    void complete_sample_and_exact_read_order()
    {
        fixture input{};
        input.set<uint8_t>(fixture::base + 0x1FB5F44, 0, 2);
        input.set<uint8_t>(fixture::base + 0x1FB5F44, 1, 9);
        input.set<uint32_t>(fixture::base + 0x1FB5F44, 4, 0xffff1fff);
        input.set<uint32_t>(fixture::base + 0x1FB5F44, 8, 0xabcdef01);
        input.set<uint32_t>(fixture::base + 0x1FB5F80, 0, 0);
        const auto output = input.sample();
        const std::vector<request> expected{{fixture::base + 0x1FB5F44, 12},
                                            {fixture::base + 0x1FB5F68, 12},
                                            {fixture::base + 0x1FB5F80, 4},
                                            {fixture::base + 0x2742FA0, 8},
                                            {fixture::base + 0x2742FB0, 8},
                                            {fixture::object, 8},
                                            {fixture::object, 16}};
        REQUIRE(input.reads == expected && input.guarded == expected);
        REQUIRE(output.available_mask == 0x7f && output.unavailable_mask == 0);
        REQUIRE(output.resource_reads == 7 && output.resource_bytes == 68);
        REQUIRE(input.budget.reads == 7 && input.budget.bytes == 68);
        REQUIRE(!output.atomic);
        REQUIRE(output.registry_layout == native_marker_registry_layout::matched);
        REQUIRE(output.registry_mode == 21 && output.registry_content_handle == 0x12345fff);
        const auto slot = native_marker_decode_slot(output.resource3);
        REQUIRE(slot && slot->state == 2 && slot->detail == 9);
        REQUIRE(slot->request_handle == 0xffff1fff && slot->resource_handle == 0xabcdef01);
        REQUIRE(native_marker_handle_index(slot->request_handle) == 0x1fff);
        REQUIRE(native_marker_handle_index(0xffffffff) == 0x1fff);
        REQUIRE(native_marker_decode_integer<uint32_t>(output.outstanding) == 0);
    }

    void guard_failures_do_not_call_backend()
    {
        for (const auto rejection : {rejected_span::uncommitted, rejected_span::guard, rejected_span::unreadable, rejected_span::mmio,
                                     rejected_span::host_reserved})
        {
            fixture input{};
            input.reject = rejection;
            const auto output = input.sample();
            REQUIRE(input.guarded.size() == 5 && input.reads.empty());
            REQUIRE(output.available_mask == 0 && output.unavailable_mask == 0x7f);
            REQUIRE(output.resource3.status == native_marker_read_status::passive_span_rejected);
            REQUIRE(output.registry_object.status == native_marker_read_status::dependency_unavailable);
            REQUIRE(input.budget.reads == 0 && input.budget.bytes == 0);
        }
    }

    void invalid_module_spans_do_not_query_policies()
    {
        for (const auto module_base : {uint64_t{0}, uint64_t{0xffff}, uint64_t{0x7fffffff0000}, (std::numeric_limits<uint64_t>::max)() - 8})
        {
            fixture input{};
            const auto output = input.sample(module_base);
            REQUIRE(input.guarded.empty() && input.reads.empty());
            REQUIRE(output.resource3.status == native_marker_read_status::invalid_image);
            REQUIRE(output.available_mask == 0);
        }
        fixture input{};
        const auto output = input.sample(fixture::base, (std::numeric_limits<uint64_t>::max)());
        REQUIRE(input.guarded.empty() && input.reads.empty());
        REQUIRE(output.resource3.status == native_marker_read_status::invalid_image);
        fixture too_small{};
        const auto small = too_small.sample(fixture::base, 0x1FB5F44 + 11);
        REQUIRE(too_small.guarded.empty() && too_small.reads.empty());
        REQUIRE(small.resource3.status == native_marker_read_status::invalid_image);
    }

    void exact_module_boundary_and_independent_availability()
    {
        fixture input{};
        const auto output = input.sample(fixture::base, 0x1FB5F44 + 12);
        REQUIRE(input.reads.size() == 1 && input.reads[0].size == 12);
        REQUIRE(output.available_mask == 1 && output.unavailable_mask == 0x7e);
        REQUIRE(output.resource6.status == native_marker_read_status::invalid_image);
        REQUIRE(native_marker_decode_slot(output.resource3).has_value());
    }

    void kernel_not_held_or_throwing_is_unavailable()
    {
        for (const bool throwing : {false, true})
        {
            fixture input{};
            input.kernel.held = false;
            input.kernel.throws = throwing;
            const auto output = input.sample();
            REQUIRE(input.reads.empty() && input.guarded.empty());
            REQUIRE(output.available_mask == 0);
            REQUIRE(output.resource3.status ==
                    (throwing ? native_marker_read_status::kernel_exception : native_marker_read_status::kernel_not_held));
            REQUIRE(output.registry_vtable.status == output.resource3.status);
            REQUIRE(output.registry_object.status == output.resource3.status);
        }
    }

    void null_and_overflowing_registry_pointers_are_not_decoded()
    {
        for (const auto pointer : {uint64_t{0}, uint64_t{0xffff}, uint64_t{0x7fffffff0000} - 4, (std::numeric_limits<uint64_t>::max)() - 3})
        {
            fixture input{};
            input.set<uint64_t>(fixture::base + 0x2742FB0, 0, pointer);
            const auto output = input.sample();
            REQUIRE(input.reads.size() == 5 && output.resource_bytes == 44);
            REQUIRE(output.registry_pointer.bytes.has_value());
            REQUIRE(output.registry_vtable.status == native_marker_read_status::invalid_address);
            REQUIRE(!output.registry_object.bytes && !output.registry_mode && !output.registry_content_handle);
        }
    }

    void wrong_vtable_prevents_header_read()
    {
        fixture input{};
        input.set<uint64_t>(fixture::object, 0, fixture::base + 0x1BEB569);
        const auto output = input.sample();
        REQUIRE(input.reads.size() == 6 && output.resource_bytes == 52);
        REQUIRE(output.registry_vtable.bytes.has_value() && !output.registry_object.bytes);
        REQUIRE(output.registry_layout == native_marker_registry_layout::vtable_mismatch);
        REQUIRE(!output.registry_mode && !output.registry_content_handle);
        REQUIRE(output.available_mask == 0x3f);
    }

    void embedded_vtable_mutation_keeps_raw_but_not_interpretation()
    {
        fixture input{};
        input.mutate_header_vtable = true;
        const auto output = input.sample();
        REQUIRE(input.reads.size() == 7 && output.available_mask == 0x7f);
        REQUIRE(output.registry_vtable.bytes.has_value() && output.registry_object.bytes.has_value());
        REQUIRE(output.registry_layout == native_marker_registry_layout::vtable_mismatch);
        REQUIRE(!output.registry_mode && !output.registry_content_handle);
        REQUIRE(native_marker_little_integer<uint64_t>(*output.registry_vtable.bytes) == fixture::base + 0x1BEB568);
        REQUIRE(native_marker_little_integer<uint64_t>(*output.registry_object.bytes) == fixture::base + 0x1BEB569);
    }

    void partial_failed_and_throwing_reads_discard_prefix()
    {
        fixture partial{};
        partial.failed_address = fixture::object;
        partial.failed_size = 16;
        const auto incomplete = partial.sample();
        REQUIRE(incomplete.registry_object.status == native_marker_read_status::read_failed);
        REQUIRE(!incomplete.registry_object.bytes && !incomplete.registry_mode && !incomplete.registry_content_handle);
        REQUIRE(incomplete.available_mask == 0x3f && partial.budget.bytes == 68);
        fixture throwing{};
        throwing.reader_throws = true;
        const auto failed = throwing.sample();
        REQUIRE(throwing.reads.size() == 5 && throwing.budget.bytes == 44);
        REQUIRE(failed.resource3.status == native_marker_read_status::read_exception);
        REQUIRE(!failed.resource3.bytes && !native_marker_decode_slot(failed.resource3));
        REQUIRE(failed.available_mask == 0 && !failed.registry_mode);
    }

    void independent_loader_failures_preserve_the_other_slot()
    {
        for (const bool fail_resource6 : {false, true})
        {
            fixture input{};
            input.failed_address = fixture::base + (fail_resource6 ? 0x1FB5F68 : 0x1FB5F44);
            const auto output = input.sample();
            const auto& failed = fail_resource6 ? output.resource6 : output.resource3;
            const auto& confirmed = fail_resource6 ? output.resource3 : output.resource6;
            REQUIRE(failed.status == native_marker_read_status::read_failed && !failed.bytes);
            REQUIRE(!native_marker_decode_slot(failed));
            REQUIRE(confirmed.status == native_marker_read_status::complete && confirmed.bytes.has_value());
            REQUIRE(native_marker_decode_slot(confirmed).has_value());
            REQUIRE(output.available_mask == (fail_resource6 ? uint32_t{0x7d} : uint32_t{0x7e}));
            REQUIRE(input.reads.size() == 7 && output.resource_bytes == 68);
        }
    }

    void header_span_has_its_own_preflight()
    {
        fixture input{};
        input.rejected_address = fixture::object;
        input.rejected_size = 16;
        const auto output = input.sample();
        REQUIRE(input.guarded.size() == 7 && input.reads.size() == 6);
        REQUIRE(output.registry_vtable.status == native_marker_read_status::complete);
        REQUIRE(output.registry_object.status == native_marker_read_status::passive_span_rejected);
        REQUIRE(!output.registry_object.bytes && !output.registry_mode && !output.registry_content_handle);
        REQUIRE(output.resource_reads == 6 && output.resource_bytes == 52);
    }

    void metadata_and_clock_exceptions_are_separate_from_read_failures()
    {
        fixture metadata{};
        metadata.metadata_throws = true;
        const auto rejected = metadata.sample();
        REQUIRE(metadata.guarded.size() == 5 && metadata.reads.empty());
        REQUIRE(rejected.resource3.status == native_marker_read_status::metadata_exception);
        REQUIRE(metadata.budget.reads == 0);
        fixture clock{};
        clock.clock_throws = true;
        const auto stopped = clock.sample();
        REQUIRE(clock.guarded.empty() && clock.reads.empty());
        REQUIRE(stopped.resource3.status == native_marker_read_status::clock_invalid);
        REQUIRE(clock.budget.clock_invalid);
    }

    void preexisting_global_budgets_stop_before_metadata()
    {
        fixture reads{};
        reads.budget.reads = native_marker_read_budget::max_reads;
        const auto no_reads = reads.sample();
        REQUIRE(reads.guarded.empty() && reads.reads.empty());
        REQUIRE(no_reads.resource3.status == native_marker_read_status::read_limit);
        fixture bytes{};
        bytes.budget.bytes = native_marker_read_budget::max_bytes - 11;
        const auto limited = bytes.sample();
        REQUIRE(limited.resource3.status == native_marker_read_status::byte_limit);
        REQUIRE(!native_marker_decode_slot(limited.resource3));
        REQUIRE(bytes.budget.bytes <= native_marker_read_budget::max_bytes);
    }

    void sixteen_stack_reads_leave_exact_resource_budget()
    {
        fixture input{};
        input.budget.reads = 16;
        input.budget.bytes = 128;
        const auto output = input.sample();
        REQUIRE(output.resource_reads == 7 && output.resource_bytes == 68);
        REQUIRE(input.budget.reads == 23 && input.budget.bytes == 196);
        REQUIRE(output.available_mask == 0x7f);
    }

    void time_checks_cover_preflight_and_completed_late_read()
    {
        fixture expired{};
        expired.now += native_marker_read_budget::max_elapsed_ns;
        const auto before = expired.sample();
        REQUIRE(expired.guarded.empty() && expired.reads.empty());
        REQUIRE(before.resource3.status == native_marker_read_status::time_limit);
        fixture during_metadata{};
        during_metadata.clock_advance_after_guard = native_marker_read_budget::max_elapsed_ns;
        const auto no_backend = during_metadata.sample();
        REQUIRE(during_metadata.guarded.size() == 1 && during_metadata.reads.empty());
        REQUIRE(no_backend.resource3.status == native_marker_read_status::time_limit);
        fixture late{};
        late.clock_advance_after_read = native_marker_read_budget::max_elapsed_ns;
        const auto confirmed = late.sample();
        REQUIRE(late.reads.size() == 1 && confirmed.resource3.bytes.has_value());
        REQUIRE(confirmed.resource3.status == native_marker_read_status::complete && late.budget.time_exhausted);
        REQUIRE(confirmed.resource6.status == native_marker_read_status::time_limit);
        REQUIRE(confirmed.available_mask == 1 && confirmed.resource_bytes == 12);
        fixture backward{};
        backward.now = backward.budget.started_ns - 1;
        const auto invalid = backward.sample();
        REQUIRE(invalid.resource3.status == native_marker_read_status::clock_invalid);
        REQUIRE(backward.reads.empty() && backward.guarded.empty());
    }

    void active_pointer_is_address_only_and_zero_is_confirmed_data()
    {
        fixture input{};
        input.set<uint64_t>(fixture::base + 0x2742FA0, 0, 0x33330000);
        input.set<uint32_t>(fixture::object, 8, 0);
        input.set<uint32_t>(fixture::object, 12, 0);
        const auto output = input.sample();
        REQUIRE(native_marker_decode_integer<uint64_t>(output.active_pointer) == 0x33330000);
        REQUIRE(output.registry_mode.has_value() && *output.registry_mode == 0);
        REQUIRE(output.registry_content_handle.has_value() && *output.registry_content_handle == 0);
        REQUIRE(std::none_of(input.reads.begin(), input.reads.end(), [](const auto& call) { return call.address == 0x33330000; }));
        fixture invalid_handle{};
        invalid_handle.set<uint32_t>(fixture::object, 12, 0xffffffff);
        const auto raw_invalid = invalid_handle.sample();
        REQUIRE(raw_invalid.registry_content_handle == 0xffffffff);
        REQUIRE(!raw_invalid.atomic);
    }

    void decoder_offsets_and_inconsistent_availability_are_checked()
    {
        native_marker_raw_value<12> value{};
        value.bytes = std::array<uint8_t, 12>{};
        value.status = native_marker_read_status::read_failed;
        REQUIRE(!native_marker_decode_slot(value));
        REQUIRE(!native_marker_decode_integer<uint32_t>(value, 8));
        value.status = native_marker_read_status::complete;
        REQUIRE(native_marker_decode_integer<uint32_t>(value, 8) == 0);
        REQUIRE(!native_marker_decode_integer<uint32_t>(value, 9));
        bool threw{};
        try
        {
            native_marker_little_integer<uint64_t>(*value.bytes, (std::numeric_limits<size_t>::max)());
        }
        catch (const std::out_of_range&)
        {
            threw = true;
        }
        REQUIRE(threw);
    }

    void local_resource_cap_cannot_borrow_global_budget()
    {
        fixture input{};
        native_marker_resource_read_budget local{};
        local.reads = 7;
        auto guard = [](uint64_t, size_t) -> bool { throw std::runtime_error("must not query guard"); };
        auto reader = [](uint64_t, void*, size_t) -> bool { throw std::runtime_error("must not call reader"); };
        auto clock = [&] { return input.now; };
        const auto read_limited = native_marker_try_read<4>(fixture::base, input.budget, guard, reader, clock, &local);
        REQUIRE(read_limited.status == native_marker_read_status::read_limit && input.budget.reads == 0);
        local.reads = 0;
        local.bytes = 67;
        const auto byte_limited = native_marker_try_read<4>(fixture::base, input.budget, guard, reader, clock, &local);
        REQUIRE(byte_limited.status == native_marker_read_status::byte_limit && input.budget.bytes == 0);
    }
}

TEST(NativeMarkerResources, CompleteSampleAndExactReadOrder)
{
    complete_sample_and_exact_read_order();
}

TEST(NativeMarkerResources, GuardFailuresDoNotCallBackend)
{
    guard_failures_do_not_call_backend();
}

TEST(NativeMarkerResources, InvalidModuleSpans)
{
    invalid_module_spans_do_not_query_policies();
}

TEST(NativeMarkerResources, ExactModuleBoundaryAndIndependentAvailability)
{
    exact_module_boundary_and_independent_availability();
}

TEST(NativeMarkerResources, KernelPolicyAvailability)
{
    kernel_not_held_or_throwing_is_unavailable();
}

TEST(NativeMarkerResources, NullAndOverflowingRegistryPointers)
{
    null_and_overflowing_registry_pointers_are_not_decoded();
}

TEST(NativeMarkerResources, VtablePrecheckPreventsHeaderRead)
{
    wrong_vtable_prevents_header_read();
}

TEST(NativeMarkerResources, EmbeddedVtableMutation)
{
    embedded_vtable_mutation_keeps_raw_but_not_interpretation();
}

TEST(NativeMarkerResources, FailedPartialAndThrowingReads)
{
    partial_failed_and_throwing_reads_discard_prefix();
}

TEST(NativeMarkerResources, IndependentLoaderReadFailures)
{
    independent_loader_failures_preserve_the_other_slot();
}

TEST(NativeMarkerResources, IndependentHeaderPreflight)
{
    header_span_has_its_own_preflight();
}

TEST(NativeMarkerResources, MetadataAndClockExceptions)
{
    metadata_and_clock_exceptions_are_separate_from_read_failures();
}

TEST(NativeMarkerResources, GlobalBudgets)
{
    preexisting_global_budgets_stop_before_metadata();
}

TEST(NativeMarkerResources, CombinedStackResourceBudget)
{
    sixteen_stack_reads_leave_exact_resource_budget();
}

TEST(NativeMarkerResources, CooperativeTimingAndBackwardClock)
{
    time_checks_cover_preflight_and_completed_late_read();
}

TEST(NativeMarkerResources, ActiveAddressOnlyAndZeroData)
{
    active_pointer_is_address_only_and_zero_is_confirmed_data();
}

TEST(NativeMarkerResources, CheckedDecoders)
{
    decoder_offsets_and_inconsistent_availability_are_checked();
}

TEST(NativeMarkerResources, SeparateLocalResourceCap)
{
    local_resource_cap_cannot_borrow_global_budget();
}
