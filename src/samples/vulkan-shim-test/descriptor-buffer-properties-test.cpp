#include <windows.h>

#include <cstdio>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

bool test_descriptor_buffer_properties(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, const VkPhysicalDevice* devices,
                                       uint32_t device_count)
{
    const auto get_properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceProperties"));
    const auto get_properties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get_instance_proc(instance, "vkGetPhysicalDeviceProperties2"));
    if (!get_properties || !get_properties2)
    {
        std::printf("[shim-test] descriptor buffer property query entrypoints -> FAIL\n");
        return false;
    }

    for (uint32_t i = 0; i < device_count; ++i)
    {
        VkPhysicalDeviceProperties base{};
        get_properties(devices[i], &base);
        if (base.vendorID != 0x1002)
        {
            continue;
        }

        VkPhysicalDeviceMaintenance3Properties tail{};
        tail.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
        VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor{};
        descriptor.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;
        descriptor.pNext = &tail;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &descriptor;
        get_properties2(devices[i], &properties);

        const bool ok = descriptor.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT &&
                        descriptor.pNext == &tail && descriptor.descriptorBufferOffsetAlignment > 0 &&
                        (descriptor.descriptorBufferOffsetAlignment & (descriptor.descriptorBufferOffsetAlignment - 1)) == 0 &&
                        descriptor.samplerDescriptorSize > 0 && tail.maxPerSetDescriptors > 0;
        std::printf("[shim-test] AMD descriptor buffer properties: alignment=%llu sampler=%zu chained=%u -> %s\n",
                    static_cast<unsigned long long>(descriptor.descriptorBufferOffsetAlignment), descriptor.samplerDescriptorSize,
                    tail.maxPerSetDescriptors, ok ? "PASS" : "FAIL");
        return ok;
    }

    std::printf("[shim-test] AMD descriptor buffer property query -> SKIP (no AMD device)\n");
    return true;
}

int run_descriptor_buffer_properties_query(PFN_vkGetInstanceProcAddr get_instance_proc)
{
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc(nullptr, "vkCreateInstance"));
    if (!create_instance)
    {
        return 2;
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create_instance(&create_info, nullptr, &instance) != VK_SUCCESS)
    {
        return 3;
    }

    const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get_instance_proc(instance, "vkDestroyInstance"));
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
    if (!destroy_instance || !enumerate)
    {
        return 4;
    }
    uint32_t count = 0;
    if (enumerate(instance, &count, nullptr) != VK_SUCCESS)
    {
        destroy_instance(instance, nullptr);
        return 5;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (enumerate(instance, &count, devices.data()) != VK_SUCCESS)
    {
        destroy_instance(instance, nullptr);
        return 5;
    }
    const bool ok = test_descriptor_buffer_properties(get_instance_proc, instance, devices.data(), count);
    destroy_instance(instance, nullptr);
    return ok ? 0 : 6;
}
