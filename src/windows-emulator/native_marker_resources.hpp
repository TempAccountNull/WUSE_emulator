#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace sogen::detail
{
    enum class native_marker_read_status : uint8_t
    {
        not_attempted,
        complete,
        kernel_not_held,
        invalid_image,
        invalid_address,
        passive_span_rejected,
        metadata_exception,
        read_failed,
        read_exception,
        dependency_unavailable,
        time_limit,
        clock_invalid,
        read_limit,
        byte_limit,
        kernel_exception
    };

    struct native_marker_read_budget
    {
        static constexpr uint32_t max_reads = 23;
        static constexpr uint32_t max_bytes = 196;
        static constexpr uint64_t max_elapsed_ns = 2000000;

        uint64_t started_ns{};
        uint32_t reads{};
        uint32_t bytes{};
        bool time_exhausted{};
        bool clock_invalid{};
        bool read_exhausted{};
        bool byte_exhausted{};

        bool expired(const uint64_t now_ns)
        {
            if (now_ns < this->started_ns)
            {
                this->clock_invalid = true;
            }
            else if (now_ns - this->started_ns >= max_elapsed_ns)
            {
                this->time_exhausted = true;
            }
            return this->clock_invalid || this->time_exhausted;
        }

        bool can_read(const size_t size, const uint64_t now_ns)
        {
            if (this->expired(now_ns))
            {
                return false;
            }
            if (this->reads >= max_reads)
            {
                this->read_exhausted = true;
                return false;
            }
            if (this->bytes > max_bytes || size > max_bytes - this->bytes)
            {
                this->byte_exhausted = true;
                return false;
            }
            return size != 0;
        }

        bool try_reserve(const size_t size, const uint64_t now_ns)
        {
            if (!this->can_read(size, now_ns))
            {
                return false;
            }
            ++this->reads;
            this->bytes += static_cast<uint32_t>(size);
            return true;
        }

        native_marker_read_status failure_status() const
        {
            if (this->clock_invalid)
            {
                return native_marker_read_status::clock_invalid;
            }
            if (this->time_exhausted)
            {
                return native_marker_read_status::time_limit;
            }
            if (this->read_exhausted)
            {
                return native_marker_read_status::read_limit;
            }
            return native_marker_read_status::byte_limit;
        }
    };

    template <size_t Size>
    struct native_marker_raw_value
    {
        std::optional<std::array<uint8_t, Size>> bytes{};
        native_marker_read_status status{native_marker_read_status::not_attempted};
    };

    enum class native_marker_registry_layout : uint8_t
    {
        unavailable,
        matched,
        vtable_mismatch
    };

    enum class native_marker_resource_field : uint8_t
    {
        resource3,
        resource6,
        outstanding,
        active_pointer,
        registry_pointer,
        registry_vtable,
        registry_object
    };

    struct native_marker_resource_read_budget
    {
        static constexpr uint32_t max_reads = 7;
        static constexpr uint32_t max_bytes = 68;

        uint32_t reads{};
        uint32_t bytes{};

        bool can_read(const size_t size) const
        {
            return size && this->reads < max_reads && this->bytes <= max_bytes && size <= max_bytes - this->bytes;
        }

        void reserve(const size_t size)
        {
            ++this->reads;
            this->bytes += static_cast<uint32_t>(size);
        }
    };

    struct native_marker_resource_sample
    {
        native_marker_raw_value<12> resource3{};
        native_marker_raw_value<12> resource6{};
        native_marker_raw_value<4> outstanding{};
        native_marker_raw_value<8> active_pointer{};
        native_marker_raw_value<8> registry_pointer{};
        native_marker_raw_value<8> registry_vtable{};
        native_marker_raw_value<16> registry_object{};
        native_marker_registry_layout registry_layout{native_marker_registry_layout::unavailable};
        std::optional<uint32_t> registry_mode{};
        std::optional<uint32_t> registry_content_handle{};
        uint32_t available_mask{};
        uint32_t unavailable_mask{0x7f};
        uint32_t resource_reads{};
        uint32_t resource_bytes{};
        static constexpr bool atomic = false;
    };

    struct native_marker_loader_slot
    {
        uint8_t state{};
        uint8_t detail{};
        uint32_t request_handle{};
        uint32_t resource_handle{};
    };

    template <typename Integer, size_t Size>
    Integer native_marker_little_integer(const std::array<uint8_t, Size>& bytes, const size_t offset = 0)
    {
        static_assert(std::is_integral_v<Integer> && std::is_unsigned_v<Integer>);
        static_assert(sizeof(Integer) <= Size);
        if (offset > Size - sizeof(Integer))
        {
            throw std::out_of_range("Native marker integer offset exceeds the confirmed bytes");
        }
        Integer value{};
        for (size_t index = 0; index < sizeof(Integer); ++index)
        {
            value |= static_cast<Integer>(bytes[offset + index]) << (index * 8);
        }
        return value;
    }

    inline std::optional<native_marker_loader_slot> native_marker_decode_slot(const native_marker_raw_value<12>& raw)
    {
        if (!raw.bytes || raw.status != native_marker_read_status::complete)
        {
            return std::nullopt;
        }
        return native_marker_loader_slot{(*raw.bytes)[0], (*raw.bytes)[1], native_marker_little_integer<uint32_t>(*raw.bytes, 4),
                                         native_marker_little_integer<uint32_t>(*raw.bytes, 8)};
    }

    template <typename Integer, size_t Size>
    std::optional<Integer> native_marker_decode_integer(const native_marker_raw_value<Size>& raw, const size_t offset = 0)
    {
        static_assert(std::is_integral_v<Integer> && std::is_unsigned_v<Integer>);
        static_assert(sizeof(Integer) <= Size);
        if (!raw.bytes || raw.status != native_marker_read_status::complete || offset > Size - sizeof(Integer))
        {
            return std::nullopt;
        }
        return native_marker_little_integer<Integer>(*raw.bytes, offset);
    }

    inline uint32_t native_marker_handle_index(const uint32_t handle)
    {
        return handle & 0x1fff;
    }

    template <size_t Size, typename PassiveSpan, typename Reader, typename Clock>
    native_marker_raw_value<Size> native_marker_try_read(const uint64_t address, native_marker_read_budget& budget,
                                                         PassiveSpan& passive_span, Reader& reader, Clock& clock,
                                                         native_marker_resource_read_budget* resource_budget = nullptr)
    {
        native_marker_raw_value<Size> output{};
        constexpr uint64_t user_end = 0x7fffffff0000ULL;
        if (address < 0x10000 || address >= user_end || Size > user_end - address)
        {
            output.status = native_marker_read_status::invalid_address;
            return output;
        }
        const auto check = [&](const bool reserve) {
            try
            {
                const auto now = clock();
                return reserve ? budget.try_reserve(Size, now) : budget.can_read(Size, now);
            }
            catch (...)
            {
                budget.clock_invalid = true;
                return false;
            }
        };
        if (!check(false))
        {
            output.status = budget.failure_status();
            return output;
        }
        if (resource_budget && !resource_budget->can_read(Size))
        {
            output.status = resource_budget->reads >= native_marker_resource_read_budget::max_reads ? native_marker_read_status::read_limit
                                                                                                    : native_marker_read_status::byte_limit;
            return output;
        }
        try
        {
            if (!passive_span(address, Size))
            {
                output.status = native_marker_read_status::passive_span_rejected;
                return output;
            }
        }
        catch (...)
        {
            output.status = native_marker_read_status::metadata_exception;
            return output;
        }
        if (!check(true))
        {
            output.status = budget.failure_status();
            return output;
        }
        if (resource_budget)
        {
            resource_budget->reserve(Size);
        }
        std::array<uint8_t, Size> value{};
        try
        {
            if (reader(address, value.data(), Size))
            {
                output.bytes = value;
                output.status = native_marker_read_status::complete;
            }
            else
            {
                output.status = native_marker_read_status::read_failed;
            }
        }
        catch (...)
        {
            output.status = native_marker_read_status::read_exception;
        }
        try
        {
            budget.expired(clock());
        }
        catch (...)
        {
            budget.clock_invalid = true;
        }
        return output;
    }

    template <typename Kernel, typename PassiveSpan, typename Reader, typename Clock>
    native_marker_resource_sample sample_native_marker_resources(const uint64_t base, const uint64_t image_size, Kernel& kernel,
                                                                 native_marker_read_budget& budget, PassiveSpan&& passive_span,
                                                                 Reader&& reader, Clock&& clock)
    {
        native_marker_resource_sample output{};
        native_marker_resource_read_budget resource_budget{};
        bool held{};
        bool kernel_exception{};
        try
        {
            held = kernel.is_held_by_current_thread();
        }
        catch (...)
        {
            kernel_exception = true;
        }
        constexpr uint64_t user_end = 0x7fffffff0000ULL;
        const bool image_valid = base >= 0x10000 && base < user_end && image_size && image_size <= user_end - base;
        const auto module_read = [&]<size_t Size>(const uint64_t rva, native_marker_raw_value<Size>& destination) {
            if (kernel_exception)
            {
                destination.status = native_marker_read_status::kernel_exception;
            }
            else if (!held)
            {
                destination.status = native_marker_read_status::kernel_not_held;
            }
            else if (!image_valid || rva > image_size || Size > image_size - rva)
            {
                destination.status = native_marker_read_status::invalid_image;
            }
            else
            {
                destination = native_marker_try_read<Size>(base + rva, budget, passive_span, reader, clock, &resource_budget);
            }
        };
        module_read(0x1FB5F44, output.resource3);
        module_read(0x1FB5F68, output.resource6);
        module_read(0x1FB5F80, output.outstanding);
        module_read(0x2742FA0, output.active_pointer);
        module_read(0x2742FB0, output.registry_pointer);
        if (kernel_exception)
        {
            output.registry_vtable.status = native_marker_read_status::kernel_exception;
            output.registry_object.status = native_marker_read_status::kernel_exception;
        }
        else if (!held)
        {
            output.registry_vtable.status = native_marker_read_status::kernel_not_held;
            output.registry_object.status = native_marker_read_status::kernel_not_held;
        }
        else if (!output.registry_pointer.bytes)
        {
            output.registry_vtable.status = native_marker_read_status::dependency_unavailable;
            output.registry_object.status = native_marker_read_status::dependency_unavailable;
        }
        else
        {
            const auto registry = native_marker_little_integer<uint64_t>(*output.registry_pointer.bytes);
            output.registry_vtable = native_marker_try_read<8>(registry, budget, passive_span, reader, clock, &resource_budget);
            if (!output.registry_vtable.bytes)
            {
                output.registry_object.status = native_marker_read_status::dependency_unavailable;
            }
            else if (native_marker_little_integer<uint64_t>(*output.registry_vtable.bytes) != base + 0x1BEB568)
            {
                output.registry_layout = native_marker_registry_layout::vtable_mismatch;
                output.registry_object.status = native_marker_read_status::dependency_unavailable;
            }
            else
            {
                output.registry_object = native_marker_try_read<16>(registry, budget, passive_span, reader, clock, &resource_budget);
                if (output.registry_object.bytes)
                {
                    const auto vtable = native_marker_little_integer<uint64_t>(*output.registry_object.bytes);
                    if (vtable == base + 0x1BEB568)
                    {
                        output.registry_layout = native_marker_registry_layout::matched;
                        output.registry_mode = native_marker_little_integer<uint32_t>(*output.registry_object.bytes, 8);
                        output.registry_content_handle = native_marker_little_integer<uint32_t>(*output.registry_object.bytes, 12);
                    }
                    else
                    {
                        output.registry_layout = native_marker_registry_layout::vtable_mismatch;
                    }
                }
            }
        }
        const auto mark = [&]<size_t Size>(const native_marker_resource_field field, const native_marker_raw_value<Size>& value) {
            if (value.bytes && value.status == native_marker_read_status::complete)
            {
                output.available_mask |= uint32_t{1} << static_cast<uint8_t>(field);
            }
        };
        mark(native_marker_resource_field::resource3, output.resource3);
        mark(native_marker_resource_field::resource6, output.resource6);
        mark(native_marker_resource_field::outstanding, output.outstanding);
        mark(native_marker_resource_field::active_pointer, output.active_pointer);
        mark(native_marker_resource_field::registry_pointer, output.registry_pointer);
        mark(native_marker_resource_field::registry_vtable, output.registry_vtable);
        mark(native_marker_resource_field::registry_object, output.registry_object);
        output.unavailable_mask = 0x7f & ~output.available_mask;
        output.resource_reads = resource_budget.reads;
        output.resource_bytes = resource_budget.bytes;
        return output;
    }
}
