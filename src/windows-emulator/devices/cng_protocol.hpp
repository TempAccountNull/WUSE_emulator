#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sogen::cng
{
    inline constexpr uint32_t transport_magic = 0x1a2b3c4d;
    inline constexpr uint32_t resolve_providers = 0x20000;
    inline constexpr uint32_t register_notification = 0x10500;
    inline constexpr uint32_t unregister_notification = 0x10501;

    struct resolution_request
    {
        std::optional<std::u16string> context;
        uint32_t interface_id{};
        std::optional<std::u16string> function;
        std::optional<std::u16string> provider;
        uint32_t mode{};
        uint32_t flags{};
    };

    struct property_reference
    {
        std::u16string name;
        std::vector<uint8_t> value;
    };

    struct image_reference
    {
        std::optional<std::u16string> name;
        uint32_t flags{};
    };

    struct provider_reference
    {
        uint32_t interface_id{};
        std::optional<std::u16string> function;
        std::u16string provider;
        std::vector<property_reference> properties;
        std::optional<image_reference> user_image;
        std::optional<image_reference> kernel_image;
    };

    std::optional<resolution_request> unpack_resolution(std::span<const uint8_t> input);
    std::optional<uint64_t> unpack_notification(std::span<const uint8_t> input);
    std::vector<uint8_t> pack_provider_refs(std::span<const provider_reference> providers);
}
