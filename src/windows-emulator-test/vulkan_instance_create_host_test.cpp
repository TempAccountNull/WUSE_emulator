#include <gtest/gtest.h>

#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include <vk_instance_create_wire.hpp>
#include <vk_instance_create_policy.hpp>
#include <vk_debug_utils_messenger_wire.hpp>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <windows.h>

#include <array>
#include <cstring>
#include <vector>

namespace sogen::test
{
    VKAPI_ATTR VkBool32 VKAPI_CALL instance_creation_callback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                               VkDebugUtilsMessageTypeFlagsEXT,
                                                               const VkDebugUtilsMessengerCallbackDataEXT*, void*)
    {
        return VK_FALSE;
    }

    TEST(VulkanInstanceCreateHostTest, NativeRx6700LoaderErrorsMatchBridgeAndRequestedVersionIsPreserved)
    {
        std::array<wchar_t, MAX_PATH> system_directory{};
        const UINT length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
        ASSERT_GT(length, 0u);
        ASSERT_LT(length, system_directory.size());
        const std::wstring path = std::wstring(system_directory.data(), length) + L"\\vulkan-1.dll";
        HMODULE module = LoadLibraryW(path.c_str());
        if (!module) GTEST_SKIP() << "System Vulkan loader unavailable";
        const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
        ASSERT_NE(get, nullptr);
        const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
        ASSERT_NE(create, nullptr);
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        const char* missing_layer = "VK_LAYER_SOGEN_not_installed";
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = &missing_layer;
        VkInstance native{};
        EXPECT_EQ(create(&info, nullptr, &native), VK_ERROR_LAYER_NOT_PRESENT);
        const char* missing_extension = "VK_SOGEN_missing_extension";
        info.enabledLayerCount = 0;
        info.ppEnabledLayerNames = nullptr;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &missing_extension;
        EXPECT_EQ(create(&info, nullptr, &native), VK_ERROR_EXTENSION_NOT_PRESENT);
        info.enabledExtensionCount = 0;
        info.ppEnabledExtensionNames = nullptr;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        app.pApplicationName = "Sogen instance-create oracle";
        info.pApplicationInfo = &app;
        ASSERT_EQ(create(&info, nullptr, &native), VK_SUCCESS);
        const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(native, "vkDestroyInstance"));
        const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(native, "vkEnumeratePhysicalDevices"));
        const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(native, "vkGetPhysicalDeviceProperties"));
        ASSERT_NE(destroy, nullptr);
        ASSERT_NE(enumerate, nullptr);
        ASSERT_NE(properties, nullptr);
        uint32_t count{};
        ASSERT_EQ(enumerate(native, &count, nullptr), VK_SUCCESS);
        std::vector<VkPhysicalDevice> devices(count);
        ASSERT_EQ(enumerate(native, &count, devices.data()), VK_SUCCESS);
        bool rx6700 = false;
        for (VkPhysicalDevice device : devices)
        {
            VkPhysicalDeviceProperties value{};
            properties(device, &value);
            rx6700 |= value.vendorID == 0x1002 && std::strstr(value.deviceName, "6700") != nullptr;
        }
        destroy(native, nullptr);
        FreeLibrary(module);
        if (!rx6700) GTEST_SKIP() << "RX 6700 Vulkan device unavailable";

        vulkan_host host;
        ASSERT_TRUE(host.available());
        vulkan_host::instance_create_options options{};
        options.api_version = VK_API_VERSION_1_1;
        options.application_info_present = true;
        options.application_name = "Sogen instance-create oracle";
        options.application_name_present = true;
        options.extension_bits = 1u << 31;
        uint64_t instance{};
        EXPECT_EQ(host.create_instance(instance, false, {}, &options), VK_ERROR_EXTENSION_NOT_PRESENT);
        EXPECT_EQ(instance, 0u);
        options.extension_bits = 0;
        ASSERT_EQ(host.create_instance(instance, false, {}, &options), VK_SUCCESS);
        uint32_t actual_version{};
        EXPECT_EQ(host.get_instance_api_version(instance, actual_version), VK_SUCCESS);
        EXPECT_EQ(actual_version, VK_API_VERSION_1_1);
        host.destroy_instance(instance);
        options = {};
        ASSERT_EQ(host.create_instance(instance, false, {}, &options), VK_SUCCESS);
        actual_version = 0;
        EXPECT_EQ(host.get_instance_api_version(instance, actual_version), VK_SUCCESS);
        EXPECT_EQ(actual_version, VK_API_VERSION_1_0);
        host.destroy_instance(instance);
        if (host.debug_utils_available())
        {
            VkDebugUtilsMessengerCreateInfoEXT callback{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            callback.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            callback.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
            callback.pfnUserCallback = instance_creation_callback;
            const auto packet = gpu_bridge::debug_utils_messenger_wire::marshal_create(0, 0, callback, 8);
            options.extension_bits = gpu_bridge::instance_ext_debug_utils;
            ASSERT_EQ(host.create_instance(instance, true, packet, &options), VK_SUCCESS);
            host.destroy_instance(instance);
        }
    }

    TEST(VulkanInstanceCreateHostTest, GuestPolicyMatchesAdvertisedExtensionsAndRejectsUnsupportedChains)
    {
        VkInstance instance{};
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        using gpu_bridge::instance_create_policy::validate;
        const char* missing_layer = "VK_LAYER_SOGEN_not_installed";
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = &missing_layer;
        EXPECT_EQ(validate(&info, nullptr, &instance, false, false).result, VK_ERROR_LAYER_NOT_PRESENT);
        info.enabledLayerCount = 0;
        info.ppEnabledLayerNames = nullptr;
        const char* missing_extension = "VK_SOGEN_missing_extension";
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &missing_extension;
        EXPECT_EQ(validate(&info, nullptr, &instance, false, false).result, VK_ERROR_EXTENSION_NOT_PRESENT);
        const std::array<const char*, 2> readback_names{VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_win32_surface"};
        info.enabledExtensionCount = static_cast<uint32_t>(readback_names.size());
        info.ppEnabledExtensionNames = readback_names.data();
        auto outcome = validate(&info, nullptr, &instance, false, false);
        ASSERT_EQ(outcome.result, VK_SUCCESS);
        EXPECT_EQ(outcome.extension_bits, gpu_bridge::instance_ext_surface | gpu_bridge::instance_ext_win32_surface);
        const std::array<const char*, 3> native_names{VK_KHR_SURFACE_EXTENSION_NAME,
                                                       "VK_KHR_win32_surface",
                                                       VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
        info.enabledExtensionCount = static_cast<uint32_t>(native_names.size());
        info.ppEnabledExtensionNames = native_names.data();
        EXPECT_EQ(validate(&info, nullptr, &instance, false, false).result, VK_ERROR_EXTENSION_NOT_PRESENT);
        outcome = validate(&info, nullptr, &instance, true, false);
        ASSERT_EQ(outcome.result, VK_SUCCESS);
        EXPECT_EQ(outcome.extension_bits, gpu_bridge::instance_ext_surface |
                                          gpu_bridge::instance_ext_win32_surface |
                                          gpu_bridge::instance_ext_surface_capabilities2);
        const std::array<const char*, 2> debug_names{VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
        info.enabledExtensionCount = static_cast<uint32_t>(debug_names.size());
        info.ppEnabledExtensionNames = debug_names.data();
        EXPECT_EQ(validate(&info, nullptr, &instance, true, false).result, VK_ERROR_EXTENSION_NOT_PRESENT);
        VkDebugUtilsMessengerCreateInfoEXT callback{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        callback.pfnUserCallback = instance_creation_callback;
        info.pNext = &callback;
        outcome = validate(&info, nullptr, &instance, true, true);
        ASSERT_EQ(outcome.result, VK_SUCCESS);
        EXPECT_EQ(outcome.extension_bits, gpu_bridge::instance_ext_surface | gpu_bridge::instance_ext_debug_utils);
        EXPECT_EQ(outcome.callback, &callback);
        VkValidationFeaturesEXT unsupported{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        info.pNext = &unsupported;
        outcome = validate(&info, nullptr, &instance, true, true);
        EXPECT_EQ(outcome.result, VK_ERROR_EXTENSION_NOT_PRESENT);
        EXPECT_EQ(outcome.rejected_pnext, VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT);
        info.pNext = nullptr;
        info.enabledExtensionCount = 0;
        info.ppEnabledExtensionNames = nullptr;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_MAKE_API_VERSION(0, 1, 4, 0);
        info.pApplicationInfo = &app;
        EXPECT_EQ(validate(&info, nullptr, &instance, false, false).result, VK_ERROR_INCOMPATIBLE_DRIVER);
        info.pApplicationInfo = nullptr;
        EXPECT_EQ(validate(&info, nullptr, &instance, false, false).result, VK_SUCCESS);
    }

    TEST(VulkanInstanceCreateHostTest, WireDecoderRejectsTruncatedMalformedAndUnknownOptions)
    {
        gpu_bridge::create_instance_request request{};
        request.magic = gpu_bridge::create_instance_request_magic;
        request.application_info_present = 1;
        request.api_version = VK_API_VERSION_1_1;
        request.application_version = 7;
        request.engine_version = 9;
        request.application_name_bytes = 6;
        request.extension_bits = gpu_bridge::instance_ext_surface | gpu_bridge::instance_ext_win32_surface;
        std::array<std::byte, sizeof(request) + 6> packet{};
        std::memcpy(packet.data(), &request, sizeof(request));
        std::memcpy(packet.data() + sizeof(request), "probe", 6); // includes NUL
        auto decoded = gpu_bridge::instance_create_wire::decode(packet);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->application_name, "probe");
        EXPECT_EQ(decoded->request.api_version, VK_API_VERSION_1_1);
        EXPECT_EQ(decoded->request.application_version, 7u);
        EXPECT_EQ(decoded->request.engine_version, 9u);
        EXPECT_EQ(decoded->request.extension_bits, request.extension_bits);
        vulkan_host host;
        if (host.available())
        {
            vulkan_host::instance_create_options options{};
            options.api_version = decoded->request.api_version;
            options.application_version = decoded->request.application_version;
            options.engine_version = decoded->request.engine_version;
            options.extension_bits = decoded->request.extension_bits;
            options.application_info_present = decoded->request.application_info_present != 0;
            options.application_name_present = decoded->request.application_name_bytes != 0;
            options.application_name = decoded->application_name;
            uint64_t instance{};
            ASSERT_EQ(host.create_instance(instance, false, {}, &options), VK_SUCCESS);
            uint32_t version{};
            EXPECT_EQ(host.get_instance_api_version(instance, version), VK_SUCCESS);
            EXPECT_EQ(version, VK_API_VERSION_1_1);
            host.destroy_instance(instance);
        }
        EXPECT_FALSE(gpu_bridge::instance_create_wire::decode({packet.data(), sizeof(request) - 1}));
        EXPECT_FALSE(gpu_bridge::instance_create_wire::decode({packet.data(), packet.size() - 1}));
        packet.back() = std::byte{'x'};
        EXPECT_FALSE(gpu_bridge::instance_create_wire::decode(packet));
        packet.back() = std::byte{0};
        request.extension_bits = 1u << 31;
        std::memcpy(packet.data(), &request, sizeof(request));
        EXPECT_FALSE(gpu_bridge::instance_create_wire::decode(packet));
        request.extension_bits = 0;
        request.reserved = 1;
        std::memcpy(packet.data(), &request, sizeof(request));
        EXPECT_FALSE(gpu_bridge::instance_create_wire::decode(packet));
        request.reserved = 0;
        request.application_info_present = 0;
        std::memcpy(packet.data(), &request, sizeof(request));
        EXPECT_FALSE(gpu_bridge::instance_create_wire::decode(packet));
        request.application_info_present = 1;
        request.callback_size = 1;
        std::memcpy(packet.data(), &request, sizeof(request));
        EXPECT_FALSE(gpu_bridge::instance_create_wire::decode(packet));
        request.callback_size = 0;
        std::memcpy(packet.data(), &request, sizeof(request));
        EXPECT_TRUE(gpu_bridge::instance_create_wire::decode(packet));
    }
}
