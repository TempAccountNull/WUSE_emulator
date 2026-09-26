#pragma once

#include <gpu_bridge_protocol.hpp>
#include <vk_debug_utils_messenger_wire.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sogen::gpu_bridge::instance_create_wire
{
    inline constexpr size_t max_packet_bytes = sizeof(create_instance_request) + 2 * max_instance_name_bytes +
                                                debug_utils_messenger_wire::max_packet_bytes;

    struct decoded
    {
        create_instance_request request{};
        std::string application_name;
        std::string engine_name;
        std::vector<std::byte> callback;
    };

    inline std::optional<decoded> decode(const std::span<const std::byte> bytes)
    {
        if (bytes.size() < sizeof(create_instance_request) || bytes.size() > max_packet_bytes)
            return std::nullopt;
        decoded out{};
        std::memcpy(&out.request, bytes.data(), sizeof(out.request));
        const auto& request = out.request;
        if (request.magic != create_instance_request_magic || request.reserved || request.application_info_present > 1 ||
            (request.extension_bits & ~instance_ext_supported) ||
            request.application_name_bytes > max_instance_name_bytes || request.engine_name_bytes > max_instance_name_bytes ||
            request.callback_size > debug_utils_messenger_wire::max_packet_bytes ||
            bytes.size() - sizeof(request) !=
                static_cast<size_t>(request.application_name_bytes) + request.engine_name_bytes + request.callback_size ||
            (!request.application_info_present && (request.api_version || request.application_version ||
                                                   request.engine_version || request.application_name_bytes ||
                                                   request.engine_name_bytes)) ||
            (request.callback_size && !(request.extension_bits & instance_ext_debug_utils)))
            return std::nullopt;
        size_t cursor = sizeof(request);
        const auto read_name = [&](const uint32_t size, std::string& name) {
            if (!size) return true;
            const auto* data = reinterpret_cast<const char*>(bytes.data() + cursor);
            cursor += size;
            if (data[size - 1] != '\0' || std::memchr(data, '\0', size - 1)) return false;
            name.assign(data, size - 1);
            return true;
        };
        if (!read_name(request.application_name_bytes, out.application_name) ||
            !read_name(request.engine_name_bytes, out.engine_name))
            return std::nullopt;
        out.callback.assign(bytes.begin() + cursor, bytes.end());
        return out;
    }
}
