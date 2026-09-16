#pragma once

// Generated from Vulkan-Headers 31386378257ac8653ce5b32c93baec385259ebbe (1.4.361).
// The wire contains only named VkBool32 fields: native tail padding is never a feature.
#include <cstddef>
#include <vulkan/vulkan_core.h>

namespace sogen::gpu_bridge
{
    struct feature_layout
    {
        size_t structure_size;
        size_t body_size;
    };

    // This generated registry dispatch has one case per supported structure.
    // NOLINTNEXTLINE(hicpp-function-size, readability-function-size)
    inline feature_layout registry_feature_layout(VkStructureType type)
    {
        switch (type)
        {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            static_assert(offsetof(VkPhysicalDeviceFeatures2, features) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFeatures2, features) + sizeof(VkPhysicalDeviceFeatures2::features) -
                              offsetof(VkPhysicalDeviceFeatures2, features) ==
                          55 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFeatures2), .body_size = 55 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
            static_assert(offsetof(VkPhysicalDevice16BitStorageFeatures, storageBuffer16BitAccess) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevice16BitStorageFeatures, storageInputOutput16) +
                              sizeof(VkPhysicalDevice16BitStorageFeatures::storageInputOutput16) -
                              offsetof(VkPhysicalDevice16BitStorageFeatures, storageBuffer16BitAccess) ==
                          4 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevice16BitStorageFeatures), .body_size = 4 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDevice4444FormatsFeaturesEXT, formatA4R4G4B4) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevice4444FormatsFeaturesEXT, formatA4B4G4R4) +
                              sizeof(VkPhysicalDevice4444FormatsFeaturesEXT::formatA4B4G4R4) -
                              offsetof(VkPhysicalDevice4444FormatsFeaturesEXT, formatA4R4G4B4) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevice4444FormatsFeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
            static_assert(offsetof(VkPhysicalDevice8BitStorageFeatures, storageBuffer8BitAccess) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevice8BitStorageFeatures, storagePushConstant8) +
                              sizeof(VkPhysicalDevice8BitStorageFeatures::storagePushConstant8) -
                              offsetof(VkPhysicalDevice8BitStorageFeatures, storageBuffer8BitAccess) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevice8BitStorageFeatures), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ASTC_DECODE_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceASTCDecodeFeaturesEXT, decodeModeSharedExponent) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceASTCDecodeFeaturesEXT, decodeModeSharedExponent) +
                              sizeof(VkPhysicalDeviceASTCDecodeFeaturesEXT::decodeModeSharedExponent) -
                              offsetof(VkPhysicalDeviceASTCDecodeFeaturesEXT, decodeModeSharedExponent) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceASTCDecodeFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceAccelerationStructureFeaturesKHR, accelerationStructure) == 2 * sizeof(void*));
            static_assert(
                offsetof(VkPhysicalDeviceAccelerationStructureFeaturesKHR, descriptorBindingAccelerationStructureUpdateAfterBind) +
                    sizeof(VkPhysicalDeviceAccelerationStructureFeaturesKHR::descriptorBindingAccelerationStructureUpdateAfterBind) -
                    offsetof(VkPhysicalDeviceAccelerationStructureFeaturesKHR, accelerationStructure) ==
                5 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceAccelerationStructureFeaturesKHR), .body_size = 5 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceAddressBindingReportFeaturesEXT, reportAddressBinding) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceAddressBindingReportFeaturesEXT, reportAddressBinding) +
                              sizeof(VkPhysicalDeviceAddressBindingReportFeaturesEXT::reportAddressBinding) -
                              offsetof(VkPhysicalDeviceAddressBindingReportFeaturesEXT, reportAddressBinding) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceAddressBindingReportFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_AMIGO_PROFILING_FEATURES_SEC:
            static_assert(offsetof(VkPhysicalDeviceAmigoProfilingFeaturesSEC, amigoProfiling) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceAmigoProfilingFeaturesSEC, amigoProfiling) +
                              sizeof(VkPhysicalDeviceAmigoProfilingFeaturesSEC::amigoProfiling) -
                              offsetof(VkPhysicalDeviceAmigoProfilingFeaturesSEC, amigoProfiling) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceAmigoProfilingFeaturesSEC), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD:
            static_assert(offsetof(VkPhysicalDeviceAntiLagFeaturesAMD, antiLag) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceAntiLagFeaturesAMD, antiLag) + sizeof(VkPhysicalDeviceAntiLagFeaturesAMD::antiLag) -
                              offsetof(VkPhysicalDeviceAntiLagFeaturesAMD, antiLag) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceAntiLagFeaturesAMD), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT, attachmentFeedbackLoopDynamicState) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT, attachmentFeedbackLoopDynamicState) +
                              sizeof(VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT::attachmentFeedbackLoopDynamicState) -
                              offsetof(VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT, attachmentFeedbackLoopDynamicState) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT),
                    .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT, attachmentFeedbackLoopLayout) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT, attachmentFeedbackLoopLayout) +
                              sizeof(VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT::attachmentFeedbackLoopLayout) -
                              offsetof(VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT, attachmentFeedbackLoopLayout) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT, advancedBlendCoherentOperations) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT, advancedBlendCoherentOperations) +
                              sizeof(VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT::advancedBlendCoherentOperations) -
                              offsetof(VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT, advancedBlendCoherentOperations) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceBorderColorSwizzleFeaturesEXT, borderColorSwizzle) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceBorderColorSwizzleFeaturesEXT, borderColorSwizzleFromImage) +
                              sizeof(VkPhysicalDeviceBorderColorSwizzleFeaturesEXT::borderColorSwizzleFromImage) -
                              offsetof(VkPhysicalDeviceBorderColorSwizzleFeaturesEXT, borderColorSwizzle) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceBorderColorSwizzleFeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceBufferDeviceAddressFeatures, bufferDeviceAddress) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceBufferDeviceAddressFeatures, bufferDeviceAddressMultiDevice) +
                              sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures::bufferDeviceAddressMultiDevice) -
                              offsetof(VkPhysicalDeviceBufferDeviceAddressFeatures, bufferDeviceAddress) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceBufferDeviceAddressFeaturesEXT, bufferDeviceAddress) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceBufferDeviceAddressFeaturesEXT, bufferDeviceAddressMultiDevice) +
                              sizeof(VkPhysicalDeviceBufferDeviceAddressFeaturesEXT::bufferDeviceAddressMultiDevice) -
                              offsetof(VkPhysicalDeviceBufferDeviceAddressFeaturesEXT, bufferDeviceAddress) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceBufferDeviceAddressFeaturesEXT), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceClusterAccelerationStructureFeaturesNV, clusterAccelerationStructure) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceClusterAccelerationStructureFeaturesNV, clusterAccelerationStructure) +
                              sizeof(VkPhysicalDeviceClusterAccelerationStructureFeaturesNV::clusterAccelerationStructure) -
                              offsetof(VkPhysicalDeviceClusterAccelerationStructureFeaturesNV, clusterAccelerationStructure) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceClusterAccelerationStructureFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_CULLING_SHADER_FEATURES_HUAWEI:
            static_assert(offsetof(VkPhysicalDeviceClusterCullingShaderFeaturesHUAWEI, clustercullingShader) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceClusterCullingShaderFeaturesHUAWEI, multiviewClusterCullingShader) +
                              sizeof(VkPhysicalDeviceClusterCullingShaderFeaturesHUAWEI::multiviewClusterCullingShader) -
                              offsetof(VkPhysicalDeviceClusterCullingShaderFeaturesHUAWEI, clustercullingShader) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceClusterCullingShaderFeaturesHUAWEI), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD:
            static_assert(offsetof(VkPhysicalDeviceCoherentMemoryFeaturesAMD, deviceCoherentMemory) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCoherentMemoryFeaturesAMD, deviceCoherentMemory) +
                              sizeof(VkPhysicalDeviceCoherentMemoryFeaturesAMD::deviceCoherentMemory) -
                              offsetof(VkPhysicalDeviceCoherentMemoryFeaturesAMD, deviceCoherentMemory) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCoherentMemoryFeaturesAMD), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceColorWriteEnableFeaturesEXT, colorWriteEnable) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceColorWriteEnableFeaturesEXT, colorWriteEnable) +
                              sizeof(VkPhysicalDeviceColorWriteEnableFeaturesEXT::colorWriteEnable) -
                              offsetof(VkPhysicalDeviceColorWriteEnableFeaturesEXT, colorWriteEnable) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceColorWriteEnableFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMMAND_BUFFER_INHERITANCE_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceCommandBufferInheritanceFeaturesNV, commandBufferInheritance) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCommandBufferInheritanceFeaturesNV, commandBufferInheritance) +
                              sizeof(VkPhysicalDeviceCommandBufferInheritanceFeaturesNV::commandBufferInheritance) -
                              offsetof(VkPhysicalDeviceCommandBufferInheritanceFeaturesNV, commandBufferInheritance) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCommandBufferInheritanceFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_OCCUPANCY_PRIORITY_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceComputeOccupancyPriorityFeaturesNV, computeOccupancyPriority) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceComputeOccupancyPriorityFeaturesNV, computeOccupancyPriority) +
                              sizeof(VkPhysicalDeviceComputeOccupancyPriorityFeaturesNV::computeOccupancyPriority) -
                              offsetof(VkPhysicalDeviceComputeOccupancyPriorityFeaturesNV, computeOccupancyPriority) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceComputeOccupancyPriorityFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR, computeDerivativeGroupQuads) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR, computeDerivativeGroupLinear) +
                              sizeof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR::computeDerivativeGroupLinear) -
                              offsetof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR, computeDerivativeGroupQuads) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceConditionalRenderingFeaturesEXT, conditionalRendering) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceConditionalRenderingFeaturesEXT, inheritedConditionalRendering) +
                              sizeof(VkPhysicalDeviceConditionalRenderingFeaturesEXT::inheritedConditionalRendering) -
                              offsetof(VkPhysicalDeviceConditionalRenderingFeaturesEXT, conditionalRendering) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceConditionalRenderingFeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceCooperativeMatrix2FeaturesNV, cooperativeMatrixWorkgroupScope) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCooperativeMatrix2FeaturesNV, cooperativeMatrixBlockLoads) +
                              sizeof(VkPhysicalDeviceCooperativeMatrix2FeaturesNV::cooperativeMatrixBlockLoads) -
                              offsetof(VkPhysicalDeviceCooperativeMatrix2FeaturesNV, cooperativeMatrixWorkgroupScope) ==
                          7 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCooperativeMatrix2FeaturesNV), .body_size = 7 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_CONVERSION_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceCooperativeMatrixConversionFeaturesQCOM, cooperativeMatrixConversion) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCooperativeMatrixConversionFeaturesQCOM, cooperativeMatrixConversion) +
                              sizeof(VkPhysicalDeviceCooperativeMatrixConversionFeaturesQCOM::cooperativeMatrixConversion) -
                              offsetof(VkPhysicalDeviceCooperativeMatrixConversionFeaturesQCOM, cooperativeMatrixConversion) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCooperativeMatrixConversionFeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_DECODE_VECTOR_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceCooperativeMatrixDecodeVectorFeaturesNV, cooperativeMatrixDecodeVector) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCooperativeMatrixDecodeVectorFeaturesNV, cooperativeMatrixDecodeVector) +
                              sizeof(VkPhysicalDeviceCooperativeMatrixDecodeVectorFeaturesNV::cooperativeMatrixDecodeVector) -
                              offsetof(VkPhysicalDeviceCooperativeMatrixDecodeVectorFeaturesNV, cooperativeMatrixDecodeVector) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCooperativeMatrixDecodeVectorFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceCooperativeMatrixFeaturesKHR, cooperativeMatrix) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCooperativeMatrixFeaturesKHR, cooperativeMatrixRobustBufferAccess) +
                              sizeof(VkPhysicalDeviceCooperativeMatrixFeaturesKHR::cooperativeMatrixRobustBufferAccess) -
                              offsetof(VkPhysicalDeviceCooperativeMatrixFeaturesKHR, cooperativeMatrix) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCooperativeMatrixFeaturesKHR), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceCooperativeMatrixFeaturesNV, cooperativeMatrix) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCooperativeMatrixFeaturesNV, cooperativeMatrixRobustBufferAccess) +
                              sizeof(VkPhysicalDeviceCooperativeMatrixFeaturesNV::cooperativeMatrixRobustBufferAccess) -
                              offsetof(VkPhysicalDeviceCooperativeMatrixFeaturesNV, cooperativeMatrix) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCooperativeMatrixFeaturesNV), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_MAINTENANCE_1_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT, cooperativeMatrixProperties2) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT, cooperativeMatrixGetCoordinate) +
                              sizeof(VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT::cooperativeMatrixGetCoordinate) -
                              offsetof(VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT, cooperativeMatrixProperties2) ==
                          5 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT), .body_size = 5 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_VECTOR_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceCooperativeVectorFeaturesNV, cooperativeVector) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCooperativeVectorFeaturesNV, cooperativeVectorTraining) +
                              sizeof(VkPhysicalDeviceCooperativeVectorFeaturesNV::cooperativeVectorTraining) -
                              offsetof(VkPhysicalDeviceCooperativeVectorFeaturesNV, cooperativeVector) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCooperativeVectorFeaturesNV), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR, indirectMemoryCopy) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR, indirectMemoryToImageCopy) +
                              sizeof(VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR::indirectMemoryToImageCopy) -
                              offsetof(VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR, indirectMemoryCopy) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceCopyMemoryIndirectFeaturesNV, indirectCopy) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCopyMemoryIndirectFeaturesNV, indirectCopy) +
                              sizeof(VkPhysicalDeviceCopyMemoryIndirectFeaturesNV::indirectCopy) -
                              offsetof(VkPhysicalDeviceCopyMemoryIndirectFeaturesNV, indirectCopy) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCopyMemoryIndirectFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CORNER_SAMPLED_IMAGE_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceCornerSampledImageFeaturesNV, cornerSampledImage) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCornerSampledImageFeaturesNV, cornerSampledImage) +
                              sizeof(VkPhysicalDeviceCornerSampledImageFeaturesNV::cornerSampledImage) -
                              offsetof(VkPhysicalDeviceCornerSampledImageFeaturesNV, cornerSampledImage) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCornerSampledImageFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COVERAGE_REDUCTION_MODE_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceCoverageReductionModeFeaturesNV, coverageReductionMode) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCoverageReductionModeFeaturesNV, coverageReductionMode) +
                              sizeof(VkPhysicalDeviceCoverageReductionModeFeaturesNV::coverageReductionMode) -
                              offsetof(VkPhysicalDeviceCoverageReductionModeFeaturesNV, coverageReductionMode) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCoverageReductionModeFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUBIC_CLAMP_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceCubicClampFeaturesQCOM, cubicRangeClamp) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCubicClampFeaturesQCOM, cubicRangeClamp) +
                              sizeof(VkPhysicalDeviceCubicClampFeaturesQCOM::cubicRangeClamp) -
                              offsetof(VkPhysicalDeviceCubicClampFeaturesQCOM, cubicRangeClamp) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCubicClampFeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUBIC_WEIGHTS_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceCubicWeightsFeaturesQCOM, selectableCubicWeights) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCubicWeightsFeaturesQCOM, selectableCubicWeights) +
                              sizeof(VkPhysicalDeviceCubicWeightsFeaturesQCOM::selectableCubicWeights) -
                              offsetof(VkPhysicalDeviceCubicWeightsFeaturesQCOM, selectableCubicWeights) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCubicWeightsFeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceCustomBorderColorFeaturesEXT, customBorderColors) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCustomBorderColorFeaturesEXT, customBorderColorWithoutFormat) +
                              sizeof(VkPhysicalDeviceCustomBorderColorFeaturesEXT::customBorderColorWithoutFormat) -
                              offsetof(VkPhysicalDeviceCustomBorderColorFeaturesEXT, customBorderColors) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCustomBorderColorFeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_RESOLVE_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceCustomResolveFeaturesEXT, customResolve) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceCustomResolveFeaturesEXT, customResolve) +
                              sizeof(VkPhysicalDeviceCustomResolveFeaturesEXT::customResolve) -
                              offsetof(VkPhysicalDeviceCustomResolveFeaturesEXT, customResolve) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceCustomResolveFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DATA_GRAPH_FEATURES_ARM:
            static_assert(offsetof(VkPhysicalDeviceDataGraphFeaturesARM, dataGraph) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDataGraphFeaturesARM, dataGraphShaderModule) +
                              sizeof(VkPhysicalDeviceDataGraphFeaturesARM::dataGraphShaderModule) -
                              offsetof(VkPhysicalDeviceDataGraphFeaturesARM, dataGraph) ==
                          5 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDataGraphFeaturesARM), .body_size = 5 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DATA_GRAPH_MODEL_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceDataGraphModelFeaturesQCOM, dataGraphModel) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDataGraphModelFeaturesQCOM, dataGraphModel) +
                              sizeof(VkPhysicalDeviceDataGraphModelFeaturesQCOM::dataGraphModel) -
                              offsetof(VkPhysicalDeviceDataGraphModelFeaturesQCOM, dataGraphModel) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDataGraphModelFeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DATA_GRAPH_NEURAL_ACCELERATOR_STATISTICS_FEATURES_ARM:
            static_assert(offsetof(VkPhysicalDeviceDataGraphNeuralAcceleratorStatisticsFeaturesARM, dataGraphNeuralAcceleratorStatistics) ==
                          2 * sizeof(void*));
            static_assert(
                offsetof(VkPhysicalDeviceDataGraphNeuralAcceleratorStatisticsFeaturesARM, dataGraphNeuralAcceleratorStatistics) +
                    sizeof(VkPhysicalDeviceDataGraphNeuralAcceleratorStatisticsFeaturesARM::dataGraphNeuralAcceleratorStatistics) -
                    offsetof(VkPhysicalDeviceDataGraphNeuralAcceleratorStatisticsFeaturesARM, dataGraphNeuralAcceleratorStatistics) ==
                1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDataGraphNeuralAcceleratorStatisticsFeaturesARM),
                    .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DATA_GRAPH_OPTICAL_FLOW_FEATURES_ARM:
            static_assert(offsetof(VkPhysicalDeviceDataGraphOpticalFlowFeaturesARM, dataGraphOpticalFlow) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDataGraphOpticalFlowFeaturesARM, dataGraphOpticalFlow) +
                              sizeof(VkPhysicalDeviceDataGraphOpticalFlowFeaturesARM::dataGraphOpticalFlow) -
                              offsetof(VkPhysicalDeviceDataGraphOpticalFlowFeaturesARM, dataGraphOpticalFlow) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDataGraphOpticalFlowFeaturesARM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEDICATED_ALLOCATION_IMAGE_ALIASING_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV, dedicatedAllocationImageAliasing) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV, dedicatedAllocationImageAliasing) +
                              sizeof(VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV::dedicatedAllocationImageAliasing) -
                              offsetof(VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV, dedicatedAllocationImageAliasing) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV),
                    .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceDepthBiasControlFeaturesEXT, depthBiasControl) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDepthBiasControlFeaturesEXT, depthBiasExact) +
                              sizeof(VkPhysicalDeviceDepthBiasControlFeaturesEXT::depthBiasExact) -
                              offsetof(VkPhysicalDeviceDepthBiasControlFeaturesEXT, depthBiasControl) ==
                          4 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDepthBiasControlFeaturesEXT), .body_size = 4 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_CONTROL_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceDepthClampControlFeaturesEXT, depthClampControl) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDepthClampControlFeaturesEXT, depthClampControl) +
                              sizeof(VkPhysicalDeviceDepthClampControlFeaturesEXT::depthClampControl) -
                              offsetof(VkPhysicalDeviceDepthClampControlFeaturesEXT, depthClampControl) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDepthClampControlFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_ZERO_ONE_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceDepthClampZeroOneFeaturesKHR, depthClampZeroOne) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDepthClampZeroOneFeaturesKHR, depthClampZeroOne) +
                              sizeof(VkPhysicalDeviceDepthClampZeroOneFeaturesKHR::depthClampZeroOne) -
                              offsetof(VkPhysicalDeviceDepthClampZeroOneFeaturesKHR, depthClampZeroOne) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDepthClampZeroOneFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceDepthClipControlFeaturesEXT, depthClipControl) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDepthClipControlFeaturesEXT, depthClipControl) +
                              sizeof(VkPhysicalDeviceDepthClipControlFeaturesEXT::depthClipControl) -
                              offsetof(VkPhysicalDeviceDepthClipControlFeaturesEXT, depthClipControl) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDepthClipControlFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceDepthClipEnableFeaturesEXT, depthClipEnable) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDepthClipEnableFeaturesEXT, depthClipEnable) +
                              sizeof(VkPhysicalDeviceDepthClipEnableFeaturesEXT::depthClipEnable) -
                              offsetof(VkPhysicalDeviceDepthClipEnableFeaturesEXT, depthClipEnable) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDepthClipEnableFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceDescriptorBufferFeaturesEXT, descriptorBuffer) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDescriptorBufferFeaturesEXT, descriptorBufferPushDescriptors) +
                              sizeof(VkPhysicalDeviceDescriptorBufferFeaturesEXT::descriptorBufferPushDescriptors) -
                              offsetof(VkPhysicalDeviceDescriptorBufferFeaturesEXT, descriptorBuffer) ==
                          4 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDescriptorBufferFeaturesEXT), .body_size = 4 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_TENSOR_FEATURES_ARM:
            static_assert(offsetof(VkPhysicalDeviceDescriptorBufferTensorFeaturesARM, descriptorBufferTensorDescriptors) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDescriptorBufferTensorFeaturesARM, descriptorBufferTensorDescriptors) +
                              sizeof(VkPhysicalDeviceDescriptorBufferTensorFeaturesARM::descriptorBufferTensorDescriptors) -
                              offsetof(VkPhysicalDeviceDescriptorBufferTensorFeaturesARM, descriptorBufferTensorDescriptors) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDescriptorBufferTensorFeaturesARM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceDescriptorHeapFeaturesEXT, descriptorHeap) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDescriptorHeapFeaturesEXT, descriptorHeapCaptureReplay) +
                              sizeof(VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeapCaptureReplay) -
                              offsetof(VkPhysicalDeviceDescriptorHeapFeaturesEXT, descriptorHeap) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDescriptorHeapFeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceDescriptorIndexingFeatures, shaderInputAttachmentArrayDynamicIndexing) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDescriptorIndexingFeatures, runtimeDescriptorArray) +
                              sizeof(VkPhysicalDeviceDescriptorIndexingFeatures::runtimeDescriptorArray) -
                              offsetof(VkPhysicalDeviceDescriptorIndexingFeatures, shaderInputAttachmentArrayDynamicIndexing) ==
                          20 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDescriptorIndexingFeatures), .body_size = 20 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_POOL_OVERALLOCATION_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV, descriptorPoolOverallocation) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV, descriptorPoolOverallocation) +
                              sizeof(VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV::descriptorPoolOverallocation) -
                              offsetof(VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV, descriptorPoolOverallocation) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_SET_HOST_MAPPING_FEATURES_VALVE:
            static_assert(offsetof(VkPhysicalDeviceDescriptorSetHostMappingFeaturesVALVE, descriptorSetHostMapping) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDescriptorSetHostMappingFeaturesVALVE, descriptorSetHostMapping) +
                              sizeof(VkPhysicalDeviceDescriptorSetHostMappingFeaturesVALVE::descriptorSetHostMapping) -
                              offsetof(VkPhysicalDeviceDescriptorSetHostMappingFeaturesVALVE, descriptorSetHostMapping) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDescriptorSetHostMappingFeaturesVALVE), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR, deviceAddressCommands) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR, deviceAddressCommands) +
                              sizeof(VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR::deviceAddressCommands) -
                              offsetof(VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR, deviceAddressCommands) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_COMPUTE_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV, deviceGeneratedCompute) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV, deviceGeneratedComputeCaptureReplay) +
                              sizeof(VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV::deviceGeneratedComputeCaptureReplay) -
                              offsetof(VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV, deviceGeneratedCompute) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT, deviceGeneratedCommands) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT, dynamicGeneratedPipelineLayout) +
                              sizeof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT::dynamicGeneratedPipelineLayout) -
                              offsetof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT, deviceGeneratedCommands) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV, deviceGeneratedCommands) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV, deviceGeneratedCommands) +
                              sizeof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV::deviceGeneratedCommands) -
                              offsetof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV, deviceGeneratedCommands) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceDeviceMemoryReportFeaturesEXT, deviceMemoryReport) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDeviceMemoryReportFeaturesEXT, deviceMemoryReport) +
                              sizeof(VkPhysicalDeviceDeviceMemoryReportFeaturesEXT::deviceMemoryReport) -
                              offsetof(VkPhysicalDeviceDeviceMemoryReportFeaturesEXT, deviceMemoryReport) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDeviceMemoryReportFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceDiagnosticsConfigFeaturesNV, diagnosticsConfig) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDiagnosticsConfigFeaturesNV, diagnosticsConfig) +
                              sizeof(VkPhysicalDeviceDiagnosticsConfigFeaturesNV::diagnosticsConfig) -
                              offsetof(VkPhysicalDeviceDiagnosticsConfigFeaturesNV, diagnosticsConfig) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDiagnosticsConfigFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceDynamicRenderingFeatures, dynamicRendering) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDynamicRenderingFeatures, dynamicRendering) +
                              sizeof(VkPhysicalDeviceDynamicRenderingFeatures::dynamicRendering) -
                              offsetof(VkPhysicalDeviceDynamicRenderingFeatures, dynamicRendering) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDynamicRenderingFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceDynamicRenderingLocalReadFeatures, dynamicRenderingLocalRead) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDynamicRenderingLocalReadFeatures, dynamicRenderingLocalRead) +
                              sizeof(VkPhysicalDeviceDynamicRenderingLocalReadFeatures::dynamicRenderingLocalRead) -
                              offsetof(VkPhysicalDeviceDynamicRenderingLocalReadFeatures, dynamicRenderingLocalRead) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDynamicRenderingLocalReadFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT, dynamicRenderingUnusedAttachments) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT, dynamicRenderingUnusedAttachments) +
                              sizeof(VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT::dynamicRenderingUnusedAttachments) -
                              offsetof(VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT, dynamicRenderingUnusedAttachments) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT),
                    .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ELAPSED_TIMER_QUERY_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceElapsedTimerQueryFeaturesQCOM, elapsedTimerQuery) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceElapsedTimerQueryFeaturesQCOM, elapsedTimerQuery) +
                              sizeof(VkPhysicalDeviceElapsedTimerQueryFeaturesQCOM::elapsedTimerQuery) -
                              offsetof(VkPhysicalDeviceElapsedTimerQueryFeaturesQCOM, elapsedTimerQuery) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceElapsedTimerQueryFeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXCLUSIVE_SCISSOR_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceExclusiveScissorFeaturesNV, exclusiveScissor) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceExclusiveScissorFeaturesNV, exclusiveScissor) +
                              sizeof(VkPhysicalDeviceExclusiveScissorFeaturesNV::exclusiveScissor) -
                              offsetof(VkPhysicalDeviceExclusiveScissorFeaturesNV, exclusiveScissor) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceExclusiveScissorFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT, extendedDynamicState2) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT, extendedDynamicState2PatchControlPoints) +
                              sizeof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT::extendedDynamicState2PatchControlPoints) -
                              offsetof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT, extendedDynamicState2) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3TessellationDomainOrigin) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3ShadingRateImageEnable) +
                              sizeof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT::extendedDynamicState3ShadingRateImageEnable) -
                              offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3TessellationDomainOrigin) ==
                          31 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT), .body_size = 31 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT, extendedDynamicState) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT, extendedDynamicState) +
                              sizeof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT::extendedDynamicState) -
                              offsetof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT, extendedDynamicState) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_FLAGS_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceExtendedFlagsFeaturesKHR, extendedFlags) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceExtendedFlagsFeaturesKHR, extendedFlags) +
                              sizeof(VkPhysicalDeviceExtendedFlagsFeaturesKHR::extendedFlags) -
                              offsetof(VkPhysicalDeviceExtendedFlagsFeaturesKHR, extendedFlags) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceExtendedFlagsFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_SPARSE_ADDRESS_SPACE_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceExtendedSparseAddressSpaceFeaturesNV, extendedSparseAddressSpace) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceExtendedSparseAddressSpaceFeaturesNV, extendedSparseAddressSpace) +
                              sizeof(VkPhysicalDeviceExtendedSparseAddressSpaceFeaturesNV::extendedSparseAddressSpace) -
                              offsetof(VkPhysicalDeviceExtendedSparseAddressSpaceFeaturesNV, extendedSparseAddressSpace) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceExtendedSparseAddressSpaceFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_RDMA_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceExternalMemoryRDMAFeaturesNV, externalMemoryRDMA) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceExternalMemoryRDMAFeaturesNV, externalMemoryRDMA) +
                              sizeof(VkPhysicalDeviceExternalMemoryRDMAFeaturesNV::externalMemoryRDMA) -
                              offsetof(VkPhysicalDeviceExternalMemoryRDMAFeaturesNV, externalMemoryRDMA) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceExternalMemoryRDMAFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceFaultFeaturesEXT, deviceFault) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFaultFeaturesEXT, deviceFaultVendorBinary) +
                              sizeof(VkPhysicalDeviceFaultFeaturesEXT::deviceFaultVendorBinary) -
                              offsetof(VkPhysicalDeviceFaultFeaturesEXT, deviceFault) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFaultFeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceFaultFeaturesKHR, deviceFault) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFaultFeaturesKHR, deviceFaultDeviceLostOnMasked) +
                              sizeof(VkPhysicalDeviceFaultFeaturesKHR::deviceFaultDeviceLostOnMasked) -
                              offsetof(VkPhysicalDeviceFaultFeaturesKHR, deviceFault) ==
                          4 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFaultFeaturesKHR), .body_size = 4 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FORMAT_PACK_FEATURES_ARM:
            static_assert(offsetof(VkPhysicalDeviceFormatPackFeaturesARM, formatPack) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFormatPackFeaturesARM, formatPack) +
                              sizeof(VkPhysicalDeviceFormatPackFeaturesARM::formatPack) -
                              offsetof(VkPhysicalDeviceFormatPackFeaturesARM, formatPack) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFormatPackFeaturesARM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_2_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceFragmentDensityMap2FeaturesEXT, fragmentDensityMapDeferred) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFragmentDensityMap2FeaturesEXT, fragmentDensityMapDeferred) +
                              sizeof(VkPhysicalDeviceFragmentDensityMap2FeaturesEXT::fragmentDensityMapDeferred) -
                              offsetof(VkPhysicalDeviceFragmentDensityMap2FeaturesEXT, fragmentDensityMapDeferred) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFragmentDensityMap2FeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceFragmentDensityMapFeaturesEXT, fragmentDensityMap) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFragmentDensityMapFeaturesEXT, fragmentDensityMapNonSubsampledImages) +
                              sizeof(VkPhysicalDeviceFragmentDensityMapFeaturesEXT::fragmentDensityMapNonSubsampledImages) -
                              offsetof(VkPhysicalDeviceFragmentDensityMapFeaturesEXT, fragmentDensityMap) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFragmentDensityMapFeaturesEXT), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_LAYERED_FEATURES_VALVE:
            static_assert(offsetof(VkPhysicalDeviceFragmentDensityMapLayeredFeaturesVALVE, fragmentDensityMapLayered) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFragmentDensityMapLayeredFeaturesVALVE, fragmentDensityMapLayered) +
                              sizeof(VkPhysicalDeviceFragmentDensityMapLayeredFeaturesVALVE::fragmentDensityMapLayered) -
                              offsetof(VkPhysicalDeviceFragmentDensityMapLayeredFeaturesVALVE, fragmentDensityMapLayered) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFragmentDensityMapLayeredFeaturesVALVE), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_OFFSET_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceFragmentDensityMapOffsetFeaturesEXT, fragmentDensityMapOffset) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFragmentDensityMapOffsetFeaturesEXT, fragmentDensityMapOffset) +
                              sizeof(VkPhysicalDeviceFragmentDensityMapOffsetFeaturesEXT::fragmentDensityMapOffset) -
                              offsetof(VkPhysicalDeviceFragmentDensityMapOffsetFeaturesEXT, fragmentDensityMapOffset) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFragmentDensityMapOffsetFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR, fragmentShaderBarycentric) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR, fragmentShaderBarycentric) +
                              sizeof(VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR::fragmentShaderBarycentric) -
                              offsetof(VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR, fragmentShaderBarycentric) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT, fragmentShaderSampleInterlock) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT, fragmentShaderShadingRateInterlock) +
                              sizeof(VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT::fragmentShaderShadingRateInterlock) -
                              offsetof(VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT, fragmentShaderSampleInterlock) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV, fragmentShadingRateEnums) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV, noInvocationFragmentShadingRates) +
                              sizeof(VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV::noInvocationFragmentShadingRates) -
                              offsetof(VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV, fragmentShadingRateEnums) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceFragmentShadingRateFeaturesKHR, pipelineFragmentShadingRate) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFragmentShadingRateFeaturesKHR, attachmentFragmentShadingRate) +
                              sizeof(VkPhysicalDeviceFragmentShadingRateFeaturesKHR::attachmentFragmentShadingRate) -
                              offsetof(VkPhysicalDeviceFragmentShadingRateFeaturesKHR, pipelineFragmentShadingRate) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFragmentShadingRateFeaturesKHR), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAME_BOUNDARY_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceFrameBoundaryFeaturesEXT, frameBoundary) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceFrameBoundaryFeaturesEXT, frameBoundary) +
                              sizeof(VkPhysicalDeviceFrameBoundaryFeaturesEXT::frameBoundary) -
                              offsetof(VkPhysicalDeviceFrameBoundaryFeaturesEXT, frameBoundary) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceFrameBoundaryFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GLOBAL_PRIORITY_QUERY_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceGlobalPriorityQueryFeatures, globalPriorityQuery) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceGlobalPriorityQueryFeatures, globalPriorityQuery) +
                              sizeof(VkPhysicalDeviceGlobalPriorityQueryFeatures::globalPriorityQuery) -
                              offsetof(VkPhysicalDeviceGlobalPriorityQueryFeatures, globalPriorityQuery) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceGlobalPriorityQueryFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GPA_FEATURES_AMD:
            static_assert(offsetof(VkPhysicalDeviceGpaFeaturesAMD, perfCounters) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceGpaFeaturesAMD, clockModes) + sizeof(VkPhysicalDeviceGpaFeaturesAMD::clockModes) -
                              offsetof(VkPhysicalDeviceGpaFeaturesAMD, perfCounters) ==
                          4 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceGpaFeaturesAMD), .body_size = 4 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT, graphicsPipelineLibrary) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT, graphicsPipelineLibrary) +
                              sizeof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT::graphicsPipelineLibrary) -
                              offsetof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT, graphicsPipelineLibrary) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HDR_VIVID_FEATURES_HUAWEI:
            static_assert(offsetof(VkPhysicalDeviceHdrVividFeaturesHUAWEI, hdrVivid) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceHdrVividFeaturesHUAWEI, hdrVivid) +
                              sizeof(VkPhysicalDeviceHdrVividFeaturesHUAWEI::hdrVivid) -
                              offsetof(VkPhysicalDeviceHdrVividFeaturesHUAWEI, hdrVivid) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceHdrVividFeaturesHUAWEI), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceHostImageCopyFeatures, hostImageCopy) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceHostImageCopyFeatures, hostImageCopy) +
                              sizeof(VkPhysicalDeviceHostImageCopyFeatures::hostImageCopy) -
                              offsetof(VkPhysicalDeviceHostImageCopyFeatures, hostImageCopy) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceHostImageCopyFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceHostQueryResetFeatures, hostQueryReset) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceHostQueryResetFeatures, hostQueryReset) +
                              sizeof(VkPhysicalDeviceHostQueryResetFeatures::hostQueryReset) -
                              offsetof(VkPhysicalDeviceHostQueryResetFeatures, hostQueryReset) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceHostQueryResetFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceImage2DViewOf3DFeaturesEXT, image2DViewOf3D) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceImage2DViewOf3DFeaturesEXT, sampler2DViewOf3D) +
                              sizeof(VkPhysicalDeviceImage2DViewOf3DFeaturesEXT::sampler2DViewOf3D) -
                              offsetof(VkPhysicalDeviceImage2DViewOf3DFeaturesEXT, image2DViewOf3D) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceImage2DViewOf3DFeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ALIGNMENT_CONTROL_FEATURES_MESA:
            static_assert(offsetof(VkPhysicalDeviceImageAlignmentControlFeaturesMESA, imageAlignmentControl) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceImageAlignmentControlFeaturesMESA, imageAlignmentControl) +
                              sizeof(VkPhysicalDeviceImageAlignmentControlFeaturesMESA::imageAlignmentControl) -
                              offsetof(VkPhysicalDeviceImageAlignmentControlFeaturesMESA, imageAlignmentControl) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceImageAlignmentControlFeaturesMESA), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceImageCompressionControlFeaturesEXT, imageCompressionControl) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceImageCompressionControlFeaturesEXT, imageCompressionControl) +
                              sizeof(VkPhysicalDeviceImageCompressionControlFeaturesEXT::imageCompressionControl) -
                              offsetof(VkPhysicalDeviceImageCompressionControlFeaturesEXT, imageCompressionControl) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceImageCompressionControlFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT, imageCompressionControlSwapchain) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT, imageCompressionControlSwapchain) +
                              sizeof(VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT::imageCompressionControlSwapchain) -
                              offsetof(VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT, imageCompressionControlSwapchain) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT),
                    .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_2_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceImageProcessing2FeaturesQCOM, textureBlockMatch2) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceImageProcessing2FeaturesQCOM, textureBlockMatch2) +
                              sizeof(VkPhysicalDeviceImageProcessing2FeaturesQCOM::textureBlockMatch2) -
                              offsetof(VkPhysicalDeviceImageProcessing2FeaturesQCOM, textureBlockMatch2) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceImageProcessing2FeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_3_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceImageProcessing3FeaturesQCOM, imageGatherLinear) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceImageProcessing3FeaturesQCOM, blockMatchExtendedClampToEdge) +
                              sizeof(VkPhysicalDeviceImageProcessing3FeaturesQCOM::blockMatchExtendedClampToEdge) -
                              offsetof(VkPhysicalDeviceImageProcessing3FeaturesQCOM, imageGatherLinear) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceImageProcessing3FeaturesQCOM), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceImageProcessingFeaturesQCOM, textureSampleWeighted) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceImageProcessingFeaturesQCOM, textureBlockMatch) +
                              sizeof(VkPhysicalDeviceImageProcessingFeaturesQCOM::textureBlockMatch) -
                              offsetof(VkPhysicalDeviceImageProcessingFeaturesQCOM, textureSampleWeighted) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceImageProcessingFeaturesQCOM), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceImageRobustnessFeatures, robustImageAccess) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceImageRobustnessFeatures, robustImageAccess) +
                              sizeof(VkPhysicalDeviceImageRobustnessFeatures::robustImageAccess) -
                              offsetof(VkPhysicalDeviceImageRobustnessFeatures, robustImageAccess) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceImageRobustnessFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_SLICED_VIEW_OF_3D_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT, imageSlicedViewOf3D) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT, imageSlicedViewOf3D) +
                              sizeof(VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT::imageSlicedViewOf3D) -
                              offsetof(VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT, imageSlicedViewOf3D) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_TILING_CONTROL_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceImageTilingControlFeaturesEXT, imageTilingControl) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceImageTilingControlFeaturesEXT, imageTilingControl) +
                              sizeof(VkPhysicalDeviceImageTilingControlFeaturesEXT::imageTilingControl) -
                              offsetof(VkPhysicalDeviceImageTilingControlFeaturesEXT, imageTilingControl) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceImageTilingControlFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceImageViewMinLodFeaturesEXT, minLod) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceImageViewMinLodFeaturesEXT, minLod) +
                              sizeof(VkPhysicalDeviceImageViewMinLodFeaturesEXT::minLod) -
                              offsetof(VkPhysicalDeviceImageViewMinLodFeaturesEXT, minLod) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceImageViewMinLodFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceImagelessFramebufferFeatures, imagelessFramebuffer) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceImagelessFramebufferFeatures, imagelessFramebuffer) +
                              sizeof(VkPhysicalDeviceImagelessFramebufferFeatures::imagelessFramebuffer) -
                              offsetof(VkPhysicalDeviceImagelessFramebufferFeatures, imagelessFramebuffer) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceImagelessFramebufferFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceIndexTypeUint8Features, indexTypeUint8) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceIndexTypeUint8Features, indexTypeUint8) +
                              sizeof(VkPhysicalDeviceIndexTypeUint8Features::indexTypeUint8) -
                              offsetof(VkPhysicalDeviceIndexTypeUint8Features, indexTypeUint8) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceIndexTypeUint8Features), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INHERITED_VIEWPORT_SCISSOR_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceInheritedViewportScissorFeaturesNV, inheritedViewportScissor2D) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceInheritedViewportScissorFeaturesNV, inheritedViewportScissor2D) +
                              sizeof(VkPhysicalDeviceInheritedViewportScissorFeaturesNV::inheritedViewportScissor2D) -
                              offsetof(VkPhysicalDeviceInheritedViewportScissorFeaturesNV, inheritedViewportScissor2D) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceInheritedViewportScissorFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceInlineUniformBlockFeatures, inlineUniformBlock) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceInlineUniformBlockFeatures, descriptorBindingInlineUniformBlockUpdateAfterBind) +
                              sizeof(VkPhysicalDeviceInlineUniformBlockFeatures::descriptorBindingInlineUniformBlockUpdateAfterBind) -
                              offsetof(VkPhysicalDeviceInlineUniformBlockFeatures, inlineUniformBlock) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceInlineUniformBlockFeatures), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INTERNALLY_SYNCHRONIZED_QUEUES_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR, internallySynchronizedQueues) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR, internallySynchronizedQueues) +
                              sizeof(VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR::internallySynchronizedQueues) -
                              offsetof(VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR, internallySynchronizedQueues) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INVOCATION_MASK_FEATURES_HUAWEI:
            static_assert(offsetof(VkPhysicalDeviceInvocationMaskFeaturesHUAWEI, invocationMask) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceInvocationMaskFeaturesHUAWEI, invocationMask) +
                              sizeof(VkPhysicalDeviceInvocationMaskFeaturesHUAWEI::invocationMask) -
                              offsetof(VkPhysicalDeviceInvocationMaskFeaturesHUAWEI, invocationMask) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceInvocationMaskFeaturesHUAWEI), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_DITHERING_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceLegacyDitheringFeaturesEXT, legacyDithering) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceLegacyDitheringFeaturesEXT, legacyDithering) +
                              sizeof(VkPhysicalDeviceLegacyDitheringFeaturesEXT::legacyDithering) -
                              offsetof(VkPhysicalDeviceLegacyDitheringFeaturesEXT, legacyDithering) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceLegacyDitheringFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT, legacyVertexAttributes) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT, legacyVertexAttributes) +
                              sizeof(VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT::legacyVertexAttributes) -
                              offsetof(VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT, legacyVertexAttributes) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceLineRasterizationFeatures, rectangularLines) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceLineRasterizationFeatures, stippledSmoothLines) +
                              sizeof(VkPhysicalDeviceLineRasterizationFeatures::stippledSmoothLines) -
                              offsetof(VkPhysicalDeviceLineRasterizationFeatures, rectangularLines) ==
                          6 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceLineRasterizationFeatures), .body_size = 6 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINEAR_COLOR_ATTACHMENT_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceLinearColorAttachmentFeaturesNV, linearColorAttachment) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceLinearColorAttachmentFeaturesNV, linearColorAttachment) +
                              sizeof(VkPhysicalDeviceLinearColorAttachmentFeaturesNV::linearColorAttachment) -
                              offsetof(VkPhysicalDeviceLinearColorAttachmentFeaturesNV, linearColorAttachment) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceLinearColorAttachmentFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_10_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceMaintenance10FeaturesKHR, maintenance10) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMaintenance10FeaturesKHR, maintenance10) +
                              sizeof(VkPhysicalDeviceMaintenance10FeaturesKHR::maintenance10) -
                              offsetof(VkPhysicalDeviceMaintenance10FeaturesKHR, maintenance10) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMaintenance10FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_11_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceMaintenance11FeaturesKHR, maintenance11) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMaintenance11FeaturesKHR, maintenance11) +
                              sizeof(VkPhysicalDeviceMaintenance11FeaturesKHR::maintenance11) -
                              offsetof(VkPhysicalDeviceMaintenance11FeaturesKHR, maintenance11) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMaintenance11FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceMaintenance4Features, maintenance4) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMaintenance4Features, maintenance4) +
                              sizeof(VkPhysicalDeviceMaintenance4Features::maintenance4) -
                              offsetof(VkPhysicalDeviceMaintenance4Features, maintenance4) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMaintenance4Features), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceMaintenance5Features, maintenance5) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMaintenance5Features, maintenance5) +
                              sizeof(VkPhysicalDeviceMaintenance5Features::maintenance5) -
                              offsetof(VkPhysicalDeviceMaintenance5Features, maintenance5) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMaintenance5Features), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceMaintenance6Features, maintenance6) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMaintenance6Features, maintenance6) +
                              sizeof(VkPhysicalDeviceMaintenance6Features::maintenance6) -
                              offsetof(VkPhysicalDeviceMaintenance6Features, maintenance6) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMaintenance6Features), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceMaintenance7FeaturesKHR, maintenance7) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMaintenance7FeaturesKHR, maintenance7) +
                              sizeof(VkPhysicalDeviceMaintenance7FeaturesKHR::maintenance7) -
                              offsetof(VkPhysicalDeviceMaintenance7FeaturesKHR, maintenance7) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMaintenance7FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceMaintenance8FeaturesKHR, maintenance8) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMaintenance8FeaturesKHR, maintenance8) +
                              sizeof(VkPhysicalDeviceMaintenance8FeaturesKHR::maintenance8) -
                              offsetof(VkPhysicalDeviceMaintenance8FeaturesKHR, maintenance8) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMaintenance8FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceMaintenance9FeaturesKHR, maintenance9) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMaintenance9FeaturesKHR, maintenance9) +
                              sizeof(VkPhysicalDeviceMaintenance9FeaturesKHR::maintenance9) -
                              offsetof(VkPhysicalDeviceMaintenance9FeaturesKHR, maintenance9) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMaintenance9FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceMapMemoryPlacedFeaturesEXT, memoryMapPlaced) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMapMemoryPlacedFeaturesEXT, memoryUnmapReserve) +
                              sizeof(VkPhysicalDeviceMapMemoryPlacedFeaturesEXT::memoryUnmapReserve) -
                              offsetof(VkPhysicalDeviceMapMemoryPlacedFeaturesEXT, memoryMapPlaced) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMapMemoryPlacedFeaturesEXT), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceMemoryDecompressionFeaturesEXT, memoryDecompression) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMemoryDecompressionFeaturesEXT, memoryDecompression) +
                              sizeof(VkPhysicalDeviceMemoryDecompressionFeaturesEXT::memoryDecompression) -
                              offsetof(VkPhysicalDeviceMemoryDecompressionFeaturesEXT, memoryDecompression) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMemoryDecompressionFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceMemoryPriorityFeaturesEXT, memoryPriority) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMemoryPriorityFeaturesEXT, memoryPriority) +
                              sizeof(VkPhysicalDeviceMemoryPriorityFeaturesEXT::memoryPriority) -
                              offsetof(VkPhysicalDeviceMemoryPriorityFeaturesEXT, memoryPriority) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMemoryPriorityFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceMeshShaderFeaturesEXT, taskShader) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMeshShaderFeaturesEXT, meshShaderQueries) +
                              sizeof(VkPhysicalDeviceMeshShaderFeaturesEXT::meshShaderQueries) -
                              offsetof(VkPhysicalDeviceMeshShaderFeaturesEXT, taskShader) ==
                          5 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMeshShaderFeaturesEXT), .body_size = 5 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceMeshShaderFeaturesNV, taskShader) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMeshShaderFeaturesNV, meshShader) +
                              sizeof(VkPhysicalDeviceMeshShaderFeaturesNV::meshShader) -
                              offsetof(VkPhysicalDeviceMeshShaderFeaturesNV, taskShader) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMeshShaderFeaturesNV), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceMultiDrawFeaturesEXT, multiDraw) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMultiDrawFeaturesEXT, multiDraw) +
                              sizeof(VkPhysicalDeviceMultiDrawFeaturesEXT::multiDraw) -
                              offsetof(VkPhysicalDeviceMultiDrawFeaturesEXT, multiDraw) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMultiDrawFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT, multisampledRenderToSingleSampled) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT, multisampledRenderToSingleSampled) +
                              sizeof(VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT::multisampledRenderToSingleSampled) -
                              offsetof(VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT, multisampledRenderToSingleSampled) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT),
                    .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTISAMPLED_RENDER_TO_SWAPCHAIN_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceMultisampledRenderToSwapchainFeaturesEXT, multisampledRenderToSwapchain) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMultisampledRenderToSwapchainFeaturesEXT, multisampledRenderToSwapchain) +
                              sizeof(VkPhysicalDeviceMultisampledRenderToSwapchainFeaturesEXT::multisampledRenderToSwapchain) -
                              offsetof(VkPhysicalDeviceMultisampledRenderToSwapchainFeaturesEXT, multisampledRenderToSwapchain) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMultisampledRenderToSwapchainFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceMultiviewFeatures, multiview) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMultiviewFeatures, multiviewTessellationShader) +
                              sizeof(VkPhysicalDeviceMultiviewFeatures::multiviewTessellationShader) -
                              offsetof(VkPhysicalDeviceMultiviewFeatures, multiview) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMultiviewFeatures), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PER_VIEW_RENDER_AREAS_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM, multiviewPerViewRenderAreas) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM, multiviewPerViewRenderAreas) +
                              sizeof(VkPhysicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM::multiviewPerViewRenderAreas) -
                              offsetof(VkPhysicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM, multiviewPerViewRenderAreas) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PER_VIEW_VIEWPORTS_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceMultiviewPerViewViewportsFeaturesQCOM, multiviewPerViewViewports) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMultiviewPerViewViewportsFeaturesQCOM, multiviewPerViewViewports) +
                              sizeof(VkPhysicalDeviceMultiviewPerViewViewportsFeaturesQCOM::multiviewPerViewViewports) -
                              offsetof(VkPhysicalDeviceMultiviewPerViewViewportsFeaturesQCOM, multiviewPerViewViewports) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMultiviewPerViewViewportsFeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT, mutableDescriptorType) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT, mutableDescriptorType) +
                              sizeof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT::mutableDescriptorType) -
                              offsetof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT, mutableDescriptorType) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NESTED_COMMAND_BUFFER_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceNestedCommandBufferFeaturesEXT, nestedCommandBuffer) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceNestedCommandBufferFeaturesEXT, nestedCommandBufferSimultaneousUse) +
                              sizeof(VkPhysicalDeviceNestedCommandBufferFeaturesEXT::nestedCommandBufferSimultaneousUse) -
                              offsetof(VkPhysicalDeviceNestedCommandBufferFeaturesEXT, nestedCommandBuffer) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceNestedCommandBufferFeaturesEXT), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT, nonSeamlessCubeMap) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT, nonSeamlessCubeMap) +
                              sizeof(VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT::nonSeamlessCubeMap) -
                              offsetof(VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT, nonSeamlessCubeMap) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceOpacityMicromapFeaturesEXT, micromap) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceOpacityMicromapFeaturesEXT, micromapHostCommands) +
                              sizeof(VkPhysicalDeviceOpacityMicromapFeaturesEXT::micromapHostCommands) -
                              offsetof(VkPhysicalDeviceOpacityMicromapFeaturesEXT, micromap) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceOpacityMicromapFeaturesEXT), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceOpacityMicromapFeaturesKHR, micromap) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceOpacityMicromapFeaturesKHR, micromap) +
                              sizeof(VkPhysicalDeviceOpacityMicromapFeaturesKHR::micromap) -
                              offsetof(VkPhysicalDeviceOpacityMicromapFeaturesKHR, micromap) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceOpacityMicromapFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceOpticalFlowFeaturesNV, opticalFlow) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceOpticalFlowFeaturesNV, opticalFlow) +
                              sizeof(VkPhysicalDeviceOpticalFlowFeaturesNV::opticalFlow) -
                              offsetof(VkPhysicalDeviceOpticalFlowFeaturesNV, opticalFlow) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceOpticalFlowFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT, pageableDeviceLocalMemory) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT, pageableDeviceLocalMemory) +
                              sizeof(VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT::pageableDeviceLocalMemory) -
                              offsetof(VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT, pageableDeviceLocalMemory) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PARTITIONED_ACCELERATION_STRUCTURE_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV, partitionedAccelerationStructure) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV, partitionedAccelerationStructure) +
                              sizeof(VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV::partitionedAccelerationStructure) -
                              offsetof(VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV, partitionedAccelerationStructure) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV),
                    .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDevicePerStageDescriptorSetFeaturesNV, perStageDescriptorSet) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePerStageDescriptorSetFeaturesNV, dynamicPipelineLayout) +
                              sizeof(VkPhysicalDevicePerStageDescriptorSetFeaturesNV::dynamicPipelineLayout) -
                              offsetof(VkPhysicalDevicePerStageDescriptorSetFeaturesNV, perStageDescriptorSet) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePerStageDescriptorSetFeaturesNV), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_COUNTERS_BY_REGION_FEATURES_ARM:
            static_assert(offsetof(VkPhysicalDevicePerformanceCountersByRegionFeaturesARM, performanceCountersByRegion) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePerformanceCountersByRegionFeaturesARM, performanceCountersByRegion) +
                              sizeof(VkPhysicalDevicePerformanceCountersByRegionFeaturesARM::performanceCountersByRegion) -
                              offsetof(VkPhysicalDevicePerformanceCountersByRegionFeaturesARM, performanceCountersByRegion) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePerformanceCountersByRegionFeaturesARM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDevicePerformanceQueryFeaturesKHR, performanceCounterQueryPools) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePerformanceQueryFeaturesKHR, performanceCounterMultipleQueryPools) +
                              sizeof(VkPhysicalDevicePerformanceQueryFeaturesKHR::performanceCounterMultipleQueryPools) -
                              offsetof(VkPhysicalDevicePerformanceQueryFeaturesKHR, performanceCounterQueryPools) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePerformanceQueryFeaturesKHR), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDevicePipelineBinaryFeaturesKHR, pipelineBinaries) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePipelineBinaryFeaturesKHR, pipelineBinaries) +
                              sizeof(VkPhysicalDevicePipelineBinaryFeaturesKHR::pipelineBinaries) -
                              offsetof(VkPhysicalDevicePipelineBinaryFeaturesKHR, pipelineBinaries) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePipelineBinaryFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CACHE_INCREMENTAL_MODE_FEATURES_SEC:
            static_assert(offsetof(VkPhysicalDevicePipelineCacheIncrementalModeFeaturesSEC, pipelineCacheIncrementalMode) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePipelineCacheIncrementalModeFeaturesSEC, pipelineCacheIncrementalMode) +
                              sizeof(VkPhysicalDevicePipelineCacheIncrementalModeFeaturesSEC::pipelineCacheIncrementalMode) -
                              offsetof(VkPhysicalDevicePipelineCacheIncrementalModeFeaturesSEC, pipelineCacheIncrementalMode) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePipelineCacheIncrementalModeFeaturesSEC), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES:
            static_assert(offsetof(VkPhysicalDevicePipelineCreationCacheControlFeatures, pipelineCreationCacheControl) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePipelineCreationCacheControlFeatures, pipelineCreationCacheControl) +
                              sizeof(VkPhysicalDevicePipelineCreationCacheControlFeatures::pipelineCreationCacheControl) -
                              offsetof(VkPhysicalDevicePipelineCreationCacheControlFeatures, pipelineCreationCacheControl) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePipelineCreationCacheControlFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR, pipelineExecutableInfo) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR, pipelineExecutableInfo) +
                              sizeof(VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR::pipelineExecutableInfo) -
                              offsetof(VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR, pipelineExecutableInfo) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_LIBRARY_GROUP_HANDLES_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT, pipelineLibraryGroupHandles) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT, pipelineLibraryGroupHandles) +
                              sizeof(VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT::pipelineLibraryGroupHandles) -
                              offsetof(VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT, pipelineLibraryGroupHandles) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_OPACITY_MICROMAP_FEATURES_ARM:
            static_assert(offsetof(VkPhysicalDevicePipelineOpacityMicromapFeaturesARM, pipelineOpacityMicromap) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePipelineOpacityMicromapFeaturesARM, pipelineOpacityMicromap) +
                              sizeof(VkPhysicalDevicePipelineOpacityMicromapFeaturesARM::pipelineOpacityMicromap) -
                              offsetof(VkPhysicalDevicePipelineOpacityMicromapFeaturesARM, pipelineOpacityMicromap) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePipelineOpacityMicromapFeaturesARM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROPERTIES_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDevicePipelinePropertiesFeaturesEXT, pipelinePropertiesIdentifier) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePipelinePropertiesFeaturesEXT, pipelinePropertiesIdentifier) +
                              sizeof(VkPhysicalDevicePipelinePropertiesFeaturesEXT::pipelinePropertiesIdentifier) -
                              offsetof(VkPhysicalDevicePipelinePropertiesFeaturesEXT, pipelinePropertiesIdentifier) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePipelinePropertiesFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROTECTED_ACCESS_FEATURES:
            static_assert(offsetof(VkPhysicalDevicePipelineProtectedAccessFeatures, pipelineProtectedAccess) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePipelineProtectedAccessFeatures, pipelineProtectedAccess) +
                              sizeof(VkPhysicalDevicePipelineProtectedAccessFeatures::pipelineProtectedAccess) -
                              offsetof(VkPhysicalDevicePipelineProtectedAccessFeatures, pipelineProtectedAccess) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePipelineProtectedAccessFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES:
            static_assert(offsetof(VkPhysicalDevicePipelineRobustnessFeatures, pipelineRobustness) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePipelineRobustnessFeatures, pipelineRobustness) +
                              sizeof(VkPhysicalDevicePipelineRobustnessFeatures::pipelineRobustness) -
                              offsetof(VkPhysicalDevicePipelineRobustnessFeatures, pipelineRobustness) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePipelineRobustnessFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_BARRIER_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDevicePresentBarrierFeaturesNV, presentBarrier) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePresentBarrierFeaturesNV, presentBarrier) +
                              sizeof(VkPhysicalDevicePresentBarrierFeaturesNV::presentBarrier) -
                              offsetof(VkPhysicalDevicePresentBarrierFeaturesNV, presentBarrier) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePresentBarrierFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDevicePresentId2FeaturesKHR, presentId2) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePresentId2FeaturesKHR, presentId2) +
                              sizeof(VkPhysicalDevicePresentId2FeaturesKHR::presentId2) -
                              offsetof(VkPhysicalDevicePresentId2FeaturesKHR, presentId2) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePresentId2FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDevicePresentIdFeaturesKHR, presentId) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePresentIdFeaturesKHR, presentId) +
                              sizeof(VkPhysicalDevicePresentIdFeaturesKHR::presentId) -
                              offsetof(VkPhysicalDevicePresentIdFeaturesKHR, presentId) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePresentIdFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_METERING_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDevicePresentMeteringFeaturesNV, presentMetering) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePresentMeteringFeaturesNV, presentMetering) +
                              sizeof(VkPhysicalDevicePresentMeteringFeaturesNV::presentMetering) -
                              offsetof(VkPhysicalDevicePresentMeteringFeaturesNV, presentMetering) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePresentMeteringFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR, presentModeFifoLatestReady) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR, presentModeFifoLatestReady) +
                              sizeof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR::presentModeFifoLatestReady) -
                              offsetof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR, presentModeFifoLatestReady) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentTiming) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentAtRelativeTime) +
                              sizeof(VkPhysicalDevicePresentTimingFeaturesEXT::presentAtRelativeTime) -
                              offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentTiming) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePresentTimingFeaturesEXT), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDevicePresentWait2FeaturesKHR, presentWait2) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePresentWait2FeaturesKHR, presentWait2) +
                              sizeof(VkPhysicalDevicePresentWait2FeaturesKHR::presentWait2) -
                              offsetof(VkPhysicalDevicePresentWait2FeaturesKHR, presentWait2) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePresentWait2FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDevicePresentWaitFeaturesKHR, presentWait) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePresentWaitFeaturesKHR, presentWait) +
                              sizeof(VkPhysicalDevicePresentWaitFeaturesKHR::presentWait) -
                              offsetof(VkPhysicalDevicePresentWaitFeaturesKHR, presentWait) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePresentWaitFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_RESTART_INDEX_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDevicePrimitiveRestartIndexFeaturesEXT, primitiveRestartIndex) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePrimitiveRestartIndexFeaturesEXT, primitiveRestartIndex) +
                              sizeof(VkPhysicalDevicePrimitiveRestartIndexFeaturesEXT::primitiveRestartIndex) -
                              offsetof(VkPhysicalDevicePrimitiveRestartIndexFeaturesEXT, primitiveRestartIndex) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePrimitiveRestartIndexFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT, primitiveTopologyListRestart) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT, primitiveTopologyPatchListRestart) +
                              sizeof(VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT::primitiveTopologyPatchListRestart) -
                              offsetof(VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT, primitiveTopologyListRestart) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT, primitivesGeneratedQuery) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT, primitivesGeneratedQueryWithNonZeroStreams) +
                              sizeof(VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT::primitivesGeneratedQueryWithNonZeroStreams) -
                              offsetof(VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT, primitivesGeneratedQuery) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_BASE_HANDLE_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDevicePrivateDataBaseHandleFeaturesNV, privateDataBaseHandle) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePrivateDataBaseHandleFeaturesNV, privateDataBaseHandle) +
                              sizeof(VkPhysicalDevicePrivateDataBaseHandleFeaturesNV::privateDataBaseHandle) -
                              offsetof(VkPhysicalDevicePrivateDataBaseHandleFeaturesNV, privateDataBaseHandle) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePrivateDataBaseHandleFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES:
            static_assert(offsetof(VkPhysicalDevicePrivateDataFeatures, privateData) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePrivateDataFeatures, privateData) +
                              sizeof(VkPhysicalDevicePrivateDataFeatures::privateData) -
                              offsetof(VkPhysicalDevicePrivateDataFeatures, privateData) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePrivateDataFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceProtectedMemoryFeatures, protectedMemory) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceProtectedMemoryFeatures, protectedMemory) +
                              sizeof(VkPhysicalDeviceProtectedMemoryFeatures::protectedMemory) -
                              offsetof(VkPhysicalDeviceProtectedMemoryFeatures, protectedMemory) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceProtectedMemoryFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceProvokingVertexFeaturesEXT, provokingVertexLast) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceProvokingVertexFeaturesEXT, transformFeedbackPreservesProvokingVertex) +
                              sizeof(VkPhysicalDeviceProvokingVertexFeaturesEXT::transformFeedbackPreservesProvokingVertex) -
                              offsetof(VkPhysicalDeviceProvokingVertexFeaturesEXT, provokingVertexLast) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceProvokingVertexFeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_CONSTANT_BANK_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDevicePushConstantBankFeaturesNV, pushConstantBank) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDevicePushConstantBankFeaturesNV, pushConstantBank) +
                              sizeof(VkPhysicalDevicePushConstantBankFeaturesNV::pushConstantBank) -
                              offsetof(VkPhysicalDevicePushConstantBankFeaturesNV, pushConstantBank) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDevicePushConstantBankFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_QUEUE_PERF_HINT_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceQueuePerfHintFeaturesQCOM, queuePerfHint) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceQueuePerfHintFeaturesQCOM, queuePerfHint) +
                              sizeof(VkPhysicalDeviceQueuePerfHintFeaturesQCOM::queuePerfHint) -
                              offsetof(VkPhysicalDeviceQueuePerfHintFeaturesQCOM, queuePerfHint) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceQueuePerfHintFeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RGBA10X6_FORMATS_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceRGBA10X6FormatsFeaturesEXT, formatRgba10x6WithoutYCbCrSampler) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRGBA10X6FormatsFeaturesEXT, formatRgba10x6WithoutYCbCrSampler) +
                              sizeof(VkPhysicalDeviceRGBA10X6FormatsFeaturesEXT::formatRgba10x6WithoutYCbCrSampler) -
                              offsetof(VkPhysicalDeviceRGBA10X6FormatsFeaturesEXT, formatRgba10x6WithoutYCbCrSampler) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRGBA10X6FormatsFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT,
                                   rasterizationOrderColorAttachmentAccess) == 2 * sizeof(void*));
            static_assert(
                offsetof(VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT, rasterizationOrderStencilAttachmentAccess) +
                    sizeof(VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT::rasterizationOrderStencilAttachmentAccess) -
                    offsetof(VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT, rasterizationOrderColorAttachmentAccess) ==
                3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT),
                    .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAW_ACCESS_CHAINS_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceRawAccessChainsFeaturesNV, shaderRawAccessChains) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRawAccessChainsFeaturesNV, shaderRawAccessChains) +
                              sizeof(VkPhysicalDeviceRawAccessChainsFeaturesNV::shaderRawAccessChains) -
                              offsetof(VkPhysicalDeviceRawAccessChainsFeaturesNV, shaderRawAccessChains) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRawAccessChainsFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceRayQueryFeaturesKHR, rayQuery) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRayQueryFeaturesKHR, rayQuery) + sizeof(VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery) -
                              offsetof(VkPhysicalDeviceRayQueryFeaturesKHR, rayQuery) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRayQueryFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT, rayTracingInvocationReorder) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT, rayTracingInvocationReorder) +
                              sizeof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT::rayTracingInvocationReorder) -
                              offsetof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT, rayTracingInvocationReorder) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV, rayTracingInvocationReorder) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV, rayTracingInvocationReorder) +
                              sizeof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV::rayTracingInvocationReorder) -
                              offsetof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV, rayTracingInvocationReorder) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_LINEAR_SWEPT_SPHERES_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV, spheres) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV, linearSweptSpheres) +
                              sizeof(VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV::linearSweptSpheres) -
                              offsetof(VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV, spheres) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR, rayTracingMaintenance1) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR, rayTracingPipelineTraceRaysIndirect2) +
                              sizeof(VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR::rayTracingPipelineTraceRaysIndirect2) -
                              offsetof(VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR, rayTracingMaintenance1) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MOTION_BLUR_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceRayTracingMotionBlurFeaturesNV, rayTracingMotionBlur) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRayTracingMotionBlurFeaturesNV, rayTracingMotionBlurPipelineTraceRaysIndirect) +
                              sizeof(VkPhysicalDeviceRayTracingMotionBlurFeaturesNV::rayTracingMotionBlurPipelineTraceRaysIndirect) -
                              offsetof(VkPhysicalDeviceRayTracingMotionBlurFeaturesNV, rayTracingMotionBlur) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRayTracingMotionBlurFeaturesNV), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceRayTracingPipelineFeaturesKHR, rayTracingPipeline) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRayTracingPipelineFeaturesKHR, rayTraversalPrimitiveCulling) +
                              sizeof(VkPhysicalDeviceRayTracingPipelineFeaturesKHR::rayTraversalPrimitiveCulling) -
                              offsetof(VkPhysicalDeviceRayTracingPipelineFeaturesKHR, rayTracingPipeline) ==
                          5 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRayTracingPipelineFeaturesKHR), .body_size = 5 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR, rayTracingPositionFetch) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR, rayTracingPositionFetch) +
                              sizeof(VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR::rayTracingPositionFetch) -
                              offsetof(VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR, rayTracingPositionFetch) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_VALIDATION_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceRayTracingValidationFeaturesNV, rayTracingValidation) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRayTracingValidationFeaturesNV, rayTracingValidation) +
                              sizeof(VkPhysicalDeviceRayTracingValidationFeaturesNV::rayTracingValidation) -
                              offsetof(VkPhysicalDeviceRayTracingValidationFeaturesNV, rayTracingValidation) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRayTracingValidationFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG:
            static_assert(offsetof(VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG, relaxedLineRasterization) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG, relaxedLineRasterization) +
                              sizeof(VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG::relaxedLineRasterization) -
                              offsetof(VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG, relaxedLineRasterization) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RENDER_PASS_STRIPED_FEATURES_ARM:
            static_assert(offsetof(VkPhysicalDeviceRenderPassStripedFeaturesARM, renderPassStriped) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRenderPassStripedFeaturesARM, renderPassStriped) +
                              sizeof(VkPhysicalDeviceRenderPassStripedFeaturesARM::renderPassStriped) -
                              offsetof(VkPhysicalDeviceRenderPassStripedFeaturesARM, renderPassStriped) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRenderPassStripedFeaturesARM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_REPRESENTATIVE_FRAGMENT_TEST_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceRepresentativeFragmentTestFeaturesNV, representativeFragmentTest) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRepresentativeFragmentTestFeaturesNV, representativeFragmentTest) +
                              sizeof(VkPhysicalDeviceRepresentativeFragmentTestFeaturesNV::representativeFragmentTest) -
                              offsetof(VkPhysicalDeviceRepresentativeFragmentTestFeaturesNV, representativeFragmentTest) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRepresentativeFragmentTestFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceRobustness2FeaturesKHR, robustBufferAccess2) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceRobustness2FeaturesKHR, nullDescriptor) +
                              sizeof(VkPhysicalDeviceRobustness2FeaturesKHR::nullDescriptor) -
                              offsetof(VkPhysicalDeviceRobustness2FeaturesKHR, robustBufferAccess2) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceRobustness2FeaturesKHR), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceSamplerYcbcrConversionFeatures, samplerYcbcrConversion) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceSamplerYcbcrConversionFeatures, samplerYcbcrConversion) +
                              sizeof(VkPhysicalDeviceSamplerYcbcrConversionFeatures::samplerYcbcrConversion) -
                              offsetof(VkPhysicalDeviceSamplerYcbcrConversionFeatures, samplerYcbcrConversion) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceSamplerYcbcrConversionFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceScalarBlockLayoutFeatures, scalarBlockLayout) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceScalarBlockLayoutFeatures, scalarBlockLayout) +
                              sizeof(VkPhysicalDeviceScalarBlockLayoutFeatures::scalarBlockLayout) -
                              offsetof(VkPhysicalDeviceScalarBlockLayoutFeatures, scalarBlockLayout) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceScalarBlockLayoutFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCHEDULING_CONTROLS_FEATURES_ARM:
            static_assert(offsetof(VkPhysicalDeviceSchedulingControlsFeaturesARM, schedulingControls) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceSchedulingControlsFeaturesARM, schedulingControls) +
                              sizeof(VkPhysicalDeviceSchedulingControlsFeaturesARM::schedulingControls) -
                              offsetof(VkPhysicalDeviceSchedulingControlsFeaturesARM, schedulingControls) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceSchedulingControlsFeaturesARM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures, separateDepthStencilLayouts) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures, separateDepthStencilLayouts) +
                              sizeof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures::separateDepthStencilLayouts) -
                              offsetof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures, separateDepthStencilLayouts) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_64_BIT_INDEXING_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShader64BitIndexingFeaturesEXT, shader64BitIndexing) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShader64BitIndexingFeaturesEXT, shader64BitIndexing) +
                              sizeof(VkPhysicalDeviceShader64BitIndexingFeaturesEXT::shader64BitIndexing) -
                              offsetof(VkPhysicalDeviceShader64BitIndexingFeaturesEXT, shader64BitIndexing) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShader64BitIndexingFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ABORT_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceShaderAbortFeaturesKHR, shaderAbort) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderAbortFeaturesKHR, shaderAbort) +
                              sizeof(VkPhysicalDeviceShaderAbortFeaturesKHR::shaderAbort) -
                              offsetof(VkPhysicalDeviceShaderAbortFeaturesKHR, shaderAbort) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderAbortFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT16_VECTOR_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceShaderAtomicFloat16VectorFeaturesNV, shaderFloat16VectorAtomics) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderAtomicFloat16VectorFeaturesNV, shaderFloat16VectorAtomics) +
                              sizeof(VkPhysicalDeviceShaderAtomicFloat16VectorFeaturesNV::shaderFloat16VectorAtomics) -
                              offsetof(VkPhysicalDeviceShaderAtomicFloat16VectorFeaturesNV, shaderFloat16VectorAtomics) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderAtomicFloat16VectorFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT, shaderBufferFloat16Atomics) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT, sparseImageFloat32AtomicMinMax) +
                              sizeof(VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT::sparseImageFloat32AtomicMinMax) -
                              offsetof(VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT, shaderBufferFloat16Atomics) ==
                          12 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT), .body_size = 12 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT, shaderBufferFloat32Atomics) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT, sparseImageFloat32AtomicAdd) +
                              sizeof(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT::sparseImageFloat32AtomicAdd) -
                              offsetof(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT, shaderBufferFloat32Atomics) ==
                          12 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT), .body_size = 12 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceShaderAtomicInt64Features, shaderBufferInt64Atomics) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderAtomicInt64Features, shaderSharedInt64Atomics) +
                              sizeof(VkPhysicalDeviceShaderAtomicInt64Features::shaderSharedInt64Atomics) -
                              offsetof(VkPhysicalDeviceShaderAtomicInt64Features, shaderBufferInt64Atomics) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderAtomicInt64Features), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_BFLOAT16_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceShaderBfloat16FeaturesKHR, shaderBFloat16Type) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderBfloat16FeaturesKHR, shaderBFloat16CooperativeMatrix) +
                              sizeof(VkPhysicalDeviceShaderBfloat16FeaturesKHR::shaderBFloat16CooperativeMatrix) -
                              offsetof(VkPhysicalDeviceShaderBfloat16FeaturesKHR, shaderBFloat16Type) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderBfloat16FeaturesKHR), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceShaderClockFeaturesKHR, shaderSubgroupClock) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderClockFeaturesKHR, shaderDeviceClock) +
                              sizeof(VkPhysicalDeviceShaderClockFeaturesKHR::shaderDeviceClock) -
                              offsetof(VkPhysicalDeviceShaderClockFeaturesKHR, shaderSubgroupClock) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderClockFeaturesKHR), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CONSTANT_DATA_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceShaderConstantDataFeaturesKHR, shaderConstantData) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderConstantDataFeaturesKHR, shaderConstantData) +
                              sizeof(VkPhysicalDeviceShaderConstantDataFeaturesKHR::shaderConstantData) -
                              offsetof(VkPhysicalDeviceShaderConstantDataFeaturesKHR, shaderConstantData) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderConstantDataFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_BUILTINS_FEATURES_ARM:
            static_assert(offsetof(VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM, shaderCoreBuiltins) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM, shaderCoreBuiltins) +
                              sizeof(VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM::shaderCoreBuiltins) -
                              offsetof(VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM, shaderCoreBuiltins) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, shaderDemoteToHelperInvocation) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, shaderDemoteToHelperInvocation) +
                              sizeof(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures::shaderDemoteToHelperInvocation) -
                              offsetof(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, shaderDemoteToHelperInvocation) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceShaderDrawParametersFeatures, shaderDrawParameters) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderDrawParametersFeatures, shaderDrawParameters) +
                              sizeof(VkPhysicalDeviceShaderDrawParametersFeatures::shaderDrawParameters) -
                              offsetof(VkPhysicalDeviceShaderDrawParametersFeatures, shaderDrawParameters) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderDrawParametersFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EARLY_AND_LATE_FRAGMENT_TESTS_FEATURES_AMD:
            static_assert(offsetof(VkPhysicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD, shaderEarlyAndLateFragmentTests) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD, shaderEarlyAndLateFragmentTests) +
                              sizeof(VkPhysicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD::shaderEarlyAndLateFragmentTests) -
                              offsetof(VkPhysicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD, shaderEarlyAndLateFragmentTests) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD),
                    .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EXPECT_ASSUME_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceShaderExpectAssumeFeatures, shaderExpectAssume) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderExpectAssumeFeatures, shaderExpectAssume) +
                              sizeof(VkPhysicalDeviceShaderExpectAssumeFeatures::shaderExpectAssume) -
                              offsetof(VkPhysicalDeviceShaderExpectAssumeFeatures, shaderExpectAssume) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderExpectAssumeFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceShaderFloat16Int8Features, shaderFloat16) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderFloat16Int8Features, shaderInt8) +
                              sizeof(VkPhysicalDeviceShaderFloat16Int8Features::shaderInt8) -
                              offsetof(VkPhysicalDeviceShaderFloat16Int8Features, shaderFloat16) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderFloat16Int8Features), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderFloat8FeaturesEXT, shaderFloat8) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderFloat8FeaturesEXT, shaderFloat8CooperativeMatrix) +
                              sizeof(VkPhysicalDeviceShaderFloat8FeaturesEXT::shaderFloat8CooperativeMatrix) -
                              offsetof(VkPhysicalDeviceShaderFloat8FeaturesEXT, shaderFloat8) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderFloat8FeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceShaderFloatControls2Features, shaderFloatControls2) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderFloatControls2Features, shaderFloatControls2) +
                              sizeof(VkPhysicalDeviceShaderFloatControls2Features::shaderFloatControls2) -
                              offsetof(VkPhysicalDeviceShaderFloatControls2Features, shaderFloatControls2) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderFloatControls2Features), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FMA_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceShaderFmaFeaturesKHR, shaderFmaFloat16) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderFmaFeaturesKHR, shaderFmaFloat64) +
                              sizeof(VkPhysicalDeviceShaderFmaFeaturesKHR::shaderFmaFloat64) -
                              offsetof(VkPhysicalDeviceShaderFmaFeaturesKHR, shaderFmaFloat16) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderFmaFeaturesKHR), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT, shaderImageInt64Atomics) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT, sparseImageInt64Atomics) +
                              sizeof(VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT::sparseImageInt64Atomics) -
                              offsetof(VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT, shaderImageInt64Atomics) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_FOOTPRINT_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceShaderImageFootprintFeaturesNV, imageFootprint) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderImageFootprintFeaturesNV, imageFootprint) +
                              sizeof(VkPhysicalDeviceShaderImageFootprintFeaturesNV::imageFootprint) -
                              offsetof(VkPhysicalDeviceShaderImageFootprintFeaturesNV, imageFootprint) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderImageFootprintFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INSTRUMENTATION_FEATURES_ARM:
            static_assert(offsetof(VkPhysicalDeviceShaderInstrumentationFeaturesARM, shaderInstrumentation) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderInstrumentationFeaturesARM, shaderInstrumentation) +
                              sizeof(VkPhysicalDeviceShaderInstrumentationFeaturesARM::shaderInstrumentation) -
                              offsetof(VkPhysicalDeviceShaderInstrumentationFeaturesARM, shaderInstrumentation) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderInstrumentationFeaturesARM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceShaderIntegerDotProductFeatures, shaderIntegerDotProduct) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderIntegerDotProductFeatures, shaderIntegerDotProduct) +
                              sizeof(VkPhysicalDeviceShaderIntegerDotProductFeatures::shaderIntegerDotProduct) -
                              offsetof(VkPhysicalDeviceShaderIntegerDotProductFeatures, shaderIntegerDotProduct) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderIntegerDotProductFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_FUNCTIONS_2_FEATURES_INTEL:
            static_assert(offsetof(VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL, shaderIntegerFunctions2) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL, shaderIntegerFunctions2) +
                              sizeof(VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL::shaderIntegerFunctions2) -
                              offsetof(VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL, shaderIntegerFunctions2) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_LONG_VECTOR_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderLongVectorFeaturesEXT, longVector) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderLongVectorFeaturesEXT, longVector) +
                              sizeof(VkPhysicalDeviceShaderLongVectorFeaturesEXT::longVector) -
                              offsetof(VkPhysicalDeviceShaderLongVectorFeaturesEXT, longVector) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderLongVectorFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR, shaderMaximalReconvergence) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR, shaderMaximalReconvergence) +
                              sizeof(VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR::shaderMaximalReconvergence) -
                              offsetof(VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR, shaderMaximalReconvergence) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE:
            static_assert(offsetof(VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE, shaderMixedFloatDotProductFloat16AccFloat32) ==
                          2 * sizeof(void*));
            static_assert(
                offsetof(VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE, shaderMixedFloatDotProductFloat8AccFloat32) +
                    sizeof(VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE::shaderMixedFloatDotProductFloat8AccFloat32) -
                    offsetof(VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE, shaderMixedFloatDotProductFloat16AccFloat32) ==
                4 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE), .body_size = 4 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT, shaderModuleIdentifier) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT, shaderModuleIdentifier) +
                              sizeof(VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT::shaderModuleIdentifier) -
                              offsetof(VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT, shaderModuleIdentifier) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MULTIPLE_WAIT_QUEUES_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceShaderMultipleWaitQueuesFeaturesQCOM, shaderMultipleWaitQueues) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderMultipleWaitQueuesFeaturesQCOM, shaderMultipleWaitQueues) +
                              sizeof(VkPhysicalDeviceShaderMultipleWaitQueuesFeaturesQCOM::shaderMultipleWaitQueues) -
                              offsetof(VkPhysicalDeviceShaderMultipleWaitQueuesFeaturesQCOM, shaderMultipleWaitQueues) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderMultipleWaitQueuesFeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OCP_MICROSCALING_TYPES_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderOCPMicroscalingTypesFeaturesEXT, shaderFloat4) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderOCPMicroscalingTypesFeaturesEXT, shaderMXInt8) +
                              sizeof(VkPhysicalDeviceShaderOCPMicroscalingTypesFeaturesEXT::shaderMXInt8) -
                              offsetof(VkPhysicalDeviceShaderOCPMicroscalingTypesFeaturesEXT, shaderFloat4) ==
                          4 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderOCPMicroscalingTypesFeaturesEXT), .body_size = 4 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderObjectFeaturesEXT, shaderObject) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderObjectFeaturesEXT, shaderObject) +
                              sizeof(VkPhysicalDeviceShaderObjectFeaturesEXT::shaderObject) -
                              offsetof(VkPhysicalDeviceShaderObjectFeaturesEXT, shaderObject) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderObjectFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceShaderQuadControlFeaturesKHR, shaderQuadControl) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderQuadControlFeaturesKHR, shaderQuadControl) +
                              sizeof(VkPhysicalDeviceShaderQuadControlFeaturesKHR::shaderQuadControl) -
                              offsetof(VkPhysicalDeviceShaderQuadControlFeaturesKHR, shaderQuadControl) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderQuadControlFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_RELAXED_EXTENDED_INSTRUCTION_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR, shaderRelaxedExtendedInstruction) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR, shaderRelaxedExtendedInstruction) +
                              sizeof(VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR::shaderRelaxedExtendedInstruction) -
                              offsetof(VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR, shaderRelaxedExtendedInstruction) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR),
                    .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_REPLICATED_COMPOSITES_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT, shaderReplicatedComposites) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT, shaderReplicatedComposites) +
                              sizeof(VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT::shaderReplicatedComposites) -
                              offsetof(VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT, shaderReplicatedComposites) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceShaderSMBuiltinsFeaturesNV, shaderSMBuiltins) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderSMBuiltinsFeaturesNV, shaderSMBuiltins) +
                              sizeof(VkPhysicalDeviceShaderSMBuiltinsFeaturesNV::shaderSMBuiltins) -
                              offsetof(VkPhysicalDeviceShaderSMBuiltinsFeaturesNV, shaderSMBuiltins) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderSMBuiltinsFeaturesNV), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SPLIT_BARRIER_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderSplitBarrierFeaturesEXT, shaderSplitBarrier) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderSplitBarrierFeaturesEXT, shaderSplitBarrier) +
                              sizeof(VkPhysicalDeviceShaderSplitBarrierFeaturesEXT::shaderSplitBarrier) -
                              offsetof(VkPhysicalDeviceShaderSplitBarrierFeaturesEXT, shaderSplitBarrier) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderSplitBarrierFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, shaderSubgroupExtendedTypes) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, shaderSubgroupExtendedTypes) +
                              sizeof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures::shaderSubgroupExtendedTypes) -
                              offsetof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, shaderSubgroupExtendedTypes) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_PARTITIONED_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderSubgroupPartitionedFeaturesEXT, shaderSubgroupPartitioned) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderSubgroupPartitionedFeaturesEXT, shaderSubgroupPartitioned) +
                              sizeof(VkPhysicalDeviceShaderSubgroupPartitionedFeaturesEXT::shaderSubgroupPartitioned) -
                              offsetof(VkPhysicalDeviceShaderSubgroupPartitionedFeaturesEXT, shaderSubgroupPartitioned) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderSubgroupPartitionedFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_ROTATE_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceShaderSubgroupRotateFeatures, shaderSubgroupRotate) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderSubgroupRotateFeatures, shaderSubgroupRotateClustered) +
                              sizeof(VkPhysicalDeviceShaderSubgroupRotateFeatures::shaderSubgroupRotateClustered) -
                              offsetof(VkPhysicalDeviceShaderSubgroupRotateFeatures, shaderSubgroupRotate) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderSubgroupRotateFeatures), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR, shaderSubgroupUniformControlFlow) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR, shaderSubgroupUniformControlFlow) +
                              sizeof(VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR::shaderSubgroupUniformControlFlow) -
                              offsetof(VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR, shaderSubgroupUniformControlFlow) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR),
                    .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceShaderTerminateInvocationFeatures, shaderTerminateInvocation) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderTerminateInvocationFeatures, shaderTerminateInvocation) +
                              sizeof(VkPhysicalDeviceShaderTerminateInvocationFeatures::shaderTerminateInvocation) -
                              offsetof(VkPhysicalDeviceShaderTerminateInvocationFeatures, shaderTerminateInvocation) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderTerminateInvocationFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderTileImageFeaturesEXT, shaderTileImageColorReadAccess) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderTileImageFeaturesEXT, shaderTileImageStencilReadAccess) +
                              sizeof(VkPhysicalDeviceShaderTileImageFeaturesEXT::shaderTileImageStencilReadAccess) -
                              offsetof(VkPhysicalDeviceShaderTileImageFeaturesEXT, shaderTileImageColorReadAccess) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderTileImageFeaturesEXT), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNIFORM_BUFFER_UNSIZED_ARRAY_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceShaderUniformBufferUnsizedArrayFeaturesEXT, shaderUniformBufferUnsizedArray) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderUniformBufferUnsizedArrayFeaturesEXT, shaderUniformBufferUnsizedArray) +
                              sizeof(VkPhysicalDeviceShaderUniformBufferUnsizedArrayFeaturesEXT::shaderUniformBufferUnsizedArray) -
                              offsetof(VkPhysicalDeviceShaderUniformBufferUnsizedArrayFeaturesEXT, shaderUniformBufferUnsizedArray) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderUniformBufferUnsizedArrayFeaturesEXT),
                    .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceShaderUntypedPointersFeaturesKHR, shaderUntypedPointers) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShaderUntypedPointersFeaturesKHR, shaderUntypedPointers) +
                              sizeof(VkPhysicalDeviceShaderUntypedPointersFeaturesKHR::shaderUntypedPointers) -
                              offsetof(VkPhysicalDeviceShaderUntypedPointersFeaturesKHR, shaderUntypedPointers) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShaderUntypedPointersFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV:
            static_assert(offsetof(VkPhysicalDeviceShadingRateImageFeaturesNV, shadingRateImage) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceShadingRateImageFeaturesNV, shadingRateCoarseSampleOrder) +
                              sizeof(VkPhysicalDeviceShadingRateImageFeaturesNV::shadingRateCoarseSampleOrder) -
                              offsetof(VkPhysicalDeviceShadingRateImageFeaturesNV, shadingRateImage) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceShadingRateImageFeaturesNV), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceSubgroupSizeControlFeatures, subgroupSizeControl) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceSubgroupSizeControlFeatures, computeFullSubgroups) +
                              sizeof(VkPhysicalDeviceSubgroupSizeControlFeatures::computeFullSubgroups) -
                              offsetof(VkPhysicalDeviceSubgroupSizeControlFeatures, subgroupSizeControl) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceSubgroupSizeControlFeatures), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_MERGE_FEEDBACK_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT, subpassMergeFeedback) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT, subpassMergeFeedback) +
                              sizeof(VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT::subpassMergeFeedback) -
                              offsetof(VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT, subpassMergeFeedback) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_SHADING_FEATURES_HUAWEI:
            static_assert(offsetof(VkPhysicalDeviceSubpassShadingFeaturesHUAWEI, subpassShading) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceSubpassShadingFeaturesHUAWEI, subpassShading) +
                              sizeof(VkPhysicalDeviceSubpassShadingFeaturesHUAWEI::subpassShading) -
                              offsetof(VkPhysicalDeviceSubpassShadingFeaturesHUAWEI, subpassShading) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceSubpassShadingFeaturesHUAWEI), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR, swapchainMaintenance1) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR, swapchainMaintenance1) +
                              sizeof(VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR::swapchainMaintenance1) -
                              offsetof(VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR, swapchainMaintenance1) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceSynchronization2Features, synchronization2) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceSynchronization2Features, synchronization2) +
                              sizeof(VkPhysicalDeviceSynchronization2Features::synchronization2) -
                              offsetof(VkPhysicalDeviceSynchronization2Features, synchronization2) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceSynchronization2Features), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TENSOR_FEATURES_ARM:
            static_assert(offsetof(VkPhysicalDeviceTensorFeaturesARM, tensorNonPacked) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceTensorFeaturesARM, tensors) + sizeof(VkPhysicalDeviceTensorFeaturesARM::tensors) -
                              offsetof(VkPhysicalDeviceTensorFeaturesARM, tensorNonPacked) ==
                          6 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceTensorFeaturesARM), .body_size = 6 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT, texelBufferAlignment) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT, texelBufferAlignment) +
                              sizeof(VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT::texelBufferAlignment) -
                              offsetof(VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT, texelBufferAlignment) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_3D_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceTextureCompressionASTC3DFeaturesEXT, textureCompressionASTC_3D) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceTextureCompressionASTC3DFeaturesEXT, textureCompressionASTC_3D) +
                              sizeof(VkPhysicalDeviceTextureCompressionASTC3DFeaturesEXT::textureCompressionASTC_3D) -
                              offsetof(VkPhysicalDeviceTextureCompressionASTC3DFeaturesEXT, textureCompressionASTC_3D) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceTextureCompressionASTC3DFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceTextureCompressionASTCHDRFeatures, textureCompressionASTC_HDR) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceTextureCompressionASTCHDRFeatures, textureCompressionASTC_HDR) +
                              sizeof(VkPhysicalDeviceTextureCompressionASTCHDRFeatures::textureCompressionASTC_HDR) -
                              offsetof(VkPhysicalDeviceTextureCompressionASTCHDRFeatures, textureCompressionASTC_HDR) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceTextureCompressionASTCHDRFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_THROTTLE_HINT_FEATURES_SEC:
            static_assert(offsetof(VkPhysicalDeviceThrottleHintFeaturesSEC, throttleHint) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceThrottleHintFeaturesSEC, throttleHint) +
                              sizeof(VkPhysicalDeviceThrottleHintFeaturesSEC::throttleHint) -
                              offsetof(VkPhysicalDeviceThrottleHintFeaturesSEC, throttleHint) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceThrottleHintFeaturesSEC), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_MEMORY_HEAP_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceTileMemoryHeapFeaturesQCOM, tileMemoryHeap) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceTileMemoryHeapFeaturesQCOM, tileMemoryHeap) +
                              sizeof(VkPhysicalDeviceTileMemoryHeapFeaturesQCOM::tileMemoryHeap) -
                              offsetof(VkPhysicalDeviceTileMemoryHeapFeaturesQCOM, tileMemoryHeap) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceTileMemoryHeapFeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_PROPERTIES_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceTilePropertiesFeaturesQCOM, tileProperties) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceTilePropertiesFeaturesQCOM, tileProperties) +
                              sizeof(VkPhysicalDeviceTilePropertiesFeaturesQCOM::tileProperties) -
                              offsetof(VkPhysicalDeviceTilePropertiesFeaturesQCOM, tileProperties) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceTilePropertiesFeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_SHADING_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceTileShadingFeaturesQCOM, tileShading) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceTileShadingFeaturesQCOM, tileShadingImageProcessing) +
                              sizeof(VkPhysicalDeviceTileShadingFeaturesQCOM::tileShadingImageProcessing) -
                              offsetof(VkPhysicalDeviceTileShadingFeaturesQCOM, tileShading) ==
                          14 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceTileShadingFeaturesQCOM), .body_size = 14 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceTimelineSemaphoreFeatures, timelineSemaphore) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceTimelineSemaphoreFeatures, timelineSemaphore) +
                              sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures::timelineSemaphore) -
                              offsetof(VkPhysicalDeviceTimelineSemaphoreFeatures, timelineSemaphore) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceTransformFeedbackFeaturesEXT, transformFeedback) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceTransformFeedbackFeaturesEXT, geometryStreams) +
                              sizeof(VkPhysicalDeviceTransformFeedbackFeaturesEXT::geometryStreams) -
                              offsetof(VkPhysicalDeviceTransformFeedbackFeaturesEXT, transformFeedback) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceTransformFeedbackFeaturesEXT), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR, unifiedImageLayouts) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR, unifiedImageLayoutsVideo) +
                              sizeof(VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR::unifiedImageLayoutsVideo) -
                              offsetof(VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR, unifiedImageLayouts) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures, uniformBufferStandardLayout) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures, uniformBufferStandardLayout) +
                              sizeof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures::uniformBufferStandardLayout) -
                              offsetof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures, uniformBufferStandardLayout) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceVariablePointersFeatures, variablePointersStorageBuffer) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVariablePointersFeatures, variablePointers) +
                              sizeof(VkPhysicalDeviceVariablePointersFeatures::variablePointers) -
                              offsetof(VkPhysicalDeviceVariablePointersFeatures, variablePointersStorageBuffer) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVariablePointersFeatures), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceVertexAttributeDivisorFeatures, vertexAttributeInstanceRateDivisor) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVertexAttributeDivisorFeatures, vertexAttributeInstanceRateZeroDivisor) +
                              sizeof(VkPhysicalDeviceVertexAttributeDivisorFeatures::vertexAttributeInstanceRateZeroDivisor) -
                              offsetof(VkPhysicalDeviceVertexAttributeDivisorFeatures, vertexAttributeInstanceRateDivisor) ==
                          2 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVertexAttributeDivisorFeatures), .body_size = 2 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_ROBUSTNESS_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceVertexAttributeRobustnessFeaturesEXT, vertexAttributeRobustness) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVertexAttributeRobustnessFeaturesEXT, vertexAttributeRobustness) +
                              sizeof(VkPhysicalDeviceVertexAttributeRobustnessFeaturesEXT::vertexAttributeRobustness) -
                              offsetof(VkPhysicalDeviceVertexAttributeRobustnessFeaturesEXT, vertexAttributeRobustness) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVertexAttributeRobustnessFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT, vertexInputDynamicState) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT, vertexInputDynamicState) +
                              sizeof(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT::vertexInputDynamicState) -
                              offsetof(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT, vertexInputDynamicState) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_DECODE_VP9_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceVideoDecodeVP9FeaturesKHR, videoDecodeVP9) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVideoDecodeVP9FeaturesKHR, videoDecodeVP9) +
                              sizeof(VkPhysicalDeviceVideoDecodeVP9FeaturesKHR::videoDecodeVP9) -
                              offsetof(VkPhysicalDeviceVideoDecodeVP9FeaturesKHR, videoDecodeVP9) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVideoDecodeVP9FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_AV1_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceVideoEncodeAV1FeaturesKHR, videoEncodeAV1) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVideoEncodeAV1FeaturesKHR, videoEncodeAV1) +
                              sizeof(VkPhysicalDeviceVideoEncodeAV1FeaturesKHR::videoEncodeAV1) -
                              offsetof(VkPhysicalDeviceVideoEncodeAV1FeaturesKHR, videoEncodeAV1) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVideoEncodeAV1FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_FEEDBACK_2_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceVideoEncodeFeedback2FeaturesKHR, videoEncodeFeedback2) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVideoEncodeFeedback2FeaturesKHR, videoEncodeFeedback2) +
                              sizeof(VkPhysicalDeviceVideoEncodeFeedback2FeaturesKHR::videoEncodeFeedback2) -
                              offsetof(VkPhysicalDeviceVideoEncodeFeedback2FeaturesKHR, videoEncodeFeedback2) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVideoEncodeFeedback2FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_INTRA_REFRESH_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR, videoEncodeIntraRefresh) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR, videoEncodeIntraRefresh) +
                              sizeof(VkPhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR::videoEncodeIntraRefresh) -
                              offsetof(VkPhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR, videoEncodeIntraRefresh) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_QUANTIZATION_MAP_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceVideoEncodeQuantizationMapFeaturesKHR, videoEncodeQuantizationMap) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVideoEncodeQuantizationMapFeaturesKHR, videoEncodeQuantizationMap) +
                              sizeof(VkPhysicalDeviceVideoEncodeQuantizationMapFeaturesKHR::videoEncodeQuantizationMap) -
                              offsetof(VkPhysicalDeviceVideoEncodeQuantizationMapFeaturesKHR, videoEncodeQuantizationMap) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVideoEncodeQuantizationMapFeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_RGB_CONVERSION_FEATURES_VALVE:
            static_assert(offsetof(VkPhysicalDeviceVideoEncodeRgbConversionFeaturesVALVE, videoEncodeRgbConversion) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVideoEncodeRgbConversionFeaturesVALVE, videoEncodeRgbConversion) +
                              sizeof(VkPhysicalDeviceVideoEncodeRgbConversionFeaturesVALVE::videoEncodeRgbConversion) -
                              offsetof(VkPhysicalDeviceVideoEncodeRgbConversionFeaturesVALVE, videoEncodeRgbConversion) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVideoEncodeRgbConversionFeaturesVALVE), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceVideoMaintenance1FeaturesKHR, videoMaintenance1) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVideoMaintenance1FeaturesKHR, videoMaintenance1) +
                              sizeof(VkPhysicalDeviceVideoMaintenance1FeaturesKHR::videoMaintenance1) -
                              offsetof(VkPhysicalDeviceVideoMaintenance1FeaturesKHR, videoMaintenance1) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVideoMaintenance1FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceVideoMaintenance2FeaturesKHR, videoMaintenance2) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVideoMaintenance2FeaturesKHR, videoMaintenance2) +
                              sizeof(VkPhysicalDeviceVideoMaintenance2FeaturesKHR::videoMaintenance2) -
                              offsetof(VkPhysicalDeviceVideoMaintenance2FeaturesKHR, videoMaintenance2) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVideoMaintenance2FeaturesKHR), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVulkan11Features, shaderDrawParameters) +
                              sizeof(VkPhysicalDeviceVulkan11Features::shaderDrawParameters) -
                              offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess) ==
                          12 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVulkan11Features), .body_size = 12 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVulkan12Features, subgroupBroadcastDynamicId) +
                              sizeof(VkPhysicalDeviceVulkan12Features::subgroupBroadcastDynamicId) -
                              offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge) ==
                          47 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVulkan12Features), .body_size = 47 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVulkan13Features, maintenance4) +
                              sizeof(VkPhysicalDeviceVulkan13Features::maintenance4) -
                              offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess) ==
                          15 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVulkan13Features), .body_size = 15 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceVulkan14Features, globalPriorityQuery) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVulkan14Features, pushDescriptor) +
                              sizeof(VkPhysicalDeviceVulkan14Features::pushDescriptor) -
                              offsetof(VkPhysicalDeviceVulkan14Features, globalPriorityQuery) ==
                          21 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVulkan14Features), .body_size = 21 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceVulkanMemoryModelFeatures, vulkanMemoryModel) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceVulkanMemoryModelFeatures, vulkanMemoryModelAvailabilityVisibilityChains) +
                              sizeof(VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModelAvailabilityVisibilityChains) -
                              offsetof(VkPhysicalDeviceVulkanMemoryModelFeatures, vulkanMemoryModel) ==
                          3 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceVulkanMemoryModelFeatures), .body_size = 3 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR:
            static_assert(offsetof(VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR, workgroupMemoryExplicitLayout) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR, workgroupMemoryExplicitLayout16BitAccess) +
                              sizeof(VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR::workgroupMemoryExplicitLayout16BitAccess) -
                              offsetof(VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR, workgroupMemoryExplicitLayout) ==
                          4 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR), .body_size = 4 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_2_PLANE_444_FORMATS_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT, ycbcr2plane444Formats) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT, ycbcr2plane444Formats) +
                              sizeof(VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT::ycbcr2plane444Formats) -
                              offsetof(VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT, ycbcr2plane444Formats) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_DEGAMMA_FEATURES_QCOM:
            static_assert(offsetof(VkPhysicalDeviceYcbcrDegammaFeaturesQCOM, ycbcrDegamma) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceYcbcrDegammaFeaturesQCOM, ycbcrDegamma) +
                              sizeof(VkPhysicalDeviceYcbcrDegammaFeaturesQCOM::ycbcrDegamma) -
                              offsetof(VkPhysicalDeviceYcbcrDegammaFeaturesQCOM, ycbcrDegamma) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceYcbcrDegammaFeaturesQCOM), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_IMAGE_ARRAYS_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceYcbcrImageArraysFeaturesEXT, ycbcrImageArrays) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceYcbcrImageArraysFeaturesEXT, ycbcrImageArrays) +
                              sizeof(VkPhysicalDeviceYcbcrImageArraysFeaturesEXT::ycbcrImageArrays) -
                              offsetof(VkPhysicalDeviceYcbcrImageArraysFeaturesEXT, ycbcrImageArrays) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceYcbcrImageArraysFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_DEVICE_MEMORY_FEATURES_EXT:
            static_assert(offsetof(VkPhysicalDeviceZeroInitializeDeviceMemoryFeaturesEXT, zeroInitializeDeviceMemory) == 2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceZeroInitializeDeviceMemoryFeaturesEXT, zeroInitializeDeviceMemory) +
                              sizeof(VkPhysicalDeviceZeroInitializeDeviceMemoryFeaturesEXT::zeroInitializeDeviceMemory) -
                              offsetof(VkPhysicalDeviceZeroInitializeDeviceMemoryFeaturesEXT, zeroInitializeDeviceMemory) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceZeroInitializeDeviceMemoryFeaturesEXT), .body_size = 1 * sizeof(VkBool32)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES:
            static_assert(offsetof(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures, shaderZeroInitializeWorkgroupMemory) ==
                          2 * sizeof(void*));
            static_assert(offsetof(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures, shaderZeroInitializeWorkgroupMemory) +
                              sizeof(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures::shaderZeroInitializeWorkgroupMemory) -
                              offsetof(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures, shaderZeroInitializeWorkgroupMemory) ==
                          1 * sizeof(VkBool32));
            return {.structure_size = sizeof(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures), .body_size = 1 * sizeof(VkBool32)};
        default:
            return {};
        }
    }
}
