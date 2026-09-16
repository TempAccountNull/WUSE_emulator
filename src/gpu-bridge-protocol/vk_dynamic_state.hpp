#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace sogen::gpu_bridge
{
    enum class dynamic_command : uint32_t
    {
        alpha_to_coverage,
        alpha_to_one,
        depth_clamp,
        logic_op_enable,
        polygon_mode,
        rasterization_samples,
        rasterization_stream,
        conservative_rasterization,
        sample_locations_enable,
        line_rasterization,
        tessellation_domain,
        extra_overestimation,
        color_blend_enable,
        color_blend_equation,
        color_write_mask,
        sample_mask,
        sample_locations,
        attachment_feedback_loop,
        coverage_modulation_mode,
        coverage_table_enable,
        coverage_reduction_mode,
        coverage_to_color_enable,
        coverage_to_color_location,
        depth_clip_negative_one,
        discard_rectangle_enable,
        discard_rectangle_mode,
        line_stipple_enable,
        logic_op,
        patch_control_points,
        primitive_restart_index,
        provoking_vertex_mode,
        ray_tracing_stack_size,
        representative_fragment_test,
        shading_rate_image_enable,
        viewport_w_scaling_enable,
        device_mask,
        color_blend_advanced,
        color_write_enable,
        coverage_modulation_table,
        discard_rectangles,
        exclusive_scissor_enable,
        exclusive_scissors,
        viewport_swizzles,
        viewport_w_scaling,
        line_stipple,
        fragment_shading_rate,
        fragment_shading_rate_enum,
        depth_clamp_range,
    };

    // No native pointers or pointer-sized handles cross this x86/x64 boundary. Array bodies contain
    // only 32-bit Vulkan scalars; floats retain their bit pattern, including signed zero.
    struct dynamic_request
    {
        uint64_t command_buffer{};
        dynamic_command kind{};
        uint32_t first{};
        uint32_t count{};
        uint32_t value{};
        uint32_t width{};
        uint32_t height{};
        int32_t error{};
        uint32_t reserved{};
    };

    static_assert(sizeof(dynamic_request) == 40);
    static_assert(offsetof(dynamic_request, error) == 32);
    static_assert(sizeof(VkColorBlendEquationEXT) == 24);
    static_assert(sizeof(VkSampleLocationEXT) == 8);
    static_assert(sizeof(VkColorBlendAdvancedEXT) == 20);
    static_assert(sizeof(VkViewportSwizzleNV) == 16);
    static_assert(sizeof(VkViewportWScalingNV) == 8);
    static_assert(sizeof(VkDepthClampRangeEXT) == 8);
    static_assert(sizeof(VkRect2D) == 16);
    static_assert(sizeof(VkFragmentShadingRateCombinerOpKHR) == 4);

    inline bool decode_dynamic_header(std::span<const uint8_t> payload, dynamic_request& request)
    {
        if (payload.size() < sizeof(request))
        {
            return false;
        }
        std::memcpy(&request, payload.data(), sizeof(request));
        return request.reserved == 0;
    }

    inline std::vector<uint8_t> encode_dynamic(dynamic_request request, const void* values, size_t stride)
    {
        size_t bytes{};
        if (request.error == VK_SUCCESS && request.count != 0)
        {
            if (!values || stride == 0 || request.count > (UINT32_MAX - sizeof(request)) / stride)
            {
                request.error = VK_ERROR_INITIALIZATION_FAILED;
            }
            else
            {
                bytes = static_cast<size_t>(request.count) * stride;
            }
        }
        if (request.error != VK_SUCCESS)
        {
            bytes = 0;
        }
        std::vector<uint8_t> result(sizeof(request) + bytes);
        std::memcpy(result.data(), &request, sizeof(request));
        if (bytes)
        {
            std::memcpy(result.data() + sizeof(request), values, bytes);
        }
        return result;
    }

    template <typename T>
    bool decode_array(std::span<const uint8_t> body, uint32_t count, std::vector<T>& output)
    {
        if (body.size() % sizeof(T) != 0 || body.size() / sizeof(T) != count)
        {
            return false;
        }
        output.resize(count);
        if (!body.empty())
        {
            std::memcpy(output.data(), body.data(), body.size());
        }
        return true;
    }

    inline bool valid_sample_count(uint32_t count)
    {
        return count != 0 && count <= 64 && std::has_single_bit(count);
    }

    struct dynamic_dispatch
    {
        PFN_vkCmdSetAlphaToCoverageEnableEXT alpha_to_coverage{};
        PFN_vkCmdSetAlphaToOneEnableEXT alpha_to_one{};
        PFN_vkCmdSetDepthClampEnableEXT depth_clamp{};
        PFN_vkCmdSetLogicOpEnableEXT logic_op_enable{};
        PFN_vkCmdSetPolygonModeEXT polygon_mode{};
        PFN_vkCmdSetRasterizationSamplesEXT rasterization_samples{};
        PFN_vkCmdSetRasterizationStreamEXT rasterization_stream{};
        PFN_vkCmdSetConservativeRasterizationModeEXT conservative_rasterization{};
        PFN_vkCmdSetSampleLocationsEnableEXT sample_locations_enable{};
        PFN_vkCmdSetLineRasterizationModeEXT line_rasterization{};
        PFN_vkCmdSetTessellationDomainOriginEXT tessellation_domain{};
        PFN_vkCmdSetExtraPrimitiveOverestimationSizeEXT extra_overestimation{};
        PFN_vkCmdSetColorBlendEnableEXT color_blend_enable{};
        PFN_vkCmdSetColorBlendEquationEXT color_blend_equation{};
        PFN_vkCmdSetColorWriteMaskEXT color_write_mask{};
        PFN_vkCmdSetSampleMaskEXT sample_mask{};
        PFN_vkCmdSetSampleLocationsEXT sample_locations{};
        PFN_vkCmdSetAttachmentFeedbackLoopEnableEXT attachment_feedback_loop{};
        PFN_vkCmdSetCoverageModulationModeNV coverage_modulation_mode{};
        PFN_vkCmdSetCoverageModulationTableEnableNV coverage_table_enable{};
        PFN_vkCmdSetCoverageReductionModeNV coverage_reduction_mode{};
        PFN_vkCmdSetCoverageToColorEnableNV coverage_to_color_enable{};
        PFN_vkCmdSetCoverageToColorLocationNV coverage_to_color_location{};
        PFN_vkCmdSetDepthClipNegativeOneToOneEXT depth_clip_negative_one{};
        PFN_vkCmdSetDiscardRectangleEnableEXT discard_rectangle_enable{};
        PFN_vkCmdSetDiscardRectangleModeEXT discard_rectangle_mode{};
        PFN_vkCmdSetLineStippleEnableEXT line_stipple_enable{};
        PFN_vkCmdSetLogicOpEXT logic_op{};
        PFN_vkCmdSetPatchControlPointsEXT patch_control_points{};
        PFN_vkCmdSetPrimitiveRestartIndexEXT primitive_restart_index{};
        PFN_vkCmdSetProvokingVertexModeEXT provoking_vertex_mode{};
        PFN_vkCmdSetRayTracingPipelineStackSizeKHR ray_tracing_stack_size{};
        PFN_vkCmdSetRepresentativeFragmentTestEnableNV representative_fragment_test{};
        PFN_vkCmdSetShadingRateImageEnableNV shading_rate_image_enable{};
        PFN_vkCmdSetViewportWScalingEnableNV viewport_w_scaling_enable{};
        PFN_vkCmdSetDeviceMask device_mask{};
        PFN_vkCmdSetColorBlendAdvancedEXT color_blend_advanced{};
        PFN_vkCmdSetColorWriteEnableEXT color_write_enable{};
        PFN_vkCmdSetCoverageModulationTableNV coverage_modulation_table{};
        PFN_vkCmdSetDiscardRectangleEXT discard_rectangles{};
        PFN_vkCmdSetExclusiveScissorEnableNV exclusive_scissor_enable{};
        PFN_vkCmdSetExclusiveScissorNV exclusive_scissors{};
        PFN_vkCmdSetViewportSwizzleNV viewport_swizzles{};
        PFN_vkCmdSetViewportWScalingNV viewport_w_scaling{};
        PFN_vkCmdSetLineStipple line_stipple{};
        PFN_vkCmdSetFragmentShadingRateKHR fragment_shading_rate{};
        PFN_vkCmdSetFragmentShadingRateEnumNV fragment_shading_rate_enum{};
        PFN_vkCmdSetDepthClampRangeEXT depth_clamp_range{};

        template <typename Resolver>
        void load(Resolver&& resolve)
        {
            this->alpha_to_coverage = reinterpret_cast<PFN_vkCmdSetAlphaToCoverageEnableEXT>(resolve("vkCmdSetAlphaToCoverageEnableEXT"));
            this->alpha_to_one = reinterpret_cast<PFN_vkCmdSetAlphaToOneEnableEXT>(resolve("vkCmdSetAlphaToOneEnableEXT"));
            this->depth_clamp = reinterpret_cast<PFN_vkCmdSetDepthClampEnableEXT>(resolve("vkCmdSetDepthClampEnableEXT"));
            this->logic_op_enable = reinterpret_cast<PFN_vkCmdSetLogicOpEnableEXT>(resolve("vkCmdSetLogicOpEnableEXT"));
            this->polygon_mode = reinterpret_cast<PFN_vkCmdSetPolygonModeEXT>(resolve("vkCmdSetPolygonModeEXT"));
            this->rasterization_samples = reinterpret_cast<PFN_vkCmdSetRasterizationSamplesEXT>(resolve("vkCmdSetRasterizationSamplesEXT"));
            this->rasterization_stream = reinterpret_cast<PFN_vkCmdSetRasterizationStreamEXT>(resolve("vkCmdSetRasterizationStreamEXT"));
            this->conservative_rasterization =
                reinterpret_cast<PFN_vkCmdSetConservativeRasterizationModeEXT>(resolve("vkCmdSetConservativeRasterizationModeEXT"));
            this->sample_locations_enable =
                reinterpret_cast<PFN_vkCmdSetSampleLocationsEnableEXT>(resolve("vkCmdSetSampleLocationsEnableEXT"));
            this->line_rasterization = reinterpret_cast<PFN_vkCmdSetLineRasterizationModeEXT>(resolve("vkCmdSetLineRasterizationModeEXT"));
            this->tessellation_domain =
                reinterpret_cast<PFN_vkCmdSetTessellationDomainOriginEXT>(resolve("vkCmdSetTessellationDomainOriginEXT"));
            this->extra_overestimation =
                reinterpret_cast<PFN_vkCmdSetExtraPrimitiveOverestimationSizeEXT>(resolve("vkCmdSetExtraPrimitiveOverestimationSizeEXT"));
            this->color_blend_enable = reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(resolve("vkCmdSetColorBlendEnableEXT"));
            this->color_blend_equation = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(resolve("vkCmdSetColorBlendEquationEXT"));
            this->color_write_mask = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(resolve("vkCmdSetColorWriteMaskEXT"));
            this->sample_mask = reinterpret_cast<PFN_vkCmdSetSampleMaskEXT>(resolve("vkCmdSetSampleMaskEXT"));
            this->sample_locations = reinterpret_cast<PFN_vkCmdSetSampleLocationsEXT>(resolve("vkCmdSetSampleLocationsEXT"));
            this->attachment_feedback_loop =
                reinterpret_cast<PFN_vkCmdSetAttachmentFeedbackLoopEnableEXT>(resolve("vkCmdSetAttachmentFeedbackLoopEnableEXT"));
            this->coverage_modulation_mode =
                reinterpret_cast<PFN_vkCmdSetCoverageModulationModeNV>(resolve("vkCmdSetCoverageModulationModeNV"));
            this->coverage_table_enable =
                reinterpret_cast<PFN_vkCmdSetCoverageModulationTableEnableNV>(resolve("vkCmdSetCoverageModulationTableEnableNV"));
            this->coverage_reduction_mode =
                reinterpret_cast<PFN_vkCmdSetCoverageReductionModeNV>(resolve("vkCmdSetCoverageReductionModeNV"));
            this->coverage_to_color_enable =
                reinterpret_cast<PFN_vkCmdSetCoverageToColorEnableNV>(resolve("vkCmdSetCoverageToColorEnableNV"));
            this->coverage_to_color_location =
                reinterpret_cast<PFN_vkCmdSetCoverageToColorLocationNV>(resolve("vkCmdSetCoverageToColorLocationNV"));
            this->depth_clip_negative_one =
                reinterpret_cast<PFN_vkCmdSetDepthClipNegativeOneToOneEXT>(resolve("vkCmdSetDepthClipNegativeOneToOneEXT"));
            this->discard_rectangle_enable =
                reinterpret_cast<PFN_vkCmdSetDiscardRectangleEnableEXT>(resolve("vkCmdSetDiscardRectangleEnableEXT"));
            this->discard_rectangle_mode =
                reinterpret_cast<PFN_vkCmdSetDiscardRectangleModeEXT>(resolve("vkCmdSetDiscardRectangleModeEXT"));
            this->line_stipple_enable = reinterpret_cast<PFN_vkCmdSetLineStippleEnableEXT>(resolve("vkCmdSetLineStippleEnableEXT"));
            this->logic_op = reinterpret_cast<PFN_vkCmdSetLogicOpEXT>(resolve("vkCmdSetLogicOpEXT"));
            this->patch_control_points = reinterpret_cast<PFN_vkCmdSetPatchControlPointsEXT>(resolve("vkCmdSetPatchControlPointsEXT"));
            this->primitive_restart_index =
                reinterpret_cast<PFN_vkCmdSetPrimitiveRestartIndexEXT>(resolve("vkCmdSetPrimitiveRestartIndexEXT"));
            this->provoking_vertex_mode = reinterpret_cast<PFN_vkCmdSetProvokingVertexModeEXT>(resolve("vkCmdSetProvokingVertexModeEXT"));
            this->ray_tracing_stack_size =
                reinterpret_cast<PFN_vkCmdSetRayTracingPipelineStackSizeKHR>(resolve("vkCmdSetRayTracingPipelineStackSizeKHR"));
            this->representative_fragment_test =
                reinterpret_cast<PFN_vkCmdSetRepresentativeFragmentTestEnableNV>(resolve("vkCmdSetRepresentativeFragmentTestEnableNV"));
            this->shading_rate_image_enable =
                reinterpret_cast<PFN_vkCmdSetShadingRateImageEnableNV>(resolve("vkCmdSetShadingRateImageEnableNV"));
            this->viewport_w_scaling_enable =
                reinterpret_cast<PFN_vkCmdSetViewportWScalingEnableNV>(resolve("vkCmdSetViewportWScalingEnableNV"));
            this->device_mask = reinterpret_cast<PFN_vkCmdSetDeviceMask>(resolve("vkCmdSetDeviceMask"));
            if (!this->device_mask)
            {
                this->device_mask = reinterpret_cast<PFN_vkCmdSetDeviceMask>(resolve("vkCmdSetDeviceMaskKHR"));
            }
            this->color_blend_advanced = reinterpret_cast<PFN_vkCmdSetColorBlendAdvancedEXT>(resolve("vkCmdSetColorBlendAdvancedEXT"));
            this->color_write_enable = reinterpret_cast<PFN_vkCmdSetColorWriteEnableEXT>(resolve("vkCmdSetColorWriteEnableEXT"));
            this->coverage_modulation_table =
                reinterpret_cast<PFN_vkCmdSetCoverageModulationTableNV>(resolve("vkCmdSetCoverageModulationTableNV"));
            this->discard_rectangles = reinterpret_cast<PFN_vkCmdSetDiscardRectangleEXT>(resolve("vkCmdSetDiscardRectangleEXT"));
            this->exclusive_scissor_enable =
                reinterpret_cast<PFN_vkCmdSetExclusiveScissorEnableNV>(resolve("vkCmdSetExclusiveScissorEnableNV"));
            this->exclusive_scissors = reinterpret_cast<PFN_vkCmdSetExclusiveScissorNV>(resolve("vkCmdSetExclusiveScissorNV"));
            this->viewport_swizzles = reinterpret_cast<PFN_vkCmdSetViewportSwizzleNV>(resolve("vkCmdSetViewportSwizzleNV"));
            this->viewport_w_scaling = reinterpret_cast<PFN_vkCmdSetViewportWScalingNV>(resolve("vkCmdSetViewportWScalingNV"));
            this->line_stipple = reinterpret_cast<PFN_vkCmdSetLineStipple>(resolve("vkCmdSetLineStipple"));
            if (!this->line_stipple)
            {
                this->line_stipple = reinterpret_cast<PFN_vkCmdSetLineStipple>(resolve("vkCmdSetLineStippleKHR"));
            }
            if (!this->line_stipple)
            {
                this->line_stipple = reinterpret_cast<PFN_vkCmdSetLineStipple>(resolve("vkCmdSetLineStippleEXT"));
            }
            this->fragment_shading_rate = reinterpret_cast<PFN_vkCmdSetFragmentShadingRateKHR>(resolve("vkCmdSetFragmentShadingRateKHR"));
            this->fragment_shading_rate_enum =
                reinterpret_cast<PFN_vkCmdSetFragmentShadingRateEnumNV>(resolve("vkCmdSetFragmentShadingRateEnumNV"));
            this->depth_clamp_range = reinterpret_cast<PFN_vkCmdSetDepthClampRangeEXT>(resolve("vkCmdSetDepthClampRangeEXT"));
        }

        VkResult dispatch(VkCommandBuffer buffer, const dynamic_request& request, std::span<const uint8_t> body) const
        {
            if (request.error != VK_SUCCESS)
            {
                if (!body.empty())
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                return static_cast<VkResult>(request.error);
            }
            switch (request.kind)
            {
            case dynamic_command::alpha_to_coverage:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->alpha_to_coverage)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->alpha_to_coverage(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::alpha_to_one:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->alpha_to_one)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->alpha_to_one(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::depth_clamp:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->depth_clamp)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->depth_clamp(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::logic_op_enable:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->logic_op_enable)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->logic_op_enable(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::polygon_mode:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->polygon_mode)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->polygon_mode(buffer, static_cast<VkPolygonMode>(request.value));
                return VK_SUCCESS;
            case dynamic_command::rasterization_samples:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->rasterization_samples)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->rasterization_samples(buffer, static_cast<VkSampleCountFlagBits>(request.value));
                return VK_SUCCESS;
            case dynamic_command::rasterization_stream:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->rasterization_stream)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->rasterization_stream(buffer, static_cast<uint32_t>(request.value));
                return VK_SUCCESS;
            case dynamic_command::conservative_rasterization:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->conservative_rasterization)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->conservative_rasterization(buffer, static_cast<VkConservativeRasterizationModeEXT>(request.value));
                return VK_SUCCESS;
            case dynamic_command::sample_locations_enable:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->sample_locations_enable)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->sample_locations_enable(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::line_rasterization:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->line_rasterization)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->line_rasterization(buffer, static_cast<VkLineRasterizationModeEXT>(request.value));
                return VK_SUCCESS;
            case dynamic_command::tessellation_domain:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->tessellation_domain)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->tessellation_domain(buffer, static_cast<VkTessellationDomainOrigin>(request.value));
                return VK_SUCCESS;
            case dynamic_command::extra_overestimation:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->extra_overestimation)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->extra_overestimation(buffer, std::bit_cast<float>(request.value));
                return VK_SUCCESS;
            case dynamic_command::color_blend_enable: {
                std::vector<VkBool32> values;
                if (request.first > UINT32_MAX - request.count || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->color_blend_enable)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->color_blend_enable(buffer, request.first, request.count, values.data());
                return VK_SUCCESS;
            }
            case dynamic_command::color_blend_equation: {
                std::vector<VkColorBlendEquationEXT> values;
                if (request.first > UINT32_MAX - request.count || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->color_blend_equation)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->color_blend_equation(buffer, request.first, request.count, values.data());
                return VK_SUCCESS;
            }
            case dynamic_command::color_write_mask: {
                std::vector<VkColorComponentFlags> values;
                if (request.first > UINT32_MAX - request.count || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->color_write_mask)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->color_write_mask(buffer, request.first, request.count, values.data());
                return VK_SUCCESS;
            }
            case dynamic_command::sample_mask: {
                std::vector<VkSampleMask> values;
                if (!valid_sample_count(request.value) || request.count != (request.value + 31) / 32 ||
                    !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->sample_mask)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->sample_mask(buffer, static_cast<VkSampleCountFlagBits>(request.value), values.data());
                return VK_SUCCESS;
            }
            case dynamic_command::sample_locations: {
                std::vector<VkSampleLocationEXT> values;
                if (!valid_sample_count(request.value) || request.width == 0 || request.height == 0 ||
                    static_cast<uint64_t>(request.width) * request.height > UINT32_MAX / request.value ||
                    static_cast<uint64_t>(request.width) * request.height * request.value != request.count ||
                    !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->sample_locations)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                const VkSampleLocationsInfoEXT info{VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
                                                    nullptr,
                                                    static_cast<VkSampleCountFlagBits>(request.value),
                                                    {request.width, request.height},
                                                    request.count,
                                                    values.data()};
                this->sample_locations(buffer, &info);
                return VK_SUCCESS;
            }
            case dynamic_command::attachment_feedback_loop:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->attachment_feedback_loop)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->attachment_feedback_loop(buffer, static_cast<VkImageAspectFlags>(request.value));
                return VK_SUCCESS;
            case dynamic_command::coverage_modulation_mode:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->coverage_modulation_mode)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->coverage_modulation_mode(buffer, static_cast<VkCoverageModulationModeNV>(request.value));
                return VK_SUCCESS;
            case dynamic_command::coverage_table_enable:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->coverage_table_enable)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->coverage_table_enable(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::coverage_reduction_mode:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->coverage_reduction_mode)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->coverage_reduction_mode(buffer, static_cast<VkCoverageReductionModeNV>(request.value));
                return VK_SUCCESS;
            case dynamic_command::coverage_to_color_enable:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->coverage_to_color_enable)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->coverage_to_color_enable(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::coverage_to_color_location:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->coverage_to_color_location)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->coverage_to_color_location(buffer, static_cast<uint32_t>(request.value));
                return VK_SUCCESS;
            case dynamic_command::depth_clip_negative_one:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->depth_clip_negative_one)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->depth_clip_negative_one(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::discard_rectangle_enable:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->discard_rectangle_enable)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->discard_rectangle_enable(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::discard_rectangle_mode:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->discard_rectangle_mode)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->discard_rectangle_mode(buffer, static_cast<VkDiscardRectangleModeEXT>(request.value));
                return VK_SUCCESS;
            case dynamic_command::line_stipple_enable:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->line_stipple_enable)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->line_stipple_enable(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::logic_op:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->logic_op)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->logic_op(buffer, static_cast<VkLogicOp>(request.value));
                return VK_SUCCESS;
            case dynamic_command::patch_control_points:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->patch_control_points)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->patch_control_points(buffer, static_cast<uint32_t>(request.value));
                return VK_SUCCESS;
            case dynamic_command::primitive_restart_index:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->primitive_restart_index)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->primitive_restart_index(buffer, static_cast<uint32_t>(request.value));
                return VK_SUCCESS;
            case dynamic_command::provoking_vertex_mode:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->provoking_vertex_mode)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->provoking_vertex_mode(buffer, static_cast<VkProvokingVertexModeEXT>(request.value));
                return VK_SUCCESS;
            case dynamic_command::ray_tracing_stack_size:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->ray_tracing_stack_size)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->ray_tracing_stack_size(buffer, static_cast<uint32_t>(request.value));
                return VK_SUCCESS;
            case dynamic_command::representative_fragment_test:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->representative_fragment_test)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->representative_fragment_test(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::shading_rate_image_enable:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->shading_rate_image_enable)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->shading_rate_image_enable(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::viewport_w_scaling_enable:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->viewport_w_scaling_enable)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->viewport_w_scaling_enable(buffer, static_cast<VkBool32>(request.value));
                return VK_SUCCESS;
            case dynamic_command::device_mask:
                if (!body.empty() || request.count != 0)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->device_mask)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->device_mask(buffer, static_cast<uint32_t>(request.value));
                return VK_SUCCESS;
            case dynamic_command::color_blend_advanced: {
                std::vector<VkColorBlendAdvancedEXT> values;
                if (request.first > UINT32_MAX - request.count || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->color_blend_advanced)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->color_blend_advanced(buffer, request.first, request.count, values.data());
                return VK_SUCCESS;
            }
            case dynamic_command::color_write_enable: {
                std::vector<VkBool32> values;
                if (request.first > UINT32_MAX - request.count || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->color_write_enable)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->color_write_enable(buffer, request.count, values.data());
                return VK_SUCCESS;
            }
            case dynamic_command::coverage_modulation_table: {
                std::vector<float> values;
                if (request.first > UINT32_MAX - request.count || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->coverage_modulation_table)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->coverage_modulation_table(buffer, request.count, values.data());
                return VK_SUCCESS;
            }
            case dynamic_command::discard_rectangles: {
                std::vector<VkRect2D> values;
                if (request.first > UINT32_MAX - request.count || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->discard_rectangles)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->discard_rectangles(buffer, request.first, request.count, values.data());
                return VK_SUCCESS;
            }
            case dynamic_command::exclusive_scissor_enable: {
                std::vector<VkBool32> values;
                if (request.first > UINT32_MAX - request.count || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->exclusive_scissor_enable)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->exclusive_scissor_enable(buffer, request.first, request.count, values.data());
                return VK_SUCCESS;
            }
            case dynamic_command::exclusive_scissors: {
                std::vector<VkRect2D> values;
                if (request.first > UINT32_MAX - request.count || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->exclusive_scissors)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->exclusive_scissors(buffer, request.first, request.count, values.data());
                return VK_SUCCESS;
            }
            case dynamic_command::viewport_swizzles: {
                std::vector<VkViewportSwizzleNV> values;
                if (request.first > UINT32_MAX - request.count || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->viewport_swizzles)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->viewport_swizzles(buffer, request.first, request.count, values.data());
                return VK_SUCCESS;
            }
            case dynamic_command::viewport_w_scaling: {
                std::vector<VkViewportWScalingNV> values;
                if (request.first > UINT32_MAX - request.count || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->viewport_w_scaling)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->viewport_w_scaling(buffer, request.first, request.count, values.data());
                return VK_SUCCESS;
            }
            case dynamic_command::line_stipple:
                if (!body.empty() || request.count != 0 || request.first > UINT16_MAX)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->line_stipple)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->line_stipple(buffer, request.value, static_cast<uint16_t>(request.first));
                return VK_SUCCESS;
            case dynamic_command::fragment_shading_rate:
            case dynamic_command::fragment_shading_rate_enum: {
                std::vector<VkFragmentShadingRateCombinerOpKHR> values;
                if (request.count != 2 || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (request.kind == dynamic_command::fragment_shading_rate)
                {
                    if (!this->fragment_shading_rate)
                    {
                        return VK_ERROR_EXTENSION_NOT_PRESENT;
                    }
                    const VkExtent2D size{request.width, request.height};
                    this->fragment_shading_rate(buffer, &size, values.data());
                }
                else
                {
                    if (!this->fragment_shading_rate_enum)
                    {
                        return VK_ERROR_EXTENSION_NOT_PRESENT;
                    }
                    this->fragment_shading_rate_enum(buffer, static_cast<VkFragmentShadingRateNV>(request.value), values.data());
                }
                return VK_SUCCESS;
            }
            case dynamic_command::depth_clamp_range: {
                std::vector<VkDepthClampRangeEXT> values;
                const bool has_range = request.value == VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT;
                if (request.count != (has_range ? 1u : 0u) || !decode_array(body, request.count, values))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                if (!this->depth_clamp_range)
                {
                    return VK_ERROR_EXTENSION_NOT_PRESENT;
                }
                this->depth_clamp_range(buffer, static_cast<VkDepthClampModeEXT>(request.value), has_range ? values.data() : nullptr);
                return VK_SUCCESS;
            }
            default:
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
    };
}
