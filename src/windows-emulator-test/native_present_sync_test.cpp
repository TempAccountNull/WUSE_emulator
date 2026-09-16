#include <gtest/gtest.h>
#include <devices/native_present_sync.hpp>
#include <array>
#include <map>
#include <type_traits>

namespace sogen::test
{
    namespace sync = native_present_sync;

    template <typename T>
    T handle(uintptr_t value)
    {
        if constexpr (std::is_pointer_v<T>)
        {
            return reinterpret_cast<T>(value);
        }
        else
        {
            return static_cast<T>(value);
        }
    }

    struct present_driver
    {
        static inline thread_local present_driver* active{};
        uintptr_t next{10};
        size_t fence_creations{}, fail_fence_at{}, destroys{}, pools_destroyed{}, semaphores_destroyed{}, idle_calls{};
        std::map<VkFence, VkResult> statuses;
        std::vector<VkResult> submits;
        std::vector<VkFence> submitted_fences;
        std::vector<VkSemaphore> waited, signaled;
        std::vector<const char*> instance_extensions{"VK_KHR_surface", "VK_KHR_win32_surface", "VK_KHR_get_surface_capabilities2"};
        std::vector<const char*> device_extensions;
        bool maintenance_feature{};

        present_driver()
        {
            active = this;
        }

        ~present_driver()
        {
            active = nullptr;
        }

        static VKAPI_ATTR VkResult VKAPI_CALL create_fence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* out)
        {
            auto& d = *active;
            if (++d.fence_creations == d.fail_fence_at)
            {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            *out = handle<VkFence>(d.next++);
            d.statuses[*out] = VK_NOT_READY;
            return VK_SUCCESS;
        }

        static VKAPI_ATTR void VKAPI_CALL destroy_fence(VkDevice, VkFence fence, const VkAllocationCallbacks*)
        {
            ++active->destroys;
            active->statuses.erase(fence);
        }

        static VKAPI_ATTR VkResult VKAPI_CALL status(VkDevice, VkFence fence)
        {
            return active->statuses.at(fence);
        }

        static VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice, uint32_t count, const VkFence* fences, VkBool32, uint64_t)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto value = active->statuses.at(fences[i]);
                if (value != VK_SUCCESS)
                {
                    return value == VK_NOT_READY ? VK_TIMEOUT : value;
                }
            }
            return VK_SUCCESS;
        }

        static VKAPI_ATTR void VKAPI_CALL destroy_pool(VkDevice, VkCommandPool, const VkAllocationCallbacks*)
        {
            ++active->pools_destroyed;
        }

        static VKAPI_ATTR void VKAPI_CALL destroy_semaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*)
        {
            ++active->semaphores_destroyed;
        }

        static VKAPI_ATTR VkResult VKAPI_CALL create_semaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*,
                                                               VkSemaphore* out)
        {
            *out = handle<VkSemaphore>(active->next++);
            return VK_SUCCESS;
        }

        static VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue, uint32_t count, const VkSubmitInfo* infos, VkFence fence)
        {
            auto& d = *active;
            d.submitted_fences.push_back(fence);
            for (uint32_t i = 0; i < count; ++i)
            {
                for (uint32_t j = 0; j < infos[i].waitSemaphoreCount; ++j)
                {
                    d.waited.push_back(infos[i].pWaitSemaphores[j]);
                }
                for (uint32_t j = 0; j < infos[i].signalSemaphoreCount; ++j)
                {
                    d.signaled.push_back(infos[i].pSignalSemaphores[j]);
                }
            }
            const auto i = d.submitted_fences.size() - 1;
            return i < d.submits.size() ? d.submits[i] : VK_SUCCESS;
        }

        static VKAPI_ATTR VkResult VKAPI_CALL idle(VkQueue)
        {
            ++active->idle_calls;
            return VK_SUCCESS;
        }

        static VkResult enumerate(const std::vector<const char*>& names, uint32_t* count, VkExtensionProperties* out)
        {
            const auto capacity = *count;
            *count = static_cast<uint32_t>(names.size());
            if (!out)
            {
                return VK_SUCCESS;
            }
            *count = std::min(*count, capacity);
            for (uint32_t i = 0; i < *count; ++i)
            {
                std::memcpy(out[i].extensionName, names[i], std::strlen(names[i]) + 1);
            }
            return capacity < names.size() ? VK_INCOMPLETE : VK_SUCCESS;
        }

        static VKAPI_ATTR VkResult VKAPI_CALL instance_ext(const char*, uint32_t* count, VkExtensionProperties* out)
        {
            return enumerate(active->instance_extensions, count, out);
        }

        static VKAPI_ATTR VkResult VKAPI_CALL device_ext(VkPhysicalDevice, const char*, uint32_t* count, VkExtensionProperties* out)
        {
            return enumerate(active->device_extensions, count, out);
        }

        static VKAPI_ATTR void VKAPI_CALL features(VkPhysicalDevice, VkPhysicalDeviceFeatures2* out)
        {
            static_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR*>(out->pNext)->swapchainMaintenance1 = active->maintenance_feature;
        }

        static sync::dispatch dispatch()
        {
            return {.create_fence = create_fence,
                    .destroy_fence = destroy_fence,
                    .get_fence_status = status,
                    .wait_for_fences = wait,
                    .destroy_pool = destroy_pool,
                    .destroy_semaphore = destroy_semaphore,
                    .create_semaphore = create_semaphore,
                    .queue_submit = submit,
                    .queue_wait_idle = idle};
        }

        void signal_all()
        {
            for (auto& [fence, result] : statuses)
            {
                result = VK_SUCCESS;
            }
        }
    };

    struct tracked_present
    {
        sync::submission value;

        tracked_present(present_driver&, bool maintenance, bool copy = true, uint32_t count = 1)
            : value(present_driver::dispatch(), handle<VkDevice>(1), handle<VkQueue>(2))
        {
            const std::array swapchains{handle<VkSwapchainKHR>(3), handle<VkSwapchainKHR>(4)};
            const std::array<uint32_t, 2> indices{0, 1};
            value.initialize(std::span(swapchains).first(count), std::span(indices).first(count), copy, maintenance);
            if (copy)
            {
                value.pool = handle<VkCommandPool>(5);
                value.ready = handle<VkSemaphore>(6);
            }
        }

        ~tracked_present()
        {
            value.presented(VK_ERROR_DEVICE_LOST);
        }
    };

    TEST(NativePresentSync, MissingMaintenanceExtensionsStillAdmitsUnextendedStrategy)
    {
        present_driver d;
        const auto surface = sync::surface_extensions(present_driver::instance_ext, VK_API_VERSION_1_1);
        EXPECT_FALSE(surface.khr);
        EXPECT_FALSE(surface.ext);
        EXPECT_EQ(surface.names.size(), 3);
        std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkPhysicalDeviceFeatures2 requested{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR storage{};
        EXPECT_FALSE(sync::enable_device(handle<VkPhysicalDevice>(1), present_driver::device_ext, present_driver::features, false, false,
                                         extensions, requested, storage));
        EXPECT_EQ(extensions.size(), 1);
        EXPECT_EQ(requested.pNext, nullptr);
    }

    TEST(NativePresentSync, MaintenanceIsEnabledOnlyWithMatchingSurfaceExtensionAndFeature)
    {
        present_driver d;
        d.device_extensions = {VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
        std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkPhysicalDeviceFeatures2 requested{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR storage{};
        EXPECT_FALSE(sync::enable_device(handle<VkPhysicalDevice>(1), present_driver::device_ext, present_driver::features, false, true,
                                         extensions, requested, storage));
        d.maintenance_feature = true;
        EXPECT_FALSE(sync::enable_device(handle<VkPhysicalDevice>(1), present_driver::device_ext, present_driver::features, true, false,
                                         extensions, requested, storage));
        VkPhysicalDeviceTimelineSemaphoreFeatures sibling{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        requested.pNext = &sibling;
        EXPECT_TRUE(sync::enable_device(handle<VkPhysicalDevice>(1), present_driver::device_ext, present_driver::features, false, true,
                                        extensions, requested, storage));
        EXPECT_EQ(requested.pNext, &storage);
        EXPECT_EQ(storage.pNext, &sibling);
        EXPECT_EQ(storage.swapchainMaintenance1, VK_TRUE);
        EXPECT_STREQ(extensions.back(), VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    }

    TEST(NativePresentSync, CopyFenceAloneDoesNotReleasePresentationResources)
    {
        present_driver d;
        {
            tracked_present batch(d, true, true, 2);
            batch.value.submitted(VK_SUCCESS);
            batch.value.presented(VK_SUCCESS);
            d.statuses[batch.value.copy_fence] = VK_SUCCESS;
            const auto info = batch.value.fence_info();
            d.statuses[info.pFences[0]] = VK_SUCCESS;
            EXPECT_EQ(batch.value.poll(), VK_NOT_READY);
            EXPECT_EQ(d.pools_destroyed, 0);
            d.statuses[info.pFences[1]] = VK_SUCCESS;
            EXPECT_EQ(batch.value.poll(), VK_SUCCESS);
        }
        EXPECT_EQ(d.pools_destroyed, 1);
        EXPECT_EQ(d.semaphores_destroyed, 1);
        EXPECT_EQ(d.destroys, 3);
    }

    TEST(NativePresentSync, ReacquireReturnAndWrongImageAreNotCompletion)
    {
        present_driver d;
        tracked_present batch(d, false, false);
        batch.value.presented(VK_SUCCESS);
        EXPECT_EQ(batch.value.wait(), VK_NOT_READY);
        auto proof = std::make_shared<sync::acquisition>(present_driver::dispatch(), handle<VkDevice>(1), handle<VkQueue>(2));
        proof->initialize(false);
        proof->acquired(VK_SUCCESS);
        uint32_t wrong = 1;
        batch.value.attach(handle<VkSwapchainKHR>(3), proof, &wrong);
        EXPECT_EQ(batch.value.poll(), VK_NOT_READY);
        uint32_t correct = 0;
        batch.value.attach(handle<VkSwapchainKHR>(3), proof, &correct);
        EXPECT_EQ(batch.value.poll(), VK_NOT_READY);
        d.statuses[proof->fence()] = VK_SUCCESS;
        EXPECT_EQ(batch.value.poll(), VK_SUCCESS);
    }

    TEST(NativePresentSync, EverySwapchainNeedsItsOwnRetirementWitness)
    {
        present_driver d;
        tracked_present batch(d, false, false, 2);
        batch.value.presented(VK_SUBOPTIMAL_KHR);
        auto proof = std::make_shared<sync::acquisition>(present_driver::dispatch(), handle<VkDevice>(1), handle<VkQueue>(2));
        proof->initialize(false);
        proof->acquired(VK_SUCCESS);
        d.signal_all();
        batch.value.attach(handle<VkSwapchainKHR>(3), proof);
        EXPECT_EQ(batch.value.poll(), VK_NOT_READY);
        batch.value.attach(handle<VkSwapchainKHR>(4), proof);
        EXPECT_EQ(batch.value.poll(), VK_SUCCESS);
    }

    TEST(NativePresentSync, OomBeforeCopyIsUnchangedButOomAfterCopyBecomesLogicalLoss)
    {
        present_driver d;
        tracked_present direct(d, false, false);
        auto result = direct.value.presented(VK_ERROR_OUT_OF_HOST_MEMORY);
        EXPECT_EQ(result.result, VK_ERROR_OUT_OF_HOST_MEMORY);
        EXPECT_FALSE(result.logical_device_lost);
        EXPECT_EQ(direct.value.poll(), VK_SUCCESS);
        tracked_present copy(d, false);
        copy.value.submitted(VK_SUCCESS);
        result = copy.value.presented(VK_ERROR_OUT_OF_DEVICE_MEMORY);
        EXPECT_EQ(result.result, VK_ERROR_DEVICE_LOST);
        EXPECT_TRUE(result.logical_device_lost);
        EXPECT_EQ(copy.value.poll(), VK_NOT_READY);
        d.statuses[copy.value.copy_fence] = VK_SUCCESS;
        EXPECT_EQ(copy.value.poll(), VK_SUCCESS);
    }

    TEST(NativePresentSync, OutOfDateAndSurfaceLostStillRetainEnqueuedWaits)
    {
        present_driver d;
        for (const auto result : {VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_SURFACE_LOST_KHR})
        {
            tracked_present batch(d, false, false);
            EXPECT_EQ(batch.value.presented(result).result, result);
            EXPECT_EQ(batch.value.wait(), VK_NOT_READY);
        }
    }

    TEST(NativePresentSync, NativeWaitErrorRetainsAllResources)
    {
        present_driver d;
        tracked_present batch(d, true);
        batch.value.submitted(VK_SUCCESS);
        batch.value.presented(VK_SUCCESS);
        d.statuses[batch.value.copy_fence] = VK_ERROR_OUT_OF_HOST_MEMORY;
        EXPECT_EQ(batch.value.wait(), VK_ERROR_OUT_OF_HOST_MEMORY);
        EXPECT_EQ(d.pools_destroyed, 0);
        EXPECT_EQ(d.semaphores_destroyed, 0);
        d.signal_all();
        EXPECT_EQ(batch.value.wait(), VK_SUCCESS);
    }

    TEST(NativePresentSync, PartialFenceAllocationRollsBackOnlyCreatedObjects)
    {
        present_driver d;
        d.fail_fence_at = 2;
        EXPECT_THROW((tracked_present(d, true, true, 2)), sync::error);
        EXPECT_EQ(d.destroys, 1);
        EXPECT_EQ(d.pools_destroyed, 0);
    }

    TEST(NativePresentSync, GuestFenceForwardUsesIndependentPrivateCompletionFence)
    {
        present_driver d;
        sync::acquisition proof(present_driver::dispatch(), handle<VkDevice>(1), handle<VkQueue>(2));
        proof.initialize(true);
        proof.guest_targets(handle<VkSemaphore>(80), handle<VkFence>(81));
        proof.acquired(VK_SUCCESS);
        EXPECT_EQ(proof.forward(handle<VkSemaphore>(80), handle<VkFence>(81)), VK_SUCCESS);
        ASSERT_EQ(d.submitted_fences.size(), 2);
        EXPECT_EQ(d.submitted_fences[0], handle<VkFence>(81));
        EXPECT_NE(d.submitted_fences[1], handle<VkFence>(81));
        EXPECT_EQ(d.waited, std::vector<VkSemaphore>{proof.semaphore(VK_NULL_HANDLE)});
        EXPECT_EQ(d.signaled, std::vector<VkSemaphore>{handle<VkSemaphore>(80)});
        d.statuses[proof.fence()] = VK_SUCCESS;
        EXPECT_EQ(proof.complete(false), VK_NOT_READY);
        EXPECT_TRUE(proof.references_fence(handle<VkFence>(81)));
        d.signal_all();
        EXPECT_EQ(proof.complete(false), VK_SUCCESS);
        EXPECT_FALSE(proof.references_fence(handle<VkFence>(81)));
    }

    TEST(NativePresentSync, TrailingSubmitFailureRetainsWithoutBlockingIdle)
    {
        present_driver d;
        d.submits = {VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY};
        sync::acquisition proof(present_driver::dispatch(), handle<VkDevice>(1), handle<VkQueue>(2));
        proof.initialize(true);
        proof.acquired(VK_SUCCESS);
        EXPECT_EQ(proof.forward(VK_NULL_HANDLE, handle<VkFence>(81)), VK_ERROR_DEVICE_LOST);
        d.statuses[proof.fence()] = VK_SUCCESS;
        EXPECT_EQ(proof.complete(false), VK_NOT_READY);
        EXPECT_EQ(d.idle_calls, 0);
        EXPECT_EQ(proof.complete(true), VK_NOT_READY);
        EXPECT_EQ(d.idle_calls, 0);
        proof.acquired(VK_ERROR_DEVICE_LOST);
    }

    TEST(NativePresentSync, GuestWaitLifetimeDiffersBetweenDirectPresentAndInternalCopy)
    {
        present_driver d;
        const std::array waits{handle<VkSemaphore>(90)};
        tracked_present direct(d, true, false);
        direct.value.guest_waits(waits);
        direct.value.presented(VK_SUCCESS);
        EXPECT_TRUE(direct.value.references(waits[0]));
        tracked_present copy(d, true);
        copy.value.guest_waits(waits);
        copy.value.submitted(VK_SUCCESS);
        copy.value.presented(VK_SUCCESS);
        d.statuses[copy.value.copy_fence] = VK_SUCCESS;
        EXPECT_EQ(copy.value.poll(), VK_NOT_READY);
        EXPECT_FALSE(copy.value.references(waits[0]));
        EXPECT_TRUE(direct.value.references(waits[0]));
    }

    TEST(NativePresentSync, SwapchainPredecessorSeparatesGuestAndNativeRetirement)
    {
        sync::swapchain_generation generation{.native = handle<VkSwapchainKHR>(3), .surface_id = 7, .device_id = 5};
        EXPECT_TRUE(generation.explicit_predecessor(5, 7));
        EXPECT_FALSE(generation.deferred_predecessor(5, 7));

        generation.retired = generation.destroy_requested = true;
        EXPECT_FALSE(generation.explicit_predecessor(5, 7));
        EXPECT_TRUE(generation.deferred_predecessor(5, 7));

        generation.replaced();
        EXPECT_TRUE(generation.retired);
        EXPECT_TRUE(generation.destroy_requested);
        EXPECT_TRUE(generation.native_retired);
        EXPECT_FALSE(generation.deferred_predecessor(5, 7));
    }

    TEST(NativePresentSync, DeferredPredecessorMustBeUniqueForDeviceAndSurface)
    {
        sync::swapchain_generation first{
            .native = handle<VkSwapchainKHR>(3), .surface_id = 7, .device_id = 5, .retired = true, .destroy_requested = true};
        sync::swapchain_generation other_surface{
            .native = handle<VkSwapchainKHR>(4), .surface_id = 8, .device_id = 5, .retired = true, .destroy_requested = true};
        std::array<sync::swapchain_generation*, 2> candidates{&first, &other_surface};
        auto selected = sync::select_deferred_generation(candidates, 5, 7);
        EXPECT_EQ(selected.value, &first);
        EXPECT_FALSE(selected.ambiguous);

        sync::swapchain_generation duplicate{
            .native = handle<VkSwapchainKHR>(5), .surface_id = 7, .device_id = 5, .retired = true, .destroy_requested = true};
        candidates.back() = &duplicate;
        selected = sync::select_deferred_generation(candidates, 5, 7);
        EXPECT_EQ(selected.value, nullptr);
        EXPECT_TRUE(selected.ambiguous);

        duplicate.native_retired = true;
        selected = sync::select_deferred_generation(candidates, 5, 7);
        EXPECT_EQ(selected.value, &first);
        EXPECT_FALSE(selected.ambiguous);
    }
}
