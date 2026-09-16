#pragma once

// Vulkan pointers and native padding never cross this wire. Both endpoints visit named fields,
// and decoded arrays own their storage until the synchronous driver call has completed.
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace sogen::gpu_bridge::render_pass_wire
{
    inline constexpr size_t max_bytes = 16 * 1024 * 1024;
    inline constexpr uint32_t max_elements = 1024 * 1024;
    inline constexpr uint32_t max_chain = 32;

    class error : public std::runtime_error
    {
      public:
        VkResult result;

        error(const char* message, VkResult code = VK_ERROR_UNKNOWN)
            : std::runtime_error(message),
              result(code)
        {
        }
    };

    template <typename T>
    uint64_t handle_id(T value)
    {
        if constexpr (std::is_pointer_v<T>)
        {
            return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
        }
        else
        {
            return static_cast<uint64_t>(value);
        }
    }

    template <typename T>
    T handle_value(uint64_t value)
    {
        if constexpr (std::is_pointer_v<T>)
        {
            return reinterpret_cast<T>(static_cast<uintptr_t>(value));
        }
        else
        {
            return static_cast<T>(value);
        }
    }

    template <typename T>
    struct structure_type;
    template <typename A, typename T>
    void value(A& archive, T& object);
    template <typename A>
    void chain(A& archive, const void*& next, VkStructureType parent);

    class writer
    {
      public:
        static constexpr bool reading = false;
        std::vector<std::byte> bytes;
        size_t depth = 0;

        template <typename T>
        void scalar(T& object)
        {
            static_assert(sizeof(T) == 4 || sizeof(T) == 8);
            if (sizeof(T) > max_bytes - bytes.size())
            {
                throw error("render-pass packet exceeds byte limit");
            }
            const auto* first = reinterpret_cast<const std::byte*>(&object);
            bytes.insert(bytes.end(), first, first + sizeof(T));
        }

        template <typename T, typename F>
        void array(const T*& objects, uint32_t count, bool optional, F visit)
        {
            if (count > max_elements)
            {
                throw error("render-pass array exceeds element limit");
            }
            uint32_t present = objects && count ? 1 : 0;
            scalar(present);
            if (count && !present && !optional)
            {
                throw error("missing render-pass array");
            }
            if (present)
            {
                for (uint32_t i = 0; i < count; ++i)
                {
                    T copy = objects[i];
                    visit(*this, copy);
                }
            }
        }
    };

    class reader
    {
      public:
        static constexpr bool reading = true;

        explicit reader(std::span<const std::byte> bytes)
            : bytes_(bytes)
        {
            if (bytes.size() > max_bytes)
            {
                throw error("render-pass packet exceeds byte limit");
            }
        }

        size_t depth = 0;

        template <typename T>
        void scalar(T& object)
        {
            static_assert(sizeof(T) == 4 || sizeof(T) == 8);
            if (sizeof(T) > bytes_.size() - offset_)
            {
                throw error("truncated render-pass packet");
            }
            std::memcpy(&object, bytes_.data() + offset_, sizeof(T));
            offset_ += sizeof(T);
        }

        template <typename T>
        T* allocate(uint32_t count)
        {
            if (count > max_elements || count > (max_bytes - allocated_) / sizeof(T))
            {
                throw error("decoded render-pass allocation exceeds limit");
            }
            auto storage = std::make_shared<std::vector<T>>(count);
            auto* pointer = storage->data();
            storage_.emplace_back(storage, pointer);
            allocated_ += sizeof(T) * count;
            return pointer;
        }

        template <typename T, typename F>
        void array(const T*& objects, uint32_t count, bool optional, F visit)
        {
            if (count > max_elements)
            {
                throw error("render-pass array exceeds element limit");
            }
            uint32_t present = 0;
            scalar(present);
            if (present > 1 || (!count && present) || (count && !present && !optional))
            {
                throw error("invalid render-pass array presence");
            }
            objects = nullptr;
            if (present)
            {
                auto* decoded = allocate<T>(count);
                objects = decoded;
                for (uint32_t i = 0; i < count; ++i)
                {
                    visit(*this, decoded[i]);
                }
            }
        }

        void finish() const
        {
            if (offset_ != bytes_.size())
            {
                throw error("trailing render-pass packet bytes");
            }
        }

      private:
        std::span<const std::byte> bytes_;
        size_t offset_ = 0;
        size_t allocated_ = 0;
        std::vector<std::shared_ptr<void>> storage_;
    };

    template <typename A, typename T>
    void handle(A& archive, T& object)
    {
        uint64_t id = handle_id(object);
        archive.scalar(id);
        if constexpr (A::reading)
        {
            object = handle_value<T>(id);
        }
    }

    template <typename A, typename T>
    void array(A& archive, const T*& objects, uint32_t count, bool optional = false)
    {
        archive.array(objects, count, optional, [](auto& a, auto& item) { value(a, item); });
    }

    template <typename A, typename T>
    void handles(A& archive, const T*& objects, uint32_t count, bool optional = false)
    {
        archive.array(objects, count, optional, [](auto& a, auto& item) { handle(a, item); });
    }

    template <typename A>
    void fields(A& a, VkOffset2D& v)
    {
        value(a, v.x);
        value(a, v.y);
    }

    template <typename A>
    void fields(A& a, VkExtent2D& v)
    {
        value(a, v.width);
        value(a, v.height);
    }

    template <typename A>
    void fields(A& a, VkRect2D& v)
    {
        value(a, v.offset);
        value(a, v.extent);
    }

    template <typename A>
    void fields(A& a, VkImageSubresourceRange& v)
    {
        value(a, v.aspectMask);
        value(a, v.baseMipLevel);
        value(a, v.levelCount);
        value(a, v.baseArrayLayer);
        value(a, v.layerCount);
    }

    template <typename A>
    void fields(A& a, VkClearValue& v)
    {
        // The union has no tag. Preserve all 16 bytes, including integer and stencil clear values.
        auto bits = std::bit_cast<std::array<uint32_t, 4>>(v);
        for (auto& word : bits)
        {
            value(a, word);
        }
        if constexpr (A::reading)
        {
            v = std::bit_cast<VkClearValue>(bits);
        }
    }

    template <typename A>
    void fields(A& a, VkAttachmentReference& v)
    {
        value(a, v.attachment);
        value(a, v.layout);
    }

    template <typename A>
    void fields(A& a, VkSampleLocationEXT& v)
    {
        value(a, v.x);
        value(a, v.y);
    }

    template <typename A>
    void fields(A& a, VkAttachmentSampleLocationsEXT& v)
    {
        value(a, v.attachmentIndex);
        value(a, v.sampleLocationsInfo);
    }

    template <typename A>
    void fields(A& a, VkSubpassSampleLocationsEXT& v)
    {
        value(a, v.subpassIndex);
        value(a, v.sampleLocationsInfo);
    }

    template <>
    struct structure_type<VkMemoryBarrier>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    };

    template <typename A>
    void fields(A& a, VkMemoryBarrier& v)
    {
        value(a, v.srcAccessMask);
        value(a, v.dstAccessMask);
    }

    template <>
    struct structure_type<VkBufferMemoryBarrier>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    };

    template <typename A>
    void fields(A& a, VkBufferMemoryBarrier& v)
    {
        value(a, v.srcAccessMask);
        value(a, v.dstAccessMask);
        value(a, v.srcQueueFamilyIndex);
        value(a, v.dstQueueFamilyIndex);
        handle(a, v.buffer);
        value(a, v.offset);
        value(a, v.size);
    }

    template <>
    struct structure_type<VkImageMemoryBarrier>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    };

    template <typename A>
    void fields(A& a, VkImageMemoryBarrier& v)
    {
        value(a, v.srcAccessMask);
        value(a, v.dstAccessMask);
        value(a, v.oldLayout);
        value(a, v.newLayout);
        value(a, v.srcQueueFamilyIndex);
        value(a, v.dstQueueFamilyIndex);
        handle(a, v.image);
        value(a, v.subresourceRange);
    }

    template <>
    struct structure_type<VkBufferMemoryBarrier2>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    };

    template <typename A>
    void fields(A& a, VkBufferMemoryBarrier2& v)
    {
        value(a, v.srcStageMask);
        value(a, v.srcAccessMask);
        value(a, v.dstStageMask);
        value(a, v.dstAccessMask);
        value(a, v.srcQueueFamilyIndex);
        value(a, v.dstQueueFamilyIndex);
        handle(a, v.buffer);
        value(a, v.offset);
        value(a, v.size);
    }

    template <>
    struct structure_type<VkImageMemoryBarrier2>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    };

    template <typename A>
    void fields(A& a, VkImageMemoryBarrier2& v)
    {
        value(a, v.srcStageMask);
        value(a, v.srcAccessMask);
        value(a, v.dstStageMask);
        value(a, v.dstAccessMask);
        value(a, v.oldLayout);
        value(a, v.newLayout);
        value(a, v.srcQueueFamilyIndex);
        value(a, v.dstQueueFamilyIndex);
        handle(a, v.image);
        value(a, v.subresourceRange);
    }

    template <>
    struct structure_type<VkDependencyInfo>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    };

    template <typename A>
    void fields(A& a, VkDependencyInfo& v)
    {
        value(a, v.dependencyFlags);
        value(a, v.memoryBarrierCount);
        array(a, v.pMemoryBarriers, v.memoryBarrierCount);
        value(a, v.bufferMemoryBarrierCount);
        array(a, v.pBufferMemoryBarriers, v.bufferMemoryBarrierCount);
        value(a, v.imageMemoryBarrierCount);
        array(a, v.pImageMemoryBarriers, v.imageMemoryBarrierCount);
    }

    template <>
    struct structure_type<VkExternalMemoryAcquireUnmodifiedEXT>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_ACQUIRE_UNMODIFIED_EXT;
    };

    template <typename A>
    void fields(A& a, VkExternalMemoryAcquireUnmodifiedEXT& v)
    {
        value(a, v.acquireUnmodifiedMemory);
    }

    template <>
    struct structure_type<VkMemoryBarrierAccessFlags3KHR>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_MEMORY_BARRIER_ACCESS_FLAGS_3_KHR;
    };

    template <typename A>
    void fields(A& a, VkMemoryBarrierAccessFlags3KHR& v)
    {
        value(a, v.srcAccessMask3);
        value(a, v.dstAccessMask3);
    }

    template <>
    struct structure_type<VkAttachmentDescription2>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    };

    template <typename A>
    void fields(A& a, VkAttachmentDescription2& v)
    {
        value(a, v.flags);
        value(a, v.format);
        value(a, v.samples);
        value(a, v.loadOp);
        value(a, v.storeOp);
        value(a, v.stencilLoadOp);
        value(a, v.stencilStoreOp);
        value(a, v.initialLayout);
        value(a, v.finalLayout);
    }

    template <>
    struct structure_type<VkAttachmentReference2>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    };

    template <typename A>
    void fields(A& a, VkAttachmentReference2& v)
    {
        value(a, v.attachment);
        value(a, v.layout);
        value(a, v.aspectMask);
    }

    template <>
    struct structure_type<VkSubpassDescription2>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
    };

    template <typename A>
    void fields(A& a, VkSubpassDescription2& v)
    {
        value(a, v.flags);
        value(a, v.pipelineBindPoint);
        value(a, v.viewMask);
        value(a, v.inputAttachmentCount);
        array(a, v.pInputAttachments, v.inputAttachmentCount);
        value(a, v.colorAttachmentCount);
        array(a, v.pColorAttachments, v.colorAttachmentCount);
        array(a, v.pResolveAttachments, v.colorAttachmentCount, true);
        array(a, v.pDepthStencilAttachment, 1, true);
        value(a, v.preserveAttachmentCount);
        array(a, v.pPreserveAttachments, v.preserveAttachmentCount);
    }

    template <>
    struct structure_type<VkSubpassDependency2>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    };

    template <typename A>
    void fields(A& a, VkSubpassDependency2& v)
    {
        value(a, v.srcSubpass);
        value(a, v.dstSubpass);
        value(a, v.srcStageMask);
        value(a, v.dstStageMask);
        value(a, v.srcAccessMask);
        value(a, v.dstAccessMask);
        value(a, v.dependencyFlags);
        value(a, v.viewOffset);
    }

    template <>
    struct structure_type<VkRenderPassCreateInfo2>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
    };

    template <typename A>
    void fields(A& a, VkRenderPassCreateInfo2& v)
    {
        value(a, v.flags);
        value(a, v.attachmentCount);
        array(a, v.pAttachments, v.attachmentCount);
        value(a, v.subpassCount);
        array(a, v.pSubpasses, v.subpassCount);
        value(a, v.dependencyCount);
        array(a, v.pDependencies, v.dependencyCount);
        value(a, v.correlatedViewMaskCount);
        array(a, v.pCorrelatedViewMasks, v.correlatedViewMaskCount);
    }

    template <>
    struct structure_type<VkSubpassBeginInfo>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;
    };

    template <typename A>
    void fields(A& a, VkSubpassBeginInfo& v)
    {
        value(a, v.contents);
    }

    template <>
    struct structure_type<VkSubpassEndInfo>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_SUBPASS_END_INFO;
    };

    template <typename A>
    void fields(A& a, VkSubpassEndInfo& v)
    {
        (void)a;
        (void)v;
    }

    template <>
    struct structure_type<VkRenderPassBeginInfo>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    };

    template <typename A>
    void fields(A& a, VkRenderPassBeginInfo& v)
    {
        handle(a, v.renderPass);
        handle(a, v.framebuffer);
        value(a, v.renderArea);
        value(a, v.clearValueCount);
        array(a, v.pClearValues, v.clearValueCount);
    }

    template <>
    struct structure_type<VkFramebufferCreateInfo>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    };

    template <typename A>
    void fields(A& a, VkFramebufferCreateInfo& v)
    {
        value(a, v.flags);
        handle(a, v.renderPass);
        value(a, v.attachmentCount);
        // Imageless framebuffer attachments are supplied at begin time; pAttachments is ignored.
        if ((v.flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0)
        {
            if constexpr (A::reading)
            {
                v.pAttachments = nullptr;
            }
        }
        else
        {
            handles(a, v.pAttachments, v.attachmentCount);
        }
        value(a, v.width);
        value(a, v.height);
        value(a, v.layers);
    }

    template <>
    struct structure_type<VkAttachmentDescriptionStencilLayout>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT;
    };

    template <typename A>
    void fields(A& a, VkAttachmentDescriptionStencilLayout& v)
    {
        value(a, v.stencilInitialLayout);
        value(a, v.stencilFinalLayout);
    }

    template <>
    struct structure_type<VkAttachmentReferenceStencilLayout>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT;
    };

    template <typename A>
    void fields(A& a, VkAttachmentReferenceStencilLayout& v)
    {
        value(a, v.stencilLayout);
    }

    template <>
    struct structure_type<VkSubpassDescriptionDepthStencilResolve>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
    };

    template <typename A>
    void fields(A& a, VkSubpassDescriptionDepthStencilResolve& v)
    {
        value(a, v.depthResolveMode);
        value(a, v.stencilResolveMode);
        array(a, v.pDepthStencilResolveAttachment, 1, true);
    }

    template <>
    struct structure_type<VkMemoryBarrier2>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    };

    template <typename A>
    void fields(A& a, VkMemoryBarrier2& v)
    {
        value(a, v.srcStageMask);
        value(a, v.srcAccessMask);
        value(a, v.dstStageMask);
        value(a, v.dstAccessMask);
    }

    template <>
    struct structure_type<VkDeviceGroupRenderPassBeginInfo>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO;
    };

    template <typename A>
    void fields(A& a, VkDeviceGroupRenderPassBeginInfo& v)
    {
        value(a, v.deviceMask);
        value(a, v.deviceRenderAreaCount);
        array(a, v.pDeviceRenderAreas, v.deviceRenderAreaCount);
    }

    template <>
    struct structure_type<VkRenderPassAttachmentBeginInfo>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO;
    };

    template <typename A>
    void fields(A& a, VkRenderPassAttachmentBeginInfo& v)
    {
        value(a, v.attachmentCount);
        handles(a, v.pAttachments, v.attachmentCount);
    }

    template <>
    struct structure_type<VkFramebufferAttachmentsCreateInfo>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO;
    };

    template <typename A>
    void fields(A& a, VkFramebufferAttachmentsCreateInfo& v)
    {
        value(a, v.attachmentImageInfoCount);
        array(a, v.pAttachmentImageInfos, v.attachmentImageInfoCount);
    }

    template <>
    struct structure_type<VkFramebufferAttachmentImageInfo>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO;
    };

    template <typename A>
    void fields(A& a, VkFramebufferAttachmentImageInfo& v)
    {
        value(a, v.flags);
        value(a, v.usage);
        value(a, v.width);
        value(a, v.height);
        value(a, v.layerCount);
        value(a, v.viewFormatCount);
        array(a, v.pViewFormats, v.viewFormatCount);
    }

    template <>
    struct structure_type<VkSampleLocationsInfoEXT>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT;
    };

    template <typename A>
    void fields(A& a, VkSampleLocationsInfoEXT& v)
    {
        value(a, v.sampleLocationsPerPixel);
        value(a, v.sampleLocationGridSize);
        value(a, v.sampleLocationsCount);
        array(a, v.pSampleLocations, v.sampleLocationsCount);
    }

    template <>
    struct structure_type<VkRenderPassSampleLocationsBeginInfoEXT>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT;
    };

    template <typename A>
    void fields(A& a, VkRenderPassSampleLocationsBeginInfoEXT& v)
    {
        value(a, v.attachmentInitialSampleLocationsCount);
        array(a, v.pAttachmentInitialSampleLocations, v.attachmentInitialSampleLocationsCount);
        value(a, v.postSubpassSampleLocationsCount);
        array(a, v.pPostSubpassSampleLocations, v.postSubpassSampleLocationsCount);
    }

    template <>
    struct structure_type<VkRenderPassFragmentDensityMapCreateInfoEXT>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT;
    };

    template <typename A>
    void fields(A& a, VkRenderPassFragmentDensityMapCreateInfoEXT& v)
    {
        value(a, v.fragmentDensityMapAttachment);
    }

    template <>
    struct structure_type<VkFragmentShadingRateAttachmentInfoKHR>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
    };

    template <typename A>
    void fields(A& a, VkFragmentShadingRateAttachmentInfoKHR& v)
    {
        array(a, v.pFragmentShadingRateAttachment, 1, true);
        value(a, v.shadingRateAttachmentTexelSize);
    }

    template <>
    struct structure_type<VkMultisampledRenderToSingleSampledInfoEXT>
    {
        static constexpr auto type = VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT;
    };

    template <typename A>
    void fields(A& a, VkMultisampledRenderToSingleSampledInfoEXT& v)
    {
        value(a, v.multisampledRenderToSingleSampledEnable);
        value(a, v.rasterizationSamples);
    }

    template <typename A, typename T>
    void value(A& archive, T& object)
    {
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
        {
            archive.scalar(object);
        }
        else if constexpr (requires { structure_type<T>::type; })
        {
            if (++archive.depth > max_chain)
            {
                throw error("render-pass nesting exceeds limit");
            }
            archive.scalar(object.sType);
            if (object.sType != structure_type<T>::type)
            {
                throw error("unexpected render-pass structure type");
            }
            const void* chain_head = object.pNext;
            chain(archive, chain_head, object.sType);
            if constexpr (A::reading)
            {
                object.pNext = const_cast<void*>(chain_head);
            }
            fields(archive, object);
            --archive.depth;
        }
        else
        {
            fields(archive, object);
        }
    }

    inline bool allowed_chain(VkStructureType type, VkStructureType parent)
    {
        switch (type)
        {
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_ACQUIRE_UNMODIFIED_EXT:
            return parent == VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER || parent == VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 ||
                   parent == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER || parent == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        case VK_STRUCTURE_TYPE_MEMORY_BARRIER_ACCESS_FLAGS_3_KHR:
            return parent == VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2 || parent == VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 ||
                   parent == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        case VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT:
            return parent == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER || parent == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        case VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT:
            return parent == VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        case VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT:
            return parent == VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        case VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE:
            return parent == VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        case VK_STRUCTURE_TYPE_MEMORY_BARRIER_2:
            return parent == VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        case VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO:
            return parent == VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        case VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO:
            return parent == VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        case VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO:
            return parent == VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        case VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT:
            return parent == VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        case VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT:
            return parent == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
        case VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR:
            return parent == VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        case VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT:
            return parent == VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        default:
            return false;
        }
    }

    template <typename A>
    void chain(A& archive, const void*& next, VkStructureType parent)
    {
        std::array<const VkBaseInStructure*, max_chain> nodes{};
        uint32_t count = 0;
        if constexpr (!A::reading)
        {
            for (const auto* node = static_cast<const VkBaseInStructure*>(next); node; node = node->pNext)
            {
                if (count == max_chain)
                {
                    throw error("render-pass extension chain exceeds limit");
                }
                nodes[count++] = node;
            }
        }
        archive.scalar(count);
        if (count > max_chain)
        {
            throw error("render-pass extension chain exceeds limit");
        }
        std::array<VkStructureType, max_chain> seen{};
        VkBaseOutStructure* previous = nullptr;
        if constexpr (A::reading)
        {
            next = nullptr;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            VkStructureType type{};
            if constexpr (!A::reading)
            {
                type = nodes[i]->sType;
            }
            archive.scalar(type);
            if (!allowed_chain(type, parent))
            {
                throw error("unsupported render-pass pNext structure", VK_ERROR_FEATURE_NOT_PRESENT);
            }
            if (std::find(seen.begin(), seen.begin() + i, type) != seen.begin() + i)
            {
                throw error("duplicate render-pass pNext structure");
            }
            seen[i] = type;
            switch (type)
            {
            case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_ACQUIRE_UNMODIFIED_EXT:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkExternalMemoryAcquireUnmodifiedEXT>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkExternalMemoryAcquireUnmodifiedEXT*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_MEMORY_BARRIER_ACCESS_FLAGS_3_KHR:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkMemoryBarrierAccessFlags3KHR>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkMemoryBarrierAccessFlags3KHR*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkAttachmentDescriptionStencilLayout>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkAttachmentDescriptionStencilLayout*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkAttachmentReferenceStencilLayout>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkAttachmentReferenceStencilLayout*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkSubpassDescriptionDepthStencilResolve>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkSubpassDescriptionDepthStencilResolve*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_MEMORY_BARRIER_2:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkMemoryBarrier2>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkMemoryBarrier2*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkDeviceGroupRenderPassBeginInfo>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkDeviceGroupRenderPassBeginInfo*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkRenderPassAttachmentBeginInfo>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkFramebufferAttachmentsCreateInfo>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkFramebufferAttachmentsCreateInfo*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkSampleLocationsInfoEXT>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkSampleLocationsInfoEXT*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkRenderPassSampleLocationsBeginInfoEXT>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkRenderPassSampleLocationsBeginInfoEXT*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkRenderPassFragmentDensityMapCreateInfoEXT>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkRenderPassFragmentDensityMapCreateInfoEXT*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkFragmentShadingRateAttachmentInfoKHR>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkFragmentShadingRateAttachmentInfoKHR*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            case VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT:
                if constexpr (A::reading)
                {
                    auto* node = archive.template allocate<VkMultisampledRenderToSingleSampledInfoEXT>(1);
                    node->sType = type;
                    fields(archive, *node);
                    auto* base = reinterpret_cast<VkBaseOutStructure*>(node);
                    if (previous)
                    {
                        previous->pNext = base;
                    }
                    else
                    {
                        next = node;
                    }
                    previous = base;
                }
                else
                {
                    auto node = *reinterpret_cast<const VkMultisampledRenderToSingleSampledInfoEXT*>(nodes[i]);
                    fields(archive, node);
                }
                break;
            default:
                throw error("unsupported render-pass pNext structure", VK_ERROR_FEATURE_NOT_PRESENT);
            }
        }
    }

    template <typename... T>
    std::vector<std::byte> encode(const T&... objects)
    {
        writer archive;
        // A local copy permits one field visitor without modifying guest input structures.
        (
            [&] {
                auto copy = objects;
                value(archive, copy);
            }(),
            ...);
        return std::move(archive.bytes);
    }

    template <typename... T>
    void decode(reader& archive, T&... objects)
    {
        (value(archive, objects), ...);
        archive.finish();
    }
}
