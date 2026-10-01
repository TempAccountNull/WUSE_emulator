#include "native_marker_registers.hpp"

namespace sogen::detail
{
    std::string native_marker_raw_hex(const std::span<const std::byte> bytes)
    {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string result;
        result.reserve(bytes.size() * 2);
        for (const auto value : bytes)
        {
            const auto byte = std::to_integer<uint8_t>(value);
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 15]);
        }
        return result;
    }
}
