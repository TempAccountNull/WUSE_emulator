#include <gtest/gtest.h>
#include <vk_dynamic_state.hpp>
#include <array>
#include <limits>

namespace sogen::test
{
    namespace gb = gpu_bridge;

    namespace
    {
        VkCommandBuffer observed_buffer{};
        uint32_t observed_first{};
        uint32_t observed_count{};
        uint32_t observed_value{};
        VkExtent2D observed_grid{};
        std::vector<uint8_t> observed_array;

        template <typename T>
        void save_array(uint32_t count, const T* values)
        {
            observed_count = count;
            observed_array.resize(sizeof(T) * count);
            if (count)
            {
                std::memcpy(observed_array.data(), values, observed_array.size());
            }
        }

        VKAPI_ATTR void VKAPI_CALL blend(VkCommandBuffer buffer, uint32_t first, uint32_t count, const VkColorBlendEquationEXT* values)
        {
            observed_buffer = buffer;
            observed_first = first;
            save_array(count, values);
        }

        VKAPI_ATTR void VKAPI_CALL mask(VkCommandBuffer buffer, VkSampleCountFlagBits samples, const VkSampleMask* values)
        {
            observed_buffer = buffer;
            observed_value = samples;
            save_array((static_cast<uint32_t>(samples) + 31) / 32, values);
        }

        VKAPI_ATTR void VKAPI_CALL locations(VkCommandBuffer buffer, const VkSampleLocationsInfoEXT* info)
        {
            observed_buffer = buffer;
            EXPECT_EQ(info->sType, VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT);
            EXPECT_EQ(info->pNext, nullptr);
            observed_grid = info->sampleLocationGridSize;
            observed_value = info->sampleLocationsPerPixel;
            save_array(info->sampleLocationsCount, info->pSampleLocations);
        }

        VKAPI_ATTR void VKAPI_CALL overestimation(VkCommandBuffer buffer, float value)
        {
            observed_buffer = buffer;
            observed_value = std::bit_cast<uint32_t>(value);
        }

        VkResult execute(const gb::dynamic_dispatch& dispatch, const std::vector<uint8_t>& packet)
        {
            gb::dynamic_request request{};
            if (!gb::decode_dynamic_header(packet, request))
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            return dispatch.dispatch(reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(request.command_buffer)), request,
                                     std::span{packet}.subspan(sizeof(request)));
        }
    }

    TEST(VulkanDynamicStateTest, CopiesAllBlendAttachmentsAtRecordingTime)
    {
        gb::dynamic_dispatch dispatch{};
        dispatch.color_blend_equation = blend;
        std::array<VkColorBlendEquationEXT, 3> source{{{.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                        .colorBlendOp = VK_BLEND_OP_ADD,
                                                        .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                                                        .dstAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA,
                                                        .alphaBlendOp = VK_BLEND_OP_SUBTRACT},
                                                       {.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR,
                                                        .dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR,
                                                        .colorBlendOp = VK_BLEND_OP_MIN,
                                                        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                        .alphaBlendOp = VK_BLEND_OP_MAX},
                                                       {.srcColorBlendFactor = VK_BLEND_FACTOR_SRC1_COLOR,
                                                        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
                                                        .colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT,
                                                        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                        .alphaBlendOp = VK_BLEND_OP_ADD}}};
        const auto original = source;
        gb::dynamic_request request{
            .command_buffer = 0x12345678, .kind = gb::dynamic_command::color_blend_equation, .first = 2, .count = 3};
        const auto packet = gb::encode_dynamic(request, source.data(), sizeof(source[0]));
        source = {};
        ASSERT_EQ(execute(dispatch, packet), VK_SUCCESS);
        EXPECT_EQ(observed_buffer, reinterpret_cast<VkCommandBuffer>(uintptr_t{0x12345678}));
        EXPECT_EQ(observed_first, 2u);
        EXPECT_EQ(observed_count, 3u);
        ASSERT_EQ(observed_array.size(), sizeof(original));
        EXPECT_EQ(std::memcmp(observed_array.data(), original.data(), sizeof(original)), 0);
    }

    TEST(VulkanDynamicStateTest, PreservesBothWordsOf64SampleMask)
    {
        gb::dynamic_dispatch dispatch{};
        dispatch.sample_mask = mask;
        const std::array<VkSampleMask, 2> source{{0x80000001, 0xa5b6c7d8}};
        const gb::dynamic_request request{
            .command_buffer = 9, .kind = gb::dynamic_command::sample_mask, .count = 2, .value = VK_SAMPLE_COUNT_64_BIT};
        ASSERT_EQ(execute(dispatch, gb::encode_dynamic(request, source.data(), sizeof(source[0]))), VK_SUCCESS);
        EXPECT_EQ(observed_value, 64u);
        ASSERT_EQ(observed_array.size(), sizeof(source));
        EXPECT_EQ(std::memcmp(observed_array.data(), source.data(), sizeof(source)), 0);
    }

    TEST(VulkanDynamicStateTest, PreservesGridAndSampleCoordinates)
    {
        gb::dynamic_dispatch dispatch{};
        dispatch.sample_locations = locations;
        const std::array<VkSampleLocationEXT, 4> source{
            {{.x = 0.125f, .y = 0.875f}, {.x = 0.375f, .y = 0.625f}, {.x = 0.625f, .y = 0.375f}, {.x = 0.875f, .y = 0.125f}}};
        const gb::dynamic_request request{.command_buffer = 11,
                                          .kind = gb::dynamic_command::sample_locations,
                                          .count = 4,
                                          .value = VK_SAMPLE_COUNT_2_BIT,
                                          .width = 2,
                                          .height = 1};
        ASSERT_EQ(execute(dispatch, gb::encode_dynamic(request, source.data(), sizeof(source[0]))), VK_SUCCESS);
        EXPECT_EQ(observed_grid.width, 2u);
        EXPECT_EQ(observed_grid.height, 1u);
        EXPECT_EQ(observed_value, 2u);
        ASSERT_EQ(observed_array.size(), sizeof(source));
        EXPECT_EQ(std::memcmp(observed_array.data(), source.data(), sizeof(source)), 0);
    }

    TEST(VulkanDynamicStateTest, PreservesFloatBitsInsteadOfConvertingToInteger)
    {
        gb::dynamic_dispatch dispatch{};
        dispatch.extra_overestimation = overestimation;
        for (const float value : {0.125f, 1.875f, -0.0f})
        {
            const gb::dynamic_request request{
                .command_buffer = 1, .kind = gb::dynamic_command::extra_overestimation, .value = std::bit_cast<uint32_t>(value)};
            ASSERT_EQ(execute(dispatch, gb::encode_dynamic(request, nullptr, 0)), VK_SUCCESS);
            EXPECT_EQ(observed_value, std::bit_cast<uint32_t>(value));
        }
    }

    TEST(VulkanDynamicStateTest, RejectsTruncationTrailingDataOverflowAndUnknownKinds)
    {
        gb::dynamic_dispatch dispatch{};
        dispatch.color_blend_equation = blend;
        const VkColorBlendEquationEXT source{};
        gb::dynamic_request request{.command_buffer = 1, .kind = gb::dynamic_command::color_blend_equation, .count = 1};
        auto packet = gb::encode_dynamic(request, &source, sizeof(source));
        packet.pop_back();
        EXPECT_EQ(execute(dispatch, packet), VK_ERROR_INITIALIZATION_FAILED);
        packet = gb::encode_dynamic(request, &source, sizeof(source));
        packet.push_back(0);
        EXPECT_EQ(execute(dispatch, packet), VK_ERROR_INITIALIZATION_FAILED);
        request.first = UINT32_MAX;
        EXPECT_EQ(execute(dispatch, gb::encode_dynamic(request, &source, sizeof(source))), VK_ERROR_INITIALIZATION_FAILED);
        request.count = UINT32_MAX;
        EXPECT_EQ(execute(dispatch, gb::encode_dynamic(request, &source, sizeof(source))), VK_ERROR_INITIALIZATION_FAILED);
        request = {.kind = static_cast<gb::dynamic_command>(UINT32_MAX)};
        EXPECT_EQ(execute(dispatch, gb::encode_dynamic(request, nullptr, 0)), VK_ERROR_INITIALIZATION_FAILED);
        packet.resize(sizeof(request) - 1);
        EXPECT_EQ(execute(dispatch, packet), VK_ERROR_INITIALIZATION_FAILED);
    }

    TEST(VulkanDynamicStateTest, MissingDriverFunctionAndRecordedErrorsDoNotReportSuccess)
    {
        const gb::dynamic_dispatch dispatch{};
        gb::dynamic_request request{.kind = gb::dynamic_command::depth_clamp, .value = VK_TRUE};
        EXPECT_EQ(execute(dispatch, gb::encode_dynamic(request, nullptr, 0)), VK_ERROR_EXTENSION_NOT_PRESENT);
        request.error = VK_ERROR_OUT_OF_HOST_MEMORY;
        EXPECT_EQ(execute(dispatch, gb::encode_dynamic(request, nullptr, 0)), VK_ERROR_OUT_OF_HOST_MEMORY);
        request.error = VK_ERROR_EXTENSION_NOT_PRESENT;
        EXPECT_EQ(execute(dispatch, gb::encode_dynamic(request, nullptr, 0)), VK_ERROR_EXTENSION_NOT_PRESENT);
    }

    TEST(VulkanDynamicStateTest, RejectsInvalidSampleMaskAndGridWithoutCallingDriver)
    {
        const gb::dynamic_dispatch dispatch{};
        const VkSampleMask mask_value = 1;
        gb::dynamic_request request{.kind = gb::dynamic_command::sample_mask, .count = 1, .value = 3};
        EXPECT_EQ(execute(dispatch, gb::encode_dynamic(request, &mask_value, sizeof(mask_value))), VK_ERROR_INITIALIZATION_FAILED);
        request.value = 64;
        EXPECT_EQ(execute(dispatch, gb::encode_dynamic(request, &mask_value, sizeof(mask_value))), VK_ERROR_INITIALIZATION_FAILED);
        request = {.kind = gb::dynamic_command::sample_locations, .value = 64, .width = UINT32_MAX, .height = UINT32_MAX};
        EXPECT_EQ(execute(dispatch, gb::encode_dynamic(request, nullptr, 0)), VK_ERROR_INITIALIZATION_FAILED);
    }

    namespace
    {
        VKAPI_ATTR void VKAPI_CALL advanced_blend(VkCommandBuffer, uint32_t first, uint32_t count, const VkColorBlendAdvancedEXT* values)
        {
            observed_first = first;
            save_array(count, values);
        }

        VKAPI_ATTR void VKAPI_CALL rectangles(VkCommandBuffer, uint32_t first, uint32_t count, const VkRect2D* values)
        {
            observed_first = first;
            save_array(count, values);
        }

        VKAPI_ATTR void VKAPI_CALL coverage_table(VkCommandBuffer, uint32_t count, const float* values)
        {
            save_array(count, values);
        }

        VKAPI_ATTR void VKAPI_CALL viewport_scaling(VkCommandBuffer, uint32_t first, uint32_t count, const VkViewportWScalingNV* values)
        {
            observed_first = first;
            save_array(count, values);
        }

        VKAPI_ATTR void VKAPI_CALL stipple(VkCommandBuffer, uint32_t factor, uint16_t pattern)
        {
            observed_first = factor;
            observed_value = pattern;
        }

        VKAPI_ATTR void VKAPI_CALL shading_rate(VkCommandBuffer, const VkExtent2D* size, const VkFragmentShadingRateCombinerOpKHR* ops)
        {
            observed_grid = *size;
            save_array(2, ops);
        }

        VKAPI_ATTR void VKAPI_CALL clamp_range(VkCommandBuffer, VkDepthClampModeEXT mode, const VkDepthClampRangeEXT* range)
        {
            observed_value = mode;
            save_array(range ? 1 : 0, range);
        }
    }

    TEST(VulkanDynamicStateTest, PreservesAdvancedBlendFieldsForEveryAttachment)
    {
        gb::dynamic_dispatch dispatch{};
        dispatch.color_blend_advanced = advanced_blend;
        std::array<VkColorBlendAdvancedEXT, 2> source{{{.advancedBlendOp = VK_BLEND_OP_MULTIPLY_EXT,
                                                        .srcPremultiplied = VK_TRUE,
                                                        .dstPremultiplied = VK_FALSE,
                                                        .blendOverlap = VK_BLEND_OVERLAP_DISJOINT_EXT,
                                                        .clampResults = VK_TRUE},
                                                       {.advancedBlendOp = VK_BLEND_OP_HSL_COLOR_EXT,
                                                        .srcPremultiplied = VK_FALSE,
                                                        .dstPremultiplied = VK_TRUE,
                                                        .blendOverlap = VK_BLEND_OVERLAP_CONJOINT_EXT,
                                                        .clampResults = VK_FALSE}}};
        const auto original = source;
        const gb::dynamic_request request{.kind = gb::dynamic_command::color_blend_advanced, .first = 3, .count = 2};
        auto packet = gb::encode_dynamic(request, source.data(), sizeof(source[0]));
        source[0] = {};
        ASSERT_EQ(execute(dispatch, packet), VK_SUCCESS);
        EXPECT_EQ(observed_first, 3u);
        ASSERT_EQ(observed_array.size(), sizeof(original));
        EXPECT_EQ(std::memcmp(observed_array.data(), original.data(), sizeof(original)), 0);
    }

    TEST(VulkanDynamicStateTest, RectangleArraysRetainSignedOffsetsAndAllExtents)
    {
        gb::dynamic_dispatch dispatch{};
        dispatch.discard_rectangles = rectangles;
        dispatch.exclusive_scissors = rectangles;
        const std::array<VkRect2D, 2> source{{{.offset = {-17, 29}, .extent = {300, 400}}, {.offset = {1024, -7}, .extent = {8, 64}}}};
        for (const auto kind : {gb::dynamic_command::discard_rectangles, gb::dynamic_command::exclusive_scissors})
        {
            const gb::dynamic_request request{.kind = kind, .first = 7, .count = 2};
            ASSERT_EQ(execute(dispatch, gb::encode_dynamic(request, source.data(), sizeof(source[0]))), VK_SUCCESS);
            EXPECT_EQ(observed_first, 7u);
            ASSERT_EQ(observed_array.size(), sizeof(source));
            EXPECT_EQ(std::memcmp(observed_array.data(), source.data(), sizeof(source)), 0);
        }
    }

    TEST(VulkanDynamicStateTest, FloatArraysPreserveFractionsAndSignedZero)
    {
        gb::dynamic_dispatch dispatch{};
        dispatch.coverage_modulation_table = coverage_table;
        const std::array<float, 3> values{{0.125f, -0.0f, 0.9375f}};
        const gb::dynamic_request request{.kind = gb::dynamic_command::coverage_modulation_table, .count = 3};
        ASSERT_EQ(execute(dispatch, gb::encode_dynamic(request, values.data(), sizeof(float))), VK_SUCCESS);
        ASSERT_EQ(observed_array.size(), sizeof(values));
        EXPECT_EQ(std::memcmp(observed_array.data(), values.data(), sizeof(values)), 0);

        dispatch.viewport_w_scaling = viewport_scaling;
        const std::array<VkViewportWScalingNV, 2> scaling{{{.xcoeff = -0.0f, .ycoeff = -1.25f}, {.xcoeff = 0.875f, .ycoeff = 0.5f}}};
        const gb::dynamic_request scaling_request{.kind = gb::dynamic_command::viewport_w_scaling, .first = 1, .count = 2};
        ASSERT_EQ(execute(dispatch, gb::encode_dynamic(scaling_request, scaling.data(), sizeof(scaling[0]))), VK_SUCCESS);
        EXPECT_EQ(observed_first, 1u);
        ASSERT_EQ(observed_array.size(), sizeof(scaling));
        EXPECT_EQ(std::memcmp(observed_array.data(), scaling.data(), sizeof(scaling)), 0);
    }

    TEST(VulkanDynamicStateTest, LineStippleResolvesOfficialExtensionAliasesAndPreserves16Bits)
    {
        for (const auto* const name : {"vkCmdSetLineStipple", "vkCmdSetLineStippleKHR", "vkCmdSetLineStippleEXT"})
        {
            gb::dynamic_dispatch dispatch{};
            dispatch.load([&](const char* candidate) -> PFN_vkVoidFunction {
                return std::strcmp(candidate, name) == 0 ? reinterpret_cast<PFN_vkVoidFunction>(stipple) : nullptr;
            });
            gb::dynamic_request request{.kind = gb::dynamic_command::line_stipple, .first = 0xf781, .value = 256};
            ASSERT_EQ(execute(dispatch, gb::encode_dynamic(request, nullptr, 0)), VK_SUCCESS);
            EXPECT_EQ(observed_first, 256u);
            EXPECT_EQ(observed_value, 0xf781u);
            request.first = 0x10000;
            EXPECT_EQ(execute(dispatch, gb::encode_dynamic(request, nullptr, 0)), VK_ERROR_INITIALIZATION_FAILED);
        }
    }

    TEST(VulkanDynamicStateTest, FragmentShadingRateRetainsBothCombinerOperations)
    {
        gb::dynamic_dispatch dispatch{};
        dispatch.fragment_shading_rate = shading_rate;
        const std::array<VkFragmentShadingRateCombinerOpKHR, 2> ops{
            {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR}};
        gb::dynamic_request request{.kind = gb::dynamic_command::fragment_shading_rate, .count = 2, .width = 4, .height = 2};
        ASSERT_EQ(execute(dispatch, gb::encode_dynamic(request, ops.data(), sizeof(ops[0]))), VK_SUCCESS);
        EXPECT_EQ(observed_grid.width, 4u);
        EXPECT_EQ(observed_grid.height, 2u);
        ASSERT_EQ(observed_array.size(), sizeof(ops));
        EXPECT_EQ(std::memcmp(observed_array.data(), ops.data(), sizeof(ops)), 0);
        request.count = 1;
        EXPECT_EQ(execute(dispatch, gb::encode_dynamic(request, ops.data(), sizeof(ops[0]))), VK_ERROR_INITIALIZATION_FAILED);
    }

    TEST(VulkanDynamicStateTest, DepthClampRangePointerIsOnlyConsumedInUserDefinedMode)
    {
        gb::dynamic_dispatch dispatch{};
        dispatch.depth_clamp_range = clamp_range;
        const VkDepthClampRangeEXT range{-0.75f, 0.8125f};
        gb::dynamic_request request{
            .kind = gb::dynamic_command::depth_clamp_range, .count = 1, .value = VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT};
        ASSERT_EQ(execute(dispatch, gb::encode_dynamic(request, &range, sizeof(range))), VK_SUCCESS);
        ASSERT_EQ(observed_array.size(), sizeof(range));
        EXPECT_EQ(std::memcmp(observed_array.data(), &range, sizeof(range)), 0);
        request.count = 0;
        request.value = VK_DEPTH_CLAMP_MODE_VIEWPORT_RANGE_EXT;
        ASSERT_EQ(execute(dispatch, gb::encode_dynamic(request, nullptr, sizeof(range))), VK_SUCCESS);
        EXPECT_TRUE(observed_array.empty());
        request.count = 1;
        EXPECT_EQ(execute(dispatch, gb::encode_dynamic(request, &range, sizeof(range))), VK_ERROR_INITIALIZATION_FAILED);
    }

}
