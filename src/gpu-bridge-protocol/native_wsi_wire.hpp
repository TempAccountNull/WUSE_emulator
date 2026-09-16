#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace sogen::gpu_bridge::native_wsi
{
    inline constexpr uint32_t ioctl = (0x22u << 16) | (0x8D8u << 2);
    inline constexpr uint32_t version = 1;
    inline constexpr size_t max_packet = 1024 * 1024;

    enum class operation : uint32_t
    {
        enabled,
        create_surface,
        destroy_surface,
        query_surface,
        create_swapchain,
        destroy_swapchain,
        images,
        acquire,
        present,
        destroy_device,
        destroy_instance,
    };

    enum class query : uint32_t
    {
        capabilities,
        support,
        formats,
        modes,
        win32_support,
    };

    // All handle fields are bridge IDs or a guest HWND key, never native pointers. words has
    // operation-specific scalar fields; trailing arrays contain fixed-width values only.
    struct alignas(8) request
    {
        uint32_t protocol{version};
        operation op{};
        uint32_t bytes{120};
        uint32_t reserved{};
        uint64_t owner{};
        uint64_t object{};
        uint64_t related{};
        uint64_t timeout{};
        uint64_t extra{};
        std::array<uint32_t, 16> words{};
    };

    struct alignas(8) response
    {
        int32_t result{};
        uint32_t count{};
        uint64_t object{};
        uint32_t payload_bytes{};
        uint32_t reserved{};
    };

    struct alignas(8) present_entry
    {
        uint64_t swapchain{};
        uint32_t image_index{};
        uint32_t reserved{};
    };

    enum swapchain_word : size_t
    {
        flags,
        min_images,
        format,
        color_space,
        width,
        height,
        layers,
        usage,
        sharing,
        transform,
        alpha,
        present_mode,
        clipped,
        family_count,
        format_count,
        swapchain_reserved,
    };

    inline bool append_size(size_t& size, const uint32_t count, const size_t stride) noexcept
    {
        if (size > max_packet || stride == 0 || count > (max_packet - size) / stride)
        {
            return false;
        }
        size += static_cast<size_t>(count) * stride;
        return true;
    }

    inline bool decode(const std::span<const std::byte> packet, request& value) noexcept
    {
        if (packet.size() < sizeof(request) || packet.size() > max_packet)
        {
            return false;
        }
        std::memcpy(&value, packet.data(), sizeof(value));
        if (value.protocol != version || value.reserved != 0 || value.bytes != packet.size() || value.op > operation::destroy_instance)
        {
            return false;
        }
        size_t expected = sizeof(request);
        if (value.op == operation::create_swapchain)
        {
            if (value.words[swapchain_reserved] != 0 || !append_size(expected, value.words[family_count], sizeof(uint32_t)) ||
                !append_size(expected, value.words[format_count], sizeof(uint32_t)))
            {
                return false;
            }
        }
        else if (value.op == operation::present)
        {
            if (!append_size(expected, value.words[0], sizeof(uint64_t)) || !append_size(expected, value.words[1], sizeof(present_entry)))
            {
                return false;
            }
            const auto offset = sizeof(request) + static_cast<size_t>(value.words[0]) * sizeof(uint64_t);
            if (expected != packet.size())
            {
                return false;
            }
            for (uint32_t i = 0; i < value.words[1]; ++i)
            {
                present_entry entry{};
                std::memcpy(&entry, packet.data() + offset + static_cast<size_t>(i) * sizeof(entry), sizeof(entry));
                if (entry.reserved != 0)
                {
                    return false;
                }
            }
        }
        return expected == packet.size();
    }

    inline std::vector<std::byte> encode(request value, const std::span<const std::byte> trailing = {})
    {
        if (trailing.size() > max_packet - sizeof(value))
        {
            return {};
        }
        value.bytes = static_cast<uint32_t>(sizeof(value) + trailing.size());
        std::vector<std::byte> bytes(value.bytes);
        std::memcpy(bytes.data(), &value, sizeof(value));
        if (!trailing.empty())
        {
            std::memcpy(bytes.data() + sizeof(value), trailing.data(), trailing.size());
        }
        return bytes;
    }

    static_assert(std::endian::native == std::endian::little);
    static_assert(sizeof(request) == 120 && alignof(request) == 8);
    static_assert(offsetof(request, owner) == 16 && offsetof(request, timeout) == 40 && offsetof(request, words) == 56);
    static_assert(sizeof(response) == 24 && offsetof(response, object) == 8);
    static_assert(sizeof(present_entry) == 16 && offsetof(present_entry, image_index) == 8);
}
