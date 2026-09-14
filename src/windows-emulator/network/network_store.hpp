#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace sogen::network
{
    enum class network_store_operation
    {
        parameter,
        all_parameters,
        enumerate,
    };

    struct network_store_buffer
    {
        uint32_t element_size{};
        std::vector<uint8_t> bytes;
    };

    struct network_store_query
    {
        alignas(8) std::array<uint8_t, 24> module{};
        network_store_operation operation{};
        uint32_t table{};
        uint32_t flags{};
        uint32_t second_flags{};
        uint32_t parameter_type{};
        uint32_t parameter_offset{};
        uint32_t count{1};
        std::array<network_store_buffer, 4> buffers;
    };

    uint32_t query_host_network_store(network_store_query& query);
}
