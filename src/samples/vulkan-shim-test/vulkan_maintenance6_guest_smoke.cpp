#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

int run_maintenance6_guest_smoke(PFN_vkGetInstanceProcAddr get)
{
    const auto proc = [&](VkInstance instance, const char* name) { return get(instance, name); };
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(proc(nullptr, "vkCreateInstance"));
    const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(proc(nullptr, "vkDestroyInstance"));
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(proc(nullptr, "vkEnumeratePhysicalDevices"));
    const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(proc(nullptr, "vkGetPhysicalDeviceProperties"));
    const auto enumerate_extensions =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(proc(nullptr, "vkEnumerateDeviceExtensionProperties"));
    const auto get_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(proc(nullptr, "vkGetPhysicalDeviceFeatures2"));
    const auto get_families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        proc(nullptr, "vkGetPhysicalDeviceQueueFamilyProperties"));
    const auto create_device = reinterpret_cast<PFN_vkCreateDevice>(proc(nullptr, "vkCreateDevice"));
    const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(proc(nullptr, "vkGetDeviceProcAddr"));
    if (!create_instance || !destroy_instance || !enumerate || !get_properties || !enumerate_extensions ||
        !get_features2 || !get_families || !create_device || !get_device_proc)
    {
        std::printf("[maintenance6-smoke] missing instance-level proc\n");
        return 2;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS)
        return 3;
    uint32_t count = 0;
    if (enumerate(instance, &count, nullptr) != VK_SUCCESS || count == 0)
        return 4;
    std::vector<VkPhysicalDevice> physical(count);
    if (enumerate(instance, &count, physical.data()) != VK_SUCCESS)
        return 5;
    VkPhysicalDevice amd = VK_NULL_HANDLE;
    for (auto device : physical)
    {
        VkPhysicalDeviceProperties properties{};
        get_properties(device, &properties);
        if (properties.vendorID == 0x1002) { amd = device; break; }
    }
    if (!amd)
    {
        std::printf("[maintenance6-smoke] AMD device missing\n");
        return 6;
    }
    uint32_t extension_count = 0;
    if (enumerate_extensions(amd, nullptr, &extension_count, nullptr) != VK_SUCCESS)
        return 7;
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (enumerate_extensions(amd, nullptr, &extension_count, extensions.data()) != VK_SUCCESS)
        return 8;
    const auto has_extension = [&](const char* name) {
        return std::any_of(extensions.begin(), extensions.begin() + std::min<size_t>(extension_count, extensions.size()),
                           [&](const auto& ext) { return std::strcmp(ext.extensionName, name) == 0; });
    };
    if (!has_extension(VK_KHR_MAINTENANCE_6_EXTENSION_NAME) || !has_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
    {
        std::printf("[maintenance6-smoke] required extension hidden\n");
        return 9;
    }
    VkPhysicalDeviceMaintenance6FeaturesKHR maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &maintenance;
    get_features2(amd, &features);
    if (maintenance.maintenance6 != VK_TRUE)
    {
        std::printf("[maintenance6-smoke] maintenance6 feature false\n");
        return 10;
    }
    uint32_t family_count = 0;
    get_families(amd, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    get_families(amd, &family_count, families.data());
    uint32_t graphics_family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i)
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { graphics_family = i; break; }
    if (graphics_family == UINT32_MAX) return 11;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = graphics_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* enabled_extensions[]{VK_KHR_MAINTENANCE_6_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 2;
    device_info.ppEnabledExtensionNames = enabled_extensions;
    VkDevice device = VK_NULL_HANDLE;
    const VkResult created = create_device(amd, &device_info, nullptr, &device);
    if (created != VK_SUCCESS)
    {
        std::printf("[maintenance6-smoke] vkCreateDevice=%d\n", created);
        return 12;
    }
    const auto dproc = [&](const char* name) { return get_device_proc(device, name); };
    const auto bind2 = reinterpret_cast<PFN_vkCmdBindDescriptorSets2KHR>(dproc("vkCmdBindDescriptorSets2KHR"));
    const auto constants2 = reinterpret_cast<PFN_vkCmdPushConstants2KHR>(dproc("vkCmdPushConstants2KHR"));
    const auto push = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(dproc("vkCmdPushDescriptorSetKHR"));
    const auto push2 = reinterpret_cast<PFN_vkCmdPushDescriptorSet2KHR>(dproc("vkCmdPushDescriptorSet2KHR"));
    const auto push_template =
        reinterpret_cast<PFN_vkCmdPushDescriptorSetWithTemplateKHR>(dproc("vkCmdPushDescriptorSetWithTemplateKHR"));
    const auto push_template2 =
        reinterpret_cast<PFN_vkCmdPushDescriptorSetWithTemplate2KHR>(dproc("vkCmdPushDescriptorSetWithTemplate2KHR"));
    std::printf("[maintenance6-smoke] procs bind2=%d constants2=%d push=%d push2=%d template=%d template2=%d\n",
                bind2 != nullptr, constants2 != nullptr, push != nullptr, push2 != nullptr,
                push_template != nullptr, push_template2 != nullptr);
    if (!bind2 || !constants2 || !push || !push2 || !push_template || !push_template2)
        return 13;
    const auto create_sampler = reinterpret_cast<PFN_vkCreateSampler>(dproc("vkCreateSampler"));
    const auto create_set_layout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(dproc("vkCreateDescriptorSetLayout"));
    const auto create_pipeline_layout = reinterpret_cast<PFN_vkCreatePipelineLayout>(dproc("vkCreatePipelineLayout"));
    const auto create_pool = reinterpret_cast<PFN_vkCreateCommandPool>(dproc("vkCreateCommandPool"));
    const auto allocate = reinterpret_cast<PFN_vkAllocateCommandBuffers>(dproc("vkAllocateCommandBuffers"));
    const auto begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(dproc("vkBeginCommandBuffer"));
    const auto end = reinterpret_cast<PFN_vkEndCommandBuffer>(dproc("vkEndCommandBuffer"));
    const auto create_template = reinterpret_cast<PFN_vkCreateDescriptorUpdateTemplate>(dproc("vkCreateDescriptorUpdateTemplate"));
    if (!create_sampler || !create_set_layout || !create_pipeline_layout || !create_pool || !allocate ||
        !begin || !end || !create_template)
        return 14;

    const auto memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
    const auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(dproc("vkCreateBuffer"));
    const auto get_requirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(dproc("vkGetBufferMemoryRequirements"));
    const auto allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(dproc("vkAllocateMemory"));
    const auto bind_memory2 = reinterpret_cast<PFN_vkBindBufferMemory2>(dproc("vkBindBufferMemory2"));
    if (!memory_properties || !create_buffer || !get_requirements || !allocate_memory || !bind_memory2)
        return 23;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = 64;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (create_buffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) return 24;
    VkMemoryRequirements requirements{};
    get_requirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory_types{};
    memory_properties(amd, &memory_types);
    uint32_t memory_index = UINT32_MAX;
    for (uint32_t i = 0; i < memory_types.memoryTypeCount; ++i)
        if (requirements.memoryTypeBits & (1u << i)) { memory_index = i; break; }
    if (memory_index == UINT32_MAX) return 25;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_index;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (allocate_memory(device, &allocation, nullptr, &memory) != VK_SUCCESS) return 26;
    VkResult per_bind = VK_ERROR_UNKNOWN;
    VkBindMemoryStatusKHR bind_status{VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR};
    bind_status.pResult = &per_bind;
    VkBindBufferMemoryInfo memory_bind_info{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};
    memory_bind_info.pNext = &bind_status;
    memory_bind_info.buffer = buffer;
    memory_bind_info.memory = memory;
    const VkResult bind_result = bind_memory2(device, 1, &memory_bind_info);
    std::printf("[maintenance6-smoke] vkBindBufferMemory2=%d per-entry=%d\n", bind_result, per_bind);
    if (bind_result != VK_SUCCESS || per_bind != VK_SUCCESS) return 27;

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 1.0f;
    VkSampler sampler = VK_NULL_HANDLE;
    if (create_sampler(device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) return 15;
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if (create_set_layout(device, &layout_info, nullptr, &set_layout) != VK_SUCCESS) return 16;
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pipeline_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_info.setLayoutCount = 1;
    pipeline_info.pSetLayouts = &set_layout;
    pipeline_info.pushConstantRangeCount = 1;
    pipeline_info.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (create_pipeline_layout(device, &pipeline_info, nullptr, &layout) != VK_SUCCESS) return 17;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = graphics_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (create_pool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) return 18;
    VkCommandBufferAllocateInfo allocate_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (allocate(device, &allocate_info, &command) != VK_SUCCESS) return 19;
    VkDescriptorUpdateTemplateEntry entry{0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 0, sizeof(VkDescriptorImageInfo)};
    VkDescriptorUpdateTemplateCreateInfo template_info{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
    template_info.descriptorUpdateEntryCount = 1;
    template_info.pDescriptorUpdateEntries = &entry;
    template_info.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
    template_info.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    template_info.pipelineLayout = layout;
    template_info.set = 0;
    VkDescriptorUpdateTemplate update_template = VK_NULL_HANDLE;
    if (create_template(device, &template_info, nullptr, &update_template) != VK_SUCCESS) return 20;
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (begin(command, &begin_info) != VK_SUCCESS) return 21;
    VkBindDescriptorSetsInfoKHR bind_info{VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO_KHR};
    bind_info.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bind_info.layout = layout;
    bind2(command, &bind_info);
    const uint32_t value = 0x12345678;
    VkPushConstantsInfoKHR constants_info{VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO_KHR};
    constants_info.layout = layout;
    constants_info.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    constants_info.size = sizeof(value);
    constants_info.pValues = &value;
    constants2(command, &constants_info);
    VkDescriptorImageInfo image_info{};
    image_info.sampler = sampler;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image_info;
    push(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &write);
    VkPushDescriptorSetInfoKHR push_info{VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO_KHR};
    push_info.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_info.layout = layout;
    push_info.set = 0;
    push_info.descriptorWriteCount = 1;
    push_info.pDescriptorWrites = &write;
    push2(command, &push_info);
    push_template(command, update_template, layout, 0, &image_info);
    VkPushDescriptorSetWithTemplateInfoKHR template2_info{VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_INFO_KHR};
    template2_info.descriptorUpdateTemplate = update_template;
    template2_info.layout = layout;
    template2_info.set = 0;
    template2_info.pData = &image_info;
    push_template2(command, &template2_info);
    const VkResult recorded = end(command);
    std::printf("[maintenance6-smoke] vkEndCommandBuffer=%d %s\n", recorded,
                recorded == VK_SUCCESS ? "PASS" : "FAIL");
    return recorded == VK_SUCCESS ? 0 : 22;
}
