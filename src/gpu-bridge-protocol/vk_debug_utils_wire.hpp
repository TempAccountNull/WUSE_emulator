#pragma once

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace sogen::gpu_bridge::debug_utils_wire
{
    inline constexpr uint32_t magic = 0x55444256;
    inline constexpr uint32_t version = 1;
    inline constexpr uint32_t null_string_length = UINT32_MAX;
    inline constexpr size_t header_size = 52;
    inline constexpr size_t max_packet_bytes = 1024 * 1024;
    inline constexpr size_t max_message_bytes = 256 * 1024;
    inline constexpr size_t max_name_bytes = 64 * 1024;
    inline constexpr uint32_t max_labels = 256;
    inline constexpr uint32_t max_objects = 1024;

    struct callback_storage
    {
        VkDebugUtilsMessageSeverityFlagBitsEXT severity{};
        VkDebugUtilsMessageTypeFlagsEXT types{};
        VkDebugUtilsMessengerCallbackDataEXT data{};
        std::vector<VkDebugUtilsLabelEXT> queue_labels;
        std::vector<VkDebugUtilsLabelEXT> command_labels;
        std::vector<VkDebugUtilsObjectNameInfoEXT> objects;
        std::vector<std::vector<char>> strings;
        std::optional<VkDeviceAddressBindingCallbackDataEXT> address_binding;

        callback_storage() = default;
        callback_storage(const callback_storage&) = delete;
        callback_storage& operator=(const callback_storage&) = delete;
        callback_storage(callback_storage&&) = delete;
        callback_storage& operator=(callback_storage&&) = delete;
    };

    inline void validate_message_class(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types)
    {
        switch (severity)
        {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            break;
        default:
            throw std::invalid_argument("invalid debug-utils message severity");
        }
        constexpr uint32_t supported_types = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
        if (types == 0 || (types & ~supported_types) != 0)
        {
            throw std::invalid_argument("invalid debug-utils message types");
        }
    }

    inline void validate_address_binding(const VkDeviceAddressBindingCallbackDataEXT& binding)
    {
        if (binding.sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT || binding.pNext != nullptr ||
            (binding.flags & ~static_cast<VkDeviceAddressBindingFlagsEXT>(VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT)) != 0 ||
            (binding.bindingType != VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT &&
             binding.bindingType != VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT))
        {
            throw std::invalid_argument("unsupported device-address binding callback data");
        }
    }

    inline std::optional<std::string_view> bounded_text(const char* text, size_t limit)
    {
        if (!text)
        {
            return std::nullopt;
        }
        for (size_t length = 0; length <= limit; ++length)
        {
            if (text[length] == '\0')
            {
                return std::string_view(text, length);
            }
        }
        throw std::length_error("debug-utils callback string exceeds wire limit");
    }

    class writer
    {
      public:
        std::vector<std::byte> bytes;

        void u32(uint32_t value)
        {
            require(4);
            for (unsigned shift = 0; shift < 32; shift += 8)
            {
                bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
            }
        }

        void u64(uint64_t value)
        {
            require(8);
            for (unsigned shift = 0; shift < 64; shift += 8)
            {
                bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
            }
        }

        void text(std::optional<std::string_view> value)
        {
            if (!value)
            {
                u32(null_string_length);
                return;
            }
            u32(static_cast<uint32_t>(value->size()));
            text_payload(value);
        }

        void text_payload(std::optional<std::string_view> value)
        {
            if (!value)
            {
                return;
            }
            require(value->size());
            for (char character : *value)
            {
                bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
            }
        }

        void overwrite_u32(size_t offset, uint32_t value)
        {
            for (unsigned shift = 0; shift < 32; shift += 8)
            {
                bytes[offset + shift / 8] = static_cast<std::byte>((value >> shift) & 0xff);
            }
        }

      private:
        void require(size_t additional) const
        {
            if (additional > max_packet_bytes - bytes.size())
            {
                throw std::length_error("debug-utils callback packet exceeds wire limit");
            }
        }
    };

    class reader
    {
      public:
        explicit reader(std::span<const std::byte> input)
            : input_(input)
        {
            if (input.size() > max_packet_bytes)
            {
                throw std::length_error("debug-utils callback packet exceeds wire limit");
            }
        }

        uint32_t u32()
        {
            uint32_t value = 0;
            for (unsigned shift = 0; shift < 32; shift += 8)
            {
                value |= static_cast<uint32_t>(std::to_integer<uint8_t>(take(1)[0])) << shift;
            }
            return value;
        }

        uint64_t u64()
        {
            uint64_t value = 0;
            for (unsigned shift = 0; shift < 64; shift += 8)
            {
                value |= static_cast<uint64_t>(std::to_integer<uint8_t>(take(1)[0])) << shift;
            }
            return value;
        }

        const char* text(uint32_t length, size_t maximum, callback_storage& owner)
        {
            if (length == null_string_length)
            {
                return nullptr;
            }
            if (length > maximum)
            {
                throw std::length_error("debug-utils callback string exceeds wire limit");
            }
            const auto source = take(length);
            if (std::find(source.begin(), source.end(), std::byte{0}) != source.end())
            {
                throw std::invalid_argument("debug-utils callback string contains an embedded terminator");
            }
            auto& destination = owner.strings.emplace_back(length + 1);
            std::memcpy(destination.data(), source.data(), source.size());
            destination[length] = '\0';
            return destination.data();
        }

        size_t remaining() const
        {
            return input_.size() - offset_;
        }

      private:
        std::span<const std::byte> take(size_t length)
        {
            if (length > remaining())
            {
                throw std::invalid_argument("truncated debug-utils callback packet");
            }
            const auto result = input_.subspan(offset_, length);
            offset_ += length;
            return result;
        }

        std::span<const std::byte> input_;
        size_t offset_{};
    };

    inline std::vector<std::byte> encode(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                                         const VkDebugUtilsMessengerCallbackDataEXT& data)
    {
        validate_message_class(severity, types);
        if (data.sType != VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT || data.flags != 0 ||
            data.queueLabelCount > max_labels || data.cmdBufLabelCount > max_labels || data.objectCount > max_objects ||
            (data.queueLabelCount && !data.pQueueLabels) || (data.cmdBufLabelCount && !data.pCmdBufLabels) ||
            (data.objectCount && !data.pObjects))
        {
            throw std::invalid_argument("invalid debug-utils callback data");
        }
        const auto id_name = bounded_text(data.pMessageIdName, max_name_bytes);
        const auto message = bounded_text(data.pMessage, max_message_bytes);
        const auto* next = static_cast<const VkBaseInStructure*>(data.pNext);
        const VkDeviceAddressBindingCallbackDataEXT* binding = nullptr;
        if (next)
        {
            if (next->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT)
            {
                throw std::invalid_argument("unsupported debug-utils callback pNext type");
            }
            binding = static_cast<const VkDeviceAddressBindingCallbackDataEXT*>(data.pNext);
            validate_address_binding(*binding);
        }

        writer output;
        output.u32(magic);
        output.u32(version);
        output.u32(0);
        output.u32(static_cast<uint32_t>(severity));
        output.u32(types);
        output.u32(data.flags);
        output.u32(static_cast<uint32_t>(data.messageIdNumber));
        output.u32(id_name ? static_cast<uint32_t>(id_name->size()) : null_string_length);
        output.u32(message ? static_cast<uint32_t>(message->size()) : null_string_length);
        output.u32(data.queueLabelCount);
        output.u32(data.cmdBufLabelCount);
        output.u32(data.objectCount);
        output.u32(binding ? VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT : 0);
        output.text_payload(id_name);
        output.text_payload(message);

        const auto append_label = [&](const VkDebugUtilsLabelEXT& label) {
            if (label.sType != VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT || label.pNext)
            {
                throw std::invalid_argument("unsupported debug-utils label pNext");
            }
            const auto name = bounded_text(label.pLabelName, max_name_bytes);
            output.u32(name ? static_cast<uint32_t>(name->size()) : null_string_length);
            for (float component : label.color)
            {
                output.u32(std::bit_cast<uint32_t>(component));
            }
            output.text_payload(name);
        };
        for (uint32_t index = 0; index < data.queueLabelCount; ++index)
        {
            append_label(data.pQueueLabels[index]);
        }
        for (uint32_t index = 0; index < data.cmdBufLabelCount; ++index)
        {
            append_label(data.pCmdBufLabels[index]);
        }
        for (uint32_t index = 0; index < data.objectCount; ++index)
        {
            const auto& object = data.pObjects[index];
            if (object.sType != VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT || object.pNext)
            {
                throw std::invalid_argument("unsupported debug-utils object-name pNext");
            }
            const auto name = bounded_text(object.pObjectName, max_name_bytes);
            output.u32(name ? static_cast<uint32_t>(name->size()) : null_string_length);
            output.u32(static_cast<uint32_t>(object.objectType));
            output.u64(object.objectHandle);
            output.text_payload(name);
        }
        if (binding)
        {
            output.u32(binding->flags);
            output.u64(binding->baseAddress);
            output.u64(binding->size);
            output.u32(static_cast<uint32_t>(binding->bindingType));
        }
        output.overwrite_u32(8, static_cast<uint32_t>(output.bytes.size()));
        return output.bytes;
    }

    inline std::unique_ptr<callback_storage> decode(std::span<const std::byte> packet)
    {
        if (packet.size() < header_size)
        {
            throw std::invalid_argument("truncated debug-utils callback header");
        }
        reader input(packet);
        if (input.u32() != magic || input.u32() != version || input.u32() != packet.size())
        {
            throw std::invalid_argument("invalid debug-utils callback header");
        }
        const auto severity = static_cast<VkDebugUtilsMessageSeverityFlagBitsEXT>(input.u32());
        const VkDebugUtilsMessageTypeFlagsEXT types = input.u32();
        validate_message_class(severity, types);
        const uint32_t flags = input.u32();
        const int32_t message_id_number = static_cast<int32_t>(input.u32());
        const uint32_t id_name_length = input.u32();
        const uint32_t message_length = input.u32();
        const uint32_t queue_count = input.u32();
        const uint32_t command_count = input.u32();
        const uint32_t object_count = input.u32();
        const uint32_t next_type = input.u32();
        if (flags != 0 || queue_count > max_labels || command_count > max_labels || object_count > max_objects ||
            (next_type != 0 && next_type != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) ||
            (id_name_length != null_string_length && id_name_length > max_name_bytes) ||
            (message_length != null_string_length && message_length > max_message_bytes) ||
            static_cast<size_t>(queue_count + command_count) * 20 + static_cast<size_t>(object_count) * 16 + (next_type ? 24 : 0) >
                input.remaining())
        {
            throw std::invalid_argument("invalid debug-utils callback lengths or counts");
        }

        auto result = std::make_unique<callback_storage>();
        result->severity = severity;
        result->types = types;
        auto& data = result->data;
        data.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
        data.flags = flags;
        data.messageIdNumber = message_id_number;
        data.pMessageIdName = input.text(id_name_length, max_name_bytes, *result);
        data.pMessage = input.text(message_length, max_message_bytes, *result);

        const auto read_labels = [&](uint32_t count, std::vector<VkDebugUtilsLabelEXT>& labels) {
            labels.resize(count);
            for (auto& label : labels)
            {
                label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
                const uint32_t name_length = input.u32();
                for (float& component : label.color)
                {
                    component = std::bit_cast<float>(input.u32());
                }
                label.pLabelName = input.text(name_length, max_name_bytes, *result);
            }
        };
        read_labels(queue_count, result->queue_labels);
        read_labels(command_count, result->command_labels);
        data.queueLabelCount = queue_count;
        data.pQueueLabels = result->queue_labels.empty() ? nullptr : result->queue_labels.data();
        data.cmdBufLabelCount = command_count;
        data.pCmdBufLabels = result->command_labels.empty() ? nullptr : result->command_labels.data();

        result->objects.resize(object_count);
        for (auto& object : result->objects)
        {
            object.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
            const uint32_t name_length = input.u32();
            object.objectType = static_cast<VkObjectType>(input.u32());
            object.objectHandle = input.u64();
            object.pObjectName = input.text(name_length, max_name_bytes, *result);
        }
        data.objectCount = object_count;
        data.pObjects = result->objects.empty() ? nullptr : result->objects.data();

        if (next_type)
        {
            result->address_binding.emplace();
            auto& binding = *result->address_binding;
            binding.sType = VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT;
            binding.flags = input.u32();
            binding.baseAddress = input.u64();
            binding.size = input.u64();
            binding.bindingType = static_cast<VkDeviceAddressBindingTypeEXT>(input.u32());
            validate_address_binding(binding);
            data.pNext = &binding;
        }
        if (input.remaining() != 0)
        {
            throw std::invalid_argument("debug-utils callback packet has trailing bytes");
        }
        return result;
    }
}
