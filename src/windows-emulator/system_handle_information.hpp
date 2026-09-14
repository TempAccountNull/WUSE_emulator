#pragma once

#include <cstdint>

namespace sogen
{
    struct system_handle_information_header
    {
        uint64_t number_of_handles;
        uint64_t reserved;
    };

    struct system_handle_information_entry
    {
        uint64_t object;
        uint64_t process_id;
        uint64_t handle_value;
        uint32_t granted_access;
        uint16_t creator_backtrace_index;
        uint16_t object_type_index;
        uint32_t handle_attributes;
        uint32_t reserved;
    };

    static_assert(sizeof(system_handle_information_header) == 16);
    static_assert(sizeof(system_handle_information_entry) == 40);
}
