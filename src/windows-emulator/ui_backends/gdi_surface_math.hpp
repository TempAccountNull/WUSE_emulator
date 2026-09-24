#pragma once

#include <platform/ui_backend.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace sogen::gdi_detail
{
    struct surface
    {
        int width{};
        int height{};
        std::vector<uint8_t> bgra;
    };

    inline std::optional<surface> copy_surface(const ui_surface_desc& source)
    {
        if (!source.pixels || source.width <= 0 || source.height <= 0 || source.width > 16384 || source.height > 16384 ||
            source.stride < static_cast<int64_t>(source.width) * 4)
        {
            return std::nullopt;
        }
        const auto count = static_cast<size_t>(source.width) * static_cast<size_t>(source.height);
        if (count > 64U * 1024U * 1024U)
        {
            return std::nullopt;
        }
        surface result{source.width, source.height, std::vector<uint8_t>(count * 4)};
        const auto* input = static_cast<const uint8_t*>(source.pixels);
        for (int y = 0; y < source.height; ++y)
        {
            const auto* row = input + static_cast<size_t>(y) * static_cast<size_t>(source.stride);
            auto* output = result.bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(source.width) * 4;
            for (int x = 0; x < source.width; ++x)
            {
                const auto* pixel = row + static_cast<size_t>(x) * 4;
                auto* dst = output + static_cast<size_t>(x) * 4;
                dst[0] = source.format == ui_surface_format::rgba8 ? pixel[2] : pixel[0];
                dst[1] = pixel[1];
                dst[2] = source.format == ui_surface_format::rgba8 ? pixel[0] : pixel[2];
                dst[3] = pixel[3];
            }
        }
        return result;
    }

    struct fit_rect
    {
        int x{};
        int y{};
        int width{};
        int height{};
    };

    inline fit_rect letterbox(const int destination_width, const int destination_height, const int source_width, const int source_height)
    {
        if (destination_width <= 0 || destination_height <= 0 || source_width <= 0 || source_height <= 0)
        {
            return {};
        }
        int width = destination_width;
        int height = static_cast<int>(std::min<int64_t>(destination_height,
            static_cast<int64_t>(destination_width) * source_height / source_width));
        if (height == destination_height)
        {
            width = static_cast<int>(std::min<int64_t>(destination_width,
                static_cast<int64_t>(destination_height) * source_width / source_height));
        }
        width = std::max(width, 1);
        height = std::max(height, 1);
        return {(destination_width - width) / 2, (destination_height - height) / 2, width, height};
    }

    inline int map_coordinate(const int host, const int offset, const int fitted_size, const int guest_size)
    {
        if (fitted_size <= 0 || guest_size <= 0)
        {
            return 0;
        }
        return static_cast<int>(static_cast<int64_t>(host - offset) * guest_size / fitted_size);
    }
}
