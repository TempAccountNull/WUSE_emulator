#include <gtest/gtest.h>
#include <vk_queue_submit.hpp>
#include <devices/legacy_queue_submit.hpp>
#include <array>
#include <unordered_map>

namespace sogen::test
{
    namespace wire = gpu_bridge::queue_submit_wire;

    namespace
    {
        template <typename T>
        T handle(const uint64_t value)
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

        struct capture
        {
            size_t calls{};
            VkResult result{VK_SUCCESS};
            bool null_batches{};
            std::vector<std::byte> bytes;
        };

        thread_local capture* active_capture{};

        VKAPI_ATTR VkResult VKAPI_CALL submit_capture(VkQueue queue, uint32_t count, const VkSubmitInfo* batches, VkFence fence)
        {
            ++active_capture->calls;
            active_capture->null_batches = batches == nullptr;
            active_capture->bytes = wire::encode(queue, count, batches, fence);
            return active_capture->result;
        }

        template <typename T>
        struct object
        {
            T handle{};
            uint64_t device_id{};
            VkSemaphoreType type{VK_SEMAPHORE_TYPE_BINARY};
            bool native_destroy_requested{};
        };
    }

    TEST(VulkanLegacyQueueSubmitWire, ExactLayoutPreservesWideHandlesAndLegacyStages)
    {
        auto* const wait = handle<VkSemaphore>(0x1234567887654321ULL);
        auto* const signal = handle<VkSemaphore>(0xabcdef0123456789ULL);
        auto* const command = handle<VkCommandBuffer>(0x3456);
        const VkPipelineStageFlags stages = 0x80001000;
        const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &wait, &stages, 1, &command, 1, &signal};
        const auto bytes = wire::encode(handle<VkQueue>(0x1234), 1, &submit, handle<VkFence>(0x1122334455667788ULL));
        const std::array<uint32_t, 28> expected{
            1,         112, 0x1234,     0,          0x55667788, 0x11223344, 1,      0, VK_STRUCTURE_TYPE_SUBMIT_INFO,
            0,         1,   1,          1,          0,          0,          0,      0, 0,
            0,         0,   0x87654321, 0x12345678, 0x80001000, 0,          0x3456, 0, 0x23456789,
            0xabcdef01};
        ASSERT_EQ(bytes.size(), sizeof(expected));
        EXPECT_EQ(std::memcmp(bytes.data(), expected.data(), sizeof(expected)), 0);
        const auto decoded = wire::decode(bytes);
        EXPECT_EQ(decoded.info.fence, 0x1122334455667788ULL);
        ASSERT_EQ(decoded.batches.size(), 1U);
        EXPECT_EQ(decoded.batches[0].waits[0].stages, stages);
        EXPECT_EQ(decoded.batches[0].signals[0], 0xabcdef0123456789ULL);
    }

    TEST(VulkanLegacyQueueSubmitWire, RejectsTruncationForgedCountsTrailingBytesAndUnknownFlags)
    {
        const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        const auto bytes = wire::encode(handle<VkQueue>(1), 1, &submit, VK_NULL_HANDLE);
        for (size_t length = 0; length < bytes.size(); ++length)
        {
            EXPECT_THROW(wire::decode({bytes.data(), length}), wire::error) << length;
        }
        auto forged = bytes;
        uint32_t count = UINT32_MAX;
        std::memcpy(forged.data() + offsetof(wire::header, batch_count), &count, sizeof(count));
        EXPECT_THROW(wire::decode(forged), wire::error);
        forged = bytes;
        std::memcpy(forged.data() + sizeof(wire::header) + offsetof(wire::batch_header, wait_count), &count, sizeof(count));
        EXPECT_THROW(wire::decode(forged), wire::error);
        forged = bytes;
        count = 8;
        std::memcpy(forged.data() + sizeof(wire::header) + offsetof(wire::batch_header, chains), &count, sizeof(count));
        EXPECT_THROW(wire::decode(forged), wire::error);
        forged = bytes;
        forged.push_back(std::byte{});
        count = static_cast<uint32_t>(forged.size());
        std::memcpy(forged.data() + offsetof(wire::header, bytes), &count, sizeof(count));
        EXPECT_THROW(wire::decode(forged), wire::error);
    }

    TEST(VulkanLegacyQueueSubmitWire, RejectsMissingArraysUnsupportedChainsDuplicatesAndCycles)
    {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        EXPECT_THROW(wire::encode(handle<VkQueue>(1), 1, &submit, VK_NULL_HANDLE), wire::error);
        auto* const wait = handle<VkSemaphore>(1);
        submit.pWaitSemaphores = &wait;
        EXPECT_THROW(wire::encode(handle<VkQueue>(1), 1, &submit, VK_NULL_HANDLE), wire::error);
        submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkPerformanceQuerySubmitInfoKHR unsupported{VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR};
        submit.pNext = &unsupported;
        try
        {
            (void)wire::encode(handle<VkQueue>(1), 1, &submit, VK_NULL_HANDLE);
            FAIL() << "Unsupported pNext was silently accepted";
        }
        catch (const wire::error& error)
        {
            EXPECT_EQ(error.result, VK_ERROR_EXTENSION_NOT_PRESENT);
        }
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.pNext = &timeline;
        submit.pNext = &timeline;
        EXPECT_THROW(wire::encode(handle<VkQueue>(1), 1, &submit, VK_NULL_HANDLE), wire::error);
        VkTimelineSemaphoreSubmitInfo duplicate{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.pNext = &duplicate;
        EXPECT_THROW(wire::encode(handle<VkQueue>(1), 1, &submit, VK_NULL_HANDLE), wire::error);
        VkDeviceGroupSubmitInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO};
        group.commandBufferCount = 1;
        submit.pNext = &group;
        EXPECT_THROW(wire::encode(handle<VkQueue>(1), 1, &submit, VK_NULL_HANDLE), wire::error);
        EXPECT_THROW(wire::encode(handle<VkQueue>(1), UINT32_MAX, nullptr, VK_NULL_HANDLE), wire::error);
        EXPECT_THROW(wire::encode(handle<VkQueue>(1), 1, nullptr, VK_NULL_HANDLE), wire::error);
    }

    class VulkanLegacyQueueSubmitHost : public testing::Test
    {
      protected:
        capture captured;
        object<VkQueue> queue{.handle = handle<VkQueue>(0xA100), .device_id = 7};
        std::unordered_map<uint64_t, object<VkSemaphore>> semaphores{
            {11, {.handle = handle<VkSemaphore>(0xA111), .device_id = 7}},
            {12, {.handle = handle<VkSemaphore>(0xA112), .device_id = 7}},
            {13, {.handle = handle<VkSemaphore>(0xA113), .device_id = 7, .type = VK_SEMAPHORE_TYPE_TIMELINE}}};
        std::unordered_map<uint64_t, object<VkCommandBuffer>> commands{{21, {.handle = handle<VkCommandBuffer>(0xA121), .device_id = 7}},
                                                                       {22, {.handle = handle<VkCommandBuffer>(0xA122), .device_id = 7}}};
        std::unordered_map<uint64_t, object<VkFence>> fences{{31, {.handle = handle<VkFence>(0xA131), .device_id = 7}}};

        void SetUp() override
        {
            active_capture = &captured;
        }

        void TearDown() override
        {
            active_capture = nullptr;
        }

        VkResult call(const uint32_t count, const VkSubmitInfo* batches, const uint64_t fence = 31)
        {
            const auto bytes = wire::encode(handle<VkQueue>(1), count, batches, handle<VkFence>(fence));
            const auto decoded = wire::decode(bytes);
            return legacy_queue_submit::execute_owned(decoded, queue, submit_capture, semaphores, commands, fences);
        }
    };

    TEST_F(VulkanLegacyQueueSubmitHost, PreservesMultipleBatchesAndSynchronizationOnlyBatchInOneNativeCall)
    {
        auto* const wait = handle<VkSemaphore>(11);
        auto* const signal = handle<VkSemaphore>(12);
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        const std::array<VkCommandBuffer, 2> buffers{handle<VkCommandBuffer>(21), handle<VkCommandBuffer>(22)};
        std::array<VkSubmitInfo, 3> batches{};
        batches[0] = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .pNext = nullptr,
                      .waitSemaphoreCount = 1,
                      .pWaitSemaphores = &wait,
                      .pWaitDstStageMask = &stage,
                      .commandBufferCount = 2,
                      .pCommandBuffers = buffers.data(),
                      .signalSemaphoreCount = 1,
                      .pSignalSemaphores = &signal};
        batches[1] = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .pNext = nullptr,
                      .waitSemaphoreCount = 1,
                      .pWaitSemaphores = &signal,
                      .pWaitDstStageMask = &stage,
                      .commandBufferCount = 0,
                      .pCommandBuffers = nullptr,
                      .signalSemaphoreCount = 1,
                      .pSignalSemaphores = &wait};
        batches[2] = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
        ASSERT_EQ(call(3, batches.data()), VK_SUCCESS);
        ASSERT_EQ(captured.calls, 1U);
        const auto native = wire::decode(captured.bytes);
        EXPECT_EQ(native.info.queue, 0xA100U);
        EXPECT_EQ(native.info.fence, 0xA131U);
        ASSERT_EQ(native.batches.size(), 3U);
        EXPECT_EQ(native.batches[0].commands, (std::vector<uint64_t>{0xA121, 0xA122}));
        ASSERT_EQ(native.batches[1].waits.size(), 1U);
        EXPECT_EQ(native.batches[1].waits[0].semaphore, 0xA112U);
        EXPECT_EQ(native.batches[1].waits[0].stages, stage);
        EXPECT_TRUE(native.batches[1].commands.empty());
        EXPECT_EQ(native.batches[1].signals, (std::vector<uint64_t>{0xA111}));
        EXPECT_TRUE(native.batches[2].waits.empty());
    }

    TEST_F(VulkanLegacyQueueSubmitHost, ZeroBatchIsNotOneEmptyBatchAndNativeResultIsPreserved)
    {
        captured.result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        EXPECT_EQ(call(0, nullptr, 0), VK_ERROR_OUT_OF_DEVICE_MEMORY);
        EXPECT_EQ(captured.calls, 1U);
        EXPECT_TRUE(captured.null_batches);
        EXPECT_EQ(wire::decode(captured.bytes).info.fence, 0U);
        captured.result = VK_SUCCESS;
        const VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        EXPECT_EQ(call(1, &empty), VK_SUCCESS);
        EXPECT_FALSE(captured.null_batches);
        EXPECT_EQ(wire::decode(captured.bytes).batches.size(), 1U);
        EXPECT_EQ(wire::decode(captured.bytes).info.fence, 0xA131U);
    }

    TEST_F(VulkanLegacyQueueSubmitHost, PreservesTimelineDeviceGroupAndProtectedChainsWithoutSubmit2)
    {
        const std::array<VkSemaphore, 2> waits{handle<VkSemaphore>(11), handle<VkSemaphore>(13)};
        const std::array<VkPipelineStageFlags, 2> stages{VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        auto* const signal = handle<VkSemaphore>(13);
        auto* const command = handle<VkCommandBuffer>(21);
        const std::array<uint64_t, 2> wait_values{0xFFFFFFFFFFFFFFFFULL, 0x123456789ABCDEF0ULL};
        const uint64_t signal_value = 0xFEDCBA9876543210ULL;
        const std::array<uint32_t, 2> wait_devices{0, 1};
        const uint32_t signal_device = 1;
        const uint32_t command_mask = 3;
        const VkProtectedSubmitInfo protection{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO, nullptr, VK_TRUE};
        const VkDeviceGroupSubmitInfo group{
            VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO, &protection, 2, wait_devices.data(), 1, &command_mask, 1, &signal_device};
        const VkTimelineSemaphoreSubmitInfo timeline{
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, &group, 2, wait_values.data(), 1, &signal_value};
        const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline, 2, waits.data(), stages.data(), 1, &command, 1, &signal};
        ASSERT_EQ(call(1, &submit), VK_SUCCESS);
        ASSERT_EQ(captured.calls, 1U);
        const auto native = wire::decode(captured.bytes);
        ASSERT_EQ(native.batches.size(), 1U);
        const auto& batch = native.batches[0];
        EXPECT_EQ(batch.info.chains, wire::timeline_chain | wire::device_group_chain | wire::protected_chain);
        EXPECT_EQ(batch.wait_values, (std::vector<uint64_t>{wait_values[0], wait_values[1]}));
        EXPECT_EQ(batch.signal_values, (std::vector<uint64_t>{signal_value}));
        EXPECT_EQ(batch.wait_devices, (std::vector<uint32_t>{0, 1}));
        EXPECT_EQ(batch.signal_devices, (std::vector<uint32_t>{1}));
        EXPECT_EQ(batch.command_masks, (std::vector<uint32_t>{3}));
        EXPECT_EQ(batch.info.protected_submit, VK_TRUE);
    }

    TEST_F(VulkanLegacyQueueSubmitHost, RejectsTimelineWithoutValuesButDoesNotValidateIgnoredBinaryValues)
    {
        auto* wait = handle<VkSemaphore>(13);
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &wait, &stage};
        EXPECT_EQ(call(1, &submit), VK_ERROR_VALIDATION_FAILED_EXT);
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        submit.pNext = &timeline;
        EXPECT_EQ(call(1, &submit), VK_ERROR_VALIDATION_FAILED_EXT);
        EXPECT_EQ(captured.calls, 0U);
        wait = handle<VkSemaphore>(11);
        EXPECT_EQ(call(1, &submit), VK_SUCCESS);
        EXPECT_EQ(captured.calls, 1U);
    }

    TEST_F(VulkanLegacyQueueSubmitHost, LaterInvalidForeignOrDestroyedObjectsNeverPartiallySubmit)
    {
        auto* const wait = handle<VkSemaphore>(11);
        auto* const signal = handle<VkSemaphore>(12);
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        auto* const command = handle<VkCommandBuffer>(21);
        std::array<VkSubmitInfo, 2> batches{{{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO},
                                             {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                              .pNext = nullptr,
                                              .waitSemaphoreCount = 1,
                                              .pWaitSemaphores = &wait,
                                              .pWaitDstStageMask = &stage,
                                              .commandBufferCount = 1,
                                              .pCommandBuffers = &command,
                                              .signalSemaphoreCount = 1,
                                              .pSignalSemaphores = &signal}}};
        for (const auto id : {11ULL, 12ULL})
        {
            semaphores.at(id).device_id = 8;
            EXPECT_EQ(call(2, batches.data()), VK_ERROR_INITIALIZATION_FAILED);
            semaphores.at(id).device_id = 7;
            semaphores.at(id).native_destroy_requested = true;
            EXPECT_EQ(call(2, batches.data()), VK_ERROR_INITIALIZATION_FAILED);
            semaphores.at(id).native_destroy_requested = false;
            auto node = semaphores.extract(id);
            EXPECT_EQ(call(2, batches.data()), VK_ERROR_INITIALIZATION_FAILED);
            semaphores.insert(std::move(node));
        }
        commands.at(21).device_id = 8;
        EXPECT_EQ(call(2, batches.data()), VK_ERROR_INITIALIZATION_FAILED);
        commands.at(21).device_id = 7;
        auto command_node = commands.extract(21);
        EXPECT_EQ(call(2, batches.data()), VK_ERROR_INITIALIZATION_FAILED);
        commands.insert(std::move(command_node));
        fences.at(31).device_id = 8;
        EXPECT_EQ(call(2, batches.data()), VK_ERROR_INITIALIZATION_FAILED);
        fences.at(31).device_id = 7;
        fences.at(31).native_destroy_requested = true;
        EXPECT_EQ(call(2, batches.data()), VK_ERROR_INITIALIZATION_FAILED);
        fences.clear();
        EXPECT_EQ(call(2, batches.data()), VK_ERROR_INITIALIZATION_FAILED);
        EXPECT_EQ(captured.calls, 0U);
    }
}
