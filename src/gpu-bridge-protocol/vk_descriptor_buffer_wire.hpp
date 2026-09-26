#pragma once

#include <vulkan/vulkan_core.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace sogen::gpu_bridge::descriptor_buffer_wire
{
    inline constexpr size_t get_info_byte_count = 40;
    inline constexpr uint64_t max_descriptor_bytes = 64 * 1024;

    enum class payload_kind : uint32_t
    {
        sampler = 1,
        image = 2,
        address = 3,
        acceleration_structure = 4,
    };

    struct get_info
    {
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        payload_kind kind = payload_kind::sampler;
        bool present = false;
        std::array<uint64_t, 3> values{};
    };

    using get_info_bytes = std::array<std::byte, get_info_byte_count>;

    inline payload_kind kind_for(VkDescriptorType type)
    {
        switch (type)
        {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            return payload_kind::sampler;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            return payload_kind::image;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return payload_kind::address;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
            return payload_kind::acceleration_structure;
        default:
            throw std::invalid_argument("unsupported VkDescriptorGetInfoEXT descriptor type");
        }
    }

    inline void validate(const get_info& info, bool null_descriptor_enabled)
    {
        if (info.kind != kind_for(info.type))
        {
            throw std::invalid_argument("VkDescriptorDataEXT payload does not match descriptor type");
        }
        if (!info.present)
        {
            if (!null_descriptor_enabled || (info.type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
                                             info.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && info.kind != payload_kind::address))
            {
                throw std::invalid_argument("descriptor type requires a present data payload");
            }
            if (info.values != std::array<uint64_t, 3>{})
            {
                throw std::invalid_argument("null descriptor payload has nonzero fields");
            }
            return;
        }
        if (info.kind == payload_kind::sampler || info.kind == payload_kind::acceleration_structure)
        {
            if (info.values[1] != 0 || info.values[2] != 0)
            {
                throw std::invalid_argument("descriptor payload has nonzero unused fields");
            }
        }
        if (info.kind == payload_kind::image)
        {
            if (info.values[2] > std::numeric_limits<uint32_t>::max() ||
                (info.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && info.values[0] != 0))
            {
                throw std::invalid_argument("invalid image descriptor payload");
            }
        }
        if (info.kind == payload_kind::address && info.values[2] > std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument("invalid descriptor address format");
        }
        if (info.kind == payload_kind::sampler && info.values[0] == 0)
        {
            throw std::invalid_argument("sampler descriptor requires a sampler handle");
        }
        if (info.kind == payload_kind::image)
        {
            if (info.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && info.values[0] == 0)
            {
                throw std::invalid_argument("combined image descriptor requires a sampler handle");
            }
            if (info.values[1] == 0 && (info.type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT || !null_descriptor_enabled))
            {
                throw std::invalid_argument("image descriptor requires a valid image view");
            }
        }
        if ((info.kind == payload_kind::address || info.kind == payload_kind::acceleration_structure) && info.values[0] == 0 &&
            !null_descriptor_enabled)
        {
            throw std::invalid_argument("descriptor requires an address unless nullDescriptor is enabled");
        }
    }

    inline void validate_descriptor_data_size(uint64_t data_size, uint64_t native_descriptor_size)
    {
        if (data_size == 0 || data_size > max_descriptor_bytes || native_descriptor_size != data_size)
        {
            throw std::invalid_argument("descriptor data size does not match native Vulkan properties");
        }
    }

    inline void write_integer(get_info_bytes& bytes, size_t offset, uint64_t value, size_t width)
    {
        for (size_t i = 0; i < width; ++i)
        {
            bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xff);
        }
    }

    inline uint64_t read_integer(std::span<const std::byte> bytes, size_t offset, size_t width)
    {
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i)
        {
            value |= static_cast<uint64_t>(std::to_integer<uint8_t>(bytes[offset + i])) << (i * 8);
        }
        return value;
    }

    template <typename MapHandle>
    get_info snapshot(const VkDescriptorGetInfoEXT& source, MapHandle&& map_handle, bool null_descriptor_enabled = false)
    {
        if (source.sType != VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT || source.pNext != nullptr)
        {
            throw std::invalid_argument("unsupported VkDescriptorGetInfoEXT structure");
        }
        get_info info{};
        info.type = source.type;
        info.kind = kind_for(source.type);
        info.present = true;
        switch (info.kind)
        {
        case payload_kind::sampler:
            if (!source.data.pSampler)
            {
                throw std::invalid_argument("missing sampler descriptor payload");
            }
            info.values[0] = map_handle(*source.data.pSampler);
            break;
        case payload_kind::image: {
            const VkDescriptorImageInfo* image = nullptr;
            switch (source.type)
            {
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                image = source.data.pCombinedImageSampler;
                break;
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                image = source.data.pInputAttachmentImage;
                break;
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                image = source.data.pSampledImage;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                image = source.data.pStorageImage;
                break;
            default:
                break;
            }
            if (!image)
            {
                info.present = false;
                break;
            }
            if (source.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            {
                info.values[0] = map_handle(image->sampler);
            }
            info.values[1] = map_handle(image->imageView);
            info.values[2] = static_cast<uint32_t>(image->imageLayout);
            break;
        }
        case payload_kind::address: {
            const VkDescriptorAddressInfoEXT* address = nullptr;
            switch (source.type)
            {
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                address = source.data.pUniformTexelBuffer;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                address = source.data.pStorageTexelBuffer;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                address = source.data.pUniformBuffer;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                address = source.data.pStorageBuffer;
                break;
            default:
                break;
            }
            if (!address)
            {
                info.present = false;
                break;
            }
            if (address->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT || address->pNext != nullptr)
            {
                throw std::invalid_argument("unsupported VkDescriptorAddressInfoEXT structure");
            }
            info.values = {address->address, address->range, static_cast<uint32_t>(address->format)};
            break;
        }
        case payload_kind::acceleration_structure:
            info.values[0] = source.data.accelerationStructure;
            break;
        }
        validate(info, null_descriptor_enabled);
        return info;
    }

    inline get_info_bytes encode(const get_info& info, bool null_descriptor_enabled = false)
    {
        validate(info, null_descriptor_enabled);
        get_info_bytes bytes{};
        write_integer(bytes, 0, static_cast<uint32_t>(info.type), 4);
        write_integer(bytes, 4, static_cast<uint32_t>(info.kind), 4);
        write_integer(bytes, 8, info.present ? 1 : 0, 4);
        for (size_t i = 0; i < info.values.size(); ++i)
        {
            write_integer(bytes, 16 + i * 8, info.values[i], 8);
        }
        return bytes;
    }

    inline get_info decode(std::span<const std::byte> bytes, bool null_descriptor_enabled = false)
    {
        if (bytes.size() != get_info_byte_count || read_integer(bytes, 12, 4) != 0)
        {
            throw std::invalid_argument("invalid descriptor get-info wire length or reserved field");
        }
        const auto present = read_integer(bytes, 8, 4);
        if (present > 1)
        {
            throw std::invalid_argument("invalid descriptor payload presence flag");
        }
        get_info info{};
        info.type = static_cast<VkDescriptorType>(read_integer(bytes, 0, 4));
        info.kind = static_cast<payload_kind>(read_integer(bytes, 4, 4));
        info.present = present != 0;
        for (size_t i = 0; i < info.values.size(); ++i)
        {
            info.values[i] = read_integer(bytes, 16 + i * 8, 8);
        }
        validate(info, null_descriptor_enabled);
        return info;
    }
}
