#pragma once

#include <vk_debug_utils_wire.hpp>

#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace sogen::gpu_bridge::debug_utils_messenger_wire
{
    inline constexpr uint32_t magic = 0x4d445556;
    inline constexpr uint32_t version = 1;
    inline constexpr size_t header_size = 64;
    inline constexpr size_t max_packet_bytes = debug_utils_wire::max_packet_bytes;
    inline constexpr size_t max_payload_bytes = max_packet_bytes - header_size;
    // An instance-create pNext callback exists only during vkCreateInstance; it has no messenger handle to destroy.
    inline constexpr uint32_t flag_instance_chain = 1;

    enum class operation : uint32_t
    {
        create = 1,
        destroy = 2,
        submit = 3,
    };

    struct request
    {
        operation op{};
        uint32_t guest_pointer_bytes{};
        uint64_t instance_id{};
        uint64_t messenger_id{};
        uint64_t callback_address{};
        uint64_t user_data{};
        VkDebugUtilsMessageSeverityFlagsEXT severity{};
        VkDebugUtilsMessageTypeFlagsEXT types{};
        uint32_t flags{};
        std::unique_ptr<debug_utils_wire::callback_storage> message;
    };

    inline uint64_t read_integer(std::span<const std::byte> bytes, size_t offset, unsigned width)
    {
        uint64_t result = 0;
        for (unsigned index = 0; index < width; ++index)
        {
            result |= static_cast<uint64_t>(std::to_integer<uint8_t>(bytes[offset + index])) << (index * 8);
        }
        return result;
    }

    inline void validate(const request& value, size_t payload_size)
    {
        constexpr uint32_t severity_bits = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        constexpr uint32_t type_bits = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
        if ((value.guest_pointer_bytes != 4 && value.guest_pointer_bytes != 8) ||
            (value.guest_pointer_bytes == 4 && (value.callback_address > UINT32_MAX || value.user_data > UINT32_MAX)) ||
            (value.guest_pointer_bytes == 4 && value.instance_id > UINT32_MAX) || payload_size > max_payload_bytes ||
            (value.instance_id == 0 && !(value.flags & flag_instance_chain)))
        {
            throw std::invalid_argument("invalid debug-utils messenger request");
        }
        switch (value.op)
        {
        case operation::create:
            if (value.callback_address == 0 || value.flags & ~flag_instance_chain ||
                ((value.flags & flag_instance_chain) != 0) != (value.instance_id == 0) ||
                ((value.flags & flag_instance_chain) != 0) != (value.messenger_id == 0) || payload_size != 0 ||
                (value.severity & ~severity_bits) != 0 || (value.types & ~type_bits) != 0 || value.message)
            {
                throw std::invalid_argument("invalid debug-utils messenger create request");
            }
            break;
        case operation::destroy:
            if (value.instance_id == 0 || value.messenger_id == 0 || value.callback_address || value.user_data || value.severity ||
                value.types || value.flags || payload_size || value.message)
            {
                throw std::invalid_argument("invalid debug-utils messenger destroy request");
            }
            break;
        case operation::submit:
            if (value.instance_id == 0 || value.messenger_id || value.callback_address || value.user_data || value.severity ||
                value.types || value.flags || !value.message || payload_size == 0)
            {
                throw std::invalid_argument("invalid debug-utils messenger submit request");
            }
            break;
        default:
            throw std::invalid_argument("unknown debug-utils messenger operation");
        }
    }

    inline std::vector<std::byte> encode(const request& value, std::span<const std::byte> payload = {})
    {
        validate(value, payload.size());
        debug_utils_wire::writer output;
        output.u32(magic);
        output.u32(version);
        output.u32(static_cast<uint32_t>(value.op));
        output.u32(value.guest_pointer_bytes);
        output.u64(value.instance_id);
        output.u64(value.messenger_id);
        output.u64(value.callback_address);
        output.u64(value.user_data);
        output.u32(value.severity);
        output.u32(value.types);
        output.u32(static_cast<uint32_t>(payload.size()));
        output.u32(value.flags);
        output.bytes.insert(output.bytes.end(), payload.begin(), payload.end());
        return output.bytes;
    }

    inline request decode(std::span<const std::byte> bytes)
    {
        if (bytes.size() < header_size || bytes.size() > max_packet_bytes || read_integer(bytes, 0, 4) != magic ||
            read_integer(bytes, 4, 4) != version)
        {
            throw std::invalid_argument("invalid debug-utils messenger packet");
        }
        request value{};
        value.op = static_cast<operation>(read_integer(bytes, 8, 4));
        value.guest_pointer_bytes = static_cast<uint32_t>(read_integer(bytes, 12, 4));
        value.instance_id = read_integer(bytes, 16, 8);
        value.messenger_id = read_integer(bytes, 24, 8);
        value.callback_address = read_integer(bytes, 32, 8);
        value.user_data = read_integer(bytes, 40, 8);
        value.severity = static_cast<uint32_t>(read_integer(bytes, 48, 4));
        value.types = static_cast<uint32_t>(read_integer(bytes, 52, 4));
        const uint32_t payload_size = static_cast<uint32_t>(read_integer(bytes, 56, 4));
        value.flags = static_cast<uint32_t>(read_integer(bytes, 60, 4));
        if (bytes.size() - header_size != payload_size)
        {
            throw std::invalid_argument("truncated debug-utils messenger payload");
        }
        if (value.op == operation::submit)
        {
            value.message = debug_utils_wire::decode(bytes.subspan(header_size));
        }
        validate(value, payload_size);
        return value;
    }

    inline std::vector<std::byte> marshal_create(uint64_t instance_id, uint64_t messenger_id,
                                                 const VkDebugUtilsMessengerCreateInfoEXT& info, uint32_t guest_pointer_bytes)
    {
        if (info.sType != VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT || info.pNext || info.flags || !info.pfnUserCallback)
        {
            throw std::invalid_argument("unsupported debug-utils messenger create structure");
        }
        request value{};
        value.op = operation::create;
        value.guest_pointer_bytes = guest_pointer_bytes;
        value.instance_id = instance_id;
        value.messenger_id = messenger_id;
        value.callback_address = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info.pfnUserCallback));
        value.user_data = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info.pUserData));
        value.severity = info.messageSeverity;
        value.types = info.messageType;
        value.flags = instance_id == 0 ? flag_instance_chain : 0;
        return encode(value);
    }

    inline std::vector<std::byte> marshal_destroy(uint64_t instance_id, uint64_t messenger_id, uint32_t guest_pointer_bytes)
    {
        request value{};
        value.op = operation::destroy;
        value.guest_pointer_bytes = guest_pointer_bytes;
        value.instance_id = instance_id;
        value.messenger_id = messenger_id;
        return encode(value);
    }

    inline std::vector<std::byte> marshal_submit(uint64_t instance_id, uint32_t guest_pointer_bytes,
                                                 VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                                                 const VkDebugUtilsMessengerCallbackDataEXT& message)
    {
        auto payload = debug_utils_wire::encode(severity, types, message);
        request value{};
        value.op = operation::submit;
        value.guest_pointer_bytes = guest_pointer_bytes;
        value.instance_id = instance_id;
        value.message = debug_utils_wire::decode(payload);
        return encode(value, payload);
    }
} // namespace sogen::gpu_bridge::debug_utils_messenger_wire
