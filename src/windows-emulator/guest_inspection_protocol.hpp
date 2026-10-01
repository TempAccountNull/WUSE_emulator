#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sogen::detail
{
    enum class inspection_operation : uint32_t
    {
        address = 1,
        regions = 2,
        context = 3
    };
    enum class inspection_status : uint32_t
    {
        complete = 0,
        invalid = 2,
        exception = 3,
        rate_limited = 4,
        identity_mismatch = 5,
        unavailable = 6,
        partial = 7
    };

    struct inspection_identity
    {
        uint32_t pid{};
        uint64_t birth{};
        uint64_t generation{};
    };

    struct guest_inspection_packet
    {
        static constexpr uint32_t magic_value = 0x49475353;
        static constexpr uint16_t version_value = 1;
        uint32_t magic{magic_value};
        uint16_t version{version_value};
        uint16_t header_size{128};
        uint32_t packet_size{4096};
        uint32_t operation{};
        uint64_t request_id{};
        uint64_t response_id{};
        uint64_t producer_birth{};
        uint64_t producer_generation{};
        uint64_t expected_birth{};
        uint64_t expected_generation{};
        uint64_t sample_sequence{};
        uint64_t steady_clock_ns{};
        uint64_t address{};
        uint64_t next_cursor{};
        uint32_t producer_pid{};
        uint32_t expected_pid{};
        uint32_t requested_cpu{};
        uint32_t requested_tid{};
        uint32_t actual_cpu{UINT32_MAX};
        uint32_t actual_tid{};
        uint32_t status{static_cast<uint32_t>(inspection_status::unavailable)};
        uint32_t limit{};
        std::array<uint8_t, 3968> payload{};
    };

    struct inspection_region
    {
        uint64_t base{};
        uint64_t length{};
        uint64_t allocation_base{};
        uint64_t allocation_length{};
        uint64_t queried_page_base{};
        uint64_t forward_length{};
        uint8_t state{};
        uint8_t kind{};
        uint8_t permissions{};
        uint8_t initial_permissions{};
        uint8_t guarded{};
        uint8_t dep_enabled{};
        uint16_t reserved{};
    };

    struct inspection_address
    {
        inspection_region region{};
        uint64_t module_base{};
        uint64_t module_size{};
        uint64_t section_base{};
        uint64_t section_length{};
        uint32_t provenance_flags{};
        uint32_t mapped_filename_units{};
        std::array<char, 96> module_name{};
        std::array<char, 16> section_name{};
        std::array<char16_t, 256> mapped_filename{};
    };

    struct inspection_region_page
    {
        uint32_t count{};
        uint32_t stop_reason{};
        uint64_t elapsed_ns{};
        std::array<inspection_region, 64> regions{};
    };

    struct inspection_context
    {
        std::array<uint64_t, 2> available{};
        std::array<uint64_t, 2> read_errors{};
        uint32_t flags_view{1};
        uint32_t register_count{78};
        uint32_t stack_requested_words{};
        uint32_t stack_status{3};
        std::array<uint64_t, 16> gpr{};
        uint64_t rip{};
        uint64_t rflags{};
        uint64_t fs_base{};
        uint64_t gs_base{};
        std::array<uint64_t, 4> cr{};
        std::array<uint64_t, 6> dr{};
        uint64_t efer{};
        std::array<uint16_t, 6> segments{};
        std::array<uint16_t, 3> fp_control{};
        uint16_t reserved{};
        uint32_t mxcsr{};
        std::array<uint32_t, 5> fp_address{};
        std::array<std::array<uint8_t, 10>, 8> st{};
        std::array<std::array<uint8_t, 16>, 16> xmm{};
        uint32_t padding{};
        std::array<uint64_t, 8> mmx{};
        uint64_t stack_base{};
        std::array<uint64_t, 2> stack_success{};
        std::array<uint64_t, 128> stack{};
    };

    static_assert(sizeof(guest_inspection_packet) == 4096);
    static_assert(offsetof(guest_inspection_packet, payload) == 128);
    static_assert(offsetof(guest_inspection_packet, producer_pid) == 96);
    static_assert(offsetof(guest_inspection_packet, status) == 120);
    static_assert(sizeof(inspection_region) == 56);
    static_assert(sizeof(inspection_address) == 720);
    static_assert(sizeof(inspection_region_page) == 3600);
    static_assert(sizeof(inspection_context) == 1792);
    static_assert(offsetof(inspection_context, gpr) == 48);
    static_assert(offsetof(inspection_context, xmm) == 420);
    static_assert(offsetof(inspection_context, stack) == 768);
    static_assert(std::is_trivially_copyable_v<guest_inspection_packet>);
}
