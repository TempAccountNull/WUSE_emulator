#pragma once

#include <vulkan/vulkan_core.h>
#include <memory>
#include <span>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <utility>

namespace sogen::native_present_sync
{
    struct error
    {
        VkResult result;
    };

    template <typename Enumerate>
    std::vector<VkExtensionProperties> extension_properties(Enumerate enumerate)
    {
        for (uint32_t attempt = 0; attempt < 8; ++attempt)
        {
            uint32_t count{};
            auto result = enumerate(&count, nullptr);
            if (result != VK_SUCCESS)
            {
                throw error{result};
            }
            if (count > 65536)
            {
                throw error{VK_ERROR_OUT_OF_HOST_MEMORY};
            }
            std::vector<VkExtensionProperties> values(count);
            if (!count)
            {
                return values;
            }
            result = enumerate(&count, values.data());
            if (result == VK_INCOMPLETE)
            {
                continue;
            }
            if (result != VK_SUCCESS)
            {
                throw error{result};
            }
            if (count > values.size())
            {
                throw error{VK_ERROR_INITIALIZATION_FAILED};
            }
            values.resize(count);
            return values;
        }
        throw error{VK_ERROR_INITIALIZATION_FAILED};
    }

    inline bool contains(std::span<const VkExtensionProperties> values, const char* name)
    {
        return std::ranges::any_of(values, [&](const auto& value) { return std::strcmp(value.extensionName, name) == 0; });
    }

    struct surface_support
    {
        bool khr{}, ext{};
        std::vector<const char*> names;
    };

    inline surface_support surface_extensions(PFN_vkEnumerateInstanceExtensionProperties enumerate, uint32_t api_version)
    {
        if (!enumerate || api_version < VK_API_VERSION_1_1)
        {
            throw error{VK_ERROR_FEATURE_NOT_PRESENT};
        }
        const auto values =
            extension_properties([&](uint32_t* count, VkExtensionProperties* out) { return enumerate(nullptr, count, out); });
        surface_support result;
        for (const auto* name : {"VK_KHR_surface", "VK_KHR_win32_surface", "VK_KHR_get_surface_capabilities2"})
        {
            if (!contains(values, name))
            {
                throw error{VK_ERROR_EXTENSION_NOT_PRESENT};
            }
            result.names.push_back(name);
        }
        result.khr = contains(values, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        result.ext = contains(values, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        if (result.khr)
        {
            result.names.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        }
        if (result.ext)
        {
            result.names.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        }
        return result;
    }

    inline bool enable_device(VkPhysicalDevice physical, PFN_vkEnumerateDeviceExtensionProperties enumerate,
                              PFN_vkGetPhysicalDeviceFeatures2 get_features, bool surface_khr, bool surface_ext,
                              std::vector<const char*>& extensions, VkPhysicalDeviceFeatures2& requested,
                              VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR& storage)
    {
        if (!std::ranges::any_of(extensions, [](const char* name) { return std::strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; }))
        {
            return false;
        }
        if (!enumerate || !get_features)
        {
            throw error{VK_ERROR_FEATURE_NOT_PRESENT};
        }
        const auto values =
            extension_properties([&](uint32_t* count, VkExtensionProperties* out) { return enumerate(physical, nullptr, count, out); });
        const char* name{};
        if (surface_khr && contains(values, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
        {
            name = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        }
        else if (surface_ext && contains(values, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
        {
            name = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        }
        else
        {
            return false;
        }
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 query{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supported};
        get_features(physical, &query);
        if (!supported.swapchainMaintenance1)
        {
            return false;
        }
        if (!std::ranges::any_of(extensions, [&](const char* value) { return std::strcmp(value, name) == 0; }))
        {
            extensions.push_back(name);
        }
        for (auto* node = static_cast<VkBaseOutStructure*>(requested.pNext); node; node = node->pNext)
        {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR)
            {
                reinterpret_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR*>(node)->swapchainMaintenance1 = VK_TRUE;
                return true;
            }
        }
        storage = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
                   .pNext = requested.pNext,
                   .swapchainMaintenance1 = VK_TRUE};
        requested.pNext = &storage;
        return true;
    }

    struct dispatch
    {
        PFN_vkCreateFence create_fence{};
        PFN_vkDestroyFence destroy_fence{};
        PFN_vkGetFenceStatus get_fence_status{};
        PFN_vkWaitForFences wait_for_fences{};
        PFN_vkDestroyCommandPool destroy_pool{};
        PFN_vkDestroySemaphore destroy_semaphore{};
        PFN_vkCreateSemaphore create_semaphore{};
        PFN_vkQueueSubmit queue_submit{};
        PFN_vkQueueWaitIdle queue_wait_idle{};
    };

    inline constexpr size_t max_retained_submissions = 256;
    inline constexpr size_t max_retained_acquisitions = 256;
    inline constexpr VkDeviceSize max_copy_allocation_bytes = 512ULL * 1024 * 1024;

    class acquisition
    {
        dispatch api_;
        VkDevice device_{};
        VkQueue queue_{};
        VkFence acquired_{};
        VkSemaphore semaphore_{};
        VkFence forwarded_{};
        bool acquire_pending_{};
        bool forward_pending_{};
        bool unfenced_forward_{};
        bool lost_{};
        VkSemaphore guest_semaphore_{};
        VkFence guest_fence_{};

        VkResult observe(VkResult result)
        {
            if (result == VK_ERROR_DEVICE_LOST)
            {
                lost_ = true;
                acquire_pending_ = forward_pending_ = unfenced_forward_ = false;
            }
            return result;
        }

      public:
        acquisition(dispatch api, VkDevice device, VkQueue queue)
            : api_(api),
              device_(device),
              queue_(queue)
        {
        }

        acquisition(const acquisition&) = delete;
        acquisition& operator=(const acquisition&) = delete;

        ~acquisition()
        {
            if (acquire_pending_ || forward_pending_ || unfenced_forward_)
            {
                std::terminate();
            }
            if (semaphore_)
            {
                api_.destroy_semaphore(device_, semaphore_, nullptr);
            }
            if (acquired_)
            {
                api_.destroy_fence(device_, acquired_, nullptr);
            }
            if (forwarded_)
            {
                api_.destroy_fence(device_, forwarded_, nullptr);
            }
        }

        void initialize(bool forward_fence)
        {
            if (!api_.create_fence || !api_.destroy_fence || !api_.wait_for_fences || !api_.get_fence_status)
            {
                throw error{VK_ERROR_FEATURE_NOT_PRESENT};
            }
            const VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence fence{};
            auto result = api_.create_fence(device_, &info, nullptr, &fence);
            if (result != VK_SUCCESS)
            {
                throw error{result};
            }
            acquired_ = fence;
            if (!forward_fence)
            {
                return;
            }
            if (!api_.create_semaphore || !api_.destroy_semaphore || !api_.queue_submit || !api_.queue_wait_idle)
            {
                throw error{VK_ERROR_FEATURE_NOT_PRESENT};
            }
            result = api_.create_fence(device_, &info, nullptr, &fence);
            if (result != VK_SUCCESS)
            {
                throw error{result};
            }
            forwarded_ = fence;
            const VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore sem{};
            result = api_.create_semaphore(device_, &sem_info, nullptr, &sem);
            if (result != VK_SUCCESS)
            {
                throw error{result};
            }
            semaphore_ = sem;
        }

        VkFence fence() const
        {
            return acquired_;
        }

        VkSemaphore semaphore(VkSemaphore guest) const
        {
            return semaphore_ ? semaphore_ : guest;
        }

        void guest_targets(VkSemaphore semaphore, VkFence fence)
        {
            guest_semaphore_ = semaphore;
            guest_fence_ = fence;
        }

        bool references(VkSemaphore value) const
        {
            return value && value == guest_semaphore_ && (acquire_pending_ || forward_pending_ || unfenced_forward_);
        }

        bool references_fence(VkFence value) const
        {
            return value && value == guest_fence_ && (forward_pending_ || unfenced_forward_);
        }

        void acquired(VkResult result)
        {
            acquire_pending_ = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
            observe(result);
        }

        VkResult forward(VkSemaphore guest_semaphore, VkFence guest_fence)
        {
            if (!guest_fence)
            {
                return VK_SUCCESS;
            }
            const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                      nullptr,
                                      1,
                                      &semaphore_,
                                      &stage,
                                      0,
                                      nullptr,
                                      guest_semaphore ? 1u : 0u,
                                      guest_semaphore ? &guest_semaphore : nullptr};
            auto result = observe(api_.queue_submit(queue_, 1, &submit, guest_fence));
            if (result != VK_SUCCESS)
            {
                return VK_ERROR_DEVICE_LOST;
            }
            // The guest may reset/destroy its fence. Only this private trailing fence owns retirement proof.
            unfenced_forward_ = true;
            result = observe(api_.queue_submit(queue_, 0, nullptr, forwarded_));
            if (result != VK_SUCCESS)
            {
                return VK_ERROR_DEVICE_LOST;
            }
            unfenced_forward_ = false;
            forward_pending_ = true;
            return VK_SUCCESS;
        }

        VkResult complete(bool wait)
        {
            if (lost_)
            {
                return VK_ERROR_DEVICE_LOST;
            }
            for (auto [pending, fence] : {std::pair{&acquire_pending_, acquired_}, std::pair{&forward_pending_, forwarded_}})
            {
                if (!*pending)
                {
                    continue;
                }
                const auto result =
                    observe(wait ? api_.wait_for_fences(device_, 1, &fence, VK_TRUE, 0) : api_.get_fence_status(device_, fence));
                if (result != VK_SUCCESS)
                {
                    return result;
                }
                *pending = false;
            }
            if (unfenced_forward_)
            {
                return VK_NOT_READY;
            }
            return VK_SUCCESS;
        }
    };

    inline bool not_enqueued(VkResult result)
    {
        return result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    struct completion
    {
        VkResult result;
        bool logical_device_lost{};
    };

    class submission
    {
        dispatch api_;
        std::vector<VkFence> present_fences_;
        bool copy_issued_{};
        bool copy_pending_{};
        bool present_pending_{};
        bool native_lost_{};

        struct image_use
        {
            VkSwapchainKHR swapchain{};
            uint32_t index{};
            std::shared_ptr<acquisition> completion;
        };

        std::vector<image_use> images_;
        std::vector<VkSemaphore> guest_waits_;

        VkResult observed(VkResult result)
        {
            if (result == VK_ERROR_DEVICE_LOST)
            {
                native_lost_ = true;
                copy_pending_ = present_pending_ = false;
            }
            return result;
        }

      public:
        VkDevice device{};
        VkQueue queue{};
        VkCommandPool pool{};
        VkCommandBuffer commands{};
        VkSemaphore ready{};
        VkFence copy_fence{};
        uint64_t serial{};

        submission(dispatch api, VkDevice device, VkQueue queue)
            : api_(api),
              device(device),
              queue(queue)
        {
            if (!api_.create_fence || !api_.destroy_fence || !api_.get_fence_status || !api_.wait_for_fences)
            {
                throw error{VK_ERROR_FEATURE_NOT_PRESENT};
            }
        }

        submission(const submission&) = delete;
        submission& operator=(const submission&) = delete;

        ~submission()
        {
            // Pending payloads cannot be released merely because an owner drops its reference.
            // Every normal last-owner path first proves retirement with a present or acquire fence.
            if (copy_pending_ || present_pending_)
            {
                std::terminate();
            }
            if (ready)
            {
                api_.destroy_semaphore(device, ready, nullptr);
            }
            if (pool)
            {
                api_.destroy_pool(device, pool, nullptr);
            }
            if (copy_fence)
            {
                api_.destroy_fence(device, copy_fence, nullptr);
            }
            for (const VkFence fence : present_fences_)
            {
                if (fence)
                {
                    api_.destroy_fence(device, fence, nullptr);
                }
            }
        }

        void initialize(std::span<const VkSwapchainKHR> swapchains, std::span<const uint32_t> indices, bool copy, bool present_fences)
        {
            if (swapchains.empty() || swapchains.size() != indices.size() || !images_.empty())
            {
                throw error{VK_ERROR_INITIALIZATION_FAILED};
            }
            if (copy && (!api_.destroy_pool || !api_.destroy_semaphore))
            {
                throw error{VK_ERROR_FEATURE_NOT_PRESENT};
            }
            for (size_t i = 0; i < swapchains.size(); ++i)
            {
                images_.push_back({.swapchain = swapchains[i], .index = indices[i], .completion = {}});
            }
            if (present_fences)
            {
                present_fences_.resize(swapchains.size());
            }
            const VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            for (auto& fence : present_fences_)
            {
                VkFence created{};
                const auto result = api_.create_fence(device, &info, nullptr, &created);
                if (result != VK_SUCCESS)
                {
                    throw error{result};
                }
                fence = created;
            }
            if (copy)
            {
                VkFence created{};
                const auto result = api_.create_fence(device, &info, nullptr, &created);
                if (result != VK_SUCCESS)
                {
                    throw error{result};
                }
                copy_fence = created;
            }
        }

        bool uses(VkSwapchainKHR swapchain, uint32_t index) const
        {
            return present_pending_ &&
                   std::ranges::any_of(images_, [&](const auto& image) { return image.swapchain == swapchain && image.index == index; });
        }

        void attach(VkSwapchainKHR swapchain, const std::shared_ptr<acquisition>& proof, const uint32_t* index = nullptr)
        {
            if (!present_pending_ || !present_fences_.empty())
            {
                return;
            }
            for (auto& image : images_)
            {
                if (image.swapchain == swapchain && (!index || image.index == *index) && !image.completion)
                {
                    image.completion = proof;
                }
            }
        }

        VkResult complete_unextended(bool wait)
        {
            for (const auto& image : images_)
            {
                if (!image.completion)
                {
                    return VK_NOT_READY;
                }
                const auto result = observed(image.completion->complete(wait));
                if (result != VK_SUCCESS)
                {
                    return result;
                }
            }
            present_pending_ = false;
            return VK_SUCCESS;
        }

        void guest_waits(std::span<const VkSemaphore> waits)
        {
            guest_waits_.assign(waits.begin(), waits.end());
        }

        bool references(VkSemaphore semaphore) const
        {
            return (copy_issued_ ? copy_pending_ : present_pending_) && std::ranges::find(guest_waits_, semaphore) != guest_waits_.end();
        }

        VkSwapchainPresentFenceInfoKHR fence_info(const void* next = nullptr) const
        {
            return {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
                    .pNext = next,
                    .swapchainCount = static_cast<uint32_t>(present_fences_.size()),
                    .pFences = present_fences_.data()};
        }

        void submitted(VkResult result)
        {
            if (result == VK_SUCCESS)
            {
                copy_issued_ = copy_pending_ = true;
            }
            else
            {
                observed(result);
            }
        }

        completion presented(VkResult result)
        {
            if (result == VK_ERROR_DEVICE_LOST)
            {
                observed(result);
                return {.result = result, .logical_device_lost = true};
            }
            if (not_enqueued(result))
            {
                // WSI requires unchanged synchronization payloads on OOM. An earlier internal copy
                // already consumed guest waits, so this implementation must report device loss.
                return {.result = copy_issued_ ? VK_ERROR_DEVICE_LOST : result, .logical_device_lost = copy_issued_};
            }
            present_pending_ = true;
            return {.result = result, .logical_device_lost = false};
        }

        VkResult poll()
        {
            if (native_lost_)
            {
                return VK_ERROR_DEVICE_LOST;
            }
            if (copy_pending_)
            {
                const auto result = observed(api_.get_fence_status(device, copy_fence));
                if (result != VK_SUCCESS)
                {
                    return result;
                }
                copy_pending_ = false;
            }
            if (present_pending_)
            {
                if (present_fences_.empty())
                {
                    return complete_unextended(false);
                }
                for (const VkFence fence : present_fences_)
                {
                    const auto result = observed(api_.get_fence_status(device, fence));
                    if (result != VK_SUCCESS)
                    {
                        return result;
                    }
                }
                present_pending_ = false;
            }
            return VK_SUCCESS;
        }

        VkResult wait()
        {
            if (native_lost_)
            {
                return VK_ERROR_DEVICE_LOST;
            }
            if (copy_pending_)
            {
                const auto result = observed(api_.wait_for_fences(device, 1, &copy_fence, VK_TRUE, 0));
                if (result != VK_SUCCESS)
                {
                    return result;
                }
                copy_pending_ = false;
            }
            if (present_pending_)
            {
                if (present_fences_.empty())
                {
                    return complete_unextended(true);
                }
                const auto result = observed(
                    api_.wait_for_fences(device, static_cast<uint32_t>(present_fences_.size()), present_fences_.data(), VK_TRUE, 0));
                if (result != VK_SUCCESS)
                {
                    return result;
                }
                present_pending_ = false;
            }
            return VK_SUCCESS;
        }
    };
}
