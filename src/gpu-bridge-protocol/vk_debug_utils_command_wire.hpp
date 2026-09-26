#pragma once

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sogen::gpu_bridge::debug_utils_command_wire
{
    constexpr uint32_t magic = 0x43445556;
    constexpr uint32_t version = 1;
    constexpr size_t max_payload_bytes = 256 * 1024;
    constexpr size_t max_packet_bytes = max_payload_bytes + 64;

    enum class operation : uint32_t
    {
        set_object_name = 1,
        set_object_tag,
        queue_begin_label,
        queue_insert_label,
        queue_end_label,
        command_begin_label,
        command_insert_label,
        command_end_label,
    };

    struct command
    {
        operation op{};
        uint64_t dispatch_id{};
        uint64_t object_id{};
        VkObjectType object_type{VK_OBJECT_TYPE_UNKNOWN};
        uint64_t tag_name{};
        std::array<float, 4> color{};
        bool null_name{};
        std::string name;
        std::vector<std::byte> tag;
    };

    inline bool valid_operation(operation op)
    {
        return op >= operation::set_object_name && op <= operation::command_end_label;
    }

    inline void validate(const command& value)
    {
        if (!valid_operation(value.op) || value.dispatch_id == 0 || value.name.size() > max_payload_bytes ||
            value.tag.size() > max_payload_bytes || value.name.find('\0') != std::string::npos)
        {
            throw std::invalid_argument("invalid debug-utils command");
        }
        switch (value.op)
        {
        case operation::set_object_name:
            if ((value.object_type == VK_OBJECT_TYPE_UNKNOWN && value.object_id == 0) || !value.tag.empty() ||
                (value.null_name && !value.name.empty()))
            {
                throw std::invalid_argument("invalid debug-utils object name");
            }
            break;
        case operation::set_object_tag:
            if (value.object_id == 0 || value.object_type == VK_OBJECT_TYPE_UNKNOWN || value.tag.empty() || !value.name.empty() ||
                value.null_name)
            {
                throw std::invalid_argument("invalid debug-utils object tag");
            }
            break;
        case operation::queue_begin_label:
        case operation::queue_insert_label:
        case operation::command_begin_label:
        case operation::command_insert_label:
            if (value.null_name || value.object_id || !value.tag.empty())
            {
                throw std::invalid_argument("invalid debug-utils label");
            }
            break;
        case operation::queue_end_label:
        case operation::command_end_label:
            if (value.object_id || !value.name.empty() || !value.tag.empty() || value.null_name)
            {
                throw std::invalid_argument("invalid debug-utils label end");
            }
            break;
        }
    }

    inline std::vector<std::byte> encode(const command& value)
    {
        validate(value);
        const auto payload = value.op == operation::set_object_tag ? std::span<const std::byte>(value.tag)
                                                                   : std::as_bytes(std::span(value.name.data(), value.name.size()));
        if (payload.size() > max_payload_bytes)
        {
            throw std::length_error("debug-utils command exceeds wire limit");
        }
        std::vector<std::byte> result;
        result.reserve(64 + payload.size());
        const auto u32 = [&](uint32_t number) {
            for (unsigned shift = 0; shift != 32; shift += 8)
            {
                result.push_back(static_cast<std::byte>((number >> shift) & 0xff));
            }
        };
        const auto u64 = [&](uint64_t number) {
            u32(static_cast<uint32_t>(number));
            u32(static_cast<uint32_t>(number >> 32));
        };
        u32(magic);
        u32(version);
        u32(static_cast<uint32_t>(value.op));
        u32(value.null_name ? 1 : 0);
        u64(value.dispatch_id);
        u64(value.object_id);
        u32(static_cast<uint32_t>(value.object_type));
        u64(value.tag_name);
        for (float component : value.color)
        {
            u32(std::bit_cast<uint32_t>(component));
        }
        u32(static_cast<uint32_t>(payload.size()));
        result.insert(result.end(), payload.begin(), payload.end());
        return result;
    }

    inline command decode(std::span<const std::byte> bytes)
    {
        if (bytes.size() < 64 || bytes.size() > max_packet_bytes)
        {
            throw std::invalid_argument("invalid debug-utils command packet length");
        }
        size_t cursor = 0;
        const auto u32 = [&]() {
            uint32_t number = 0;
            for (unsigned shift = 0; shift != 32; shift += 8)
            {
                number |= static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[cursor++])) << shift;
            }
            return number;
        };
        const auto u64 = [&]() {
            const uint64_t low = u32();
            return low | (static_cast<uint64_t>(u32()) << 32);
        };
        if (u32() != magic || u32() != version)
        {
            throw std::invalid_argument("invalid debug-utils command version");
        }
        command value{};
        value.op = static_cast<operation>(u32());
        const uint32_t flags = u32();
        if (flags & ~uint32_t{1})
        {
            throw std::invalid_argument("invalid debug-utils command flags");
        }
        value.null_name = flags != 0;
        value.dispatch_id = u64();
        value.object_id = u64();
        value.object_type = static_cast<VkObjectType>(u32());
        value.tag_name = u64();
        for (float& component : value.color)
        {
            component = std::bit_cast<float>(u32());
        }
        const uint32_t length = u32();
        if (length > max_payload_bytes || bytes.size() - cursor != length)
        {
            throw std::invalid_argument("invalid debug-utils command payload length");
        }
        if (value.op == operation::set_object_tag)
        {
            value.tag.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.end());
        }
        else
        {
            value.name.assign(reinterpret_cast<const char*>(bytes.data() + cursor), length);
        }
        validate(value);
        return value;
    }

    inline std::string_view bounded_name(const char* name)
    {
        if (!name)
        {
            throw std::invalid_argument("missing debug-utils name");
        }
        for (size_t length = 0; length <= max_payload_bytes; ++length)
        {
            if (name[length] == '\0')
            {
                return {name, length};
            }
        }
        throw std::length_error("debug-utils name exceeds wire limit");
    }

    inline std::vector<std::byte> marshal_object_name(uint64_t device_id, const VkDebugUtilsObjectNameInfoEXT& info)
    {
        if (info.sType != VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT || info.pNext)
        {
            throw std::invalid_argument("unsupported debug-utils object name structure");
        }
        command value{};
        value.op = operation::set_object_name;
        value.dispatch_id = device_id;
        value.object_id = info.objectHandle;
        value.object_type = info.objectType;
        value.null_name = info.pObjectName == nullptr;
        if (info.pObjectName)
        {
            value.name = bounded_name(info.pObjectName);
        }
        return encode(value);
    }

    inline std::vector<std::byte> marshal_object_tag(uint64_t device_id, const VkDebugUtilsObjectTagInfoEXT& info)
    {
        if (info.sType != VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT || info.pNext || info.tagSize > max_payload_bytes ||
            !info.pTag || info.tagSize == 0)
        {
            throw std::invalid_argument("unsupported debug-utils object tag structure");
        }
        command value{};
        value.op = operation::set_object_tag;
        value.dispatch_id = device_id;
        value.object_id = info.objectHandle;
        value.object_type = info.objectType;
        value.tag_name = info.tagName;
        auto* const tag_bytes = static_cast<const std::byte*>(info.pTag);
        value.tag.assign(tag_bytes, tag_bytes + info.tagSize);
        return encode(value);
    }

    inline std::vector<std::byte> marshal_label(operation op, uint64_t target_id, const VkDebugUtilsLabelEXT& info)
    {
        if (op != operation::queue_begin_label && op != operation::queue_insert_label && op != operation::command_begin_label &&
            op != operation::command_insert_label)
        {
            throw std::invalid_argument("unsupported debug-utils label operation");
        }
        if (info.sType != VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT || info.pNext)
        {
            throw std::invalid_argument("unsupported debug-utils label structure");
        }
        command value{};
        value.op = op;
        value.dispatch_id = target_id;
        value.name = bounded_name(info.pLabelName);
        std::copy(std::begin(info.color), std::end(info.color), value.color.begin());
        return encode(value);
    }

    inline std::vector<std::byte> marshal_label_end(operation op, uint64_t target_id)
    {
        if (op != operation::queue_end_label && op != operation::command_end_label)
        {
            throw std::invalid_argument("unsupported debug-utils label end operation");
        }
        command value{};
        value.op = op;
        value.dispatch_id = target_id;
        return encode(value);
    }
}
