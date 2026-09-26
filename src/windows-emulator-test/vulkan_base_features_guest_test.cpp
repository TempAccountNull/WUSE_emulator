#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace sogen::test
{
    namespace
    {
        struct loaded_module
        {
            HMODULE handle{};

            ~loaded_module()
            {
                if (handle)
                {
                    FreeLibrary(handle);
                }
            }
        };

        struct native_instance
        {
            VkInstance handle{};
            PFN_vkDestroyInstance destroy{};

            ~native_instance()
            {
                if (handle && destroy)
                {
                    destroy(handle, nullptr);
                }
            }
        };

        void mask_unbridged_sparse_features(VkPhysicalDeviceFeatures& features)
        {
            features.shaderResourceResidency = VK_FALSE;
            features.shaderResourceMinLod = VK_FALSE;
            features.sparseBinding = VK_FALSE;
            features.sparseResidencyBuffer = VK_FALSE;
            features.sparseResidencyImage2D = VK_FALSE;
            features.sparseResidencyImage3D = VK_FALSE;
            features.sparseResidency2Samples = VK_FALSE;
            features.sparseResidency4Samples = VK_FALSE;
            features.sparseResidency8Samples = VK_FALSE;
            features.sparseResidency16Samples = VK_FALSE;
            features.sparseResidencyAliased = VK_FALSE;
        }

        std::wstring system_vulkan_loader_path()
        {
            std::array<wchar_t, MAX_PATH> directory{};
            const UINT size = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
            if (size == 0 || size >= directory.size())
            {
                return {};
            }
            return std::wstring(directory.data(), size) + L"\\vulkan-1.dll";
        }

        std::wstring adjacent_shim_path()
        {
            std::array<wchar_t, MAX_PATH> executable{};
            const DWORD size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
            if (size == 0 || size >= executable.size())
            {
                return {};
            }
            std::wstring path(executable.data(), size);
            const auto separator = path.find_last_of(L"\\/");
            if (separator == std::wstring::npos)
            {
                return {};
            }
            path.resize(separator + 1);
            return path + L"vulkan-shim.dll";
        }
    }

    TEST(VulkanBaseFeaturesGuestTest, LegacyShimExportMatchesRx6700DriverAfterSparseMask)
    {
        const std::wstring loader_path = system_vulkan_loader_path();
        ASSERT_FALSE(loader_path.empty());
        loaded_module loader{LoadLibraryW(loader_path.c_str())};
        if (!loader.handle)
        {
            GTEST_SKIP() << "Native Vulkan loader unavailable";
        }

        const auto get_native_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader.handle, "vkGetInstanceProcAddr"));
        ASSERT_NE(get_native_proc, nullptr);
        const auto create_native_instance = reinterpret_cast<PFN_vkCreateInstance>(get_native_proc(VK_NULL_HANDLE, "vkCreateInstance"));
        ASSERT_NE(create_native_instance, nullptr);

        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &application;
        native_instance instance{};
        if (create_native_instance(&create_info, nullptr, &instance.handle) != VK_SUCCESS)
        {
            GTEST_SKIP() << "Native Vulkan instance unavailable";
        }
        instance.destroy = reinterpret_cast<PFN_vkDestroyInstance>(get_native_proc(instance.handle, "vkDestroyInstance"));
        ASSERT_NE(instance.destroy, nullptr);

        const auto enumerate_devices =
            reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_native_proc(instance.handle, "vkEnumeratePhysicalDevices"));
        const auto get_native_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_native_proc(instance.handle, "vkGetPhysicalDeviceProperties"));
        const auto get_native_features =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(get_native_proc(instance.handle, "vkGetPhysicalDeviceFeatures"));
        ASSERT_NE(enumerate_devices, nullptr);
        ASSERT_NE(get_native_properties, nullptr);
        ASSERT_NE(get_native_features, nullptr);

        uint32_t device_count = 0;
        ASSERT_EQ(enumerate_devices(instance.handle, &device_count, nullptr), VK_SUCCESS);
        if (device_count == 0)
        {
            GTEST_SKIP() << "Native Vulkan has no physical devices";
        }
        std::vector<VkPhysicalDevice> devices(device_count);
        ASSERT_EQ(enumerate_devices(instance.handle, &device_count, devices.data()), VK_SUCCESS);

        VkPhysicalDevice rx6700 = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties rx6700_properties{};
        for (uint32_t index = 0; index < device_count; ++index)
        {
            VkPhysicalDeviceProperties properties{};
            get_native_properties(devices[index], &properties);
            if (properties.vendorID == 0x1002 && std::strstr(properties.deviceName, "6700") != nullptr)
            {
                rx6700 = devices[index];
                rx6700_properties = properties;
                break;
            }
        }
        if (!rx6700)
        {
            GTEST_SKIP() << "AMD Radeon RX 6700 Vulkan device unavailable";
        }

        const std::wstring shim_path = adjacent_shim_path();
        ASSERT_FALSE(shim_path.empty());
        loaded_module shim{LoadLibraryW(shim_path.c_str())};
        ASSERT_NE(shim.handle, nullptr) << "Adjacent vulkan-shim.dll is required for guest feature comparison";
        const auto get_guest_features =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(GetProcAddress(shim.handle, "vkGetPhysicalDeviceFeatures"));
        ASSERT_NE(get_guest_features, nullptr);

        VkPhysicalDeviceFeatures expected{};
        get_native_features(rx6700, &expected);
        mask_unbridged_sparse_features(expected);
        VkPhysicalDeviceFeatures guest{};
        get_guest_features(rx6700, &guest);

        static_assert(sizeof(VkPhysicalDeviceFeatures) % sizeof(VkBool32) == 0);
        constexpr size_t feature_count = sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
        std::array<VkBool32, feature_count> expected_bits{};
        std::array<VkBool32, feature_count> guest_bits{};
        std::memcpy(expected_bits.data(), &expected, sizeof(expected));
        std::memcpy(guest_bits.data(), &guest, sizeof(guest));

        SCOPED_TRACE(rx6700_properties.deviceName);
        for (size_t index = 0; index < feature_count; ++index)
        {
            EXPECT_EQ(guest_bits[index], expected_bits[index]) << "VkPhysicalDeviceFeatures field index " << index;
        }

        const auto get_native_features2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(get_native_proc(instance.handle, "vkGetPhysicalDeviceFeatures2"));
        const auto get_guest_features2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(GetProcAddress(shim.handle, "vkGetPhysicalDeviceFeatures2"));
        ASSERT_NE(get_native_features2, nullptr);
        ASSERT_NE(get_guest_features2, nullptr);
        VkPhysicalDeviceVulkan11Features native_vulkan11{};
        native_vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        VkPhysicalDeviceFeatures2 native_features2{};
        native_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        native_features2.pNext = &native_vulkan11;
        get_native_features2(rx6700, &native_features2);
        mask_unbridged_sparse_features(native_features2.features);
        VkPhysicalDeviceVulkan11Features guest_vulkan11{};
        guest_vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        VkPhysicalDeviceFeatures2 guest_features2{};
        guest_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        guest_features2.pNext = &guest_vulkan11;
        get_guest_features2(rx6700, &guest_features2);

        std::array<VkBool32, feature_count> expected_features2_bits{};
        std::array<VkBool32, feature_count> guest_features2_bits{};
        std::memcpy(expected_features2_bits.data(), &native_features2.features, sizeof(native_features2.features));
        std::memcpy(guest_features2_bits.data(), &guest_features2.features, sizeof(guest_features2.features));
        for (size_t index = 0; index < feature_count; ++index)
        {
            EXPECT_EQ(guest_features2_bits[index], expected_features2_bits[index])
                << "VkPhysicalDeviceFeatures2 base field index " << index;
        }
        EXPECT_EQ(guest_vulkan11.storageBuffer16BitAccess, native_vulkan11.storageBuffer16BitAccess);
        EXPECT_EQ(guest_vulkan11.multiview, native_vulkan11.multiview);
        EXPECT_EQ(guest_vulkan11.samplerYcbcrConversion, native_vulkan11.samplerYcbcrConversion);
        EXPECT_EQ(guest_vulkan11.shaderDrawParameters, native_vulkan11.shaderDrawParameters);
    }
}
