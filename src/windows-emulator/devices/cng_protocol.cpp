#include "../std_include.hpp"
#include "cng_protocol.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace sogen::cng
{
    namespace
    {
        constexpr auto null_offset = std::numeric_limits<uint64_t>::max();

        template <typename T>
        T read_value(const std::span<const uint8_t> input, const size_t offset)
        {
            T value{};
            memcpy(&value, input.data() + offset, sizeof(value));
            return value;
        }

        struct occupied_bytes
        {
            std::vector<std::pair<size_t, size_t>> ranges;
            size_t size;

            bool claim(const size_t start, const size_t count)
            {
                if (start > size || count > size - start)
                {
                    return false;
                }
                for (const auto& [offset, length] : ranges)
                {
                    if (start < offset + length && offset < start + count)
                    {
                        return false;
                    }
                }
                ranges.emplace_back(start, count);
                return true;
            }
        };

        bool read_string(const std::span<const uint8_t> input, const size_t field, std::optional<std::u16string>& output,
                         occupied_bytes& occupied)
        {
            if (!occupied.claim(field, sizeof(uint64_t)))
            {
                return false;
            }
            const auto offset = read_value<uint64_t>(input, field);
            if (offset == null_offset)
            {
                output.reset();
                return true;
            }
            if (offset >= input.size())
            {
                return false;
            }
            std::u16string value;
            for (auto current = static_cast<size_t>(offset); input.size() - current >= sizeof(char16_t); current += sizeof(char16_t))
            {
                const auto character = read_value<char16_t>(input, current);
                if (character == u'\0')
                {
                    if (!occupied.claim(offset, current - offset + sizeof(char16_t)))
                    {
                        return false;
                    }
                    output = std::move(value);
                    return true;
                }
                value.push_back(character);
            }
            return false;
        }

        class response_builder
        {
          public:
            size_t allocate(const size_t count)
            {
                constexpr auto maximum = std::numeric_limits<uint32_t>::max();
                if (count > maximum || data_.size() > maximum - count || data_.size() + count > maximum - 7u)
                {
                    throw std::length_error("CNG response exceeds transport length");
                }
                const auto offset = data_.size();
                data_.resize((offset + count + 7u) & ~size_t{7});
                return offset;
            }

            template <typename T>
            void write(const size_t offset, const T value)
            {
                memcpy(data_.data() + offset, &value, sizeof(value));
            }

            uint64_t string(const std::u16string& value)
            {
                if (value.size() > (std::numeric_limits<uint32_t>::max() / sizeof(char16_t)) - 1)
                {
                    throw std::length_error("CNG string exceeds transport length");
                }
                const auto offset = allocate((value.size() + 1) * sizeof(char16_t));
                memcpy(data_.data() + offset, value.c_str(), (value.size() + 1) * sizeof(char16_t));
                return offset;
            }

            uint64_t string(const std::optional<std::u16string>& value)
            {
                return value ? string(*value) : null_offset;
            }

            uint64_t image(const std::optional<image_reference>& value)
            {
                if (!value)
                {
                    return null_offset;
                }
                const auto offset = allocate(16);
                const auto name = string(value->name);
                write(offset, name);
                write(offset + 8, value->flags);
                return offset;
            }

            uint64_t properties(const std::vector<property_reference>& values)
            {
                if (values.empty())
                {
                    return null_offset;
                }
                const auto array = allocate(values.size() * sizeof(uint64_t));
                for (size_t i = 0; i < values.size(); ++i)
                {
                    const auto& property = values[i];
                    const auto offset = allocate(24);
                    write<uint64_t>(array + i * sizeof(uint64_t), offset);
                    const auto name = string(property.name);
                    write(offset, name);
                    write<uint32_t>(offset + 8, static_cast<uint32_t>(property.value.size()));
                    auto value = null_offset;
                    if (!property.value.empty())
                    {
                        value = allocate(property.value.size());
                        memcpy(data_.data() + value, property.value.data(), property.value.size());
                    }
                    write(offset + 16, value);
                }
                return array;
            }

            std::vector<uint8_t> take()
            {
                return std::move(data_);
            }

          private:
            std::vector<uint8_t> data_;
        };
    }

    std::optional<resolution_request> unpack_resolution(const std::span<const uint8_t> input)
    {
        if (input.size() < 8 || read_value<uint32_t>(input, 0) != transport_magic || read_value<uint32_t>(input, 4) != resolve_providers)
        {
            return std::nullopt;
        }
        resolution_request request{};
        if (input.size() == 8)
        {
            return request;
        }
        if (input.size() < 48)
        {
            return std::nullopt;
        }
        occupied_bytes occupied{.ranges = {{0, 8}, {16, 4}, {40, 8}}, .size = input.size()};
        request.interface_id = read_value<uint32_t>(input, 16);
        request.mode = read_value<uint32_t>(input, 40);
        request.flags = read_value<uint32_t>(input, 44);
        if (!read_string(input, 8, request.context, occupied) || !read_string(input, 24, request.function, occupied) ||
            !read_string(input, 32, request.provider, occupied))
        {
            return std::nullopt;
        }
        return request;
    }

    std::optional<uint64_t> unpack_notification(const std::span<const uint8_t> input)
    {
        if (input.size() < 8 || read_value<uint32_t>(input, 0) != transport_magic)
        {
            return std::nullopt;
        }
        const auto operation = read_value<uint32_t>(input, 4);
        if (operation != register_notification && operation != unregister_notification)
        {
            return std::nullopt;
        }
        if (input.size() == 8)
        {
            return uint64_t{};
        }
        if (input.size() < 104)
        {
            return std::nullopt;
        }
        occupied_bytes occupied{.ranges = {{0, 12}, {24, 4}, {56, 4}, {80, 4}, {96, 8}}, .size = input.size()};
        std::optional<std::u16string> unused;
        for (const auto field : {16u, 32u, 40u, 48u})
        {
            if (!read_string(input, field, unused, occupied))
            {
                return std::nullopt;
            }
        }
        const auto valid_buffer = [&](const size_t field, const uint32_t count) {
            if (!occupied.claim(field, 8))
            {
                return false;
            }
            const auto offset = read_value<uint64_t>(input, field);
            if (offset == null_offset)
            {
                return count == 0;
            }
            return occupied.claim(offset, count);
        };
        for (const auto field : {64u, 72u})
        {
            const auto count = read_value<uint64_t>(input, field) == null_offset ? 0u : 8u;
            if (!valid_buffer(field, count))
            {
                return std::nullopt;
            }
        }
        if (!valid_buffer(88, read_value<uint32_t>(input, 80)))
        {
            return std::nullopt;
        }
        return read_value<uint64_t>(input, 96);
    }

    std::vector<uint8_t> pack_provider_refs(const std::span<const provider_reference> providers)
    {
        // bcrypt 10.0.19041 uses 64-bit relative pointers on both architectures; WOW64 compacts them during unpacking.
        response_builder output;
        output.allocate(16);
        output.write<uint32_t>(0, static_cast<uint32_t>(providers.size()));
        const auto array = providers.empty() ? null_offset : output.allocate(providers.size() * sizeof(uint64_t));
        output.write(8, array);
        for (size_t i = 0; i < providers.size(); ++i)
        {
            const auto& provider = providers[i];
            const auto offset = output.allocate(56);
            output.write<uint64_t>(array + i * sizeof(uint64_t), offset);
            output.write(offset, provider.interface_id);
            const auto function = output.string(provider.function);
            const auto name = output.string(provider.provider);
            const auto properties = output.properties(provider.properties);
            const auto user_image = output.image(provider.user_image);
            const auto kernel_image = output.image(provider.kernel_image);
            output.write(offset + 8, function);
            output.write(offset + 16, name);
            output.write<uint32_t>(offset + 24, static_cast<uint32_t>(provider.properties.size()));
            output.write(offset + 32, properties);
            output.write(offset + 40, user_image);
            output.write(offset + 48, kernel_image);
        }
        return output.take();
    }
}
