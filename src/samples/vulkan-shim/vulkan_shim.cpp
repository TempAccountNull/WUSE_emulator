// Guest-side Vulkan shim: a drop-in vulkan-1.dll that implements the Vulkan entry points by
// forwarding them across the Sogen GPU bridge (\\.\SogenGpu) to the host's real driver. An app
// loads it exactly like a normal ICD/loader -- LoadLibrary + vkGetInstanceProcAddr -- and never
// sees the bridge. This is the guest counterpart to the host gpu_bridge io_device.
//
// Opaque Vulkan handles (VkInstance, VkPhysicalDevice, ...) are pointer-sized, so the bridge's
// object_id is stored directly in the handle value; no guest-side handle table is required.

#ifndef NOMINMAX
#define NOMINMAX // keep the Windows min/max macros from clobbering std::min/std::max
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>

#include <gpu_bridge_protocol.hpp>
#include <native_wsi_wire.hpp>
#include <vk_feature_chain.hpp>
#include <vk_render_pass.hpp>
#include <vk_synchronization.hpp>
#include <vk_queue_submit.hpp>
#include <vk_dynamic_state.hpp>

namespace gb = sogen::gpu_bridge;

namespace
{
    HANDLE g_bridge = INVALID_HANDLE_VALUE;

    HANDLE bridge()
    {
        if (g_bridge == INVALID_HANDLE_VALUE)
        {
            g_bridge = CreateFileA(R"(\\.\SogenGpu)", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        return g_bridge;
    }

    // When the Sogen GPU bridge device (\\.\SogenGpu) is unavailable -- e.g. the same game binary is run
    // on real Windows, outside the emulator -- transparently forward to the host's real vulkan-1.dll
    // instead of failing every call, so the shim can stay in place as the game's vulkan-1.dll either way.
    // Forwarding happens at the vkGetInstanceProcAddr / vkGetDeviceProcAddr level: the real loader's
    // function pointers are handed straight to the app, so every resolved call (vkCreateInstance,
    // vkCmdDraw, ...) lands in the real driver directly. The few global commands an app may call through
    // our exports before it has those pointers (vkCreateInstance, vkEnumerateInstance*) forward explicitly.
    PFN_vkGetInstanceProcAddr g_real_get_instance_proc_addr = nullptr;
    PFN_vkGetDeviceProcAddr g_real_get_device_proc_addr = nullptr;

    void shim_log(const char* message); // defined below

    HMODULE self_module()
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&self_module), &module);
        return module;
    }

    void init_passthrough()
    {
        // Bridge is reachable: act as the real shim, no forwarding.
        if (bridge() != INVALID_HANDLE_VALUE)
        {
            return;
        }

        // Resolve <system>\vulkan-1.dll. In a 32-bit (WOW64) process the file-system redirector maps
        // System32 to SysWOW64, so this picks up the matching-bitness loader automatically.
        std::array<wchar_t, MAX_PATH> path{};
        const auto length = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
        constexpr std::wstring_view suffix = L"\\vulkan-1.dll";
        if (length == 0 || length + suffix.size() + 1 > path.size())
        {
            return;
        }
        // Copy the suffix plus its null terminator (data() points at a null-terminated literal).
        std::memcpy(path.data() + length, suffix.data(), (suffix.size() + 1) * sizeof(wchar_t));

        HMODULE real = LoadLibraryW(path.data());
        if (!real)
        {
            return;
        }

        // If the system vulkan-1.dll resolves back to this very shim (it was installed as the system
        // loader), forwarding would recurse into ourselves -- leave it disabled and behave as before.
        if (real == self_module())
        {
            FreeLibrary(real);
            return;
        }

        // Cast through void* (GetProcAddress returns FARPROC): a direct function-pointer cast trips
        // GCC/MinGW's -Wcast-function-type.
        auto* const gipa =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(real, "vkGetInstanceProcAddr")));
        if (!gipa)
        {
            FreeLibrary(real);
            return;
        }

        g_real_get_device_proc_addr =
            reinterpret_cast<PFN_vkGetDeviceProcAddr>(reinterpret_cast<void*>(GetProcAddress(real, "vkGetDeviceProcAddr")));
        g_real_get_instance_proc_addr = gipa; // published last: this is what passthrough_active() tests

        shim_log("vulkan-shim: \\\\.\\SogenGpu unavailable; forwarding to the system vulkan-1.dll\n");
    }

    // True once we've decided to forward to the real loader. Decided once, on first use.
    bool passthrough_active()
    {
        static std::once_flag once;
        std::call_once(once, init_passthrough);
        return g_real_get_instance_proc_addr != nullptr;
    }

    // Resolves a global (no-instance) command from the real loader, or null if it doesn't expose it.
    template <typename Fn>
    Fn real_global_command(const char* name)
    {
        return reinterpret_cast<Fn>(g_real_get_instance_proc_addr(VK_NULL_HANDLE, name));
    }

    // Flushes the coalesced descriptor-set updates (see vkUpdateDescriptorSets); defined below.
    void flush_descriptor_updates();

    bool bridge_call(uint32_t code, const void* in, DWORD in_len, void* out, DWORD out_len)
    {
        // Every other bridge call may make the host observe descriptor state (record, submit, ...), so drain
        // pending updates first to keep host state identical to the un-batched path.
        if (code != gb::ioctl_update_descriptor_sets_batch)
        {
            flush_descriptor_updates();
        }

        const HANDLE handle = bridge();
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        DWORD returned = 0;
        if (DeviceIoControl(handle, code, const_cast<void*>(in), in_len, out, out_len, &returned, nullptr) != FALSE)
        {
            return true;
        }

        const DWORD error = GetLastError();
        std::array<char, 256> message{};
        std::snprintf(message.data(), message.size(),
                      "vulkan-shim: GPU bridge DeviceIoControl failed: code=0x%08lX input=%lu output=%lu GetLastError=%lu\n",
                      static_cast<unsigned long>(code), static_cast<unsigned long>(in_len), static_cast<unsigned long>(out_len),
                      static_cast<unsigned long>(error));
        shim_log(message.data());
        SetLastError(error);
        return false;
    }

    // Vulkan handles come in two shapes: dispatchable handles (VkInstance, VkDevice, VkQueue,
    // VkCommandBuffer, VkPhysicalDevice) are pointers -- pointer-sized, so 64-bit on x64 and 32-bit on
    // x86/WOW64 -- while non-dispatchable handles (VkBuffer, VkImage, VkDeviceMemory, ...) are uint64_t on
    // every platform. The bridge's object_id is a uint64 that always crosses the wire in full; store it
    // directly in uint64 handles, and in the pointer value for dispatchable handles. Object ids are small
    // monotonic counters, so they fit a 32-bit pointer without loss -- and a non-dispatchable handle keeps
    // the full 64 bits regardless of guest bitness. This makes a 32-bit (WOW64) shim and a 64-bit host
    // agree on the protocol, and vice versa.
    template <typename Handle>
    gb::object_id to_object_id(Handle handle)
    {
        if constexpr (std::is_pointer_v<Handle>)
        {
            return static_cast<gb::object_id>(reinterpret_cast<uintptr_t>(handle));
        }
        else
        {
            return static_cast<gb::object_id>(handle);
        }
    }

    template <typename Handle>
    Handle to_handle(gb::object_id id)
    {
        if constexpr (std::is_pointer_v<Handle>)
        {
            return reinterpret_cast<Handle>(static_cast<uintptr_t>(id));
        }
        else
        {
            return static_cast<Handle>(id);
        }
    }

    // Sparse operations are intentionally stubbed below, so none of the related capabilities may be
    // advertised. Keep the masking in one place so the legacy and Features2/Properties2 query paths
    // cannot drift apart.
    void mask_unsupported_sparse_features(VkPhysicalDeviceFeatures& features)
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

    void mask_unsupported_sparse_properties(VkPhysicalDeviceProperties& properties)
    {
        properties.limits.sparseAddressSpaceSize = 0;
        properties.sparseProperties = {};
    }

    VkQueueFlags mask_unsupported_queue_flags(VkQueueFlags flags)
    {
        return flags & ~static_cast<VkQueueFlags>(VK_QUEUE_SPARSE_BINDING_BIT);
    }

    // VkDeviceMemory is host-side; the guest can't see a host pointer. We emulate vkMapMemory by
    // staging a guest-side copy: download the host range on map, hand the app that buffer, and upload
    // it back on unmap so writes persist. allocationSize is tracked here to resolve VK_WHOLE_SIZE.
    std::unordered_map<gb::object_id, uint64_t> g_memory_sizes;
    std::mutex g_memory_sizes_mutex;
    std::unordered_set<gb::object_id> g_buffer_marker_devices;
    std::unordered_set<gb::object_id> g_buffer_marker2_devices;
    std::mutex g_buffer_marker_devices_mutex;
    std::unordered_set<gb::object_id> g_multi_draw_devices;
    std::mutex g_multi_draw_devices_mutex;

    struct mapped_range
    {
        std::vector<uint8_t> staging;
        uint64_t offset{};
        uint64_t size{};
    };

    std::unordered_map<gb::object_id, mapped_range> g_mapped_ranges;

    // Memory objects the bridge aliased straight into the guest address space (no staging copy). These are
    // not in g_mapped_ranges; vkUnmapMemory tears them down via the bridge instead of uploading.
    std::unordered_set<gb::object_id> g_direct_mapped;

    // vkDestroyX(device, child) for the non-dispatchable device children that share device_child_request.
    template <typename Handle>
    void destroy_device_child(uint32_t code, VkDevice device, Handle child)
    {
        if (!child)
        {
            return;
        }
        gb::device_child_request request{};
        request.device = to_object_id(device);
        request.object = to_object_id(child);
        bridge_call(code, &request, sizeof(request), nullptr, 0);
    }

    // The original single-thread assumption below is historical: guest preemption requires the map lock.
    // Command-buffer recording is batched: instead of one IOCTL per vkCmd*, each command is appended to a
    // per-command-buffer byte stream and the whole stream (begin -> cmds -> end) is flushed to the bridge
    // in a single IOCTL at vkEndCommandBuffer. This amortises the boundary crossing, which dominates
    // emulated frame time. The shim only runs inside the single-threaded emulator, so the map needs no
    // lock. Each record is a gb::command_record_header followed by that command's request payload.
    struct command_stream
    {
        std::vector<uint8_t> bytes;
        VkResult error = VK_SUCCESS;

        void clear()
        {
            bytes.clear();
            error = VK_SUCCESS;
        }
    };

    std::unordered_map<gb::object_id, command_stream> g_command_streams;
    std::mutex g_command_streams_mutex;

    command_stream* find_command_stream(gb::object_id command_buffer)
    {
        std::lock_guard<std::mutex> lock(g_command_streams_mutex);
        const auto it = g_command_streams.find(command_buffer);
        // Rehash preserves references to elements. Vulkan externally synchronizes each command buffer,
        // so other threads may alter the map but cannot erase or mutate this buffer during recording.
        return it == g_command_streams.end() ? nullptr : &it->second;
    }

    // Pending coalesced vkUpdateDescriptorSets blobs (the hottest bridge call - DXVK updates per draw).
    // Unlike the per-command-buffer streams (each synchronised to one thread by Vulkan), this global is
    // touched by every DXVK thread, and the preemptive time-slice can interrupt a thread mid-append, so the
    // mutex makes append and flush-swap atomic.
    std::vector<uint8_t> g_pending_descriptor_updates;
    std::mutex g_pending_descriptor_updates_mutex;

    void flush_descriptor_updates()
    {
        // Hold the lock across the IOCTL so concurrent flushes stay ordered: releasing it after the swap lets a
        // preempting thread append + flush and get its batch applied on the host before this (earlier) one. The
        // batch IOCTL never re-enters flush_descriptor_updates (bridge_call skips the flush for it) and runs
        // synchronously, so the lock cannot self-deadlock; a preempted appender just waits for this flush.
        std::lock_guard<std::mutex> lock(g_pending_descriptor_updates_mutex);
        if (g_pending_descriptor_updates.empty())
        {
            return;
        }

        std::vector<uint8_t> batch;
        batch.swap(g_pending_descriptor_updates);

        gb::result_response response{};
        bridge_call(gb::ioctl_update_descriptor_sets_batch, batch.data(), static_cast<DWORD>(batch.size()), &response, sizeof(response));
    }

    void record_command(gb::object_id command_buffer, gb::command command, const void* payload, size_t size)
    {
        std::lock_guard<std::mutex> lock(g_command_streams_mutex);
        auto& stream = g_command_streams[command_buffer];
        if (stream.error != VK_SUCCESS)
        {
            return;
        }
        constexpr size_t limit = 256 * 1024 * 1024;
        if (size > limit - sizeof(gb::command_record_header) || stream.bytes.size() > limit - sizeof(gb::command_record_header) - size)
        {
            stream.error = VK_ERROR_OUT_OF_HOST_MEMORY;
            return;
        }
        try
        {
            // Reserve the complete record before appending so allocation failure cannot leave a partial header.
            const size_t required = stream.bytes.size() + sizeof(gb::command_record_header) + size;
            if (required > stream.bytes.capacity())
            {
                stream.bytes.reserve(std::max(required, std::min(limit, stream.bytes.capacity() * 2)));
            }
            gb::command_record_header header{.command = static_cast<uint32_t>(command), .size = static_cast<uint32_t>(size)};
            const auto* header_bytes = reinterpret_cast<const uint8_t*>(&header);
            stream.bytes.insert(stream.bytes.end(), header_bytes, header_bytes + sizeof(header));
            const auto* payload_bytes = reinterpret_cast<const uint8_t*>(payload);
            if (size)
            {
                stream.bytes.insert(stream.bytes.end(), payload_bytes, payload_bytes + size);
            }
        }
        catch (const std::bad_alloc&)
        {
            stream.error = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    void fail_recorded_command(gb::object_id command_buffer, VkResult error)
    {
        std::lock_guard<std::mutex> lock(g_command_streams_mutex);
        auto& stream = g_command_streams[command_buffer];
        if (stream.error == VK_SUCCESS)
        {
            stream.error = error;
        }
    }

    void record_extended_dynamic(gb::dynamic_request request, const void* values, size_t stride)
    {
        try
        {
            const auto payload = gb::encode_dynamic(request, values, stride);
            record_command(request.command_buffer, gb::command::cmd_extended_dynamic, payload.data(), payload.size());
        }
        catch (const std::bad_alloc&)
        {
            request.error = VK_ERROR_OUT_OF_HOST_MEMORY;
            record_command(request.command_buffer, gb::command::cmd_extended_dynamic, &request, sizeof(request));
        }
    }

    template <typename... T>
    std::vector<std::byte> render_pass_packet(gb::object_id object, const T&... values)
    {
        auto bytes = gb::render_pass_wire::encode(values...);
        const gb::render_pass_packet header{.object = object, .version = 1, .payload_size = static_cast<uint32_t>(bytes.size())};
        bytes.insert(bytes.begin(), reinterpret_cast<const std::byte*>(&header),
                     reinterpret_cast<const std::byte*>(&header) + sizeof(header));
        return bytes;
    }

    template <typename T, typename H>
    VkResult create_render_pass_object(uint32_t code, VkDevice device, const T* info, const VkAllocationCallbacks* allocator, H* object)
    {
        if (!object)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        *object = VK_NULL_HANDLE;
        if (!info)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // Guest allocation callbacks cannot be called on the host. Reject them explicitly instead
        // of forwarding guest function pointers or silently replacing the requested allocator.
        if (allocator)
        {
            OutputDebugStringA("[vulkan-shim] render-pass allocation callbacks are unsupported\n");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        try
        {
            const auto packet = render_pass_packet(to_object_id(device), *info);
            gb::object_response response{};
            if (!bridge_call(code, packet.data(), static_cast<DWORD>(packet.size()), &response, sizeof(response)))
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            if (response.vk_result == VK_SUCCESS)
            {
                *object = to_handle<H>(response.object);
            }
            return static_cast<VkResult>(response.vk_result);
        }
        catch (const gb::render_pass_wire::error& error)
        {
            OutputDebugStringA(error.what());
            return error.result;
        }
        catch (const std::bad_alloc&)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    template <typename... T>
    void record_render_pass(VkCommandBuffer buffer, gb::command command, const T*... values)
    {
        // The stream already exists after vkBeginCommandBuffer. Store a failed void command's
        // result there so vkEndCommandBuffer cannot claim a successful, incomplete recording.
        auto* const stream = find_command_stream(to_object_id(buffer));
        if (!stream)
        {
            OutputDebugStringA("[vulkan-shim] render-pass command outside recording\n");
            return;
        }
        if (stream->error != VK_SUCCESS)
        {
            return;
        }
        if ((!values || ...))
        {
            stream->error = VK_ERROR_INITIALIZATION_FAILED;
            return;
        }
        try
        {
            const auto packet = render_pass_packet(to_object_id(buffer), *values...);
            record_command(to_object_id(buffer), command, packet.data(), packet.size());
        }
        catch (const gb::render_pass_wire::error& error)
        {
            OutputDebugStringA(error.what());
            stream->error = error.result;
        }
        catch (const std::bad_alloc&)
        {
            stream->error = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    void record_synchronization(VkCommandBuffer buffer, const gb::synchronization_wire::command& command)
    {
        auto* const stream = find_command_stream(to_object_id(buffer));
        if (!stream)
        {
            OutputDebugStringA("[vulkan-shim] synchronization command outside recording\n");
            return;
        }
        if (stream->error != VK_SUCCESS)
        {
            return;
        }
        try
        {
            auto bytes = gb::synchronization_wire::encode(command);
            const gb::render_pass_packet header{
                .object = to_object_id(buffer), .version = 1, .payload_size = static_cast<uint32_t>(bytes.size())};
            bytes.insert(bytes.begin(), reinterpret_cast<const std::byte*>(&header),
                         reinterpret_cast<const std::byte*>(&header) + sizeof(header));
            record_command(to_object_id(buffer), gb::command::cmd_synchronization, bytes.data(), bytes.size());
        }
        catch (const gb::render_pass_wire::error& error)
        {
            OutputDebugStringA(error.what());
            stream->error = error.result;
        }
        catch (const std::bad_alloc&)
        {
            stream->error = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    // OutputDebugStringA is unbuffered; printf mirrors it to stdout, which the emulator captures.
    void shim_log(const char* message)
    {
        OutputDebugStringA(message);
        std::fputs(message, stdout);
        std::fflush(stdout);
    }

}

#include "native_wsi_shim.inc"

extern "C"
{
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                                          const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
    {
        if (native_wsi_enabled())
        {
            const auto validation = validate_native_instance(pCreateInfo, pAllocator, pInstance);
            if (validation != VK_SUCCESS)
            {
                return validation;
            }
        }

        if (passthrough_active())
        {
            if (auto* const fn = real_global_command<PFN_vkCreateInstance>("vkCreateInstance"))
            {
                return fn(pCreateInfo, pAllocator, pInstance);
            }
        }

        gb::create_instance_response response{};
        if (!bridge_call(gb::ioctl_create_instance, nullptr, 0, &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pInstance = to_handle<VkInstance>(response.instance);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks*)
    {
        if (native_wsi_enabled())
        {
            native_destroy(nw::operation::destroy_instance, to_object_id(instance));
            return;
        }

        gb::destroy_instance_request request{};
        request.instance = to_object_id(instance);
        bridge_call(gb::ioctl_destroy_instance, &request, sizeof(request), nullptr, 0);
    }

    // The instance-level enumeration commands are resolved (and required) by loaders such as DXVK
    // before vkCreateInstance. The bridge's vkCreateInstance ignores the requested layers/extensions,
    // so these only need to advertise enough for callers to proceed: no layers, the Win32 WSI
    // instance extensions the bridge supports, and a modern API version.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* pApiVersion)
    {
        if (passthrough_active())
        {
            if (auto* const fn = real_global_command<PFN_vkEnumerateInstanceVersion>("vkEnumerateInstanceVersion"))
            {
                return fn(pApiVersion);
            }
        }

        if (pApiVersion)
        {
            *pApiVersion = VK_API_VERSION_1_3;
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount,
                                                                                            VkLayerProperties* pProperties)
    {
        if (passthrough_active())
        {
            if (auto* const fn = real_global_command<PFN_vkEnumerateInstanceLayerProperties>("vkEnumerateInstanceLayerProperties"))
            {
                return fn(pPropertyCount, pProperties);
            }
        }

        if (!pPropertyCount)
        {
            return VK_INCOMPLETE;
        }

        *pPropertyCount = 0;
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                                                                          uint32_t* pPropertyCount,
                                                                                          VkLayerProperties* pProperties)
    {
        if (passthrough_active())
        {
            if (auto* const fn = real_global_command<PFN_vkEnumerateDeviceLayerProperties>("vkEnumerateDeviceLayerProperties"))
            {
                return fn(physicalDevice, pPropertyCount, pProperties);
            }
        }

        if (!pPropertyCount)
        {
            return VK_INCOMPLETE;
        }

        *pPropertyCount = 0;
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* pLayerName,
                                                                                                uint32_t* pPropertyCount,
                                                                                                VkExtensionProperties* pProperties)
    {
        if (passthrough_active())
        {
            if (auto* const fn = real_global_command<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties"))
            {
                return fn(pLayerName, pPropertyCount, pProperties);
            }
        }

        static const VkExtensionProperties extensions[] = {
            {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
            {VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_SPEC_VERSION},
            {VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_SPEC_VERSION},
        };
        const uint32_t available = native_wsi_enabled() ? 3u : 2u;

        if (!pPropertyCount)
        {
            return VK_INCOMPLETE;
        }

        if (!pProperties)
        {
            *pPropertyCount = available;
            return VK_SUCCESS;
        }

        const uint32_t to_copy = std::min(*pPropertyCount, available);
        for (uint32_t i = 0; i < to_copy; ++i)
        {
            pProperties[i] = extensions[i];
        }
        *pPropertyCount = to_copy;
        return to_copy < available ? VK_INCOMPLETE : VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pCount,
                                                                                    VkPhysicalDevice* pDevices)
    {
        gb::enumerate_physical_devices_request request{};
        request.instance = to_object_id(instance);

        if (!pDevices)
        {
            request.max_count = 0;
            gb::enumerate_physical_devices_response response{};
            if (!bridge_call(gb::ioctl_enumerate_physical_devices, &request, sizeof(request), &response, sizeof(response)))
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            *pCount = response.count;
            return static_cast<VkResult>(response.vk_result);
        }

        request.max_count = *pCount;

        std::vector<std::byte> buffer(sizeof(gb::enumerate_physical_devices_response) +
                                      static_cast<size_t>(*pCount) * sizeof(gb::object_id));
        if (!bridge_call(gb::ioctl_enumerate_physical_devices, &request, sizeof(request), buffer.data(), static_cast<DWORD>(buffer.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto* response = reinterpret_cast<const gb::enumerate_physical_devices_response*>(buffer.data());
        const auto* ids = reinterpret_cast<const gb::object_id*>(buffer.data() + sizeof(*response));

        const uint32_t written = (response->count < *pCount) ? response->count : *pCount;
        for (uint32_t i = 0; i < written; ++i)
        {
            pDevices[i] = to_handle<VkPhysicalDevice>(ids[i]);
        }

        *pCount = written;
        return (written < response->count) ? VK_INCOMPLETE : VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                                                   VkPhysicalDeviceProperties* pProperties)
    {
        if (!pProperties)
        {
            return;
        }

        gb::get_physical_device_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        bridge_call(gb::ioctl_get_physical_device_properties, &request, sizeof(request), pProperties, sizeof(*pProperties));
        mask_unsupported_sparse_properties(*pProperties);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                                                                              uint32_t* pCount,
                                                                                              VkQueueFamilyProperties* pProperties)
    {
        gb::get_queue_family_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.max_count = pProperties ? *pCount : 0;

        std::vector<std::byte> buffer(sizeof(gb::get_queue_family_properties_response) +
                                      static_cast<size_t>(request.max_count) * sizeof(gb::queue_family_properties));
        if (!bridge_call(gb::ioctl_get_queue_family_properties, &request, sizeof(request), buffer.data(),
                         static_cast<DWORD>(buffer.size())))
        {
            *pCount = 0;
            return;
        }

        const auto* response = reinterpret_cast<const gb::get_queue_family_properties_response*>(buffer.data());

        if (!pProperties)
        {
            *pCount = response->count;
            return;
        }

        const uint32_t written = (response->count < *pCount) ? response->count : *pCount;
        const auto* families =
            reinterpret_cast<const gb::queue_family_properties*>(buffer.data() + sizeof(gb::get_queue_family_properties_response));
        for (uint32_t i = 0; i < written; ++i)
        {
            pProperties[i] = {.queueFlags = mask_unsupported_queue_flags(families[i].queue_flags),
                              .queueCount = families[i].queue_count,
                              .timestampValidBits = families[i].timestamp_valid_bits,
                              .minImageTransferGranularity = {.width = families[i].min_image_transfer_granularity_width,
                                                              .height = families[i].min_image_transfer_granularity_height,
                                                              .depth = families[i].min_image_transfer_granularity_depth}};
        }
        *pCount = written;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                                        const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks*,
                                                                        VkDevice* pDevice)
    {
        gb::create_device_request request{};
        request.physical_device = to_object_id(physicalDevice);

        // Marshal every requested queue family (DXVK asks for a graphics queue plus a separate
        // transfer/compute family); the host must create a queue for each so later vkGetDeviceQueue
        // calls for those families succeed.
        std::vector<gb::device_queue_create_entry> queue_entries;
        if (pCreateInfo && pCreateInfo->queueCreateInfoCount > 0 && pCreateInfo->pQueueCreateInfos)
        {
            queue_entries.reserve(pCreateInfo->queueCreateInfoCount);
            for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i)
            {
                queue_entries.push_back({.queue_family_index = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex,
                                         .queue_count = pCreateInfo->pQueueCreateInfos[i].queueCount});
            }
        }
        else
        {
            queue_entries.push_back({.queue_family_index = 0, .queue_count = 1});
        }
        request.queue_create_count = static_cast<uint32_t>(queue_entries.size());

        // Marshal the enabled device-extension names (NUL-terminated, concatenated).
        std::vector<std::byte> extension_blob;
        uint32_t extension_count = 0;
        bool buffer_marker_enabled = false;
        bool multi_draw_extension_enabled = false;
        if (pCreateInfo && pCreateInfo->ppEnabledExtensionNames)
        {
            for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
            {
                const char* name = pCreateInfo->ppEnabledExtensionNames[i];
                if (!name)
                {
                    continue;
                }
                buffer_marker_enabled |= std::strcmp(name, VK_AMD_BUFFER_MARKER_EXTENSION_NAME) == 0;
                multi_draw_extension_enabled |= std::strcmp(name, VK_EXT_MULTI_DRAW_EXTENSION_NAME) == 0;
                const auto* bytes = reinterpret_cast<const std::byte*>(name);
                extension_blob.insert(extension_blob.end(), bytes, bytes + std::strlen(name) + 1);
                ++extension_count;
            }
        }

        // Marshal the features to enable as a feature_chain_record stream: the base VkPhysicalDeviceFeatures
        // (as a FEATURES_2 record) plus each known chained feature struct. Features arrive either through a
        // VkPhysicalDeviceFeatures2 in pNext (DXVK) or via pEnabledFeatures.
        std::vector<std::byte> feature_blob;
        uint32_t feature_struct_count = 0;
        const auto append_record = [&](VkStructureType type, const void* body_src) {
            const auto body_size = static_cast<uint32_t>(gb::feature_body_size(type));
            const gb::feature_chain_record record{.s_type = static_cast<uint32_t>(type), .body_size = body_size};
            const auto* record_bytes = reinterpret_cast<const std::byte*>(&record);
            feature_blob.insert(feature_blob.end(), record_bytes, record_bytes + sizeof(record));
            if (body_size > 0 && body_src)
            {
                const auto* body_bytes = static_cast<const std::byte*>(body_src);
                feature_blob.insert(feature_blob.end(), body_bytes, body_bytes + body_size);
            }
            ++feature_struct_count;
        };

        bool have_features2 = false;
        bool multi_draw_feature_enabled = false;
        if (pCreateInfo)
        {
            for (const auto* next = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); next; next = next->pNext)
            {
                if (gb::feature_struct_size(next->sType) == 0)
                {
                    continue;
                }
                append_record(next->sType, reinterpret_cast<const uint8_t*>(next) + gb::feature_chain_header_size);
                if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT)
                {
                    multi_draw_feature_enabled =
                        reinterpret_cast<const VkPhysicalDeviceMultiDrawFeaturesEXT*>(next)->multiDraw == VK_TRUE;
                }
                have_features2 = have_features2 || next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            }
            if (!have_features2 && pCreateInfo->pEnabledFeatures)
            {
                append_record(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, pCreateInfo->pEnabledFeatures);
            }
        }

        request.extension_count = extension_count;
        request.extension_blob_size = static_cast<uint32_t>(extension_blob.size());
        request.feature_struct_count = feature_struct_count;
        request.feature_blob_size = static_cast<uint32_t>(feature_blob.size());

        const size_t queue_bytes = queue_entries.size() * sizeof(gb::device_queue_create_entry);
        std::vector<std::byte> in(sizeof(request) + queue_bytes + extension_blob.size() + feature_blob.size());
        size_t offset = 0;
        std::memcpy(in.data() + offset, &request, sizeof(request));
        offset += sizeof(request);
        std::memcpy(in.data() + offset, queue_entries.data(), queue_bytes);
        offset += queue_bytes;
        if (!extension_blob.empty())
        {
            std::memcpy(in.data() + offset, extension_blob.data(), extension_blob.size());
            offset += extension_blob.size();
        }
        if (!feature_blob.empty())
        {
            std::memcpy(in.data() + offset, feature_blob.data(), feature_blob.size());
        }

        gb::create_device_response response{};
        if (!bridge_call(gb::ioctl_create_device, in.data(), static_cast<DWORD>(in.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pDevice = to_handle<VkDevice>(response.device);
        if (buffer_marker_enabled)
        {
            std::lock_guard lock(g_buffer_marker_devices_mutex);
            g_buffer_marker_devices.insert(response.device);
            if (response.reserved & gb::device_cap_buffer_marker2)
            {
                g_buffer_marker2_devices.insert(response.device);
            }
        }
        if (multi_draw_extension_enabled && multi_draw_feature_enabled &&
            (response.reserved & gb::device_cap_multi_draw))
        {
            std::lock_guard lock(g_multi_draw_devices_mutex);
            g_multi_draw_devices.insert(response.device);
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks*)
    {
        {
            std::lock_guard lock(g_multi_draw_devices_mutex);
            g_multi_draw_devices.erase(to_object_id(device));
        }
        {
            std::lock_guard lock(g_buffer_marker_devices_mutex);
            g_buffer_marker_devices.erase(to_object_id(device));
            g_buffer_marker2_devices.erase(to_object_id(device));
        }
        if (native_wsi_enabled())
        {
            native_destroy(nw::operation::destroy_device, to_object_id(device));
            return;
        }

        gb::destroy_device_request request{};
        request.device = to_object_id(device);
        bridge_call(gb::ioctl_destroy_device, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex,
                                                                      VkQueue* pQueue)
    {
        gb::get_device_queue_request request{};
        request.device = to_object_id(device);
        request.queue_family_index = queueFamilyIndex;
        request.queue_index = queueIndex;

        gb::get_device_queue_response response{};
        if (!bridge_call(gb::ioctl_get_device_queue, &request, sizeof(request), &response, sizeof(response)))
        {
            *pQueue = VK_NULL_HANDLE;
            return;
        }

        *pQueue = to_handle<VkQueue>(response.queue);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* pCreateInfo,
                                                                             const VkAllocationCallbacks*, VkCommandPool* pCommandPool)
    {
        gb::create_command_pool_request request{};
        request.device = to_object_id(device);
        request.queue_family_index = pCreateInfo ? pCreateInfo->queueFamilyIndex : 0;
        request.flags = pCreateInfo ? pCreateInfo->flags : 0;

        gb::create_command_pool_response response{};
        if (!bridge_call(gb::ioctl_create_command_pool, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pCommandPool = to_handle<VkCommandPool>(response.command_pool);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool,
                                                                          const VkAllocationCallbacks*)
    {
        gb::destroy_command_pool_request request{};
        request.device = to_object_id(device);
        request.command_pool = to_object_id(commandPool);
        bridge_call(gb::ioctl_destroy_command_pool, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device,
                                                                                  const VkCommandBufferAllocateInfo* pAllocateInfo,
                                                                                  VkCommandBuffer* pCommandBuffers)
    {
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i)
        {
            gb::allocate_command_buffer_request request{};
            request.device = to_object_id(device);
            request.command_pool = to_object_id(pAllocateInfo->commandPool);
            request.level = static_cast<uint32_t>(pAllocateInfo->level);

            gb::allocate_command_buffer_response response{};
            if (!bridge_call(gb::ioctl_allocate_command_buffer, &request, sizeof(request), &response, sizeof(response)) ||
                response.vk_result != VK_SUCCESS)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            pCommandBuffers[i] = to_handle<VkCommandBuffer>(response.command_buffer);
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                                                                          uint32_t commandBufferCount,
                                                                          const VkCommandBuffer* pCommandBuffers)
    {
        for (uint32_t i = 0; i < commandBufferCount; ++i)
        {
            gb::free_command_buffer_request request{};
            request.device = to_object_id(device);
            request.command_pool = to_object_id(commandPool);
            request.command_buffer = to_object_id(pCommandBuffers[i]);
            bridge_call(gb::ioctl_free_command_buffer, &request, sizeof(request), nullptr, 0);
        }
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                                              const VkCommandBufferBeginInfo* pBeginInfo)
    {
        // Start a fresh recording stream; the begin is its first record. Flushed at vkEndCommandBuffer.
        gb::begin_command_buffer_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.flags = pBeginInfo ? pBeginInfo->flags : 0;

        // A secondary command buffer carries inheritance; for dynamic rendering it has a chained
        // VkCommandBufferInheritanceRenderingInfo describing the attachment formats it renders to.
        const VkCommandBufferInheritanceRenderingInfo* rendering = nullptr;
        if (pBeginInfo && pBeginInfo->pInheritanceInfo)
        {
            request.is_secondary = 1;
            for (const auto* next = static_cast<const VkBaseInStructure*>(pBeginInfo->pInheritanceInfo->pNext); next; next = next->pNext)
            {
                if (next->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO)
                {
                    rendering = reinterpret_cast<const VkCommandBufferInheritanceRenderingInfo*>(next);
                    request.inherit_rendering_info_present = 1;
                }
                else if (next->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_CONDITIONAL_RENDERING_INFO_EXT)
                {
                    const auto* conditional = reinterpret_cast<const VkCommandBufferInheritanceConditionalRenderingInfoEXT*>(next);
                    request.inherit_conditional_rendering_enabled = conditional->conditionalRenderingEnable == VK_TRUE;
                }
            }
        }

        std::vector<uint8_t> message(sizeof(request));
        if (rendering)
        {
            request.inherit_view_mask = rendering->viewMask;
            request.inherit_color_count = rendering->colorAttachmentCount;
            request.inherit_depth_format = static_cast<uint32_t>(rendering->depthAttachmentFormat);
            request.inherit_stencil_format = static_cast<uint32_t>(rendering->stencilAttachmentFormat);
            request.inherit_rasterization_samples = static_cast<uint32_t>(rendering->rasterizationSamples);
            request.inherit_rendering_flags = static_cast<uint32_t>(rendering->flags);
            message.resize(sizeof(request) + static_cast<size_t>(rendering->colorAttachmentCount) * sizeof(uint32_t));
            std::memcpy(message.data(), &request, sizeof(request));
            for (uint32_t i = 0; i < rendering->colorAttachmentCount; ++i)
            {
                const auto format = static_cast<uint32_t>(rendering->pColorAttachmentFormats[i]);
                std::memcpy(message.data() + sizeof(request) + i * sizeof(uint32_t), &format, sizeof(format));
            }
        }
        else
        {
            std::memcpy(message.data(), &request, sizeof(request));
        }

        {
            std::lock_guard<std::mutex> lock(g_command_streams_mutex);
            g_command_streams[request.command_buffer].clear();
        }
        record_command(request.command_buffer, gb::command::begin_command_buffer, message.data(), message.size());
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount,
                                                                          const VkCommandBuffer* pCommandBuffers)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_execute_commands_request) +
                                     static_cast<size_t>(commandBufferCount) * sizeof(gb::object_id));
        gb::cmd_execute_commands_request header{};
        header.command_buffer = command_buffer;
        header.count = commandBufferCount;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < commandBufferCount; ++i)
        {
            const gb::object_id id = to_object_id(pCommandBuffers[i]);
            std::memcpy(message.data() + sizeof(header) + i * sizeof(id), &id, sizeof(id));
        }
        record_command(command_buffer, gb::command::cmd_execute_commands, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer commandBuffer)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        gb::end_command_buffer_request request{};
        request.command_buffer = command_buffer;
        record_command(command_buffer, gb::command::end_command_buffer, &request, sizeof(request));

        std::vector<uint8_t> stream;
        {
            std::lock_guard<std::mutex> lock(g_command_streams_mutex);
            const auto it = g_command_streams.find(command_buffer);
            if (it == g_command_streams.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            if (it->second.error != VK_SUCCESS)
            {
                const VkResult result = it->second.error;
                g_command_streams.erase(it);
                return result;
            }
            stream = std::move(it->second.bytes);
            g_command_streams.erase(it);
        }

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_record_commands, stream.data(), static_cast<DWORD>(stream.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice device, VkCommandPool commandPool,
                                                                            VkCommandPoolResetFlags flags)
    {
        gb::reset_command_pool_request request{};
        request.device = to_object_id(device);
        request.command_pool = to_object_id(commandPool);
        request.flags = flags;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_reset_command_pool, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer commandBuffer,
                                                                              VkCommandBufferResetFlags flags)
    {
        // Drop any half-recorded local stream; recording only reaches the bridge at vkEndCommandBuffer.
        {
            std::lock_guard<std::mutex> lock(g_command_streams_mutex);
            g_command_streams.erase(to_object_id(commandBuffer));
        }

        gb::reset_command_buffer_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.flags = flags;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_reset_command_buffer, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
    {
        gb::queue_wait_idle_request request{};
        request.queue = to_object_id(queue);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_queue_wait_idle, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
    {
        gb::device_wait_idle_request request{};
        request.device = to_object_id(device);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_device_wait_idle, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice device, const VkFenceCreateInfo* pCreateInfo,
                                                                       const VkAllocationCallbacks*, VkFence* pFence)
    {
        gb::create_fence_request request{};
        request.device = to_object_id(device);
        request.flags = pCreateInfo ? pCreateInfo->flags : 0;

        gb::create_fence_response response{};
        if (!bridge_call(gb::ioctl_create_fence, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pFence = to_handle<VkFence>(response.fence);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks*)
    {
        gb::destroy_fence_request request{};
        request.device = to_object_id(device);
        request.fence = to_object_id(fence);
        bridge_call(gb::ioctl_destroy_fence, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateEvent(VkDevice device, const VkEventCreateInfo* pCreateInfo,
                                                                       const VkAllocationCallbacks*, VkEvent* pEvent)
    {
        gb::create_event_request request{};
        request.device = to_object_id(device);
        request.flags = pCreateInfo ? pCreateInfo->flags : 0;

        gb::create_event_response response{};
        if (!bridge_call(gb::ioctl_create_event, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pEvent = to_handle<VkEvent>(response.event);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyEvent(VkDevice device, VkEvent event, const VkAllocationCallbacks*)
    {
        gb::destroy_event_request request{};
        request.device = to_object_id(device);
        request.event = to_object_id(event);
        bridge_call(gb::ioctl_destroy_event, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetEventStatus(VkDevice device, VkEvent event)
    {
        const gb::event_op_request request{.device = to_object_id(device), .event = to_object_id(event)};
        gb::result_response response{};
        if (!bridge_call(gb::ioctl_get_event_status_owned, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkSetEvent(VkDevice device, VkEvent event)
    {
        gb::event_op_request request{};
        request.device = to_object_id(device);
        request.event = to_object_id(event);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_set_event, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkResetEvent(VkDevice device, VkEvent event)
    {
        gb::event_op_request request{};
        request.device = to_object_id(device);
        request.event = to_object_id(event);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_reset_event, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    // Historical behavior, superseded by the native event recorders below:
    // GPU-side event commands are not forwarded to the host command buffer: the bridge's submission model
    // is effectively synchronous, so DXVK's vkGetEventStatus always reports the event as set (see the host).
    // These recorders are no-ops; they exist so DXVK's device function table is non-null and does not call
    // through a null pointer when it records them.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer buffer, VkEvent event, VkPipelineStageFlags stages)
    {
        gb::synchronization_wire::command command{};
        command.op = gb::synchronization_wire::operation::set_event;
        command.event = event;
        command.stage = stages;
        record_synchronization(buffer, command);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent(VkCommandBuffer buffer, VkEvent event, VkPipelineStageFlags stages)
    {
        gb::synchronization_wire::command command{};
        command.op = gb::synchronization_wire::operation::reset_event;
        command.event = event;
        command.stage = stages;
        record_synchronization(buffer, command);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer buffer, uint32_t eventCount, const VkEvent* events,
                                                                     VkPipelineStageFlags sourceStages,
                                                                     VkPipelineStageFlags destinationStages, uint32_t memoryCount,
                                                                     const VkMemoryBarrier* memory, uint32_t bufferCount,
                                                                     const VkBufferMemoryBarrier* buffers, uint32_t imageCount,
                                                                     const VkImageMemoryBarrier* images)
    {
        gb::synchronization_wire::command command{};
        command.op = gb::synchronization_wire::operation::wait_events;
        command.event_count = eventCount;
        command.events = events;
        command.legacy = {.source_stages = sourceStages,
                          .destination_stages = destinationStages,
                          .flags = 0,
                          .memory_count = memoryCount,
                          .memory = memory,
                          .buffer_count = bufferCount,
                          .buffers = buffers,
                          .image_count = imageCount,
                          .images = images};
        record_synchronization(buffer, command);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2(VkCommandBuffer buffer, VkEvent event,
                                                                    const VkDependencyInfo* dependency)
    {
        gb::synchronization_wire::command command{};
        command.op = gb::synchronization_wire::operation::set_event2;
        command.event = event;
        if (dependency)
        {
            command.dependency = *dependency;
        }
        else
        {
            command.dependency.sType = VK_STRUCTURE_TYPE_MAX_ENUM;
        }
        record_synchronization(buffer, command);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2(VkCommandBuffer buffer, VkEvent event, VkPipelineStageFlags2 stages)
    {
        gb::synchronization_wire::command command{};
        command.op = gb::synchronization_wire::operation::reset_event2;
        command.event = event;
        command.stage2 = stages;
        record_synchronization(buffer, command);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2(VkCommandBuffer buffer, uint32_t eventCount, const VkEvent* events,
                                                                      const VkDependencyInfo* dependencies)
    {
        gb::synchronization_wire::command command{};
        command.op = gb::synchronization_wire::operation::wait_events2;
        command.event_count = eventCount;
        command.events = events;
        command.dependencies = dependencies;
        record_synchronization(buffer, command);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2KHR(VkCommandBuffer buffer, VkEvent event,
                                                                       const VkDependencyInfo* dependency)
    {
        vkCmdSetEvent2(buffer, event, dependency);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2KHR(VkCommandBuffer buffer, VkEvent event,
                                                                         VkPipelineStageFlags2 stages)
    {
        vkCmdResetEvent2(buffer, event, stages);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2KHR(VkCommandBuffer buffer, uint32_t eventCount, const VkEvent* events,
                                                                         const VkDependencyInfo* dependencies)
    {
        vkCmdWaitEvents2(buffer, eventCount, events, dependencies);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* pCreateInfo,
                                                                           const VkAllocationCallbacks*, VkSemaphore* pSemaphore)
    {
        gb::create_semaphore_request request{};
        request.device = to_object_id(device);
        request.flags = pCreateInfo ? pCreateInfo->flags : 0;
        request.semaphore_type = VK_SEMAPHORE_TYPE_BINARY;
        request.initial_value = 0;

        // DXVK creates timeline semaphores via a VkSemaphoreTypeCreateInfo on pNext; forward the type and
        // initial value so the host creates a real timeline semaphore (otherwise counter/signal/wait fail).
        if (pCreateInfo)
        {
            for (const auto* next = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); next; next = next->pNext)
            {
                if (next->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO)
                {
                    const auto* type_info = reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(next);
                    request.semaphore_type = static_cast<uint32_t>(type_info->semaphoreType);
                    request.initial_value = type_info->initialValue;
                    break;
                }
            }
        }

        gb::create_semaphore_response response{};
        if (!bridge_call(gb::ioctl_create_semaphore, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pSemaphore = to_handle<VkSemaphore>(response.semaphore);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore,
                                                                                    uint64_t* pValue)
    {
        gb::get_semaphore_counter_value_request request{};
        request.device = to_object_id(device);
        request.semaphore = to_object_id(semaphore);

        gb::get_semaphore_counter_value_response response{};
        if (!bridge_call(gb::ioctl_get_semaphore_counter_value, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (pValue)
        {
            *pValue = response.value;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphore(VkDevice device, const VkSemaphoreSignalInfo* pSignalInfo)
    {
        if (!pSignalInfo)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        gb::signal_semaphore_request request{};
        request.device = to_object_id(device);
        request.semaphore = to_object_id(pSignalInfo->semaphore);
        request.value = pSignalInfo->value;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_signal_semaphore, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* pWaitInfo,
                                                                          uint64_t timeout)
    {
        if (!pWaitInfo)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const uint32_t count = pWaitInfo->semaphoreCount;
        std::vector<uint8_t> message(sizeof(gb::wait_semaphores_request) + static_cast<size_t>(count) * sizeof(gb::wait_semaphore_entry));
        auto* header = reinterpret_cast<gb::wait_semaphores_request*>(message.data());
        header->device = to_object_id(device);
        header->flags = pWaitInfo->flags;
        header->semaphore_count = count;
        header->timeout = timeout;
        auto* entries = reinterpret_cast<gb::wait_semaphore_entry*>(message.data() + sizeof(gb::wait_semaphores_request));
        for (uint32_t i = 0; i < count; ++i)
        {
            entries[i].semaphore = to_object_id(pWaitInfo->pSemaphores[i]);
            entries[i].value = pWaitInfo->pValues[i];
        }

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_wait_semaphores, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(VkDevice device,
                                                                                         const VkBufferDeviceAddressInfo* pInfo)
    {
        if (!pInfo)
        {
            return 0;
        }
        gb::get_buffer_device_address_request request{};
        request.device = to_object_id(device);
        request.buffer = to_object_id(pInfo->buffer);

        gb::get_buffer_device_address_response response{};
        if (!bridge_call(gb::ioctl_get_buffer_device_address, &request, sizeof(request), &response, sizeof(response)))
        {
            return 0;
        }
        return response.address;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice device, VkSemaphore semaphore,
                                                                        const VkAllocationCallbacks*)
    {
        if (!semaphore)
        {
            return;
        }
        gb::destroy_semaphore_request request{};
        request.device = to_object_id(device);
        request.semaphore = to_object_id(semaphore);
        bridge_call(gb::ioctl_destroy_semaphore, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences)
    {
        for (uint32_t i = 0; i < fenceCount; ++i)
        {
            gb::reset_fence_request request{};
            request.device = to_object_id(device);
            request.fence = to_object_id(pFences[i]);

            gb::result_response response{};
            if (!bridge_call(gb::ioctl_reset_fence, &request, sizeof(request), &response, sizeof(response)) ||
                response.vk_result != VK_SUCCESS)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice, VkFence fence)
    {
        gb::get_fence_status_request request{};
        request.fence = to_object_id(fence);

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_get_fence_status, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits,
                                                                       VkFence fence)
    {
        try
        {
            // Preserve the API call's batch boundaries and all synchronization operations in one native submit.
            const auto packet = gb::queue_submit_wire::encode(queue, submitCount, pSubmits, fence);
            gb::result_response response{};
            if (!bridge_call(gb::ioctl_queue_submit_full, packet.data(), static_cast<DWORD>(packet.size()), &response, sizeof(response)))
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            return static_cast<VkResult>(response.vk_result);
        }
        catch (const gb::queue_submit_wire::error& error)
        {
            OutputDebugStringA(error.what());
            return error.result;
        }
        catch (const std::bad_alloc&)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    // synchronization2 submit: DXVK uses this for queue submission. Marshal each VkSubmitInfo2 (wait
    // semaphores, command buffers, signal semaphores) and forward to the host's real vkQueueSubmit2,
    // which has the real timeline semaphores. The fence is attached to the final submission.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits,
                                                                        VkFence fence)
    {
        if (submitCount == 0)
        {
            VkSubmitInfo2 empty{};
            empty.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            pSubmits = &empty;
            submitCount = fence ? 1 : 0;
        }

        for (uint32_t s = 0; s < submitCount; ++s)
        {
            const VkSubmitInfo2& si = pSubmits[s];
            const uint32_t wait_count = si.waitSemaphoreInfoCount;
            const uint32_t cmd_count = si.commandBufferInfoCount;
            const uint32_t signal_count = si.signalSemaphoreInfoCount;

            std::vector<uint8_t> message(sizeof(gb::queue_submit2_request) +
                                         static_cast<size_t>(wait_count + signal_count) * sizeof(gb::submit2_semaphore_entry) +
                                         static_cast<size_t>(cmd_count) * sizeof(uint64_t));
            auto* header = reinterpret_cast<gb::queue_submit2_request*>(message.data());
            header->queue = to_object_id(queue);
            header->fence = (s + 1 == submitCount) ? to_object_id(fence) : gb::null_object;
            header->wait_count = wait_count;
            header->command_buffer_count = cmd_count;
            header->signal_count = signal_count;
            header->reserved = 0;

            size_t offset = sizeof(*header);
            auto* waits = reinterpret_cast<gb::submit2_semaphore_entry*>(message.data() + offset);
            for (uint32_t i = 0; i < wait_count; ++i)
            {
                waits[i] = {.semaphore = to_object_id(si.pWaitSemaphoreInfos[i].semaphore),
                            .value = si.pWaitSemaphoreInfos[i].value,
                            .stage_mask = si.pWaitSemaphoreInfos[i].stageMask};
            }
            offset += static_cast<size_t>(wait_count) * sizeof(gb::submit2_semaphore_entry);

            auto* cmds = reinterpret_cast<uint64_t*>(message.data() + offset);
            for (uint32_t i = 0; i < cmd_count; ++i)
            {
                cmds[i] = to_object_id(si.pCommandBufferInfos[i].commandBuffer);
            }
            offset += static_cast<size_t>(cmd_count) * sizeof(uint64_t);

            auto* signals = reinterpret_cast<gb::submit2_semaphore_entry*>(message.data() + offset);
            for (uint32_t i = 0; i < signal_count; ++i)
            {
                signals[i] = {.semaphore = to_object_id(si.pSignalSemaphoreInfos[i].semaphore),
                              .value = si.pSignalSemaphoreInfos[i].value,
                              .stage_mask = si.pSignalSemaphoreInfos[i].stageMask};
            }

            gb::result_response response{};
            if (!bridge_call(gb::ioctl_queue_submit2, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)) ||
                response.vk_result != VK_SUCCESS)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        return VK_SUCCESS;
    }

    // Poll-and-yield: the host endpoint only ever does a non-blocking vkGetFenceStatus, so the host
    // thread is never blocked on the GPU. We spin here, yielding to the emulator between polls, until
    // the fence(s) signal or the timeout elapses. The real GPU makes progress independently of the
    // emulator thread, so the fence eventually signals.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences,
                                                                         VkBool32 waitAll, uint64_t timeout)
    {
        const ULONGLONG start = GetTickCount64();
        const ULONGLONG timeout_ms = (timeout == UINT64_MAX) ? UINT64_MAX : (timeout / 1000000ULL);

        for (;;)
        {
            uint32_t signaled = 0;
            for (uint32_t i = 0; i < fenceCount; ++i)
            {
                if (vkGetFenceStatus(device, pFences[i]) == VK_SUCCESS)
                {
                    ++signaled;
                }
            }

            if (waitAll ? (signaled == fenceCount) : (signaled > 0))
            {
                return VK_SUCCESS;
            }

            if (timeout_ms != UINT64_MAX && (GetTickCount64() - start) >= timeout_ms)
            {
                return VK_TIMEOUT;
            }

            SwitchToThread();
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL
    vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties)
    {
        if (!pMemoryProperties)
        {
            return;
        }
        *pMemoryProperties = {};

        gb::get_physical_device_memory_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        bridge_call(gb::ioctl_get_physical_device_memory_properties, &request, sizeof(request), pMemoryProperties,
                    sizeof(*pMemoryProperties));
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo,
                                                                          const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory)
    {
        if (!pMemory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        *pMemory = VK_NULL_HANDLE;
        if (!pAllocateInfo)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (pAllocator)
        {
            OutputDebugStringA("[vulkan-shim] vkAllocateMemory: guest allocation callbacks are unsupported\n");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        gb::allocate_memory_response response{};
        try
        {
            const auto packet = render_pass_packet(to_object_id(device), *pAllocateInfo);
            if (!bridge_call(gb::ioctl_allocate_memory_full, packet.data(), static_cast<DWORD>(packet.size()), &response, sizeof(response)))
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            if (response.vk_result != VK_SUCCESS)
            {
                return static_cast<VkResult>(response.vk_result);
            }
            try
            {
                const std::lock_guard lock(g_memory_sizes_mutex);
                g_memory_sizes.emplace(response.memory, pAllocateInfo->allocationSize);
            }
            catch (const std::bad_alloc&)
            {
                // The host allocation succeeded, but the guest cannot track whole-size mappings without this entry.
                const gb::free_memory_request release{.device = to_object_id(device), .memory = response.memory};
                if (!bridge_call(gb::ioctl_free_memory, &release, sizeof(release), nullptr, 0))
                {
                    OutputDebugStringA("[vulkan-shim] vkAllocateMemory: host cleanup failed after tracking allocation failure\n");
                }
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            *pMemory = to_handle<VkDeviceMemory>(response.memory);
            return VK_SUCCESS;
        }
        catch (const gb::render_pass_wire::error& error)
        {
            OutputDebugStringA(error.what());
            return error.result;
        }
        catch (const std::bad_alloc&)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkSetDeviceMemoryPriorityEXT(VkDevice device, VkDeviceMemory memory, float priority)
    {
        const gb::set_device_memory_priority_request request{.device = to_object_id(device),
                                                             .memory = to_object_id(memory),
                                                             .priority_bits = std::bit_cast<uint32_t>(priority),
                                                             .reserved = 0};
        gb::result_response response{.vk_result = VK_ERROR_INITIALIZATION_FAILED, .reserved = 0};
        const bool transported = bridge_call(gb::ioctl_set_device_memory_priority, &request, sizeof(request), &response, sizeof(response));
        if (!transported || response.vk_result != VK_SUCCESS)
        {
            std::array<char, 256> message{};
            std::snprintf(
                message.data(), message.size(),
                "[vulkan-shim] vkSetDeviceMemoryPriorityEXT failed: device=%llu memory=%llu priority=%.9g transported=%u result=%d\n",
                static_cast<unsigned long long>(request.device), static_cast<unsigned long long>(request.memory),
                static_cast<double>(priority), transported ? 1u : 0u, response.vk_result);
            OutputDebugStringA(message.data());
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks*)
    {
        if (!memory)
        {
            return;
        }
        const gb::object_id mem_id = to_object_id(memory);

        gb::free_memory_request request{};
        request.device = to_object_id(device);
        request.memory = mem_id;
        bridge_call(gb::ioctl_free_memory, &request, sizeof(request), nullptr, 0);

        {
            const std::lock_guard lock(g_memory_sizes_mutex);
            g_memory_sizes.erase(mem_id);
        }
        g_mapped_ranges.erase(mem_id);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                                                                     VkDeviceSize size, VkMemoryMapFlags, void** ppData)
    {
        const gb::object_id mem_id = to_object_id(memory);

        uint64_t actual = size;
        if (size == VK_WHOLE_SIZE)
        {
            const std::lock_guard lock(g_memory_sizes_mutex);
            const auto it = g_memory_sizes.find(mem_id);
            const uint64_t total = (it != g_memory_sizes.end()) ? it->second : 0;
            actual = (total > offset) ? (total - offset) : 0;
        }

        // Prefer aliasing the host mapping straight into the guest address space (coherent, no copy). The
        // bridge returns guest_address = 0 when that is not possible, in which case we fall back to staging.
        if (actual > 0)
        {
            gb::map_memory_direct_request request{};
            request.device = to_object_id(device);
            request.memory = mem_id;
            request.offset = offset;
            request.size = actual;

            gb::map_memory_direct_response response{};
            if (bridge_call(gb::ioctl_map_memory_direct, &request, sizeof(request), &response, sizeof(response)) &&
                response.vk_result == VK_SUCCESS && response.guest_address != 0)
            {
                g_direct_mapped.insert(mem_id);
                *ppData = reinterpret_cast<void*>(static_cast<uintptr_t>(response.guest_address));
                return VK_SUCCESS;
            }
        }

        mapped_range range{};
        range.offset = offset;
        range.size = actual;
        range.staging.resize(static_cast<size_t>(actual));

        if (actual > 0)
        {
            gb::download_memory_request request{};
            request.device = to_object_id(device);
            request.memory = mem_id;
            request.offset = offset;
            request.size = actual;
            if (!bridge_call(gb::ioctl_download_memory, &request, sizeof(request), range.staging.data(),
                             static_cast<DWORD>(range.staging.size())))
            {
                return VK_ERROR_MEMORY_MAP_FAILED;
            }
        }

        auto& stored = (g_mapped_ranges[mem_id] = std::move(range));
        *ppData = stored.staging.data();
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
    {
        const gb::object_id mem_id = to_object_id(memory);

        if (g_direct_mapped.erase(mem_id) != 0)
        {
            gb::unmap_memory_direct_request request{};
            request.device = to_object_id(device);
            request.memory = mem_id;
            bridge_call(gb::ioctl_unmap_memory_direct, &request, sizeof(request), nullptr, 0);
            return;
        }

        const auto it = g_mapped_ranges.find(mem_id);
        if (it == g_mapped_ranges.end())
        {
            return;
        }

        const auto& range = it->second;
        if (range.size > 0)
        {
            std::vector<uint8_t> message(sizeof(gb::upload_memory_request) + range.staging.size());
            gb::upload_memory_request header{};
            header.device = to_object_id(device);
            header.memory = mem_id;
            header.offset = range.offset;
            header.size = range.size;
            std::memcpy(message.data(), &header, sizeof(header));
            std::memcpy(message.data() + sizeof(header), range.staging.data(), range.staging.size());

            gb::result_response response{};
            bridge_call(gb::ioctl_upload_memory, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response));
        }

        g_mapped_ranges.erase(it);
    }

    // The bridge models a map as a private staging copy synced by download (map) / upload (unmap). DXVK may
    // instead keep a persistent mapping and explicitly flush, so honor flush by re-uploading the staging
    // copy and invalidate by re-downloading it. (Host-coherent memory makes these no-ops on a real driver.)
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
                                                                                   const VkMappedMemoryRange* pMemoryRanges)
    {
        for (uint32_t i = 0; i < memoryRangeCount; ++i)
        {
            const gb::object_id mem_id = to_object_id(pMemoryRanges[i].memory);
            if (g_direct_mapped.contains(mem_id))
            {
                gb::mapped_memory_range_request request{};
                request.device = to_object_id(device);
                request.memory = mem_id;
                request.offset = pMemoryRanges[i].offset;
                request.size = pMemoryRanges[i].size;

                gb::result_response response{};
                if (!bridge_call(gb::ioctl_flush_mapped_memory_direct, &request, sizeof(request), &response, sizeof(response)))
                {
                    return VK_ERROR_DEVICE_LOST;
                }
                if (response.vk_result != VK_SUCCESS)
                {
                    return static_cast<VkResult>(response.vk_result);
                }
                continue;
            }

            const auto it = g_mapped_ranges.find(mem_id);
            if (it == g_mapped_ranges.end() || it->second.staging.empty())
            {
                continue;
            }

            std::vector<uint8_t> message(sizeof(gb::upload_memory_request) + it->second.staging.size());
            gb::upload_memory_request header{};
            header.device = to_object_id(device);
            header.memory = mem_id;
            header.offset = it->second.offset;
            header.size = it->second.size;
            std::memcpy(message.data(), &header, sizeof(header));
            std::memcpy(message.data() + sizeof(header), it->second.staging.data(), it->second.staging.size());

            gb::result_response response{};
            bridge_call(gb::ioctl_upload_memory, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response));
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
                                                                                        const VkMappedMemoryRange* pMemoryRanges)
    {
        for (uint32_t i = 0; i < memoryRangeCount; ++i)
        {
            const gb::object_id mem_id = to_object_id(pMemoryRanges[i].memory);
            if (g_direct_mapped.contains(mem_id))
            {
                gb::mapped_memory_range_request request{};
                request.device = to_object_id(device);
                request.memory = mem_id;
                request.offset = pMemoryRanges[i].offset;
                request.size = pMemoryRanges[i].size;

                gb::result_response response{};
                if (!bridge_call(gb::ioctl_invalidate_mapped_memory_direct, &request, sizeof(request), &response, sizeof(response)))
                {
                    return VK_ERROR_DEVICE_LOST;
                }
                if (response.vk_result != VK_SUCCESS)
                {
                    return static_cast<VkResult>(response.vk_result);
                }
                continue;
            }

            const auto it = g_mapped_ranges.find(mem_id);
            if (it == g_mapped_ranges.end() || it->second.staging.empty())
            {
                continue;
            }

            gb::download_memory_request request{};
            request.device = to_object_id(device);
            request.memory = mem_id;
            request.offset = it->second.offset;
            request.size = it->second.size;
            bridge_call(gb::ioctl_download_memory, &request, sizeof(request), it->second.staging.data(),
                        static_cast<DWORD>(it->second.staging.size()));
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
                                                                        const VkAllocationCallbacks*, VkBuffer* pBuffer)
    {
        gb::create_buffer_request request{};
        request.device = to_object_id(device);
        request.size = pCreateInfo->size;
        request.usage = pCreateInfo->usage;

        gb::create_buffer_response response{};
        if (!bridge_call(gb::ioctl_create_buffer, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pBuffer = to_handle<VkBuffer>(response.buffer);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks*)
    {
        if (!buffer)
        {
            return;
        }
        gb::destroy_buffer_request request{};
        request.device = to_object_id(device);
        request.buffer = to_object_id(buffer);
        bridge_call(gb::ioctl_destroy_buffer, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateBufferView(VkDevice device, const VkBufferViewCreateInfo* pCreateInfo,
                                                                            const VkAllocationCallbacks*, VkBufferView* pView)
    {
        gb::create_buffer_view_request request{};
        request.device = to_object_id(device);
        request.buffer = to_object_id(pCreateInfo->buffer);
        request.format = static_cast<uint32_t>(pCreateInfo->format);
        request.offset = pCreateInfo->offset;
        request.range = pCreateInfo->range;

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_buffer_view, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pView = to_handle<VkBufferView>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyBufferView(VkDevice device, VkBufferView bufferView,
                                                                         const VkAllocationCallbacks*)
    {
        if (!bufferView)
        {
            return;
        }
        gb::device_child_request request{};
        request.device = to_object_id(device);
        request.object = to_object_id(bufferView);
        bridge_call(gb::ioctl_destroy_buffer_view, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo* pCreateInfo,
                                                                           const VkAllocationCallbacks*, VkQueryPool* pQueryPool)
    {
        gb::create_query_pool_request request{};
        request.device = to_object_id(device);
        request.query_type = static_cast<uint32_t>(pCreateInfo->queryType);
        request.query_count = pCreateInfo->queryCount;
        request.pipeline_statistics = static_cast<uint32_t>(pCreateInfo->pipelineStatistics);

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_query_pool, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pQueryPool = to_handle<VkQueryPool>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice device, VkQueryPool queryPool,
                                                                        const VkAllocationCallbacks*)
    {
        if (!queryPool)
        {
            return;
        }
        gb::device_child_request request{};
        request.device = to_object_id(device);
        request.object = to_object_id(queryPool);
        bridge_call(gb::ioctl_destroy_query_pool, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkResetQueryPool(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery,
                                                                      uint32_t queryCount)
    {
        gb::reset_query_pool_request request{};
        request.device = to_object_id(device);
        request.query_pool = to_object_id(queryPool);
        request.first_query = firstQuery;
        request.query_count = queryCount;

        gb::result_response response{};
        bridge_call(gb::ioctl_reset_query_pool, &request, sizeof(request), &response, sizeof(response));
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery,
                                                                               uint32_t queryCount, size_t dataSize, void* pData,
                                                                               VkDeviceSize stride, VkQueryResultFlags flags)
    {
        gb::get_query_pool_results_request request{};
        request.device = to_object_id(device);
        request.query_pool = to_object_id(queryPool);
        request.first_query = firstQuery;
        request.query_count = queryCount;
        request.data_size = static_cast<uint32_t>(dataSize);
        request.stride = static_cast<uint32_t>(stride);
        request.flags = static_cast<uint32_t>(flags);

        std::vector<uint8_t> out(sizeof(gb::get_query_pool_results_response) + dataSize);
        if (!bridge_call(gb::ioctl_get_query_pool_results, &request, sizeof(request), out.data(), static_cast<DWORD>(out.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        gb::get_query_pool_results_response response{};
        std::memcpy(&response, out.data(), sizeof(response));
        if (pData && dataSize > 0 && response.data_size > 0)
        {
            std::memcpy(pData, out.data() + sizeof(response), std::min<size_t>(dataSize, response.data_size));
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
                                                                         uint32_t firstQuery, uint32_t queryCount)
    {
        gb::cmd_reset_query_pool_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.query_pool = to_object_id(queryPool);
        request.first_query = firstQuery;
        request.query_count = queryCount;
        record_command(request.command_buffer, gb::command::cmd_reset_query_pool, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query,
                                                                     VkQueryControlFlags flags)
    {
        gb::cmd_begin_query_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.query_pool = to_object_id(queryPool);
        request.query = query;
        request.flags = static_cast<uint32_t>(flags);
        record_command(request.command_buffer, gb::command::cmd_begin_query, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL
    vkCmdBeginConditionalRenderingEXT(VkCommandBuffer commandBuffer, const VkConditionalRenderingBeginInfoEXT* pConditionalRenderingBegin)
    {
        if (!pConditionalRenderingBegin || pConditionalRenderingBegin->sType != VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT ||
            pConditionalRenderingBegin->pNext)
        {
            shim_log("vulkan-shim: invalid conditional rendering begin info\n");
            return;
        }
        gb::cmd_begin_conditional_rendering_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(pConditionalRenderingBegin->buffer);
        request.offset = pConditionalRenderingBegin->offset;
        request.flags = pConditionalRenderingBegin->flags;
        record_command(request.command_buffer, gb::command::cmd_begin_conditional_rendering, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndConditionalRenderingEXT(VkCommandBuffer commandBuffer)
    {
        gb::cmd_end_conditional_rendering_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        record_command(request.command_buffer, gb::command::cmd_end_conditional_rendering, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query)
    {
        gb::cmd_end_query_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.query_pool = to_object_id(queryPool);
        request.query = query;
        record_command(request.command_buffer, gb::command::cmd_end_query, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBeginQueryIndexedEXT(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
                                                                               uint32_t query, VkQueryControlFlags flags, uint32_t index)
    {
        gb::cmd_begin_query_indexed_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.query_pool = to_object_id(queryPool);
        request.query = query;
        request.flags = static_cast<uint32_t>(flags);
        request.index = index;
        record_command(request.command_buffer, gb::command::cmd_begin_query_indexed, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndQueryIndexedEXT(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
                                                                             uint32_t query, uint32_t index)
    {
        gb::cmd_end_query_indexed_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.query_pool = to_object_id(queryPool);
        request.query = query;
        request.index = index;
        record_command(request.command_buffer, gb::command::cmd_end_query_indexed, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindTransformFeedbackBuffersEXT(VkCommandBuffer commandBuffer,
                                                                                          uint32_t firstBinding, uint32_t bindingCount,
                                                                                          const VkBuffer* pBuffers,
                                                                                          const VkDeviceSize* pOffsets,
                                                                                          const VkDeviceSize* pSizes)
    {
        gb::cmd_bind_transform_feedback_buffers_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.first_binding = firstBinding;
        request.binding_count = bindingCount;

        std::vector<uint8_t> message(sizeof(request) + static_cast<size_t>(bindingCount) * sizeof(gb::transform_feedback_buffer_binding));
        std::memcpy(message.data(), &request, sizeof(request));
        size_t cursor = sizeof(request);
        for (uint32_t i = 0; i < bindingCount; ++i)
        {
            const gb::transform_feedback_buffer_binding binding{
                .buffer = to_object_id(pBuffers[i]), .offset = pOffsets[i], .size = pSizes ? pSizes[i] : VK_WHOLE_SIZE};
            std::memcpy(message.data() + cursor, &binding, sizeof(binding));
            cursor += sizeof(binding);
        }
        record_command(request.command_buffer, gb::command::cmd_bind_transform_feedback_buffers, message.data(), message.size());
    }

    static void record_transform_feedback_command(gb::command command, VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer,
                                                  uint32_t counterBufferCount, const VkBuffer* pCounterBuffers,
                                                  const VkDeviceSize* pCounterBufferOffsets)
    {
        gb::cmd_transform_feedback_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.first_counter_buffer = firstCounterBuffer;
        request.counter_buffer_count = counterBufferCount;
        request.has_counter_buffers = pCounterBuffers ? 1u : 0u;
        request.has_counter_buffer_offsets = pCounterBufferOffsets ? 1u : 0u;

        std::vector<uint8_t> message(sizeof(request) +
                                     static_cast<size_t>(counterBufferCount) * sizeof(gb::transform_feedback_counter_buffer));
        std::memcpy(message.data(), &request, sizeof(request));
        size_t cursor = sizeof(request);
        for (uint32_t i = 0; i < counterBufferCount; ++i)
        {
            const gb::transform_feedback_counter_buffer counter{.buffer =
                                                                    pCounterBuffers ? to_object_id(pCounterBuffers[i]) : gb::null_object,
                                                                .offset = pCounterBufferOffsets ? pCounterBufferOffsets[i] : 0};
            std::memcpy(message.data() + cursor, &counter, sizeof(counter));
            cursor += sizeof(counter);
        }
        record_command(request.command_buffer, command, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBeginTransformFeedbackEXT(VkCommandBuffer commandBuffer,
                                                                                    uint32_t firstCounterBuffer,
                                                                                    uint32_t counterBufferCount,
                                                                                    const VkBuffer* pCounterBuffers,
                                                                                    const VkDeviceSize* pCounterBufferOffsets)
    {
        record_transform_feedback_command(gb::command::cmd_begin_transform_feedback, commandBuffer, firstCounterBuffer, counterBufferCount,
                                          pCounterBuffers, pCounterBufferOffsets);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndTransformFeedbackEXT(VkCommandBuffer commandBuffer,
                                                                                  uint32_t firstCounterBuffer, uint32_t counterBufferCount,
                                                                                  const VkBuffer* pCounterBuffers,
                                                                                  const VkDeviceSize* pCounterBufferOffsets)
    {
        record_transform_feedback_command(gb::command::cmd_end_transform_feedback, commandBuffer, firstCounterBuffer, counterBufferCount,
                                          pCounterBuffers, pCounterBufferOffsets);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectByteCountEXT(VkCommandBuffer commandBuffer, uint32_t instanceCount,
                                                                                   uint32_t firstInstance, VkBuffer counterBuffer,
                                                                                   VkDeviceSize counterBufferOffset, uint32_t counterOffset,
                                                                                   uint32_t vertexStride)
    {
        gb::cmd_draw_indirect_byte_count_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.counter_buffer = to_object_id(counterBuffer);
        request.counter_buffer_offset = counterBufferOffset;
        request.counter_offset = counterOffset;
        request.vertex_stride = vertexStride;
        request.instance_count = instanceCount;
        request.first_instance = firstInstance;
        record_command(request.command_buffer, gb::command::cmd_draw_indirect_byte_count, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer commandBuffer,
                                                                         VkPipelineStageFlagBits pipelineStage, VkQueryPool queryPool,
                                                                         uint32_t query)
    {
        gb::cmd_write_timestamp_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.query_pool = to_object_id(queryPool);
        request.query = query;
        request.pipeline_stage = static_cast<uint32_t>(pipelineStage);
        record_command(request.command_buffer, gb::command::cmd_write_timestamp, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
                                                                               uint32_t firstQuery, uint32_t queryCount, VkBuffer dstBuffer,
                                                                               VkDeviceSize dstOffset, VkDeviceSize stride,
                                                                               VkQueryResultFlags flags)
    {
        gb::cmd_copy_query_pool_results_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.query_pool = to_object_id(queryPool);
        request.first_query = firstQuery;
        request.query_count = queryCount;
        request.destination_buffer = to_object_id(dstBuffer);
        request.destination_offset = dstOffset;
        request.stride = stride;
        request.flags = static_cast<uint32_t>(flags);
        record_command(request.command_buffer, gb::command::cmd_copy_query_pool_results, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2(VkCommandBuffer commandBuffer,
                                                                          VkPipelineStageFlags2 pipelineStage, VkQueryPool queryPool,
                                                                          uint32_t query)
    {
        gb::cmd_write_timestamp2_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.query_pool = to_object_id(queryPool);
        request.query = query;
        request.pipeline_stage = static_cast<uint64_t>(pipelineStage);
        record_command(request.command_buffer, gb::command::cmd_write_timestamp2, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2KHR(VkCommandBuffer commandBuffer,
                                                                             VkPipelineStageFlags2 pipelineStage, VkQueryPool queryPool,
                                                                             uint32_t query)
    {
        vkCmdWriteTimestamp2(commandBuffer, pipelineStage, queryPool, query);
    }

    // --- Extended-dynamic-state setters: record into the command stream, replayed on the host. ---

    static void record_set_dynamic_u32(VkCommandBuffer commandBuffer, gb::dynamic_state_u32 state, uint32_t value)
    {
        gb::cmd_set_dynamic_u32_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.state = static_cast<uint32_t>(state);
        request.value = value;
        record_command(request.command_buffer, gb::command::cmd_set_dynamic_u32, &request, sizeof(request));
    }

    static void record_set_stencil(VkCommandBuffer commandBuffer, gb::stencil_dynamic_state which, VkStencilFaceFlags faceMask,
                                   uint32_t value)
    {
        gb::cmd_set_stencil_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.which = static_cast<uint32_t>(which);
        request.face_mask = static_cast<uint32_t>(faceMask);
        request.value = value;
        record_command(request.command_buffer, gb::command::cmd_set_stencil, &request, sizeof(request));
    }

    static void record_set_viewport(VkCommandBuffer commandBuffer, uint32_t first, uint32_t count, const VkViewport* pViewports,
                                    bool with_count)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_set_viewport_request) + static_cast<size_t>(count) * sizeof(gb::viewport_entry));
        gb::cmd_set_viewport_request header{};
        header.command_buffer = command_buffer;
        header.first = first;
        header.count = count;
        header.with_count = with_count ? 1u : 0u;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < count; ++i)
        {
            const gb::viewport_entry entry{
                .x = pViewports[i].x,
                .y = pViewports[i].y,
                .width = pViewports[i].width,
                .height = pViewports[i].height,
                .min_depth = pViewports[i].minDepth,
                .max_depth = pViewports[i].maxDepth,
            };
            std::memcpy(message.data() + sizeof(header) + static_cast<size_t>(i) * sizeof(entry), &entry, sizeof(entry));
        }
        record_command(command_buffer, gb::command::cmd_set_viewport, message.data(), message.size());
    }

    static void record_set_scissor(VkCommandBuffer commandBuffer, uint32_t first, uint32_t count, const VkRect2D* pScissors,
                                   bool with_count)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_set_scissor_request) + static_cast<size_t>(count) * sizeof(gb::scissor_entry));
        gb::cmd_set_scissor_request header{};
        header.command_buffer = command_buffer;
        header.first = first;
        header.count = count;
        header.with_count = with_count ? 1u : 0u;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < count; ++i)
        {
            const gb::scissor_entry entry{
                .offset_x = pScissors[i].offset.x,
                .offset_y = pScissors[i].offset.y,
                .width = pScissors[i].extent.width,
                .height = pScissors[i].extent.height,
            };
            std::memcpy(message.data() + sizeof(header) + static_cast<size_t>(i) * sizeof(entry), &entry, sizeof(entry));
        }
        record_command(command_buffer, gb::command::cmd_set_scissor, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport,
                                                                      uint32_t viewportCount, const VkViewport* pViewports)
    {
        record_set_viewport(commandBuffer, firstViewport, viewportCount, pViewports, false);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWithCount(VkCommandBuffer commandBuffer, uint32_t viewportCount,
                                                                               const VkViewport* pViewports)
    {
        record_set_viewport(commandBuffer, 0, viewportCount, pViewports, true);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor,
                                                                     uint32_t scissorCount, const VkRect2D* pScissors)
    {
        record_set_scissor(commandBuffer, firstScissor, scissorCount, pScissors, false);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetScissorWithCount(VkCommandBuffer commandBuffer, uint32_t scissorCount,
                                                                              const VkRect2D* pScissors)
    {
        record_set_scissor(commandBuffer, 0, scissorCount, pScissors, true);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer commandBuffer, float depthBiasConstantFactor,
                                                                       float depthBiasClamp, float depthBiasSlopeFactor)
    {
        gb::cmd_set_depth_bias_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.constant_factor = depthBiasConstantFactor;
        request.clamp = depthBiasClamp;
        request.slope_factor = depthBiasSlopeFactor;
        record_command(request.command_buffer, gb::command::cmd_set_depth_bias, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4])
    {
        gb::cmd_set_blend_constants_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.constants[0] = blendConstants[0];
        request.constants[1] = blendConstants[1];
        request.constants[2] = blendConstants[2];
        request.constants[3] = blendConstants[3];
        record_command(request.command_buffer, gb::command::cmd_set_blend_constants, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer commandBuffer, float minDepthBounds,
                                                                         float maxDepthBounds)
    {
        gb::cmd_set_depth_bounds_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.min_depth_bounds = minDepthBounds;
        request.max_depth_bounds = maxDepthBounds;
        record_command(request.command_buffer, gb::command::cmd_set_depth_bounds, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
    {
        gb::cmd_set_line_width_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.line_width = lineWidth;
        record_command(request.command_buffer, gb::command::cmd_set_line_width, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                                                                                uint32_t compareMask)
    {
        record_set_stencil(commandBuffer, gb::stencil_dynamic_state::compare_mask, faceMask, compareMask);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                                                                              uint32_t writeMask)
    {
        record_set_stencil(commandBuffer, gb::stencil_dynamic_state::write_mask, faceMask, writeMask);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                                                                              uint32_t reference)
    {
        record_set_stencil(commandBuffer, gb::stencil_dynamic_state::reference, faceMask, reference);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOp(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                                                                       VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp,
                                                                       VkCompareOp compareOp)
    {
        gb::cmd_set_stencil_op_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.face_mask = static_cast<uint32_t>(faceMask);
        request.fail_op = static_cast<uint32_t>(failOp);
        request.pass_op = static_cast<uint32_t>(passOp);
        request.depth_fail_op = static_cast<uint32_t>(depthFailOp);
        request.compare_op = static_cast<uint32_t>(compareOp);
        record_command(request.command_buffer, gb::command::cmd_set_stencil_op, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetAttachmentFeedbackLoopEnableEXT(VkCommandBuffer commandBuffer,
                                                                                             VkImageAspectFlags value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::attachment_feedback_loop,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetCoverageModulationModeNV(VkCommandBuffer commandBuffer,
                                                                                      VkCoverageModulationModeNV value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::coverage_modulation_mode,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetCoverageModulationTableEnableNV(VkCommandBuffer commandBuffer, VkBool32 value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::coverage_table_enable,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetCoverageReductionModeNV(VkCommandBuffer commandBuffer,
                                                                                     VkCoverageReductionModeNV value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::coverage_reduction_mode,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetCoverageToColorEnableNV(VkCommandBuffer commandBuffer, VkBool32 value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::coverage_to_color_enable,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetCoverageToColorLocationNV(VkCommandBuffer commandBuffer, uint32_t value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::coverage_to_color_location,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthClipNegativeOneToOneEXT(VkCommandBuffer commandBuffer, VkBool32 value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::depth_clip_negative_one,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDiscardRectangleEnableEXT(VkCommandBuffer commandBuffer, VkBool32 value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::discard_rectangle_enable,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDiscardRectangleModeEXT(VkCommandBuffer commandBuffer,
                                                                                     VkDiscardRectangleModeEXT value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::discard_rectangle_mode,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetLineStippleEnableEXT(VkCommandBuffer commandBuffer, VkBool32 value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::line_stipple_enable,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetLogicOpEXT(VkCommandBuffer commandBuffer, VkLogicOp value)
    {
        const gb::dynamic_request request{
            .command_buffer = to_object_id(commandBuffer), .kind = gb::dynamic_command::logic_op, .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetPatchControlPointsEXT(VkCommandBuffer commandBuffer, uint32_t value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::patch_control_points,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartIndexEXT(VkCommandBuffer commandBuffer, uint32_t value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::primitive_restart_index,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetProvokingVertexModeEXT(VkCommandBuffer commandBuffer,
                                                                                    VkProvokingVertexModeEXT value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::provoking_vertex_mode,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetRayTracingPipelineStackSizeKHR(VkCommandBuffer commandBuffer, uint32_t value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::ray_tracing_stack_size,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetRepresentativeFragmentTestEnableNV(VkCommandBuffer commandBuffer,
                                                                                                VkBool32 value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::representative_fragment_test,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetShadingRateImageEnableNV(VkCommandBuffer commandBuffer, VkBool32 value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::shading_rate_image_enable,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWScalingEnableNV(VkCommandBuffer commandBuffer, VkBool32 value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::viewport_w_scaling_enable,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDeviceMask(VkCommandBuffer commandBuffer, uint32_t value)
    {
        const gb::dynamic_request request{
            .command_buffer = to_object_id(commandBuffer), .kind = gb::dynamic_command::device_mask, .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendAdvancedEXT(VkCommandBuffer commandBuffer, uint32_t first,
                                                                                   uint32_t count, const VkColorBlendAdvancedEXT* values)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::color_blend_advanced,
                                          .first = first,
                                          .count = count};
        record_extended_dynamic(request, values, sizeof(VkColorBlendAdvancedEXT));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetColorWriteEnableEXT(VkCommandBuffer commandBuffer, uint32_t count,
                                                                                 const VkBool32* values)
    {
        const gb::dynamic_request request{
            .command_buffer = to_object_id(commandBuffer), .kind = gb::dynamic_command::color_write_enable, .count = count};
        record_extended_dynamic(request, values, sizeof(VkBool32));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetCoverageModulationTableNV(VkCommandBuffer commandBuffer, uint32_t count,
                                                                                       const float* values)
    {
        const gb::dynamic_request request{
            .command_buffer = to_object_id(commandBuffer), .kind = gb::dynamic_command::coverage_modulation_table, .count = count};
        record_extended_dynamic(request, values, sizeof(float));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDiscardRectangleEXT(VkCommandBuffer commandBuffer, uint32_t first,
                                                                                 uint32_t count, const VkRect2D* values)
    {
        const gb::dynamic_request request{
            .command_buffer = to_object_id(commandBuffer), .kind = gb::dynamic_command::discard_rectangles, .first = first, .count = count};
        record_extended_dynamic(request, values, sizeof(VkRect2D));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetExclusiveScissorEnableNV(VkCommandBuffer commandBuffer, uint32_t first,
                                                                                      uint32_t count, const VkBool32* values)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::exclusive_scissor_enable,
                                          .first = first,
                                          .count = count};
        record_extended_dynamic(request, values, sizeof(VkBool32));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetExclusiveScissorNV(VkCommandBuffer commandBuffer, uint32_t first,
                                                                                uint32_t count, const VkRect2D* values)
    {
        const gb::dynamic_request request{
            .command_buffer = to_object_id(commandBuffer), .kind = gb::dynamic_command::exclusive_scissors, .first = first, .count = count};
        record_extended_dynamic(request, values, sizeof(VkRect2D));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportSwizzleNV(VkCommandBuffer commandBuffer, uint32_t first,
                                                                               uint32_t count, const VkViewportSwizzleNV* values)
    {
        const gb::dynamic_request request{
            .command_buffer = to_object_id(commandBuffer), .kind = gb::dynamic_command::viewport_swizzles, .first = first, .count = count};
        record_extended_dynamic(request, values, sizeof(VkViewportSwizzleNV));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWScalingNV(VkCommandBuffer commandBuffer, uint32_t first,
                                                                                uint32_t count, const VkViewportWScalingNV* values)
    {
        const gb::dynamic_request request{
            .command_buffer = to_object_id(commandBuffer), .kind = gb::dynamic_command::viewport_w_scaling, .first = first, .count = count};
        record_extended_dynamic(request, values, sizeof(VkViewportWScalingNV));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetLineStipple(VkCommandBuffer commandBuffer, uint32_t factor, uint16_t pattern)
    {
        const gb::dynamic_request request{
            .command_buffer = to_object_id(commandBuffer), .kind = gb::dynamic_command::line_stipple, .first = pattern, .value = factor};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetLineStippleKHR(VkCommandBuffer commandBuffer, uint32_t factor,
                                                                            uint16_t pattern)
    {
        vkCmdSetLineStipple(commandBuffer, factor, pattern);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetLineStippleEXT(VkCommandBuffer commandBuffer, uint32_t factor,
                                                                            uint16_t pattern)
    {
        vkCmdSetLineStipple(commandBuffer, factor, pattern);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDeviceMaskKHR(VkCommandBuffer commandBuffer, uint32_t deviceMask)
    {
        vkCmdSetDeviceMask(commandBuffer, deviceMask);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetFragmentShadingRateKHR(VkCommandBuffer commandBuffer,
                                                                                    const VkExtent2D* pFragmentSize,
                                                                                    const VkFragmentShadingRateCombinerOpKHR combinerOps[2])
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::fragment_shading_rate,
                                          .count = 2,
                                          .width = pFragmentSize ? pFragmentSize->width : 0,
                                          .height = pFragmentSize ? pFragmentSize->height : 0,
                                          .error = pFragmentSize ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED};
        record_extended_dynamic(request, combinerOps, sizeof(VkFragmentShadingRateCombinerOpKHR));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetFragmentShadingRateEnumNV(
        VkCommandBuffer commandBuffer, VkFragmentShadingRateNV shadingRate, const VkFragmentShadingRateCombinerOpKHR combinerOps[2])
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::fragment_shading_rate_enum,
                                          .count = 2,
                                          .value = static_cast<uint32_t>(shadingRate)};
        record_extended_dynamic(request, combinerOps, sizeof(VkFragmentShadingRateCombinerOpKHR));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthClampRangeEXT(VkCommandBuffer commandBuffer, VkDepthClampModeEXT mode,
                                                                                const VkDepthClampRangeEXT* range)
    {
        // Vulkan ignores pDepthClampRange unless the user-defined range mode is selected.
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::depth_clamp_range,
                                          .count = mode == VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT ? 1u : 0u,
                                          .value = static_cast<uint32_t>(mode)};
        record_extended_dynamic(request, range, sizeof(VkDepthClampRangeEXT));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetAlphaToCoverageEnableEXT(VkCommandBuffer commandBuffer, VkBool32 value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::alpha_to_coverage,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetAlphaToOneEnableEXT(VkCommandBuffer commandBuffer, VkBool32 value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::alpha_to_one,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthClampEnableEXT(VkCommandBuffer commandBuffer, VkBool32 value)
    {
        const gb::dynamic_request request{
            .command_buffer = to_object_id(commandBuffer), .kind = gb::dynamic_command::depth_clamp, .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetLogicOpEnableEXT(VkCommandBuffer commandBuffer, VkBool32 value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::logic_op_enable,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetPolygonModeEXT(VkCommandBuffer commandBuffer, VkPolygonMode value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::polygon_mode,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizationSamplesEXT(VkCommandBuffer commandBuffer,
                                                                                     VkSampleCountFlagBits value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::rasterization_samples,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizationStreamEXT(VkCommandBuffer commandBuffer, uint32_t value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::rasterization_stream,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetConservativeRasterizationModeEXT(VkCommandBuffer commandBuffer,
                                                                                              VkConservativeRasterizationModeEXT value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::conservative_rasterization,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetSampleLocationsEnableEXT(VkCommandBuffer commandBuffer, VkBool32 value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::sample_locations_enable,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetLineRasterizationModeEXT(VkCommandBuffer commandBuffer,
                                                                                      VkLineRasterizationModeEXT value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::line_rasterization,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetTessellationDomainOriginEXT(VkCommandBuffer commandBuffer,
                                                                                         VkTessellationDomainOrigin value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::tessellation_domain,
                                          .value = static_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetExtraPrimitiveOverestimationSizeEXT(VkCommandBuffer commandBuffer, float value)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::extra_overestimation,
                                          .value = std::bit_cast<uint32_t>(value)};
        record_extended_dynamic(request, nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendEnableEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment,
                                                                                 uint32_t attachmentCount, const VkBool32* values)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::color_blend_enable,
                                          .first = firstAttachment,
                                          .count = attachmentCount};
        record_extended_dynamic(request, values, sizeof(VkBool32));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendEquationEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment,
                                                                                   uint32_t attachmentCount,
                                                                                   const VkColorBlendEquationEXT* values)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::color_blend_equation,
                                          .first = firstAttachment,
                                          .count = attachmentCount};
        record_extended_dynamic(request, values, sizeof(VkColorBlendEquationEXT));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetColorWriteMaskEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment,
                                                                               uint32_t attachmentCount,
                                                                               const VkColorComponentFlags* values)
    {
        const gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                          .kind = gb::dynamic_command::color_write_mask,
                                          .first = firstAttachment,
                                          .count = attachmentCount};
        record_extended_dynamic(request, values, sizeof(VkColorComponentFlags));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetSampleMaskEXT(VkCommandBuffer commandBuffer, VkSampleCountFlagBits samples,
                                                                           const VkSampleMask* pSampleMask)
    {
        gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer),
                                    .kind = gb::dynamic_command::sample_mask,
                                    .count = (static_cast<uint32_t>(samples) + 31) / 32,
                                    .value = static_cast<uint32_t>(samples)};
        if (!gb::valid_sample_count(request.value))
        {
            request.error = VK_ERROR_INITIALIZATION_FAILED;
        }
        // maintenance10 allows NULL to mean all bits set. An explicit mask has identical command state
        // and keeps the wire independent of the host's maintenance10 support.
        const VkSampleMask all_samples[] = {UINT32_MAX, UINT32_MAX};
        record_extended_dynamic(request, pSampleMask ? pSampleMask : all_samples, sizeof(VkSampleMask));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMultisamplePropertiesEXT(
        VkPhysicalDevice physicalDevice, VkSampleCountFlagBits samples, VkMultisamplePropertiesEXT* pMultisampleProperties)
    {
        if (!pMultisampleProperties || pMultisampleProperties->sType != VK_STRUCTURE_TYPE_MULTISAMPLE_PROPERTIES_EXT ||
            pMultisampleProperties->pNext)
        {
            shim_log("vulkan-shim: vkGetPhysicalDeviceMultisamplePropertiesEXT invalid output structure or unsupported pNext\n");
            return;
        }
        const gb::get_multisample_properties_request request{
            .physical_device = to_object_id(physicalDevice), .samples = static_cast<uint32_t>(samples), .reserved = 0};
        gb::get_multisample_properties_response response{};
        if (!bridge_call(gb::ioctl_get_multisample_properties, &request, sizeof(request), &response, sizeof(response)) ||
            response.vk_result != VK_SUCCESS)
        {
            shim_log("vulkan-shim: vkGetPhysicalDeviceMultisamplePropertiesEXT bridge/driver failure\n");
            return;
        }
        pMultisampleProperties->maxSampleLocationGridSize = {.width = response.width, .height = response.height};
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetSampleLocationsEXT(VkCommandBuffer commandBuffer,
                                                                                const VkSampleLocationsInfoEXT* pSampleLocationsInfo)
    {
        gb::dynamic_request request{.command_buffer = to_object_id(commandBuffer), .kind = gb::dynamic_command::sample_locations};
        if (!pSampleLocationsInfo || pSampleLocationsInfo->sType != VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT)
        {
            request.error = VK_ERROR_INITIALIZATION_FAILED;
            record_extended_dynamic(request, nullptr, 0);
            return;
        }
        // The pinned registry defines no structures extending VkSampleLocationsInfoEXT. Preserve an
        // explicit error for an unknown chain instead of silently changing future extension semantics.
        if (pSampleLocationsInfo->pNext)
        {
            request.error = VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        request.value = static_cast<uint32_t>(pSampleLocationsInfo->sampleLocationsPerPixel);
        request.width = pSampleLocationsInfo->sampleLocationGridSize.width;
        request.height = pSampleLocationsInfo->sampleLocationGridSize.height;
        request.count = pSampleLocationsInfo->sampleLocationsCount;
        record_extended_dynamic(request, pSampleLocationsInfo->pSampleLocations, sizeof(VkSampleLocationEXT));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::cull_mode, static_cast<uint32_t>(cullMode));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::front_face, static_cast<uint32_t>(frontFace));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer commandBuffer,
                                                                               VkPrimitiveTopology primitiveTopology)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::primitive_topology, static_cast<uint32_t>(primitiveTopology));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::depth_test_enable, depthTestEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::depth_write_enable, depthWriteEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::depth_compare_op, static_cast<uint32_t>(depthCompareOp));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer,
                                                                                   VkBool32 depthBoundsTestEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::depth_bounds_test_enable, depthBoundsTestEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthClipEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthClipEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::depth_clip_enable, depthClipEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::stencil_test_enable, stencilTestEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnable(VkCommandBuffer commandBuffer,
                                                                                     VkBool32 rasterizerDiscardEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::rasterizer_discard_enable, rasterizerDiscardEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnable(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::depth_bias_enable, depthBiasEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnable(VkCommandBuffer commandBuffer,
                                                                                    VkBool32 primitiveRestartEnable)
    {
        record_set_dynamic_u32(commandBuffer, gb::dynamic_state_u32::primitive_restart_enable, primitiveRestartEnable);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
                                                                     uint32_t regionCount, const VkBufferCopy* pRegions)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_copy_buffer_request) +
                                     static_cast<size_t>(regionCount) * sizeof(gb::buffer_copy_region));
        gb::cmd_copy_buffer_request header{};
        header.command_buffer = command_buffer;
        header.src_buffer = to_object_id(srcBuffer);
        header.dst_buffer = to_object_id(dstBuffer);
        header.region_count = regionCount;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < regionCount; ++i)
        {
            gb::buffer_copy_region region{};
            region.src_offset = pRegions[i].srcOffset;
            region.dst_offset = pRegions[i].dstOffset;
            region.size = pRegions[i].size;
            std::memcpy(message.data() + sizeof(header) + i * sizeof(region), &region, sizeof(region));
        }
        record_command(command_buffer, gb::command::cmd_copy_buffer, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer2(VkCommandBuffer commandBuffer,
                                                                      const VkCopyBufferInfo2* pCopyBufferInfo)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        const uint32_t regionCount = pCopyBufferInfo->regionCount;
        std::vector<uint8_t> message(sizeof(gb::cmd_copy_buffer_request) +
                                     static_cast<size_t>(regionCount) * sizeof(gb::buffer_copy_region));
        gb::cmd_copy_buffer_request header{};
        header.command_buffer = command_buffer;
        header.src_buffer = to_object_id(pCopyBufferInfo->srcBuffer);
        header.dst_buffer = to_object_id(pCopyBufferInfo->dstBuffer);
        header.region_count = regionCount;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < regionCount; ++i)
        {
            gb::buffer_copy_region region{};
            region.src_offset = pCopyBufferInfo->pRegions[i].srcOffset;
            region.dst_offset = pCopyBufferInfo->pRegions[i].dstOffset;
            region.size = pCopyBufferInfo->pRegions[i].size;
            std::memcpy(message.data() + sizeof(header) + i * sizeof(region), &region, sizeof(region));
        }
        record_command(command_buffer, gb::command::cmd_copy_buffer, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                                                                   VkMemoryRequirements* pMemoryRequirements)
    {
        if (!pMemoryRequirements)
        {
            return;
        }
        *pMemoryRequirements = {};

        gb::get_buffer_memory_requirements_request request{};
        request.device = to_object_id(device);
        request.buffer = to_object_id(buffer);

        gb::memory_requirements_response response{};
        if (!bridge_call(gb::ioctl_get_buffer_memory_requirements, &request, sizeof(request), &response, sizeof(response)))
        {
            return;
        }

        pMemoryRequirements->size = response.size;
        pMemoryRequirements->alignment = response.alignment;
        pMemoryRequirements->memoryTypeBits = response.memory_type_bits;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(VkDevice device,
                                                                                    const VkBufferMemoryRequirementsInfo2* pInfo,
                                                                                    VkMemoryRequirements2* pMemoryRequirements)
    {
        if (!pInfo || !pMemoryRequirements)
        {
            return;
        }
        vkGetBufferMemoryRequirements(device, pInfo->buffer, &pMemoryRequirements->memoryRequirements);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                                                            VkDeviceSize memoryOffset)
    {
        gb::bind_buffer_memory_request request{};
        request.device = to_object_id(device);
        request.buffer = to_object_id(buffer);
        request.memory = to_object_id(memory);
        request.offset = memoryOffset;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_bind_buffer_memory, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer,
                                                                     VkDeviceSize dstOffset, VkDeviceSize size, uint32_t data)
    {
        gb::cmd_fill_buffer_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(dstBuffer);
        request.offset = dstOffset;
        request.size = size;
        request.data = data;
        record_command(request.command_buffer, gb::command::cmd_fill_buffer, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdWriteBufferMarkerAMD(VkCommandBuffer commandBuffer,
                                                                               VkPipelineStageFlagBits pipelineStage, VkBuffer dstBuffer,
                                                                               VkDeviceSize dstOffset, uint32_t marker)
    {
        gb::cmd_write_buffer_marker_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(dstBuffer);
        request.offset = dstOffset;
        request.stage = static_cast<uint64_t>(pipelineStage);
        request.marker = marker;
        record_command(request.command_buffer, gb::command::cmd_write_buffer_marker, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdWriteBufferMarker2AMD(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage,
                                                                                VkBuffer dstBuffer, VkDeviceSize dstOffset, uint32_t marker)
    {
        gb::cmd_write_buffer_marker_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(dstBuffer);
        request.offset = dstOffset;
        request.stage = stage;
        request.marker = marker;
        request.variant = 1;
        record_command(request.command_buffer, gb::command::cmd_write_buffer_marker, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo,
                                                                       const VkAllocationCallbacks*, VkImage* pImage)
    {
        gb::create_image_request request{};
        request.device = to_object_id(device);
        request.format = static_cast<uint32_t>(pCreateInfo->format);
        request.width = pCreateInfo->extent.width;
        request.height = pCreateInfo->extent.height;
        request.usage = pCreateInfo->usage;
        request.tiling = static_cast<uint32_t>(pCreateInfo->tiling);
        request.samples = static_cast<uint32_t>(pCreateInfo->samples);
        request.image_type = static_cast<uint32_t>(pCreateInfo->imageType);
        request.depth = pCreateInfo->extent.depth;
        request.mip_levels = pCreateInfo->mipLevels;
        request.array_layers = pCreateInfo->arrayLayers;
        request.flags = pCreateInfo->flags;

        gb::create_image_response response{};
        if (!bridge_call(gb::ioctl_create_image, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pImage = to_handle<VkImage>(response.image);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks*)
    {
        if (!image)
        {
            return;
        }
        gb::destroy_image_request request{};
        request.device = to_object_id(device);
        request.image = to_object_id(image);
        bridge_call(gb::ioctl_destroy_image, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image,
                                                                                  VkMemoryRequirements* pMemoryRequirements)
    {
        if (!pMemoryRequirements)
        {
            return;
        }
        *pMemoryRequirements = {};

        gb::get_image_memory_requirements_request request{};
        request.device = to_object_id(device);
        request.image = to_object_id(image);

        gb::memory_requirements_response response{};
        if (!bridge_call(gb::ioctl_get_image_memory_requirements, &request, sizeof(request), &response, sizeof(response)))
        {
            return;
        }

        pMemoryRequirements->size = response.size;
        pMemoryRequirements->alignment = response.alignment;
        pMemoryRequirements->memoryTypeBits = response.memory_type_bits;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice device, VkImage image,
                                                                                 const VkImageSubresource* pSubresource,
                                                                                 VkSubresourceLayout* pLayout)
    {
        if (!pLayout)
        {
            return;
        }
        *pLayout = {};

        gb::get_image_subresource_layout_request request{};
        request.device = to_object_id(device);
        request.image = to_object_id(image);
        if (pSubresource)
        {
            request.aspect_mask = pSubresource->aspectMask;
            request.mip_level = pSubresource->mipLevel;
            request.array_layer = pSubresource->arrayLayer;
        }

        gb::get_image_subresource_layout_response response{};
        if (!bridge_call(gb::ioctl_get_image_subresource_layout, &request, sizeof(request), &response, sizeof(response)))
        {
            return;
        }

        pLayout->offset = response.offset;
        pLayout->size = response.size;
        pLayout->rowPitch = response.row_pitch;
        pLayout->arrayPitch = response.array_pitch;
        pLayout->depthPitch = response.depth_pitch;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout2KHR(VkDevice device, VkImage image,
                                                                                     const VkImageSubresource2KHR* pSubresource,
                                                                                     VkSubresourceLayout2KHR* pLayout)
    {
        if (!pSubresource || !pLayout)
        {
            return;
        }
        vkGetImageSubresourceLayout(device, image, &pSubresource->imageSubresource, &pLayout->subresourceLayout);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2(VkDevice device,
                                                                                   const VkImageMemoryRequirementsInfo2* pInfo,
                                                                                   VkMemoryRequirements2* pMemoryRequirements)
    {
        if (!pInfo || !pMemoryRequirements)
        {
            return;
        }
        vkGetImageMemoryRequirements(device, pInfo->image, &pMemoryRequirements->memoryRequirements);
    }

    // Vulkan 1.3 (maintenance4) lets callers query memory requirements without a live object. The bridge
    // has no equivalent query, so probe a throwaway object created from the supplied create-info. DXVK's
    // memory allocator relies on these during device construction.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetDeviceBufferMemoryRequirements(VkDevice device,
                                                                                         const VkDeviceBufferMemoryRequirements* pInfo,
                                                                                         VkMemoryRequirements2* pMemoryRequirements)
    {
        if (!pInfo || !pInfo->pCreateInfo || !pMemoryRequirements)
        {
            return;
        }
        VkBuffer buffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(device, pInfo->pCreateInfo, nullptr, &buffer) != VK_SUCCESS)
        {
            return;
        }
        vkGetBufferMemoryRequirements(device, buffer, &pMemoryRequirements->memoryRequirements);
        vkDestroyBuffer(device, buffer, nullptr);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirements(VkDevice device,
                                                                                        const VkDeviceImageMemoryRequirements* pInfo,
                                                                                        VkMemoryRequirements2* pMemoryRequirements)
    {
        if (!pInfo || !pInfo->pCreateInfo || !pMemoryRequirements)
        {
            return;
        }
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(device, pInfo->pCreateInfo, nullptr, &image) != VK_SUCCESS)
        {
            return;
        }
        vkGetImageMemoryRequirements(device, image, &pMemoryRequirements->memoryRequirements);
        vkDestroyImage(device, image, nullptr);
    }

    // VK_KHR_maintenance5 / core 1.4: query an image's subresource layout straight from its create-info,
    // with no live VkImage. The bridge has no such query, so probe a throwaway image (same approach as
    // vkGetDeviceImageMemoryRequirements above). DXVK calls this during resource setup.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSubresourceLayoutKHR(VkDevice device,
                                                                                          const VkDeviceImageSubresourceInfo* pInfo,
                                                                                          VkSubresourceLayout2* pLayout)
    {
        if (!pInfo || !pInfo->pCreateInfo || !pInfo->pSubresource || !pLayout)
        {
            return;
        }
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(device, pInfo->pCreateInfo, nullptr, &image) != VK_SUCCESS)
        {
            return;
        }
        vkGetImageSubresourceLayout(device, image, &pInfo->pSubresource->imageSubresource, &pLayout->subresourceLayout);
        vkDestroyImage(device, image, nullptr);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                                                                           VkDeviceSize memoryOffset)
    {
        gb::bind_image_memory_request request{};
        request.device = to_object_id(device);
        request.image = to_object_id(image);
        request.memory = to_object_id(memory);
        request.offset = memoryOffset;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_bind_image_memory, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    // DXVK binds memory through the *2 entry points on Vulkan 1.1+ devices; forward each bind info to the
    // existing single-bind bridge command.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2(VkDevice device, uint32_t bindInfoCount,
                                                                             const VkBindBufferMemoryInfo* pBindInfos)
    {
        for (uint32_t i = 0; i < bindInfoCount; ++i)
        {
            const VkResult r = vkBindBufferMemory(device, pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
            if (r != VK_SUCCESS)
            {
                return r;
            }
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2(VkDevice device, uint32_t bindInfoCount,
                                                                            const VkBindImageMemoryInfo* pBindInfos)
    {
        for (uint32_t i = 0; i < bindInfoCount; ++i)
        {
            const VkResult r = vkBindImageMemory(device, pBindInfos[i].image, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
            if (r != VK_SUCCESS)
            {
                return r;
            }
        }
        return VK_SUCCESS;
    }

    namespace
    {
        gb::image_subresource_range to_wire_range(const VkImageSubresourceRange& range)
        {
            gb::image_subresource_range out{};
            out.aspect_mask = range.aspectMask;
            out.base_mip_level = range.baseMipLevel;
            out.level_count = range.levelCount;
            out.base_array_layer = range.baseArrayLayer;
            out.layer_count = range.layerCount;
            return out;
        }
    }

    // Historical limitation, superseded by the complete arrays passed below:
    // Only image memory barriers are remoted (one IOCTL each, using the global stage masks); memory and
    // buffer barriers are not modeled yet.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer buffer, VkPipelineStageFlags sourceStages,
                                                                          VkPipelineStageFlags destinationStages, VkDependencyFlags flags,
                                                                          uint32_t memoryCount, const VkMemoryBarrier* memory,
                                                                          uint32_t bufferCount, const VkBufferMemoryBarrier* buffers,
                                                                          uint32_t imageCount, const VkImageMemoryBarrier* images)
    {
        gb::synchronization_wire::command command{};
        command.op = gb::synchronization_wire::operation::barrier;
        command.legacy = {.source_stages = sourceStages,
                          .destination_stages = destinationStages,
                          .flags = flags,
                          .memory_count = memoryCount,
                          .memory = memory,
                          .buffer_count = bufferCount,
                          .buffers = buffers,
                          .image_count = imageCount,
                          .images = images};
        record_synchronization(buffer, command);
    }

    // Historical conversion, superseded by native synchronization2 forwarding below:
    // synchronization2 barrier: DXVK uses this exclusively. Lower its image barriers to the v1
    // cmd_pipeline_barrier records. The VkPipelineStageFlags2/VkAccessFlags2 (64-bit) don't map cleanly to
    // the v1 32-bit flags and a 0 stage mask is invalid in v1, so use ALL_COMMANDS stages (the bridge
    // executes submissions synchronously, so over-synchronizing is harmless). Layout transitions -- the
    // part DXVK actually relies on -- are preserved. Global/buffer memory barriers are no-ops here.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer buffer, const VkDependencyInfo* dependency)
    {
        gb::synchronization_wire::command command{};
        command.op = gb::synchronization_wire::operation::barrier2;
        if (dependency)
        {
            command.dependency = *dependency;
        }
        else
        {
            command.dependency.sType = VK_STRUCTURE_TYPE_MAX_ENUM;
        }
        record_synchronization(buffer, command);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2KHR(VkCommandBuffer buffer, const VkDependencyInfo* dependency)
    {
        vkCmdPipelineBarrier2(buffer, dependency);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image,
                                                                          VkImageLayout imageLayout, const VkClearColorValue* pColor,
                                                                          uint32_t rangeCount, const VkImageSubresourceRange* pRanges)
    {
        for (uint32_t i = 0; i < rangeCount; ++i)
        {
            gb::cmd_clear_color_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.image = to_object_id(image);
            request.subresource = to_wire_range(pRanges[i]);
            request.image_layout = static_cast<uint32_t>(imageLayout);
            request.color_r = pColor->float32[0];
            request.color_g = pColor->float32[1];
            request.color_b = pColor->float32[2];
            request.color_a = pColor->float32[3];
            record_command(request.command_buffer, gb::command::cmd_clear_color_image, &request, sizeof(request));
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t attachmentCount,
                                                                           const VkClearAttachment* pAttachments, uint32_t rectCount,
                                                                           const VkClearRect* pRects)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        const size_t attach_bytes = static_cast<size_t>(attachmentCount) * sizeof(VkClearAttachment);
        const size_t rect_bytes = static_cast<size_t>(rectCount) * sizeof(VkClearRect);

        std::vector<uint8_t> message(sizeof(gb::cmd_clear_attachments_request) + attach_bytes + rect_bytes);
        gb::cmd_clear_attachments_request header{};
        header.command_buffer = command_buffer;
        header.attachment_count = attachmentCount;
        header.rect_count = rectCount;
        std::memcpy(message.data(), &header, sizeof(header));
        if (attach_bytes)
        {
            std::memcpy(message.data() + sizeof(header), pAttachments, attach_bytes);
        }
        if (rect_bytes)
        {
            std::memcpy(message.data() + sizeof(header) + attach_bytes, pRects, rect_bytes);
        }
        record_command(command_buffer, gb::command::cmd_clear_attachments, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image,
                                                                                 VkImageLayout imageLayout,
                                                                                 const VkClearDepthStencilValue* pDepthStencil,
                                                                                 uint32_t rangeCount,
                                                                                 const VkImageSubresourceRange* pRanges)
    {
        for (uint32_t i = 0; i < rangeCount; ++i)
        {
            gb::cmd_clear_depth_stencil_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.image = to_object_id(image);
            request.subresource = to_wire_range(pRanges[i]);
            request.image_layout = static_cast<uint32_t>(imageLayout);
            request.depth = pDepthStencil->depth;
            request.stencil = pDepthStencil->stencil;
            record_command(request.command_buffer, gb::command::cmd_clear_depth_stencil_image, &request, sizeof(request));
        }
    }

    // The retained compatibility opcode has the historical limitations below; this entry point now sends full regions.
    // Copies are remoted assuming tight packing of mip 0 / layer 0 at image offset 0 to buffer offset 0
    // (bufferRowLength/bufferImageHeight/imageOffset are not yet honored).
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage,
                                                                            VkImageLayout srcImageLayout, VkBuffer dstBuffer,
                                                                            uint32_t regionCount, const VkBufferImageCopy* pRegions)
    {
        auto* const stream = find_command_stream(to_object_id(commandBuffer));
        if (!stream || stream->error != VK_SUCCESS)
        {
            return;
        }
        try
        {
            // The versioned full packet shares the pointer-free region codec with copy-2, but keeps the legacy native call.
            if (!regionCount || !pRegions || regionCount > gb::render_pass_wire::max_elements ||
                regionCount > gb::render_pass_wire::max_bytes / sizeof(VkBufferImageCopy2))
            {
                throw gb::render_pass_wire::error("invalid or excessive image-to-buffer copy regions");
            }
            std::vector<VkBufferImageCopy2> regions(regionCount);
            for (uint32_t i = 0; i < regionCount; ++i)
            {
                const auto& region = pRegions[i];
                regions[i] = {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                              .pNext = nullptr,
                              .bufferOffset = region.bufferOffset,
                              .bufferRowLength = region.bufferRowLength,
                              .bufferImageHeight = region.bufferImageHeight,
                              .imageSubresource = region.imageSubresource,
                              .imageOffset = region.imageOffset,
                              .imageExtent = region.imageExtent};
            }
            const VkCopyImageToBufferInfo2 info{
                VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, nullptr, srcImage, srcImageLayout, dstBuffer, regionCount, regions.data()};
            record_render_pass(commandBuffer, gb::command::cmd_copy_image_to_buffer_full, &info);
        }
        catch (const gb::render_pass_wire::error& error)
        {
            OutputDebugStringA(error.what());
            stream->error = error.result;
        }
        catch (const std::bad_alloc&)
        {
            stream->error = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    // Copies are remoted assuming tight packing into mip 0 / layer 0 at image offset 0 from buffer
    // offset 0 (bufferRowLength/bufferImageHeight/imageOffset are not yet honored).
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer,
                                                                            VkImage dstImage, VkImageLayout dstImageLayout,
                                                                            uint32_t regionCount, const VkBufferImageCopy* pRegions)
    {
        for (uint32_t i = 0; i < regionCount; ++i)
        {
            const VkBufferImageCopy& r = pRegions[i];
            gb::cmd_copy_buffer_to_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.buffer = to_object_id(srcBuffer);
            request.image = to_object_id(dstImage);
            request.buffer_offset = r.bufferOffset;
            request.image_layout = static_cast<uint32_t>(dstImageLayout);
            request.buffer_row_length = r.bufferRowLength;
            request.buffer_image_height = r.bufferImageHeight;
            request.image_offset_x = r.imageOffset.x;
            request.image_offset_y = r.imageOffset.y;
            request.image_offset_z = r.imageOffset.z;
            request.width = r.imageExtent.width;
            request.height = r.imageExtent.height;
            request.depth = r.imageExtent.depth;
            request.mip_level = r.imageSubresource.mipLevel;
            request.base_array_layer = r.imageSubresource.baseArrayLayer;
            request.layer_count = r.imageSubresource.layerCount;
            request.aspect_mask = r.imageSubresource.aspectMask;
            record_command(request.command_buffer, gb::command::cmd_copy_buffer_to_image, &request, sizeof(request));
        }
    }

    // DXVK on a Vulkan 1.3 device issues the KHR_copy_commands2 / core-1.3 variants. They carry the
    // same per-region data as the legacy entry points, so they reuse the existing copy commands.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2(VkCommandBuffer commandBuffer,
                                                                             const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo)
    {
        for (uint32_t i = 0; i < pCopyBufferToImageInfo->regionCount; ++i)
        {
            const VkBufferImageCopy2& r = pCopyBufferToImageInfo->pRegions[i];
            gb::cmd_copy_buffer_to_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.buffer = to_object_id(pCopyBufferToImageInfo->srcBuffer);
            request.image = to_object_id(pCopyBufferToImageInfo->dstImage);
            request.buffer_offset = r.bufferOffset;
            request.image_layout = static_cast<uint32_t>(pCopyBufferToImageInfo->dstImageLayout);
            request.buffer_row_length = r.bufferRowLength;
            request.buffer_image_height = r.bufferImageHeight;
            request.image_offset_x = r.imageOffset.x;
            request.image_offset_y = r.imageOffset.y;
            request.image_offset_z = r.imageOffset.z;
            request.width = r.imageExtent.width;
            request.height = r.imageExtent.height;
            request.depth = r.imageExtent.depth;
            request.mip_level = r.imageSubresource.mipLevel;
            request.base_array_layer = r.imageSubresource.baseArrayLayer;
            request.layer_count = r.imageSubresource.layerCount;
            request.aspect_mask = r.imageSubresource.aspectMask;
            record_command(request.command_buffer, gb::command::cmd_copy_buffer_to_image, &request, sizeof(request));
        }
    }

    // Resolves a multisampled source image into a single-sample destination. DXVK uses this to resolve an
    // MSAA backbuffer before presenting it. Remoted assuming full image, mip 0 / layer 0, offset 0.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer commandBuffer, VkImage srcImage,
                                                                       VkImageLayout srcImageLayout, VkImage dstImage,
                                                                       VkImageLayout dstImageLayout, uint32_t regionCount,
                                                                       const VkImageResolve* pRegions)
    {
        for (uint32_t i = 0; i < regionCount; ++i)
        {
            const VkImageResolve& r = pRegions[i];
            gb::cmd_resolve_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.src_image = to_object_id(srcImage);
            request.dst_image = to_object_id(dstImage);
            request.src_layout = static_cast<uint32_t>(srcImageLayout);
            request.dst_layout = static_cast<uint32_t>(dstImageLayout);
            request.width = r.extent.width;
            request.height = r.extent.height;
            request.aspect_mask = r.srcSubresource.aspectMask;
            record_command(request.command_buffer, gb::command::cmd_resolve_image, &request, sizeof(request));
        }
    }

    // Updates a buffer region inline. DXVK uses this for small dynamic uploads (constant/uniform data); the
    // bytes trail the request header in the recorded stream.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer,
                                                                       VkDeviceSize dstOffset, VkDeviceSize dataSize, const void* pData)
    {
        const auto size = static_cast<uint32_t>(dataSize);
        std::vector<uint8_t> message(sizeof(gb::cmd_update_buffer_request) + size);
        gb::cmd_update_buffer_request header{};
        header.command_buffer = to_object_id(commandBuffer);
        header.buffer = to_object_id(dstBuffer);
        header.offset = dstOffset;
        header.size = size;
        std::memcpy(message.data(), &header, sizeof(header));
        if (size > 0 && pData)
        {
            std::memcpy(message.data() + sizeof(header), pData, size);
        }
        record_command(header.command_buffer, gb::command::cmd_update_buffer, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2(VkCommandBuffer commandBuffer,
                                                                        const VkResolveImageInfo2* pResolveImageInfo)
    {
        for (uint32_t i = 0; i < pResolveImageInfo->regionCount; ++i)
        {
            const VkImageResolve2& r = pResolveImageInfo->pRegions[i];
            gb::cmd_resolve_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.src_image = to_object_id(pResolveImageInfo->srcImage);
            request.dst_image = to_object_id(pResolveImageInfo->dstImage);
            request.src_layout = static_cast<uint32_t>(pResolveImageInfo->srcImageLayout);
            request.dst_layout = static_cast<uint32_t>(pResolveImageInfo->dstImageLayout);
            request.width = r.extent.width;
            request.height = r.extent.height;
            request.aspect_mask = r.srcSubresource.aspectMask;
            record_command(request.command_buffer, gb::command::cmd_resolve_image, &request, sizeof(request));
        }
    }

    // Image-to-image copy. DXVK issues this for texture-to-texture transfers during scene rendering. One
    // VkImageCopy region per recorded command (the shim loops over regions).
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage,
                                                                    VkImageLayout srcImageLayout, VkImage dstImage,
                                                                    VkImageLayout dstImageLayout, uint32_t regionCount,
                                                                    const VkImageCopy* pRegions)
    {
        for (uint32_t i = 0; i < regionCount; ++i)
        {
            const VkImageCopy& r = pRegions[i];
            gb::cmd_copy_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.src_image = to_object_id(srcImage);
            request.dst_image = to_object_id(dstImage);
            request.src_layout = static_cast<uint32_t>(srcImageLayout);
            request.dst_layout = static_cast<uint32_t>(dstImageLayout);
            request.src_aspect_mask = r.srcSubresource.aspectMask;
            request.src_mip_level = r.srcSubresource.mipLevel;
            request.src_base_array_layer = r.srcSubresource.baseArrayLayer;
            request.src_layer_count = r.srcSubresource.layerCount;
            request.src_offset_x = r.srcOffset.x;
            request.src_offset_y = r.srcOffset.y;
            request.src_offset_z = r.srcOffset.z;
            request.dst_aspect_mask = r.dstSubresource.aspectMask;
            request.dst_mip_level = r.dstSubresource.mipLevel;
            request.dst_base_array_layer = r.dstSubresource.baseArrayLayer;
            request.dst_layer_count = r.dstSubresource.layerCount;
            request.dst_offset_x = r.dstOffset.x;
            request.dst_offset_y = r.dstOffset.y;
            request.dst_offset_z = r.dstOffset.z;
            request.width = r.extent.width;
            request.height = r.extent.height;
            request.depth = r.extent.depth;
            record_command(request.command_buffer, gb::command::cmd_copy_image, &request, sizeof(request));
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2(VkCommandBuffer commandBuffer, const VkCopyImageInfo2* pCopyImageInfo)
    {
        for (uint32_t i = 0; i < pCopyImageInfo->regionCount; ++i)
        {
            const VkImageCopy2& r = pCopyImageInfo->pRegions[i];
            gb::cmd_copy_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.src_image = to_object_id(pCopyImageInfo->srcImage);
            request.dst_image = to_object_id(pCopyImageInfo->dstImage);
            request.src_layout = static_cast<uint32_t>(pCopyImageInfo->srcImageLayout);
            request.dst_layout = static_cast<uint32_t>(pCopyImageInfo->dstImageLayout);
            request.src_aspect_mask = r.srcSubresource.aspectMask;
            request.src_mip_level = r.srcSubresource.mipLevel;
            request.src_base_array_layer = r.srcSubresource.baseArrayLayer;
            request.src_layer_count = r.srcSubresource.layerCount;
            request.src_offset_x = r.srcOffset.x;
            request.src_offset_y = r.srcOffset.y;
            request.src_offset_z = r.srcOffset.z;
            request.dst_aspect_mask = r.dstSubresource.aspectMask;
            request.dst_mip_level = r.dstSubresource.mipLevel;
            request.dst_base_array_layer = r.dstSubresource.baseArrayLayer;
            request.dst_layer_count = r.dstSubresource.layerCount;
            request.dst_offset_x = r.dstOffset.x;
            request.dst_offset_y = r.dstOffset.y;
            request.dst_offset_z = r.dstOffset.z;
            request.width = r.extent.width;
            request.height = r.extent.height;
            request.depth = r.extent.depth;
            record_command(request.command_buffer, gb::command::cmd_copy_image, &request, sizeof(request));
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2(VkCommandBuffer commandBuffer, const VkBlitImageInfo2* pBlitImageInfo);

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage srcImage,
                                                                    VkImageLayout srcImageLayout, VkImage dstImage,
                                                                    VkImageLayout dstImageLayout, uint32_t regionCount,
                                                                    const VkImageBlit* pRegions, VkFilter filter)
    {
        std::vector<VkImageBlit2> regions(regionCount);
        for (uint32_t i = 0; i < regionCount; ++i)
        {
            regions[i].sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
            regions[i].srcSubresource = pRegions[i].srcSubresource;
            regions[i].srcOffsets[0] = pRegions[i].srcOffsets[0];
            regions[i].srcOffsets[1] = pRegions[i].srcOffsets[1];
            regions[i].dstSubresource = pRegions[i].dstSubresource;
            regions[i].dstOffsets[0] = pRegions[i].dstOffsets[0];
            regions[i].dstOffsets[1] = pRegions[i].dstOffsets[1];
        }
        VkBlitImageInfo2 info{};
        info.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
        info.srcImage = srcImage;
        info.srcImageLayout = srcImageLayout;
        info.dstImage = dstImage;
        info.dstImageLayout = dstImageLayout;
        info.regionCount = regionCount;
        info.pRegions = regions.data();
        info.filter = filter;
        vkCmdBlitImage2(commandBuffer, &info);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2(VkCommandBuffer commandBuffer, const VkBlitImageInfo2* pBlitImageInfo)
    {
        if (!pBlitImageInfo)
        {
            return;
        }

        for (uint32_t i = 0; i < pBlitImageInfo->regionCount; ++i)
        {
            const VkImageBlit2& r = pBlitImageInfo->pRegions[i];
            gb::cmd_blit_image_request request{};
            request.command_buffer = to_object_id(commandBuffer);
            request.src_image = to_object_id(pBlitImageInfo->srcImage);
            request.dst_image = to_object_id(pBlitImageInfo->dstImage);
            request.src_layout = static_cast<uint32_t>(pBlitImageInfo->srcImageLayout);
            request.dst_layout = static_cast<uint32_t>(pBlitImageInfo->dstImageLayout);
            request.src_aspect_mask = r.srcSubresource.aspectMask;
            request.src_mip_level = r.srcSubresource.mipLevel;
            request.src_base_array_layer = r.srcSubresource.baseArrayLayer;
            request.src_layer_count = r.srcSubresource.layerCount;
            request.src_offset_x0 = r.srcOffsets[0].x;
            request.src_offset_y0 = r.srcOffsets[0].y;
            request.src_offset_z0 = r.srcOffsets[0].z;
            request.src_offset_x1 = r.srcOffsets[1].x;
            request.src_offset_y1 = r.srcOffsets[1].y;
            request.src_offset_z1 = r.srcOffsets[1].z;
            request.dst_aspect_mask = r.dstSubresource.aspectMask;
            request.dst_mip_level = r.dstSubresource.mipLevel;
            request.dst_base_array_layer = r.dstSubresource.baseArrayLayer;
            request.dst_layer_count = r.dstSubresource.layerCount;
            request.dst_offset_x0 = r.dstOffsets[0].x;
            request.dst_offset_y0 = r.dstOffsets[0].y;
            request.dst_offset_z0 = r.dstOffsets[0].z;
            request.dst_offset_x1 = r.dstOffsets[1].x;
            request.dst_offset_y1 = r.dstOffsets[1].y;
            request.dst_offset_z1 = r.dstOffsets[1].z;
            request.filter = static_cast<uint32_t>(pBlitImageInfo->filter);
            record_command(request.command_buffer, gb::command::cmd_blit_image, &request, sizeof(request));
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2KHR(VkCommandBuffer commandBuffer,
                                                                        const VkBlitImageInfo2* pBlitImageInfo)
    {
        vkCmdBlitImage2(commandBuffer, pBlitImageInfo);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2(VkCommandBuffer commandBuffer,
                                                                             const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo)
    {
        record_render_pass(commandBuffer, gb::command::cmd_copy_image_to_buffer2_full, pCopyImageToBufferInfo);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2KHR(VkCommandBuffer commandBuffer,
                                                                                const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo)
    {
        vkCmdCopyImageToBuffer2(commandBuffer, pCopyImageToBufferInfo);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance instance,
                                                                                 const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                                                                 const VkAllocationCallbacks*, VkSurfaceKHR* pSurface)
    {
        if (native_wsi_enabled())
        {
            return native_create_surface(instance, pCreateInfo, pSurface);
        }

        gb::create_surface_request request{};
        request.hwnd = reinterpret_cast<uint64_t>(pCreateInfo->hwnd);

        gb::create_surface_response response{};
        if (!bridge_call(gb::ioctl_create_surface, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pSurface = to_handle<VkSurfaceKHR>(response.surface);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                                         const VkAllocationCallbacks*)
    {
        if (native_wsi_enabled())
        {
            if (surface)
            {
                native_destroy(nw::operation::destroy_surface, to_object_id(instance), to_object_id(surface));
            }
            return;
        }

        if (!surface)
        {
            return;
        }
        gb::destroy_surface_request request{};
        request.surface = to_object_id(surface);
        bridge_call(gb::ioctl_destroy_surface, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
    {
        if (native_wsi_enabled())
        {
            uint32_t count = 1;
            return native_query(nw::query::capabilities, physicalDevice, surface, 0, &count, pSurfaceCapabilities,
                                sizeof(VkSurfaceCapabilitiesKHR));
        }

        if (!pSurfaceCapabilities)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        *pSurfaceCapabilities = {};

        gb::get_surface_capabilities_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.surface = to_object_id(surface);
        if (!bridge_call(gb::ioctl_get_surface_capabilities, &request, sizeof(request), pSurfaceCapabilities,
                         sizeof(*pSurfaceCapabilities)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return VK_SUCCESS;
    }

    // --- Core physical-device + surface queries (minimal stubs for D3D->Vulkan layers like DXVK) ---
    // The `2` variants delegate to the already-remoted base queries and drop the pNext chain. Features
    // and format support are reported permissively (everything available) so the layer proceeds; this
    // is optimistic — the bridge may not actually support every capability — but it advances bring-up.
    // Surface queries report a single common BGRA format and FIFO present mode.

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures* pFeatures)
    {
        if (!pFeatures)
        {
            return;
        }
        // VkPhysicalDeviceFeatures is a block of VkBool32 toggles; advertise them all as available.
        auto* flags = reinterpret_cast<VkBool32*>(pFeatures);
        for (size_t i = 0; i < sizeof(*pFeatures) / sizeof(VkBool32); ++i)
        {
            flags[i] = VK_TRUE;
        }
        mask_unsupported_sparse_features(*pFeatures);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                                                                  VkPhysicalDeviceFeatures2* pFeatures)
    {
        if (!pFeatures)
        {
            return;
        }

        // Keep unsupported fields deterministic even if the bridge query fails before filling the root.
        mask_unsupported_sparse_features(pFeatures->features);

        // Collect the caller's chain: the root VkPhysicalDeviceFeatures2 (carrying the base
        // VkPhysicalDeviceFeatures), then each pNext struct. For each we record its sType + pad-free
        // body size and where to write the real values back. Bodies start after the {sType,pNext}
        // header, which is feature_chain_header_size bytes on this ABI.
        struct dest
        {
            uint8_t* body;
            uint32_t body_size;
        };

        std::vector<dest> dests;
        std::vector<gb::feature_chain_record> records;

        const auto add = [&](VkStructureType type, void* base) {
            const auto body_size = static_cast<uint32_t>(gb::feature_body_size(type));
            dests.push_back({.body = reinterpret_cast<uint8_t*>(base) + gb::feature_chain_header_size, .body_size = body_size});
            records.push_back({.s_type = static_cast<uint32_t>(type), .body_size = body_size});
        };

        add(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, pFeatures);
        for (auto* next = static_cast<VkBaseOutStructure*>(pFeatures->pNext); next; next = next->pNext)
        {
            add(next->sType, next);
        }

        std::vector<std::byte> in(sizeof(gb::get_physical_device_features2_request) + records.size() * sizeof(gb::feature_chain_record));
        auto* request = reinterpret_cast<gb::get_physical_device_features2_request*>(in.data());
        request->physical_device = to_object_id(physicalDevice);
        request->struct_count = static_cast<uint32_t>(records.size());
        request->reserved = 0;
        std::memcpy(in.data() + sizeof(*request), records.data(), records.size() * sizeof(gb::feature_chain_record));

        size_t out_capacity = sizeof(gb::get_physical_device_features2_response);
        for (const auto& d : dests)
        {
            out_capacity += sizeof(gb::feature_chain_record) + d.body_size;
        }
        std::vector<std::byte> out(out_capacity);

        if (!bridge_call(gb::ioctl_get_physical_device_features2, in.data(), static_cast<DWORD>(in.size()), out.data(),
                         static_cast<DWORD>(out.size())))
        {
            return;
        }

        const auto* response = reinterpret_cast<const gb::get_physical_device_features2_response*>(out.data());
        if (response->vk_result != VK_SUCCESS)
        {
            return;
        }

        // Records come back in request order, so record i fills dests[i]. Copy the real bool run in.
        size_t offset = sizeof(gb::get_physical_device_features2_response);
        for (uint32_t i = 0; i < response->struct_count && i < dests.size(); ++i)
        {
            if (offset + sizeof(gb::feature_chain_record) > out.size())
            {
                break;
            }
            const auto* record = reinterpret_cast<const gb::feature_chain_record*>(out.data() + offset);
            offset += sizeof(gb::feature_chain_record);
            if (offset + record->body_size > out.size())
            {
                break;
            }

            const uint32_t copy = record->body_size < dests[i].body_size ? record->body_size : dests[i].body_size;
            if (copy > 0)
            {
                std::memcpy(dests[i].body, out.data() + offset, copy);
            }
            offset += record->body_size;
        }

        mask_unsupported_sparse_features(pFeatures->features);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                                                                    VkPhysicalDeviceProperties2* pProperties)
    {
        if (!pProperties)
        {
            return;
        }

        // Base VkPhysicalDeviceProperties goes through the already-remoted base query.
        vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);

        // Remote the chained property structs (e.g. VkPhysicalDeviceRobustness2PropertiesEXT, whose
        // alignment fields DXVK divides by). Same chain convention as vkGetPhysicalDeviceFeatures2.
        struct dest
        {
            VkBaseOutStructure* structure;
            uint8_t* body;
            uint32_t body_size;
        };

        std::vector<dest> dests;
        std::vector<gb::feature_chain_record> records;
        for (auto* next = static_cast<VkBaseOutStructure*>(pProperties->pNext); next; next = next->pNext)
        {
            const auto body_size = static_cast<uint32_t>(next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT
                                                             ? sizeof(gb::descriptor_buffer_properties_wire)
                                                             : gb::property_body_size(next->sType));
            if (body_size == 0)
            {
                continue; // struct the host does not know; left as the caller initialized it
            }
            dests.push_back(
                {.structure = next, .body = reinterpret_cast<uint8_t*>(next) + gb::feature_chain_header_size, .body_size = body_size});
            records.push_back({.s_type = static_cast<uint32_t>(next->sType), .body_size = body_size});
        }

        if (records.empty())
        {
            return;
        }

        std::vector<std::byte> in(sizeof(gb::get_physical_device_properties2_request) + records.size() * sizeof(gb::feature_chain_record));
        auto* request = reinterpret_cast<gb::get_physical_device_properties2_request*>(in.data());
        request->physical_device = to_object_id(physicalDevice);
        request->struct_count = static_cast<uint32_t>(records.size());
        request->reserved = 0;
        std::memcpy(in.data() + sizeof(*request), records.data(), records.size() * sizeof(gb::feature_chain_record));

        size_t out_capacity = sizeof(gb::get_physical_device_properties2_response);
        for (const auto& d : dests)
        {
            out_capacity += sizeof(gb::feature_chain_record) + d.body_size;
        }
        std::vector<std::byte> out(out_capacity);

        if (!bridge_call(gb::ioctl_get_physical_device_properties2, in.data(), static_cast<DWORD>(in.size()), out.data(),
                         static_cast<DWORD>(out.size())))
        {
            return;
        }

        const auto* response = reinterpret_cast<const gb::get_physical_device_properties2_response*>(out.data());
        if (response->vk_result != VK_SUCCESS)
        {
            return;
        }

        size_t offset = sizeof(gb::get_physical_device_properties2_response);
        for (uint32_t i = 0; i < response->struct_count && i < dests.size(); ++i)
        {
            if (offset + sizeof(gb::feature_chain_record) > out.size())
            {
                break;
            }
            const auto* record = reinterpret_cast<const gb::feature_chain_record*>(out.data() + offset);
            offset += sizeof(gb::feature_chain_record);
            if (offset + record->body_size > out.size())
            {
                break;
            }

            if (record->s_type == static_cast<uint32_t>(dests[i].structure->sType))
            {
                if (dests[i].structure->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT)
                {
                    if (record->body_size == sizeof(gb::descriptor_buffer_properties_wire))
                    {
                        gb::descriptor_buffer_properties_wire wire{};
                        std::memcpy(wire.data(), out.data() + offset, sizeof(wire));
                        gb::decode_descriptor_buffer_properties(
                            wire, *reinterpret_cast<VkPhysicalDeviceDescriptorBufferPropertiesEXT*>(dests[i].structure));
                    }
                }
                else
                {
                    const uint32_t copy = std::min(record->body_size, dests[i].body_size);
                    if (copy > 0)
                    {
                        std::memcpy(dests[i].body, out.data() + offset, copy);
                    }
                }
            }
            offset += record->body_size;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL
    vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2* pMemoryProperties)
    {
        if (!pMemoryProperties)
        {
            return;
        }

        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &pMemoryProperties->memoryProperties);

        // Fill any chained VK_EXT_memory_budget request: DXGI's QueryVideoMemoryInfo reports 0 available
        // VRAM (and apps may abort) unless the host's real per-heap budget/usage is forwarded.
        for (auto* next = static_cast<VkBaseOutStructure*>(pMemoryProperties->pNext); next != nullptr; next = next->pNext)
        {
            if (next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
            {
                continue;
            }

            auto* budget = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(next);

            gb::get_physical_device_memory_budget_request request{};
            request.physical_device = to_object_id(physicalDevice);

            gb::get_physical_device_memory_budget_response response{};
            if (bridge_call(gb::ioctl_get_physical_device_memory_budget, &request, sizeof(request), &response, sizeof(response)) &&
                response.vk_result == VK_SUCCESS)
            {
                const uint32_t count = std::min<uint32_t>(response.heap_count, VK_MAX_MEMORY_HEAPS);
                for (uint32_t i = 0; i < count; ++i)
                {
                    budget->heapBudget[i] = response.heap_budget[i];
                    budget->heapUsage[i] = response.heap_usage[i];
                }
            }
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                                                                               uint32_t* pCount,
                                                                                               VkQueueFamilyProperties2* pProperties)
    {
        if (!pCount)
        {
            return;
        }

        if (!pProperties)
        {
            vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pCount, nullptr);
            return;
        }

        gb::get_queue_family_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.max_count = *pCount;
        for (uint32_t i = 0; i < *pCount && request.query_ownership_transfer == 0; ++i)
        {
            for (const auto* next = reinterpret_cast<const VkBaseOutStructure*>(pProperties[i].pNext); next; next = next->pNext)
            {
                if (next->sType == VK_STRUCTURE_TYPE_QUEUE_FAMILY_OWNERSHIP_TRANSFER_PROPERTIES_KHR)
                {
                    request.query_ownership_transfer = 1;
                    break;
                }
            }
        }
        std::vector<std::byte> buffer(sizeof(gb::get_queue_family_properties_response) +
                                      static_cast<size_t>(request.max_count) * sizeof(gb::queue_family_properties));
        if (!bridge_call(gb::ioctl_get_queue_family_properties, &request, sizeof(request), buffer.data(),
                         static_cast<DWORD>(buffer.size())))
        {
            *pCount = 0;
            return;
        }

        const auto* response = reinterpret_cast<const gb::get_queue_family_properties_response*>(buffer.data());
        const uint32_t count = std::min(response->count, *pCount);
        const auto* families =
            reinterpret_cast<const gb::queue_family_properties*>(buffer.data() + sizeof(gb::get_queue_family_properties_response));
        for (uint32_t i = 0; i < count; ++i)
        {
            pProperties[i].queueFamilyProperties = {
                .queueFlags = mask_unsupported_queue_flags(families[i].queue_flags),
                .queueCount = families[i].queue_count,
                .timestampValidBits = families[i].timestamp_valid_bits,
                .minImageTransferGranularity = {.width = families[i].min_image_transfer_granularity_width,
                                                .height = families[i].min_image_transfer_granularity_height,
                                                .depth = families[i].min_image_transfer_granularity_depth}};
            for (auto* next = reinterpret_cast<VkBaseOutStructure*>(pProperties[i].pNext); next; next = next->pNext)
            {
                if (next->sType == VK_STRUCTURE_TYPE_QUEUE_FAMILY_OWNERSHIP_TRANSFER_PROPERTIES_KHR)
                {
                    reinterpret_cast<VkQueueFamilyOwnershipTransferPropertiesKHR*>(next)->optimalImageTransferToQueueFamilies =
                        families[i].optimal_image_transfer_to_queue_families;
                }
            }
        }
        *pCount = count;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format,
                                                                                         VkFormatProperties* pFormatProperties)
    {
        if (!pFormatProperties)
        {
            return;
        }
        *pFormatProperties = {};

        // Remote the device's real per-format support. The previous format-agnostic stub reported every
        // format as supporting everything, which made DXVK's format table accept invalid mappings (e.g. a
        // compressed format as a render target) and then reject resources that should succeed.
        gb::get_physical_device_format_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.format = static_cast<uint32_t>(format);

        gb::get_physical_device_format_properties_response response{};
        if (!bridge_call(gb::ioctl_get_physical_device_format_properties, &request, sizeof(request), &response, sizeof(response)))
        {
            return;
        }
        pFormatProperties->linearTilingFeatures = response.linear_tiling_features;
        pFormatProperties->optimalTilingFeatures = response.optimal_tiling_features;
        pFormatProperties->bufferFeatures = response.buffer_features;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format,
                                                                                          VkFormatProperties2* pFormatProperties)
    {
        if (!pFormatProperties)
        {
            return;
        }

        vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &pFormatProperties->formatProperties);

        // DXVK reads format features through VkFormatProperties3 (VkFormatFeatureFlags2) chained on
        // pNext, not the legacy VkFormatProperties. Mirror the feature bits there as well; the flag bit
        // values are identical between the legacy and the 64-bit flags. Without this DXVK observes no
        // format support and rejects every render-target format.
        for (auto* next = static_cast<VkBaseOutStructure*>(pFormatProperties->pNext); next != nullptr; next = next->pNext)
        {
            if (next->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3)
            {
                auto* properties3 = reinterpret_cast<VkFormatProperties3*>(next);
                properties3->linearTilingFeatures = pFormatProperties->formatProperties.linearTilingFeatures;
                properties3->optimalTilingFeatures = pFormatProperties->formatProperties.optimalTilingFeatures;
                properties3->bufferFeatures = pFormatProperties->formatProperties.bufferFeatures;
            }
        }
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
        VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
        VkImageCreateFlags flags, VkImageFormatProperties* pImageFormatProperties)
    {
        if (!pImageFormatProperties)
        {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }

        gb::get_physical_device_image_format_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.format = static_cast<uint32_t>(format);
        request.type = static_cast<uint32_t>(type);
        request.tiling = static_cast<uint32_t>(tiling);
        request.usage = usage;
        request.flags = flags;

        gb::get_physical_device_image_format_properties_response response{};
        if (!bridge_call(gb::ioctl_get_physical_device_image_format_properties, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pImageFormatProperties = {};
        pImageFormatProperties->maxExtent = {
            .width = response.max_extent_width, .height = response.max_extent_height, .depth = response.max_extent_depth};
        pImageFormatProperties->maxMipLevels = response.max_mip_levels;
        pImageFormatProperties->maxArrayLayers = response.max_array_layers;
        pImageFormatProperties->sampleCounts = response.sample_counts;
        pImageFormatProperties->maxResourceSize = response.max_resource_size;
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
                                              VkImageFormatProperties2* pImageFormatProperties)
    {
        if (!pImageFormatInfo || !pImageFormatProperties)
        {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        return vkGetPhysicalDeviceImageFormatProperties(physicalDevice, pImageFormatInfo->format, pImageFormatInfo->type,
                                                        pImageFormatInfo->tiling, pImageFormatInfo->usage, pImageFormatInfo->flags,
                                                        &pImageFormatProperties->imageFormatProperties);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice,
                                                                                              uint32_t queueFamily, VkSurfaceKHR surface,
                                                                                              VkBool32* pSupported)
    {
        if (native_wsi_enabled())
        {
            uint32_t count = 1;
            return native_query(nw::query::support, physicalDevice, surface, queueFamily, &count, pSupported, sizeof(VkBool32));
        }

        if (pSupported)
        {
            *pSupported = VK_TRUE;
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice,
                                                                                              VkSurfaceKHR surface, uint32_t* pCount,
                                                                                              VkSurfaceFormatKHR* pSurfaceFormats)
    {
        if (native_wsi_enabled())
        {
            return native_query(nw::query::formats, physicalDevice, surface, 0, pCount, pSurfaceFormats, sizeof(VkSurfaceFormatKHR));
        }

        static const VkSurfaceFormatKHR formats[] = {
            {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
            {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        };
        constexpr auto available = static_cast<uint32_t>(sizeof(formats) / sizeof(formats[0]));

        if (!pCount)
        {
            return VK_INCOMPLETE;
        }
        if (!pSurfaceFormats)
        {
            *pCount = available;
            return VK_SUCCESS;
        }

        const uint32_t to_copy = std::min(*pCount, available);
        for (uint32_t i = 0; i < to_copy; ++i)
        {
            pSurfaceFormats[i] = formats[i];
        }
        *pCount = to_copy;
        return to_copy < available ? VK_INCOMPLETE : VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice,
                                                                                                   VkSurfaceKHR surface, uint32_t* pCount,
                                                                                                   VkPresentModeKHR* pPresentModes)
    {
        if (native_wsi_enabled())
        {
            return native_query(nw::query::modes, physicalDevice, surface, 0, pCount, pPresentModes, sizeof(VkPresentModeKHR));
        }

        static const VkPresentModeKHR modes[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
        constexpr auto available = static_cast<uint32_t>(sizeof(modes) / sizeof(modes[0]));

        if (!pCount)
        {
            return VK_INCOMPLETE;
        }
        if (!pPresentModes)
        {
            *pCount = available;
            return VK_SUCCESS;
        }

        const uint32_t to_copy = std::min(*pCount, available);
        for (uint32_t i = 0; i < to_copy; ++i)
        {
            pPresentModes[i] = modes[i];
        }
        *pCount = to_copy;
        return to_copy < available ? VK_INCOMPLETE : VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkBool32 VKAPI_CALL vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                                                                        uint32_t queueFamily)
    {
        if (native_wsi_enabled())
        {
            uint32_t count = 1;
            VkBool32 supported{};
            return native_query(nw::query::win32_support, physicalDevice, VK_NULL_HANDLE, queueFamily, &count, &supported,
                                sizeof(supported)) == VK_SUCCESS
                       ? supported
                       : VK_FALSE;
        }

        return VK_TRUE;
    }

    // Core 1.1: the bridge does not model external semaphores, so report none supported (callers fall
    // back to internal synchronization). Core, so a layer can call it without enabling an extension --
    // returning null from vkGetInstanceProcAddr for it would crash such a caller.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalSemaphoreProperties(
        VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo*, VkExternalSemaphoreProperties* pProperties)
    {
        if (pProperties)
        {
            pProperties->exportFromImportedHandleTypes = 0;
            pProperties->compatibleHandleTypes = 0;
            pProperties->externalSemaphoreFeatures = 0;
        }
    }

    // Core: no sparse residency support.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType,
                                                                                                    VkSampleCountFlagBits,
                                                                                                    VkImageUsageFlags, VkImageTiling,
                                                                                                    uint32_t* pPropertyCount,
                                                                                                    VkSparseImageFormatProperties*)
    {
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2(
        VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2*, uint32_t* pPropertyCount, VkSparseImageFormatProperties2*)
    {
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(
        VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkCooperativeMatrixPropertiesKHR* pProperties)
    {
        if (!pPropertyCount)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        gb::physical_device_enumeration_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.max_count = pProperties ? *pPropertyCount : 0;
        request.has_entries = pProperties != nullptr;

        const size_t capacity = request.max_count;
        if (capacity > (SIZE_MAX - sizeof(gb::physical_device_enumeration_response)) / sizeof(gb::cooperative_matrix_property))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const size_t buffer_size = sizeof(gb::physical_device_enumeration_response) + capacity * sizeof(gb::cooperative_matrix_property);
        if (buffer_size > MAXDWORD)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        std::vector<std::byte> buffer(buffer_size);
        if (!bridge_call(gb::ioctl_get_physical_device_cooperative_matrix_properties, &request, sizeof(request), buffer.data(),
                         static_cast<DWORD>(buffer.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto* response = reinterpret_cast<const gb::physical_device_enumeration_response*>(buffer.data());
        const uint32_t written = std::min<uint32_t>(response->count, request.max_count);
        const auto* entries =
            reinterpret_cast<const gb::cooperative_matrix_property*>(buffer.data() + sizeof(gb::physical_device_enumeration_response));
        for (uint32_t i = 0; i < written; ++i)
        {
            pProperties[i].sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
            pProperties[i].MSize = entries[i].m_size;
            pProperties[i].NSize = entries[i].n_size;
            pProperties[i].KSize = entries[i].k_size;
            pProperties[i].AType = static_cast<VkComponentTypeKHR>(entries[i].a_type);
            pProperties[i].BType = static_cast<VkComponentTypeKHR>(entries[i].b_type);
            pProperties[i].CType = static_cast<VkComponentTypeKHR>(entries[i].c_type);
            pProperties[i].ResultType = static_cast<VkComponentTypeKHR>(entries[i].result_type);
            pProperties[i].saturatingAccumulation = entries[i].saturating_accumulation;
            pProperties[i].scope = static_cast<VkScopeKHR>(entries[i].scope);
        }
        *pPropertyCount = response->count;
        return static_cast<VkResult>(response->vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceFragmentShadingRatesKHR(
        VkPhysicalDevice physicalDevice, uint32_t* pFragmentShadingRateCount, VkPhysicalDeviceFragmentShadingRateKHR* pFragmentShadingRates)
    {
        if (!pFragmentShadingRateCount)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        gb::physical_device_enumeration_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.max_count = pFragmentShadingRates ? *pFragmentShadingRateCount : 0;
        request.has_entries = pFragmentShadingRates != nullptr;

        const size_t capacity = request.max_count;
        if (capacity > (SIZE_MAX - sizeof(gb::physical_device_enumeration_response)) / sizeof(gb::fragment_shading_rate))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const size_t buffer_size = sizeof(gb::physical_device_enumeration_response) + capacity * sizeof(gb::fragment_shading_rate);
        if (buffer_size > MAXDWORD)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        std::vector<std::byte> buffer(buffer_size);
        if (!bridge_call(gb::ioctl_get_physical_device_fragment_shading_rates, &request, sizeof(request), buffer.data(),
                         static_cast<DWORD>(buffer.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto* response = reinterpret_cast<const gb::physical_device_enumeration_response*>(buffer.data());
        const uint32_t written = std::min<uint32_t>(response->count, request.max_count);
        const auto* entries =
            reinterpret_cast<const gb::fragment_shading_rate*>(buffer.data() + sizeof(gb::physical_device_enumeration_response));
        for (uint32_t i = 0; i < written; ++i)
        {
            pFragmentShadingRates[i].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
            pFragmentShadingRates[i].sampleCounts = entries[i].sample_counts;
            pFragmentShadingRates[i].fragmentSize = {.width = entries[i].width, .height = entries[i].height};
        }
        *pFragmentShadingRateCount = response->count;
        return static_cast<VkResult>(response->vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(VkPhysicalDevice physicalDevice,
                                                                                                        uint32_t* pTimeDomainCount,
                                                                                                        VkTimeDomainKHR* pTimeDomains)
    {
        if (!pTimeDomainCount)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        gb::physical_device_enumeration_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.max_count = pTimeDomains ? *pTimeDomainCount : 0;
        request.has_entries = pTimeDomains != nullptr;

        const size_t capacity = request.max_count;
        if (capacity > (SIZE_MAX - sizeof(gb::physical_device_enumeration_response)) / sizeof(uint32_t))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const size_t buffer_size = sizeof(gb::physical_device_enumeration_response) + capacity * sizeof(uint32_t);
        if (buffer_size > MAXDWORD)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        std::vector<std::byte> buffer(buffer_size);
        if (!bridge_call(gb::ioctl_get_physical_device_calibrateable_time_domains, &request, sizeof(request), buffer.data(),
                         static_cast<DWORD>(buffer.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto* response = reinterpret_cast<const gb::physical_device_enumeration_response*>(buffer.data());
        const uint32_t written = std::min<uint32_t>(response->count, request.max_count);
        const auto* entries = reinterpret_cast<const uint32_t*>(buffer.data() + sizeof(gb::physical_device_enumeration_response));
        for (uint32_t i = 0; i < written; ++i)
        {
            pTimeDomains[i] = static_cast<VkTimeDomainKHR>(entries[i]);
        }
        *pTimeDomainCount = response->count;
        return static_cast<VkResult>(response->vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(VkPhysicalDevice physicalDevice,
                                                                                                        uint32_t* pTimeDomainCount,
                                                                                                        VkTimeDomainKHR* pTimeDomains)
    {
        return vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(physicalDevice, pTimeDomainCount, pTimeDomains);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetCalibratedTimestampsKHR(VkDevice device, uint32_t timestampCount,
                                                                                      const VkCalibratedTimestampInfoKHR* pTimestampInfos,
                                                                                      uint64_t* pTimestamps, uint64_t* pMaxDeviation)
    {
        if (timestampCount == 0 || !pTimestampInfos || !pTimestamps || !pMaxDeviation)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (timestampCount > (MAXDWORD - sizeof(gb::get_calibrated_timestamps_response)) / sizeof(uint64_t))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        gb::get_calibrated_timestamps_request request{};
        request.device = to_object_id(device);
        request.timestamp_count = timestampCount;

        const size_t input_size = sizeof(request) + static_cast<size_t>(timestampCount) * sizeof(uint32_t);
        std::vector<std::byte> input(input_size);
        std::memcpy(input.data(), &request, sizeof(request));
        for (uint32_t i = 0; i < timestampCount; ++i)
        {
            const auto time_domain = static_cast<uint32_t>(pTimestampInfos[i].timeDomain);
            std::memcpy(input.data() + sizeof(request) + static_cast<size_t>(i) * sizeof(time_domain), &time_domain, sizeof(time_domain));
        }

        const size_t output_size = sizeof(gb::get_calibrated_timestamps_response) + static_cast<size_t>(timestampCount) * sizeof(uint64_t);
        std::vector<std::byte> output(output_size);
        if (!bridge_call(gb::ioctl_get_calibrated_timestamps, input.data(), static_cast<DWORD>(input.size()), output.data(),
                         static_cast<DWORD>(output.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto* response = reinterpret_cast<const gb::get_calibrated_timestamps_response*>(output.data());
        *pMaxDeviation = response->max_deviation;
        std::memcpy(pTimestamps, output.data() + sizeof(*response), static_cast<size_t>(timestampCount) * sizeof(uint64_t));
        return static_cast<VkResult>(response->vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetCalibratedTimestampsEXT(VkDevice device, uint32_t timestampCount,
                                                                                      const VkCalibratedTimestampInfoKHR* pTimestampInfos,
                                                                                      uint64_t* pTimestamps, uint64_t* pMaxDeviation)
    {
        return vkGetCalibratedTimestampsKHR(device, timestampCount, pTimestampInfos, pTimestamps, pMaxDeviation);
    }

    // VK_KHR_get_surface_capabilities2 / VK_EXT_surface_maintenance1: delegate to the KHR queries.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
                                               VkSurfaceCapabilities2KHR* pSurfaceCapabilities)
    {
        if (native_wsi_enabled() && (!pSurfaceInfo || !pSurfaceCapabilities || pSurfaceInfo->pNext || pSurfaceCapabilities->pNext))
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        if (!pSurfaceInfo || !pSurfaceCapabilities)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, pSurfaceInfo->surface, &pSurfaceCapabilities->surfaceCapabilities);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
                                          uint32_t* pSurfaceFormatCount, VkSurfaceFormat2KHR* pSurfaceFormats)
    {
        if (native_wsi_enabled())
        {
            return native_formats2(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
        }

        if (!pSurfaceInfo || !pSurfaceFormatCount)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!pSurfaceFormats)
        {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, pSurfaceInfo->surface, pSurfaceFormatCount, nullptr);
        }

        std::vector<VkSurfaceFormatKHR> formats(*pSurfaceFormatCount);
        uint32_t count = *pSurfaceFormatCount;
        const VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, pSurfaceInfo->surface, &count, formats.data());
        for (uint32_t i = 0; i < count; ++i)
        {
            pSurfaceFormats[i].surfaceFormat = formats[i];
        }
        *pSurfaceFormatCount = count;
        return result;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkGetPhysicalDeviceSurfacePresentModes2EXT(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
                                               uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes)
    {
        if (native_wsi_enabled() && (!pSurfaceInfo || pSurfaceInfo->pNext))
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        if (!pSurfaceInfo || !pPresentModeCount)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, pSurfaceInfo->surface, pPresentModeCount, pPresentModes);
    }

    // VK_EXT_swapchain_maintenance1: the bridge's readback present has nothing to release.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkReleaseSwapchainImagesEXT(VkDevice, const VkReleaseSwapchainImagesInfoEXT*)
    {
        if (native_wsi_enabled())
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                                              const char* /*pLayerName*/,
                                                                                              uint32_t* pPropertyCount,
                                                                                              VkExtensionProperties* pProperties)
    {
        if (!pPropertyCount)
        {
            return VK_INCOMPLETE;
        }

        gb::enumerate_device_extension_properties_request request{};
        request.physical_device = to_object_id(physicalDevice);
        request.max_count = pProperties ? *pPropertyCount : 0;

        std::vector<std::byte> buffer(sizeof(gb::enumerate_device_extension_properties_response) +
                                      static_cast<size_t>(request.max_count) * sizeof(VkExtensionProperties));
        if (!bridge_call(gb::ioctl_enumerate_device_extension_properties, &request, sizeof(request), buffer.data(),
                         static_cast<DWORD>(buffer.size())))
        {
            *pPropertyCount = 0;
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto* response = reinterpret_cast<const gb::enumerate_device_extension_properties_response*>(buffer.data());
        if (response->vk_result != VK_SUCCESS)
        {
            *pPropertyCount = 0;
            return static_cast<VkResult>(response->vk_result);
        }

        if (!pProperties)
        {
            *pPropertyCount = response->count;
            return VK_SUCCESS;
        }

        const uint32_t written = (response->count < *pPropertyCount) ? response->count : *pPropertyCount;
        const auto* extensions =
            reinterpret_cast<const VkExtensionProperties*>(buffer.data() + sizeof(gb::enumerate_device_extension_properties_response));
        for (uint32_t i = 0; i < written; ++i)
        {
            pProperties[i] = extensions[i];
        }
        *pPropertyCount = written;
        return written < response->count ? VK_INCOMPLETE : VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                                              const VkAllocationCallbacks*, VkSwapchainKHR* pSwapchain)
    {
        if (native_wsi_enabled())
        {
            return native_create_swapchain(device, pCreateInfo, pSwapchain);
        }

        gb::create_swapchain_request request{};
        request.device = to_object_id(device);
        request.surface = to_object_id(pCreateInfo->surface);
        request.format = static_cast<uint32_t>(pCreateInfo->imageFormat);
        request.width = pCreateInfo->imageExtent.width;
        request.height = pCreateInfo->imageExtent.height;
        request.min_image_count = pCreateInfo->minImageCount;
        request.image_usage = pCreateInfo->imageUsage;
        request.present_mode = static_cast<uint32_t>(pCreateInfo->presentMode);

        gb::create_swapchain_response response{};
        if (!bridge_call(gb::ioctl_create_swapchain, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pSwapchain = to_handle<VkSwapchainKHR>(response.swapchain);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                                           const VkAllocationCallbacks*)
    {
        if (native_wsi_enabled())
        {
            if (swapchain)
            {
                native_destroy(nw::operation::destroy_swapchain, to_object_id(device), to_object_id(swapchain));
            }
            return;
        }

        if (!swapchain)
        {
            return;
        }
        gb::destroy_swapchain_request request{};
        request.device = to_object_id(device);
        request.swapchain = to_object_id(swapchain);
        bridge_call(gb::ioctl_destroy_swapchain, &request, sizeof(request), nullptr, 0);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                                                 uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages)
    {
        if (native_wsi_enabled())
        {
            return native_images(device, swapchain, pSwapchainImageCount, pSwapchainImages);
        }

        const uint32_t capacity = pSwapchainImages ? *pSwapchainImageCount : 0;

        gb::get_swapchain_images_request request{};
        request.swapchain = to_object_id(swapchain);
        request.max_count = capacity;

        std::vector<uint8_t> out(sizeof(gb::get_swapchain_images_response) + capacity * sizeof(gb::object_id));
        if (!bridge_call(gb::ioctl_get_swapchain_images, &request, sizeof(request), out.data(), static_cast<DWORD>(out.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        gb::get_swapchain_images_response header{};
        std::memcpy(&header, out.data(), sizeof(header));
        if (header.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(header.vk_result);
        }

        if (!pSwapchainImages)
        {
            *pSwapchainImageCount = header.count;
            return VK_SUCCESS;
        }

        const uint32_t to_write = (header.count < capacity) ? header.count : capacity;
        const auto* ids = reinterpret_cast<const gb::object_id*>(out.data() + sizeof(header));
        for (uint32_t i = 0; i < to_write; ++i)
        {
            pSwapchainImages[i] = to_handle<VkImage>(ids[i]);
        }
        *pSwapchainImageCount = to_write;
        return (to_write < header.count) ? VK_INCOMPLETE : VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                                               VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex)
    {
        if (native_wsi_enabled())
        {
            return native_acquire(device, swapchain, timeout, semaphore, fence, pImageIndex);
        }

        // The image is always immediately available, but the caller makes its render submit wait on the
        // semaphore (and may wait on the fence), so they must still be signalled by the bridge.
        gb::acquire_next_image_request request{};
        request.swapchain = to_object_id(swapchain);
        request.semaphore = to_object_id(semaphore);
        request.fence = to_object_id(fence);

        gb::acquire_next_image_response response{};
        if (!bridge_call(gb::ioctl_acquire_next_image, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pImageIndex = response.image_index;
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info,
                                                                                uint32_t* index)
    {
        if (!native_wsi_enabled() || !info || info->sType != VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR || info->pNext ||
            info->deviceMask != 1)
        {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        return native_acquire(device, info->swapchain, info->timeout, info->semaphore, info->fence, index);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        if (native_wsi_enabled())
        {
            return native_present(queue, pPresentInfo);
        }

        VkResult overall = VK_SUCCESS;
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
        {
            const uint32_t wait_count = i == 0 ? pPresentInfo->waitSemaphoreCount : 0;
            std::vector<uint8_t> message(sizeof(gb::queue_present_request) + static_cast<size_t>(wait_count) * sizeof(gb::object_id));
            auto* request = reinterpret_cast<gb::queue_present_request*>(message.data());
            request->queue = to_object_id(queue);
            request->swapchain = to_object_id(pPresentInfo->pSwapchains[i]);
            request->image_index = pPresentInfo->pImageIndices[i];
            request->wait_semaphore_count = wait_count;

            auto* waits = reinterpret_cast<gb::object_id*>(message.data() + sizeof(*request));
            for (uint32_t wait_index = 0; wait_index < wait_count; ++wait_index)
            {
                waits[wait_index] = to_object_id(pPresentInfo->pWaitSemaphores[wait_index]);
            }

            gb::result_response response{};
            const bool ok =
                bridge_call(gb::ioctl_queue_present, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response));
            const VkResult result = ok ? static_cast<VkResult>(response.vk_result) : VK_ERROR_INITIALIZATION_FAILED;
            if (pPresentInfo->pResults)
            {
                pPresentInfo->pResults[i] = result;
            }
            if (result != VK_SUCCESS)
            {
                overall = result;
            }
        }
        return overall;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo,
                                                                              const VkAllocationCallbacks*, VkShaderModule* pShaderModule)
    {
        if (pCreateInfo->codeSize > UINT32_MAX - sizeof(gb::create_shader_module_request))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const auto code_size = static_cast<uint32_t>(pCreateInfo->codeSize);
        std::vector<uint8_t> message(sizeof(gb::create_shader_module_request) + code_size);
        gb::create_shader_module_request header{};
        header.device = to_object_id(device);
        header.code_size = code_size;
        header.flags = static_cast<uint32_t>(pCreateInfo->flags);
        std::memcpy(message.data(), &header, sizeof(header));
        std::memcpy(message.data() + sizeof(header), pCreateInfo->pCode, code_size);

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_shader_module, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pShaderModule = to_handle<VkShaderModule>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule,
                                                                           const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_shader_module, device, shaderModule);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetShaderModuleIdentifierEXT(VkDevice device, VkShaderModule shaderModule,
                                                                                    VkShaderModuleIdentifierEXT* pIdentifier)
    {
        gb::device_child_request request{};
        request.device = to_object_id(device);
        request.object = to_object_id(shaderModule);

        gb::shader_module_identifier_response response{};
        if (!bridge_call(gb::ioctl_get_shader_module_identifier, &request, sizeof(request), &response, sizeof(response)) ||
            response.vk_result != VK_SUCCESS)
        {
            pIdentifier->identifierSize = 0;
            return;
        }

        pIdentifier->identifierSize = std::min<uint32_t>(response.identifier_size, VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT);
        std::memcpy(pIdentifier->identifier, response.identifier.data(), pIdentifier->identifierSize);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetShaderModuleCreateInfoIdentifierEXT(VkDevice device,
                                                                                              const VkShaderModuleCreateInfo* pCreateInfo,
                                                                                              VkShaderModuleIdentifierEXT* pIdentifier)
    {
        pIdentifier->identifierSize = 0;
        if (pCreateInfo->codeSize > UINT32_MAX - sizeof(gb::create_shader_module_request))
        {
            return;
        }

        const auto code_size = static_cast<uint32_t>(pCreateInfo->codeSize);
        std::vector<uint8_t> message(sizeof(gb::create_shader_module_request) + code_size);
        gb::create_shader_module_request header{};
        header.device = to_object_id(device);
        header.code_size = code_size;
        header.flags = static_cast<uint32_t>(pCreateInfo->flags);
        std::memcpy(message.data(), &header, sizeof(header));
        std::memcpy(message.data() + sizeof(header), pCreateInfo->pCode, code_size);

        gb::shader_module_identifier_response response{};
        if (!bridge_call(gb::ioctl_get_shader_module_create_info_identifier, message.data(), static_cast<DWORD>(message.size()), &response,
                         sizeof(response)) ||
            response.vk_result != VK_SUCCESS)
        {
            return;
        }

        pIdentifier->identifierSize = std::min<uint32_t>(response.identifier_size, VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT);
        std::memcpy(pIdentifier->identifier, response.identifier.data(), pIdentifier->identifierSize);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo,
                                                                           const VkAllocationCallbacks*, VkImageView* pView)
    {
        gb::create_image_view_request request{};
        request.device = to_object_id(device);
        request.image = to_object_id(pCreateInfo->image);
        request.format = static_cast<uint32_t>(pCreateInfo->format);
        request.aspect_mask = pCreateInfo->subresourceRange.aspectMask;
        request.view_type = static_cast<uint32_t>(pCreateInfo->viewType);
        request.base_mip_level = pCreateInfo->subresourceRange.baseMipLevel;
        request.level_count = pCreateInfo->subresourceRange.levelCount;
        request.base_array_layer = pCreateInfo->subresourceRange.baseArrayLayer;
        request.layer_count = pCreateInfo->subresourceRange.layerCount;
        request.swizzle_r = static_cast<uint32_t>(pCreateInfo->components.r);
        request.swizzle_g = static_cast<uint32_t>(pCreateInfo->components.g);
        request.swizzle_b = static_cast<uint32_t>(pCreateInfo->components.b);
        request.swizzle_a = static_cast<uint32_t>(pCreateInfo->components.a);

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_image_view, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pView = to_handle<VkImageView>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView imageView,
                                                                        const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_image_view, device, imageView);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                                                                             const VkAllocationCallbacks* pAllocator,
                                                                             VkRenderPass* pRenderPass)
    {
        return create_render_pass_object(gb::ioctl_create_render_pass2, device, pCreateInfo, pAllocator, pRenderPass);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                                                                                const VkAllocationCallbacks* pAllocator,
                                                                                VkRenderPass* pRenderPass)
    {
        return vkCreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                                                                           const VkRenderPassBeginInfo* pRenderPassBegin,
                                                                           const VkSubpassBeginInfo* pSubpassBeginInfo)
    {
        record_render_pass(commandBuffer, gb::command::cmd_begin_render_pass2, pRenderPassBegin, pSubpassBeginInfo);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer,
                                                                              const VkRenderPassBeginInfo* pRenderPassBegin,
                                                                              const VkSubpassBeginInfo* pSubpassBeginInfo)
    {
        vkCmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2(VkCommandBuffer commandBuffer,
                                                                       const VkSubpassBeginInfo* pSubpassBeginInfo,
                                                                       const VkSubpassEndInfo* pSubpassEndInfo)
    {
        record_render_pass(commandBuffer, gb::command::cmd_next_subpass2, pSubpassBeginInfo, pSubpassEndInfo);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2KHR(VkCommandBuffer commandBuffer,
                                                                          const VkSubpassBeginInfo* pSubpassBeginInfo,
                                                                          const VkSubpassEndInfo* pSubpassEndInfo)
    {
        vkCmdNextSubpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2(VkCommandBuffer commandBuffer,
                                                                         const VkSubpassEndInfo* pSubpassEndInfo)
    {
        record_render_pass(commandBuffer, gb::command::cmd_end_render_pass2, pSubpassEndInfo);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2KHR(VkCommandBuffer commandBuffer,
                                                                            const VkSubpassEndInfo* pSubpassEndInfo)
    {
        vkCmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetRenderAreaGranularity(VkDevice device, VkRenderPass renderPass,
                                                                                VkExtent2D* pGranularity)
    {
        if (!pGranularity)
        {
            return;
        }
        *pGranularity = {};
        const gb::device_child_request request{.device = to_object_id(device), .object = to_object_id(renderPass)};
        gb::render_area_granularity_response response{};
        if (bridge_call(gb::ioctl_get_render_area_granularity, &request, sizeof(request), &response, sizeof(response)) &&
            response.vk_result == VK_SUCCESS)
        {
            *pGranularity = {.width = response.width, .height = response.height};
        }
        else
        {
            OutputDebugStringA("[vulkan-shim] vkGetRenderAreaGranularity failed\n");
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetRenderingAreaGranularity(VkDevice device,
                                                                                   const VkRenderingAreaInfo* pRenderingAreaInfo,
                                                                                   VkExtent2D* pGranularity)
    {
        if (!pGranularity)
        {
            OutputDebugStringA("[vulkan-shim] vkGetRenderingAreaGranularity: null output\n");
            return;
        }
        *pGranularity = {};
        if (!pRenderingAreaInfo)
        {
            OutputDebugStringA("[vulkan-shim] vkGetRenderingAreaGranularity: null input\n");
            return;
        }
        try
        {
            const auto packet = render_pass_packet(to_object_id(device), *pRenderingAreaInfo);
            gb::render_area_granularity_response response{};
            if (bridge_call(gb::ioctl_get_rendering_area_granularity, packet.data(), static_cast<DWORD>(packet.size()), &response,
                            sizeof(response)) &&
                response.vk_result == VK_SUCCESS)
            {
                *pGranularity = {.width = response.width, .height = response.height};
            }
            else
            {
                OutputDebugStringA("[vulkan-shim] vkGetRenderingAreaGranularity: native query unavailable or bridge failure\n");
            }
        }
        catch (const gb::render_pass_wire::error& error)
        {
            OutputDebugStringA(error.what());
        }
        catch (const std::bad_alloc&)
        {
            OutputDebugStringA("[vulkan-shim] vkGetRenderingAreaGranularity: out of memory\n");
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetRenderingAreaGranularityKHR(VkDevice device,
                                                                                      const VkRenderingAreaInfo* pRenderingAreaInfo,
                                                                                      VkExtent2D* pGranularity)
    {
        vkGetRenderingAreaGranularity(device, pRenderingAreaInfo, pGranularity);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo,
                                                                            const VkAllocationCallbacks* pAllocator,
                                                                            VkRenderPass* pRenderPass)
    {
        // The original reduced packet used the interpretation below; the full packet now retains every attachment and subpass.
        // The first attachment drives the color attachment; a subpass depth-stencil attachment (if any)
        // contributes its format so the bridge adds a matching depth attachment.
        return create_render_pass_object(gb::ioctl_create_render_pass_full, device, pCreateInfo, pAllocator, pRenderPass);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass,
                                                                         const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_render_pass, device, renderPass);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* pCreateInfo,
                                                                             const VkAllocationCallbacks* pAllocator,
                                                                             VkFramebuffer* pFramebuffer)
    {
        // color attachment
        // All attachments and layers are marshalled together; imageless attachments arrive at render-pass begin.
        return create_render_pass_object(gb::ioctl_create_framebuffer_full, device, pCreateInfo, pAllocator, pFramebuffer);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                                                                          const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_framebuffer, device, framebuffer);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device,
                                                                                const VkPipelineLayoutCreateInfo* pCreateInfo,
                                                                                const VkAllocationCallbacks*,
                                                                                VkPipelineLayout* pPipelineLayout)
    {
        gb::create_pipeline_layout_request request{};
        request.device = to_object_id(device);
        // One push-constant block from offset 0 is modeled: OR the stages, take the widest extent.
        for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; ++i)
        {
            const VkPushConstantRange& r = pCreateInfo->pPushConstantRanges[i];
            request.push_constant_stages |= r.stageFlags;
            request.push_constant_size = std::max(request.push_constant_size, r.offset + r.size);
        }
        request.set_layout_count = pCreateInfo->setLayoutCount;

        // The descriptor-set-layout ids trail the header.
        std::vector<uint8_t> message(sizeof(request) + static_cast<size_t>(request.set_layout_count) * sizeof(gb::object_id));
        std::memcpy(message.data(), &request, sizeof(request));
        for (uint32_t i = 0; i < request.set_layout_count; ++i)
        {
            const gb::object_id id = to_object_id(pCreateInfo->pSetLayouts[i]);
            std::memcpy(message.data() + sizeof(request) + i * sizeof(id), &id, sizeof(id));
        }

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_pipeline_layout, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pPipelineLayout = to_handle<VkPipelineLayout>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout pipelineLayout,
                                                                             const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_pipeline_layout, device, pipelineLayout);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device,
                                                                                     const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                                                                                     const VkAllocationCallbacks*,
                                                                                     VkDescriptorSetLayout* pSetLayout)
    {
        gb::create_descriptor_set_layout_request header{};
        header.device = to_object_id(device);
        header.binding_count = pCreateInfo->bindingCount;
        header.flags = pCreateInfo->flags;

        const VkDescriptorSetLayoutBindingFlagsCreateInfo* binding_flags = nullptr;
        for (const auto* next = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); next; next = next->pNext)
        {
            if (next->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO)
            {
                binding_flags = reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(next);
                break;
            }
        }

        std::vector<uint8_t> message(sizeof(header) +
                                     static_cast<size_t>(header.binding_count) * sizeof(gb::descriptor_set_layout_binding));
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < header.binding_count; ++i)
        {
            const VkDescriptorSetLayoutBinding& b = pCreateInfo->pBindings[i];
            gb::descriptor_set_layout_binding wire{};
            wire.binding = b.binding;
            wire.descriptor_type = static_cast<uint32_t>(b.descriptorType);
            wire.descriptor_count = b.descriptorCount;
            wire.stage_flags = b.stageFlags;
            if (binding_flags && i < binding_flags->bindingCount)
            {
                wire.binding_flags = binding_flags->pBindingFlags[i];
            }
            std::memcpy(message.data() + sizeof(header) + i * sizeof(wire), &wire, sizeof(wire));
        }

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_descriptor_set_layout, message.data(), static_cast<DWORD>(message.size()), &response,
                         sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pSetLayout = to_handle<VkDescriptorSetLayout>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice device,
                                                                                  VkDescriptorSetLayout descriptorSetLayout,
                                                                                  const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_descriptor_set_layout, device, descriptorSetLayout);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice device,
                                                                                const VkDescriptorPoolCreateInfo* pCreateInfo,
                                                                                const VkAllocationCallbacks*,
                                                                                VkDescriptorPool* pDescriptorPool)
    {
        gb::create_descriptor_pool_request header{};
        header.device = to_object_id(device);
        header.max_sets = pCreateInfo->maxSets;
        header.pool_size_count = pCreateInfo->poolSizeCount;
        header.flags = pCreateInfo->flags;
        for (const auto* next = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); next; next = next->pNext)
        {
            if (next->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO)
            {
                const auto* inline_uniform_blocks = reinterpret_cast<const VkDescriptorPoolInlineUniformBlockCreateInfo*>(next);
                header.max_inline_uniform_block_bindings = inline_uniform_blocks->maxInlineUniformBlockBindings;
                break;
            }
        }

        std::vector<uint8_t> message(sizeof(header) + static_cast<size_t>(header.pool_size_count) * sizeof(gb::descriptor_pool_size));
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < header.pool_size_count; ++i)
        {
            gb::descriptor_pool_size wire{};
            wire.descriptor_type = static_cast<uint32_t>(pCreateInfo->pPoolSizes[i].type);
            wire.descriptor_count = pCreateInfo->pPoolSizes[i].descriptorCount;
            std::memcpy(message.data() + sizeof(header) + i * sizeof(wire), &wire, sizeof(wire));
        }

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_descriptor_pool, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }
        *pDescriptorPool = to_handle<VkDescriptorPool>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool,
                                                                             const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_descriptor_pool, device, descriptorPool);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool,
                                                                               VkDescriptorPoolResetFlags flags)
    {
        gb::reset_descriptor_pool_request request{};
        request.device = to_object_id(device);
        request.descriptor_pool = to_object_id(descriptorPool);
        request.flags = flags;

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_reset_descriptor_pool, &request, sizeof(request), &response, sizeof(response)) ||
            response.vk_result != VK_SUCCESS)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice device,
                                                                                  const VkDescriptorSetAllocateInfo* pAllocateInfo,
                                                                                  VkDescriptorSet* pDescriptorSets)
    {
        const uint32_t count = pAllocateInfo->descriptorSetCount;
        const VkDescriptorSetVariableDescriptorCountAllocateInfo* variable_descriptor_counts = nullptr;
        for (const auto* next = static_cast<const VkBaseInStructure*>(pAllocateInfo->pNext); next; next = next->pNext)
        {
            if (next->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO)
            {
                variable_descriptor_counts = reinterpret_cast<const VkDescriptorSetVariableDescriptorCountAllocateInfo*>(next);
                break;
            }
        }
        const uint32_t variable_descriptor_count_count = variable_descriptor_counts ? variable_descriptor_counts->descriptorSetCount : 0;
        if (variable_descriptor_count_count != 0 &&
            (variable_descriptor_count_count != count || !variable_descriptor_counts->pDescriptorCounts))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        gb::allocate_descriptor_sets_request header{};
        header.device = to_object_id(device);
        header.descriptor_pool = to_object_id(pAllocateInfo->descriptorPool);
        header.set_count = count;
        header.variable_descriptor_count_count = variable_descriptor_count_count;

        const size_t layouts_size = static_cast<size_t>(count) * sizeof(gb::object_id);
        const size_t variable_counts_size = static_cast<size_t>(header.variable_descriptor_count_count) * sizeof(uint32_t);
        std::vector<uint8_t> message(sizeof(header) + layouts_size + variable_counts_size);
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < count; ++i)
        {
            const gb::object_id id = to_object_id(pAllocateInfo->pSetLayouts[i]);
            std::memcpy(message.data() + sizeof(header) + i * sizeof(id), &id, sizeof(id));
        }
        if (variable_counts_size != 0)
        {
            std::memcpy(message.data() + sizeof(header) + layouts_size, variable_descriptor_counts->pDescriptorCounts,
                        variable_counts_size);
        }

        std::vector<uint8_t> out(sizeof(gb::allocate_descriptor_sets_response) + static_cast<size_t>(count) * sizeof(gb::object_id));
        if (!bridge_call(gb::ioctl_allocate_descriptor_sets, message.data(), static_cast<DWORD>(message.size()), out.data(),
                         static_cast<DWORD>(out.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        gb::allocate_descriptor_sets_response resp_header{};
        std::memcpy(&resp_header, out.data(), sizeof(resp_header));
        if (resp_header.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(resp_header.vk_result);
        }

        const auto* ids = reinterpret_cast<const gb::object_id*>(out.data() + sizeof(resp_header));
        const uint32_t written = (resp_header.count < count) ? resp_header.count : count;
        for (uint32_t i = 0; i < written; ++i)
        {
            pDescriptorSets[i] = to_handle<VkDescriptorSet>(ids[i]);
        }
        return VK_SUCCESS;
    }

    namespace
    {
        bool is_image_descriptor(VkDescriptorType type)
        {
            return type == VK_DESCRIPTOR_TYPE_SAMPLER || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                   type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                   type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        }

        bool is_texel_buffer_descriptor(VkDescriptorType type)
        {
            return type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
                                                                            const VkWriteDescriptorSet* pDescriptorWrites, uint32_t,
                                                                            const VkCopyDescriptorSet*)
    {
        // Descriptor copies are not modeled; only writes are forwarded. Non-inline writes are flattened
        // to `descriptorCount` single-descriptor wire writes.
        std::vector<gb::descriptor_write> writes;
        std::vector<uint8_t> inline_uniform_data;
        for (uint32_t w = 0; w < descriptorWriteCount; ++w)
        {
            const VkWriteDescriptorSet& src = pDescriptorWrites[w];
            if (src.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
            {
                const VkWriteDescriptorSetInlineUniformBlock* inline_uniform_block = nullptr;
                for (const auto* next = static_cast<const VkBaseInStructure*>(src.pNext); next; next = next->pNext)
                {
                    if (next->sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK)
                    {
                        inline_uniform_block = reinterpret_cast<const VkWriteDescriptorSetInlineUniformBlock*>(next);
                        break;
                    }
                }
                if (!inline_uniform_block || inline_uniform_block->dataSize != src.descriptorCount ||
                    (inline_uniform_block->dataSize != 0 && !inline_uniform_block->pData) ||
                    inline_uniform_data.size() > UINT32_MAX - inline_uniform_block->dataSize)
                {
                    continue;
                }

                gb::descriptor_write wire{};
                wire.dst_set = to_object_id(src.dstSet);
                wire.dst_binding = src.dstBinding;
                wire.dst_array_element = src.dstArrayElement;
                wire.descriptor_type = static_cast<uint32_t>(src.descriptorType);
                wire.inline_uniform_data_offset = static_cast<uint32_t>(inline_uniform_data.size());
                wire.inline_uniform_data_size = inline_uniform_block->dataSize;
                if (inline_uniform_block->dataSize != 0)
                {
                    const auto* data = static_cast<const uint8_t*>(inline_uniform_block->pData);
                    inline_uniform_data.insert(inline_uniform_data.end(), data, data + inline_uniform_block->dataSize);
                }
                writes.push_back(wire);
                continue;
            }

            for (uint32_t e = 0; e < src.descriptorCount; ++e)
            {
                gb::descriptor_write wire{};
                wire.dst_set = to_object_id(src.dstSet);
                wire.dst_binding = src.dstBinding;
                wire.dst_array_element = src.dstArrayElement + e;
                wire.descriptor_type = static_cast<uint32_t>(src.descriptorType);
                if (is_image_descriptor(src.descriptorType))
                {
                    wire.sampler = to_object_id(src.pImageInfo[e].sampler);
                    wire.image_view = to_object_id(src.pImageInfo[e].imageView);
                    wire.image_layout = static_cast<uint32_t>(src.pImageInfo[e].imageLayout);
                }
                else if (is_texel_buffer_descriptor(src.descriptorType))
                {
                    wire.buffer_or_view = to_object_id(src.pTexelBufferView[e]);
                }
                else
                {
                    wire.buffer_or_view = to_object_id(src.pBufferInfo[e].buffer);
                    wire.offset = src.pBufferInfo[e].offset;
                    wire.range = src.pBufferInfo[e].range;
                }
                writes.push_back(wire);
            }
        }

        gb::update_descriptor_sets_request header{};
        header.device = to_object_id(device);
        header.write_count = static_cast<uint32_t>(writes.size());
        header.inline_uniform_data_size = static_cast<uint32_t>(inline_uniform_data.size());

        // Append to the pending batch instead of issuing an IOCTL now (drained before the next bridge call).
        const auto* header_bytes = reinterpret_cast<const uint8_t*>(&header);
        const auto* write_bytes = reinterpret_cast<const uint8_t*>(writes.data());
        std::lock_guard<std::mutex> lock(g_pending_descriptor_updates_mutex);
        g_pending_descriptor_updates.insert(g_pending_descriptor_updates.end(), header_bytes, header_bytes + sizeof(header));
        if (!writes.empty())
        {
            g_pending_descriptor_updates.insert(g_pending_descriptor_updates.end(), write_bytes,
                                                write_bytes + writes.size() * sizeof(gb::descriptor_write));
        }
        g_pending_descriptor_updates.insert(g_pending_descriptor_updates.end(), inline_uniform_data.begin(), inline_uniform_data.end());
    }

    // Descriptor update templates are lowered entirely inside the shim: the template definition is kept
    // host-side (guest-side, in the shim) and vkUpdateDescriptorSetWithTemplate gathers the strided
    // caller data into ordinary VkWriteDescriptorSet records, reusing vkUpdateDescriptorSets. This avoids
    // a dedicated bridge command for an otherwise pure convenience API.
    namespace
    {
        struct shim_descriptor_update_template
        {
            std::vector<VkDescriptorUpdateTemplateEntry> entries;
        };
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkCreateDescriptorUpdateTemplate(VkDevice, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo, const VkAllocationCallbacks*,
                                     VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate)
    {
        if (!pCreateInfo || !pDescriptorUpdateTemplate)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto* tmpl = new (std::nothrow) shim_descriptor_update_template{};
        if (!tmpl)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        tmpl->entries.assign(pCreateInfo->pDescriptorUpdateEntries,
                             pCreateInfo->pDescriptorUpdateEntries + pCreateInfo->descriptorUpdateEntryCount);
        *pDescriptorUpdateTemplate = reinterpret_cast<VkDescriptorUpdateTemplate>(tmpl);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplate(VkDevice,
                                                                                       VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                                                                       const VkAllocationCallbacks*)
    {
        delete reinterpret_cast<shim_descriptor_update_template*>(descriptorUpdateTemplate);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet descriptorSet,
                                                                                       VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                                                                       const void* pData)
    {
        const auto* tmpl = reinterpret_cast<const shim_descriptor_update_template*>(descriptorUpdateTemplate);
        if (!tmpl || !pData)
        {
            return;
        }

        std::vector<VkWriteDescriptorSet> writes;
        std::vector<std::vector<VkDescriptorImageInfo>> image_infos;
        std::vector<std::vector<VkDescriptorBufferInfo>> buffer_infos;
        std::vector<std::vector<VkBufferView>> texel_views;
        std::vector<VkWriteDescriptorSetInlineUniformBlock> inline_uniform_blocks;
        writes.reserve(tmpl->entries.size());
        image_infos.reserve(tmpl->entries.size());
        buffer_infos.reserve(tmpl->entries.size());
        texel_views.reserve(tmpl->entries.size());
        inline_uniform_blocks.reserve(tmpl->entries.size());

        const auto* base = static_cast<const uint8_t*>(pData);
        for (const auto& entry : tmpl->entries)
        {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSet;
            write.dstBinding = entry.dstBinding;
            write.dstArrayElement = entry.dstArrayElement;
            write.descriptorCount = entry.descriptorCount;
            write.descriptorType = entry.descriptorType;

            if (entry.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
            {
                auto& inline_uniform_block = inline_uniform_blocks.emplace_back();
                inline_uniform_block.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK;
                inline_uniform_block.dataSize = entry.descriptorCount;
                inline_uniform_block.pData = base + entry.offset;
                write.pNext = &inline_uniform_block;
            }
            else if (is_image_descriptor(entry.descriptorType))
            {
                auto& arr = image_infos.emplace_back();
                arr.reserve(entry.descriptorCount);
                for (uint32_t e = 0; e < entry.descriptorCount; ++e)
                {
                    arr.push_back(*reinterpret_cast<const VkDescriptorImageInfo*>(base + entry.offset + e * entry.stride));
                }
                write.pImageInfo = arr.data();
            }
            else if (is_texel_buffer_descriptor(entry.descriptorType))
            {
                auto& arr = texel_views.emplace_back();
                arr.reserve(entry.descriptorCount);
                for (uint32_t e = 0; e < entry.descriptorCount; ++e)
                {
                    arr.push_back(*reinterpret_cast<const VkBufferView*>(base + entry.offset + e * entry.stride));
                }
                write.pTexelBufferView = arr.data();
            }
            else
            {
                auto& arr = buffer_infos.emplace_back();
                arr.reserve(entry.descriptorCount);
                for (uint32_t e = 0; e < entry.descriptorCount; ++e)
                {
                    arr.push_back(*reinterpret_cast<const VkDescriptorBufferInfo*>(base + entry.offset + e * entry.stride));
                }
                write.pBufferInfo = arr.data();
            }

            writes.push_back(write);
        }

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo,
                                                                         const VkAllocationCallbacks*, VkSampler* pSampler)
    {
        gb::create_sampler_request request{};
        request.device = to_object_id(device);
        request.mag_filter = static_cast<uint32_t>(pCreateInfo->magFilter);
        request.min_filter = static_cast<uint32_t>(pCreateInfo->minFilter);
        request.address_mode_u = static_cast<uint32_t>(pCreateInfo->addressModeU);
        request.address_mode_v = static_cast<uint32_t>(pCreateInfo->addressModeV);
        request.address_mode_w = static_cast<uint32_t>(pCreateInfo->addressModeW);
        request.mipmap_mode = static_cast<uint32_t>(pCreateInfo->mipmapMode);
        request.compare_enable = pCreateInfo->compareEnable;
        request.compare_op = static_cast<uint32_t>(pCreateInfo->compareOp);
        request.anisotropy_enable = pCreateInfo->anisotropyEnable;
        request.border_color = static_cast<uint32_t>(pCreateInfo->borderColor);
        request.mip_lod_bias = pCreateInfo->mipLodBias;
        request.max_anisotropy = pCreateInfo->maxAnisotropy;
        request.min_lod = pCreateInfo->minLod;
        request.max_lod = pCreateInfo->maxLod;

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_sampler, &request, sizeof(request), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (response.vk_result != VK_SUCCESS)
        {
            return static_cast<VkResult>(response.vk_result);
        }

        *pSampler = to_handle<VkSampler>(response.object);
        return VK_SUCCESS;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice device, VkDescriptorPool descriptorPool,
                                                                              uint32_t descriptorSetCount,
                                                                              const VkDescriptorSet* pDescriptorSets)
    {
        std::vector<uint8_t> message(sizeof(gb::free_descriptor_sets_request) +
                                     static_cast<size_t>(descriptorSetCount) * sizeof(gb::object_id));
        auto* request = reinterpret_cast<gb::free_descriptor_sets_request*>(message.data());
        request->device = to_object_id(device);
        request->descriptor_pool = to_object_id(descriptorPool);
        request->set_count = descriptorSetCount;

        auto* sets = reinterpret_cast<gb::object_id*>(message.data() + sizeof(*request));
        for (uint32_t i = 0; i < descriptorSetCount; ++i)
        {
            sets[i] = to_object_id(pDescriptorSets[i]);
        }

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_free_descriptor_sets, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutSupport(VkDevice device,
                                                                                     const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                                                                                     VkDescriptorSetLayoutSupport* pSupport)
    {
        if (!pSupport)
        {
            return;
        }

        pSupport->supported = VK_FALSE;

        // Initialize the one output-chain structure relevant to descriptor-layout support. Unknown output
        // structures make the query unsupported rather than leaving partially initialized data behind.
        bool unsupported_output_chain = false;
        VkDescriptorSetVariableDescriptorCountLayoutSupport* variable_support = nullptr;
        for (auto* base = static_cast<VkBaseOutStructure*>(pSupport->pNext); base != nullptr; base = base->pNext)
        {
            if (base->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT)
            {
                variable_support = reinterpret_cast<VkDescriptorSetVariableDescriptorCountLayoutSupport*>(base);
                variable_support->maxVariableDescriptorCount = 0;
            }
            else
            {
                unsupported_output_chain = true;
            }
        }

        if (unsupported_output_chain || !pCreateInfo || (pCreateInfo->bindingCount != 0 && !pCreateInfo->pBindings))
        {
            return;
        }

        const VkDescriptorSetLayoutBindingFlagsCreateInfo* binding_flags = nullptr;
        for (const auto* next = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); next; next = next->pNext)
        {
            if (next->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO)
            {
                return;
            }
            binding_flags = reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(next);
        }
        if (binding_flags && binding_flags->bindingCount != 0 &&
            (binding_flags->bindingCount != pCreateInfo->bindingCount || !binding_flags->pBindingFlags))
        {
            return;
        }
        for (uint32_t i = 0; i < pCreateInfo->bindingCount; ++i)
        {
            if (pCreateInfo->pBindings[i].pImmutableSamplers != nullptr)
            {
                return;
            }
        }

        gb::get_descriptor_set_layout_support_request header{};
        header.device = to_object_id(device);
        header.binding_count = pCreateInfo->bindingCount;
        header.flags = pCreateInfo->flags;
        if (header.binding_count > (static_cast<size_t>(UINT32_MAX) - sizeof(header)) / sizeof(gb::descriptor_set_layout_binding))
        {
            return;
        }

        std::vector<uint8_t> message(sizeof(header) +
                                     static_cast<size_t>(header.binding_count) * sizeof(gb::descriptor_set_layout_binding));
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < header.binding_count; ++i)
        {
            const VkDescriptorSetLayoutBinding& b = pCreateInfo->pBindings[i];
            gb::descriptor_set_layout_binding wire{
                .binding = b.binding,
                .descriptor_type = static_cast<uint32_t>(b.descriptorType),
                .descriptor_count = b.descriptorCount,
                .stage_flags = b.stageFlags,
                .binding_flags = 0,
            };
            if (binding_flags && binding_flags->bindingCount != 0)
            {
                wire.binding_flags = binding_flags->pBindingFlags[i];
            }
            std::memcpy(message.data() + sizeof(header) + static_cast<size_t>(i) * sizeof(wire), &wire, sizeof(wire));
        }

        gb::descriptor_set_layout_support_response response{};
        if (!bridge_call(gb::ioctl_get_descriptor_set_layout_support, message.data(), static_cast<DWORD>(message.size()), &response,
                         sizeof(response)) ||
            response.vk_result != VK_SUCCESS)
        {
            return;
        }

        pSupport->supported = response.supported ? VK_TRUE : VK_FALSE;
        if (variable_support)
        {
            variable_support->maxVariableDescriptorCount = response.max_variable_descriptor_count;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutSupportKHR(VkDevice device,
                                                                                        const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                                                                                        VkDescriptorSetLayoutSupport* pSupport)
    {
        vkGetDescriptorSetLayoutSupport(device, pCreateInfo, pSupport);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirements(VkDevice,
                                                                                              const VkDeviceImageMemoryRequirements*,
                                                                                              uint32_t* pSparseMemoryRequirementCount,
                                                                                              VkSparseImageMemoryRequirements2*)
    {
        if (pSparseMemoryRequirementCount)
        {
            *pSparseMemoryRequirementCount = 0;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory,
                                                                                 VkDeviceSize* pCommittedMemoryInBytes)
    {
        if (!pCommittedMemoryInBytes)
        {
            return;
        }

        *pCommittedMemoryInBytes = 0;
        gb::get_device_memory_commitment_request request{};
        request.device = to_object_id(device);
        request.memory = to_object_id(memory);

        gb::get_device_memory_commitment_response response{};
        if (bridge_call(gb::ioctl_get_device_memory_commitment, &request, sizeof(request), &response, sizeof(response)) &&
            response.vk_result == VK_SUCCESS)
        {
            *pCommittedMemoryInBytes = response.committed_bytes;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetImageSparseMemoryRequirements(VkDevice, VkImage,
                                                                                        uint32_t* pSparseMemoryRequirementCount,
                                                                                        VkSparseImageMemoryRequirements*)
    {
        if (pSparseMemoryRequirementCount)
        {
            *pSparseMemoryRequirementCount = 0;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetImageSparseMemoryRequirements2(VkDevice,
                                                                                         const VkImageSparseMemoryRequirementsInfo2*,
                                                                                         uint32_t* pSparseMemoryRequirementCount,
                                                                                         VkSparseImageMemoryRequirements2*)
    {
        if (pSparseMemoryRequirementCount)
        {
            *pSparseMemoryRequirementCount = 0;
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkGetImageSparseMemoryRequirements2KHR(
        VkDevice device, const VkImageSparseMemoryRequirementsInfo2* pInfo, uint32_t* pSparseMemoryRequirementCount,
        VkSparseImageMemoryRequirements2* pSparseMemoryRequirements)
    {
        vkGetImageSparseMemoryRequirements2(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(VkQueue, uint32_t, const VkBindSparseInfo*, VkFence)
    {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_sampler, device, sampler);
    }
}

extern "C"
{
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice device,
                                                                               const VkPipelineCacheCreateInfo* pCreateInfo,
                                                                               const VkAllocationCallbacks*,
                                                                               VkPipelineCache* pPipelineCache)
    {
        if (!pCreateInfo || !pPipelineCache || (pCreateInfo->initialDataSize > 0 && !pCreateInfo->pInitialData) ||
            pCreateInfo->initialDataSize > UINT32_MAX)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        gb::create_pipeline_cache_request request{};
        request.device = to_object_id(device);
        request.flags = static_cast<uint32_t>(pCreateInfo->flags);
        request.initial_data_size = static_cast<uint32_t>(pCreateInfo->initialDataSize);
        if (request.initial_data_size > MAXDWORD - sizeof(request))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        std::vector<uint8_t> message(sizeof(request) + request.initial_data_size);
        std::memcpy(message.data(), &request, sizeof(request));
        if (request.initial_data_size > 0)
        {
            std::memcpy(message.data() + sizeof(request), pCreateInfo->pInitialData, request.initial_data_size);
        }

        gb::object_response response{};
        if (!bridge_call(gb::ioctl_create_pipeline_cache, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)))
        {
            *pPipelineCache = VK_NULL_HANDLE;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        *pPipelineCache = response.vk_result == VK_SUCCESS ? to_handle<VkPipelineCache>(response.object) : VK_NULL_HANDLE;
        return static_cast<VkResult>(response.vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice device, VkPipelineCache pipelineCache,
                                                                            const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_pipeline_cache, device, pipelineCache);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice device, VkPipelineCache pipelineCache,
                                                                                size_t* pDataSize, void* pData)
    {
        if (!pDataSize)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const size_t capacity = pData ? *pDataSize : 0;
        if (capacity > MAXDWORD - sizeof(gb::get_pipeline_cache_data_response))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        gb::get_pipeline_cache_data_request request{};
        request.device = to_object_id(device);
        request.pipeline_cache = to_object_id(pipelineCache);
        request.max_data_size = capacity;
        request.has_data = pData != nullptr;

        std::vector<std::byte> buffer(sizeof(gb::get_pipeline_cache_data_response) + capacity);
        if (!bridge_call(gb::ioctl_get_pipeline_cache_data, &request, sizeof(request), buffer.data(), static_cast<DWORD>(buffer.size())))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto* response = reinterpret_cast<const gb::get_pipeline_cache_data_response*>(buffer.data());
        const size_t written = std::min<size_t>(static_cast<size_t>(response->data_size), capacity);
        if (pData && written > 0)
        {
            std::memcpy(pData, buffer.data() + sizeof(gb::get_pipeline_cache_data_response), written);
        }
        *pDataSize = static_cast<size_t>(response->data_size);
        return static_cast<VkResult>(response->vk_result);
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkMergePipelineCaches(VkDevice device, VkPipelineCache dstCache,
                                                                               uint32_t srcCacheCount, const VkPipelineCache* pSrcCaches)
    {
        if (srcCacheCount > 0 && !pSrcCaches)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (srcCacheCount > (MAXDWORD - sizeof(gb::merge_pipeline_caches_request)) / sizeof(gb::object_id))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        gb::merge_pipeline_caches_request request{};
        request.device = to_object_id(device);
        request.destination_cache = to_object_id(dstCache);
        request.source_count = srcCacheCount;

        const size_t message_size = sizeof(request) + static_cast<size_t>(srcCacheCount) * sizeof(gb::object_id);
        std::vector<uint8_t> message(message_size);
        std::memcpy(message.data(), &request, sizeof(request));
        for (uint32_t i = 0; i < srcCacheCount; ++i)
        {
            const gb::object_id source = to_object_id(pSrcCaches[i]);
            std::memcpy(message.data() + sizeof(request) + static_cast<size_t>(i) * sizeof(source), &source, sizeof(source));
        }

        gb::result_response response{};
        if (!bridge_call(gb::ioctl_merge_pipeline_caches, message.data(), static_cast<DWORD>(message.size()), &response, sizeof(response)))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return static_cast<VkResult>(response.vk_result);
    }
}

namespace
{
    struct resolved_stage_source
    {
        gb::shader_stage_source wire{};
        VkShaderModule owned_module{VK_NULL_HANDLE};
        bool valid{};
    };

    resolved_stage_source resolve_stage_source(VkDevice device, const VkPipelineShaderStageCreateInfo& stage)
    {
        resolved_stage_source result{};
        if (stage.module != VK_NULL_HANDLE)
        {
            result.wire.module = to_object_id(stage.module);
            result.valid = true;
            return result;
        }

        const VkShaderModuleCreateInfo* inline_info = nullptr;
        const VkPipelineShaderStageModuleIdentifierCreateInfoEXT* identifier_info = nullptr;
        for (const auto* next = static_cast<const VkBaseInStructure*>(stage.pNext); next != nullptr; next = next->pNext)
        {
            if (next->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
            {
                inline_info = reinterpret_cast<const VkShaderModuleCreateInfo*>(next);
            }
            else if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT)
            {
                identifier_info = reinterpret_cast<const VkPipelineShaderStageModuleIdentifierCreateInfoEXT*>(next);
            }
        }

        if (inline_info && identifier_info)
        {
            return result;
        }

        if (identifier_info)
        {
            if (identifier_info->identifierSize == 0 || identifier_info->identifierSize > gb::max_shader_module_identifier_size ||
                !identifier_info->pIdentifier)
            {
                return result;
            }
            result.wire.identifier_size = identifier_info->identifierSize;
            std::memcpy(result.wire.identifier.data(), identifier_info->pIdentifier, identifier_info->identifierSize);
            result.valid = true;
            return result;
        }

        if (inline_info && vkCreateShaderModule(device, inline_info, nullptr, &result.owned_module) == VK_SUCCESS)
        {
            result.wire.module = to_object_id(result.owned_module);
            result.valid = true;
        }
        return result;
    }

}

extern "C"
{
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                                                   uint32_t createInfoCount,
                                                                                   const VkGraphicsPipelineCreateInfo* pCreateInfos,
                                                                                   const VkAllocationCallbacks*, VkPipeline* pPipelines)
    {
        VkResult overall = VK_SUCCESS;
        for (uint32_t i = 0; i < createInfoCount; ++i)
        {
            const VkGraphicsPipelineCreateInfo& ci = pCreateInfos[i];

            resolved_stage_source vertex_shader{};
            resolved_stage_source fragment_shader{};
            const VkSpecializationInfo* vs_spec = nullptr;
            const VkSpecializationInfo* fs_spec = nullptr;
            bool stages_valid = true;
            for (uint32_t s = 0; s < ci.stageCount; ++s)
            {
                resolved_stage_source source = resolve_stage_source(device, ci.pStages[s]);
                if (!source.valid)
                {
                    stages_valid = false;
                    break;
                }
                if (ci.pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT)
                {
                    vertex_shader = source;
                    vs_spec = ci.pStages[s].pSpecializationInfo;
                }
                else if (ci.pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                {
                    fragment_shader = source;
                    fs_spec = ci.pStages[s].pSpecializationInfo;
                }
                else if (source.owned_module != VK_NULL_HANDLE)
                {
                    vkDestroyShaderModule(device, source.owned_module, nullptr);
                }
            }
            stages_valid = stages_valid && vertex_shader.valid && fragment_shader.valid;
            if (!stages_valid)
            {
                if (vertex_shader.owned_module != VK_NULL_HANDLE)
                {
                    vkDestroyShaderModule(device, vertex_shader.owned_module, nullptr);
                }
                if (fragment_shader.owned_module != VK_NULL_HANDLE)
                {
                    vkDestroyShaderModule(device, fragment_shader.owned_module, nullptr);
                }
                pPipelines[i] = VK_NULL_HANDLE;
                overall = VK_ERROR_INITIALIZATION_FAILED;
                continue;
            }
            const uint32_t vs_spec_entries = (vs_spec && vs_spec->pMapEntries) ? vs_spec->mapEntryCount : 0u;
            const uint32_t vs_spec_bytes = (vs_spec && vs_spec->pData) ? static_cast<uint32_t>(vs_spec->dataSize) : 0u;
            const uint32_t fs_spec_entries = (fs_spec && fs_spec->pMapEntries) ? fs_spec->mapEntryCount : 0u;
            const uint32_t fs_spec_bytes = (fs_spec && fs_spec->pData) ? static_cast<uint32_t>(fs_spec->dataSize) : 0u;

            uint32_t width = 0;
            uint32_t height = 0;
            if (ci.pViewportState && ci.pViewportState->pViewports && ci.pViewportState->viewportCount > 0)
            {
                width = static_cast<uint32_t>(ci.pViewportState->pViewports[0].width);
                height = static_cast<uint32_t>(ci.pViewportState->pViewports[0].height);
            }

            // Vertex input state (variable-length): flatten bindings, attributes, and the optional
            // VkPipelineVertexInputDivisorStateCreateInfo pNext payload after the request header.
            uint32_t binding_count = 0;
            uint32_t attribute_count = 0;
            uint32_t divisor_count = 0;
            const VkVertexInputBindingDescription* vk_bindings = nullptr;
            const VkVertexInputAttributeDescription* vk_attributes = nullptr;
            const VkVertexInputBindingDivisorDescription* vk_divisors = nullptr;
            if (ci.pVertexInputState)
            {
                binding_count = ci.pVertexInputState->vertexBindingDescriptionCount;
                attribute_count = ci.pVertexInputState->vertexAttributeDescriptionCount;
                vk_bindings = ci.pVertexInputState->pVertexBindingDescriptions;
                vk_attributes = ci.pVertexInputState->pVertexAttributeDescriptions;

                for (const auto* base = static_cast<const VkBaseInStructure*>(ci.pVertexInputState->pNext); base != nullptr;
                     base = base->pNext)
                {
                    if (base->sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO)
                    {
                        continue;
                    }
                    const auto* divisor_state = reinterpret_cast<const VkPipelineVertexInputDivisorStateCreateInfo*>(base);
                    divisor_count = divisor_state->vertexBindingDivisorCount;
                    vk_divisors = divisor_state->pVertexBindingDivisors;
                    break;
                }
            }

            gb::create_graphics_pipeline_request request{};
            request.device = to_object_id(device);
            request.pipeline_cache = to_object_id(pipelineCache);
            request.render_pass = to_object_id(ci.renderPass);
            request.pipeline_layout = to_object_id(ci.layout);
            request.vertex_shader = vertex_shader.wire;
            request.fragment_shader = fragment_shader.wire;
            request.flags = static_cast<uint32_t>(ci.flags & ~VK_PIPELINE_CREATE_DERIVATIVE_BIT);
            request.width = width;
            request.height = height;
            if (ci.pDepthStencilState && ci.pDepthStencilState->depthTestEnable)
            {
                request.depth_test_enable = 1;
                request.depth_write_enable = ci.pDepthStencilState->depthWriteEnable ? 1 : 0;
                request.depth_compare_op = static_cast<uint32_t>(ci.pDepthStencilState->depthCompareOp);
            }
            request.binding_count = binding_count;
            request.attribute_count = attribute_count;
            request.divisor_count = divisor_count;
            request.rasterization_samples = ci.pMultisampleState ? static_cast<uint32_t>(ci.pMultisampleState->rasterizationSamples) : 1u;

            request.primitive_topology = static_cast<uint32_t>(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

            request.primitive_restart_enable = 0;
            request.rasterization_stream = UINT32_MAX;

            if (ci.pRasterizationState)
            {
                for (const auto* base = static_cast<const VkBaseInStructure*>(ci.pRasterizationState->pNext); base != nullptr;
                     base = base->pNext)
                {
                    if (base->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT)
                    {
                        continue;
                    }
                    const auto* stream = reinterpret_cast<const VkPipelineRasterizationStateStreamCreateInfoEXT*>(base);
                    request.rasterization_stream = stream->rasterizationStream;
                    request.rasterization_stream_flags = static_cast<uint32_t>(stream->flags);
                    break;
                }
            }

            if (ci.pInputAssemblyState)
            {
                request.primitive_topology = static_cast<uint32_t>(ci.pInputAssemblyState->topology);

                request.primitive_restart_enable = ci.pInputAssemblyState->primitiveRestartEnable ? 1u : 0u;
            }

            // Forward the dynamic-state list verbatim. DXVK marks vertex-binding stride, cull, topology,
            // depth/stencil etc. dynamic and sets them via vkCmdSet*/vkCmdBindVertexBuffers2; if the host
            // pipeline does not also declare them dynamic it bakes (often wrong) defaults instead.
            const uint32_t dynamic_state_count = ci.pDynamicState ? ci.pDynamicState->dynamicStateCount : 0;
            request.dynamic_state_count = dynamic_state_count;
            request.vs_spec_entry_count = vs_spec_entries;
            request.vs_spec_data_size = vs_spec_bytes;
            request.fs_spec_entry_count = fs_spec_entries;
            request.fs_spec_data_size = fs_spec_bytes;

            // Per-attachment blend state. DXVK bakes D3D9 alpha blending statically into pColorBlendState; without
            // forwarding it the host defaults to blend-disabled and transparent geometry renders fully opaque.
            if (ci.pColorBlendState && ci.pColorBlendState->pAttachments)
            {
                request.blend_attachment_count = ci.pColorBlendState->attachmentCount < gb::max_color_attachments
                                                     ? ci.pColorBlendState->attachmentCount
                                                     : gb::max_color_attachments;
                for (uint32_t a = 0; a < request.blend_attachment_count; ++a)
                {
                    const VkPipelineColorBlendAttachmentState& src = ci.pColorBlendState->pAttachments[a];
                    gb::pipeline_blend_attachment& dst = request.blend_attachments[a];
                    dst.blend_enable = src.blendEnable ? 1u : 0u;
                    dst.src_color_blend_factor = static_cast<uint32_t>(src.srcColorBlendFactor);
                    dst.dst_color_blend_factor = static_cast<uint32_t>(src.dstColorBlendFactor);
                    dst.color_blend_op = static_cast<uint32_t>(src.colorBlendOp);
                    dst.src_alpha_blend_factor = static_cast<uint32_t>(src.srcAlphaBlendFactor);
                    dst.dst_alpha_blend_factor = static_cast<uint32_t>(src.dstAlphaBlendFactor);
                    dst.alpha_blend_op = static_cast<uint32_t>(src.alphaBlendOp);
                    dst.color_write_mask = static_cast<uint32_t>(src.colorWriteMask);
                }
            }

            // DXVK 2.x builds pipelines with VK_KHR_dynamic_rendering: renderPass is VK_NULL_HANDLE and the
            // attachment formats live in a VkPipelineRenderingCreateInfo on the pNext chain. Forward those so
            // the host can rebuild that info instead of failing for the missing render pass.
            for (const auto* base = static_cast<const VkBaseInStructure*>(ci.pNext); base != nullptr; base = base->pNext)
            {
                if (base->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
                {
                    continue;
                }

                const auto* rendering = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(base);
                request.color_attachment_count = rendering->colorAttachmentCount < gb::max_color_attachments
                                                     ? rendering->colorAttachmentCount
                                                     : gb::max_color_attachments;
                for (uint32_t c = 0; c < request.color_attachment_count; ++c)
                {
                    request.color_formats[c] = static_cast<uint32_t>(rendering->pColorAttachmentFormats[c]);
                }
                request.depth_format = static_cast<uint32_t>(rendering->depthAttachmentFormat);
                request.stencil_format = static_cast<uint32_t>(rendering->stencilAttachmentFormat);
                break;
            }

            const auto spec_block_bytes = [](uint32_t entries, uint32_t bytes) {
                return static_cast<size_t>(entries) * sizeof(gb::specialization_map_entry) + bytes;
            };
            std::vector<uint8_t> message(sizeof(request) + static_cast<size_t>(binding_count) * sizeof(gb::vertex_input_binding) +
                                         static_cast<size_t>(attribute_count) * sizeof(gb::vertex_input_attribute) +
                                         static_cast<size_t>(divisor_count) * sizeof(gb::vertex_input_divisor) +
                                         static_cast<size_t>(dynamic_state_count) * sizeof(uint32_t) +
                                         spec_block_bytes(vs_spec_entries, vs_spec_bytes) +
                                         spec_block_bytes(fs_spec_entries, fs_spec_bytes));
            std::memcpy(message.data(), &request, sizeof(request));
            size_t cursor = sizeof(request);
            for (uint32_t b = 0; b < binding_count; ++b)
            {
                gb::vertex_input_binding wire{};
                wire.binding = vk_bindings[b].binding;
                wire.stride = vk_bindings[b].stride;
                wire.input_rate = static_cast<uint32_t>(vk_bindings[b].inputRate);
                std::memcpy(message.data() + cursor, &wire, sizeof(wire));
                cursor += sizeof(wire);
            }
            for (uint32_t a = 0; a < attribute_count; ++a)
            {
                gb::vertex_input_attribute wire{};
                wire.location = vk_attributes[a].location;
                wire.binding = vk_attributes[a].binding;
                wire.format = static_cast<uint32_t>(vk_attributes[a].format);
                wire.offset = vk_attributes[a].offset;
                std::memcpy(message.data() + cursor, &wire, sizeof(wire));
                cursor += sizeof(wire);
            }
            for (uint32_t d = 0; d < divisor_count; ++d)
            {
                gb::vertex_input_divisor wire{};
                wire.binding = vk_divisors[d].binding;
                wire.divisor = vk_divisors[d].divisor;
                std::memcpy(message.data() + cursor, &wire, sizeof(wire));
                cursor += sizeof(wire);
            }
            for (uint32_t d = 0; d < dynamic_state_count; ++d)
            {
                const auto value = static_cast<uint32_t>(ci.pDynamicState->pDynamicStates[d]);
                std::memcpy(message.data() + cursor, &value, sizeof(value));
                cursor += sizeof(value);
            }
            const auto append_spec = [&](const VkSpecializationInfo* spec, uint32_t entries, uint32_t bytes) {
                for (uint32_t e = 0; e < entries; ++e)
                {
                    gb::specialization_map_entry wire{};
                    wire.constant_id = spec->pMapEntries[e].constantID;
                    wire.offset = spec->pMapEntries[e].offset;
                    wire.size = static_cast<uint32_t>(spec->pMapEntries[e].size);
                    std::memcpy(message.data() + cursor, &wire, sizeof(wire));
                    cursor += sizeof(wire);
                }
                if (bytes > 0)
                {
                    std::memcpy(message.data() + cursor, spec->pData, bytes);
                    cursor += bytes;
                }
            };
            append_spec(vs_spec, vs_spec_entries, vs_spec_bytes);
            append_spec(fs_spec, fs_spec_entries, fs_spec_bytes);

            gb::object_response response{};
            const bool ok = bridge_call(gb::ioctl_create_graphics_pipeline, message.data(), static_cast<DWORD>(message.size()), &response,
                                        sizeof(response));
            if (!ok || response.vk_result != VK_SUCCESS)
            {
                pPipelines[i] = VK_NULL_HANDLE;
                overall = ok ? static_cast<VkResult>(response.vk_result) : VK_ERROR_INITIALIZATION_FAILED;
            }
            else
            {
                pPipelines[i] = to_handle<VkPipeline>(response.object);
            }

            if (vertex_shader.owned_module != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(device, vertex_shader.owned_module, nullptr);
            }
            if (fragment_shader.owned_module != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(device, fragment_shader.owned_module, nullptr);
            }
        }
        return overall;
    }

    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                                                  uint32_t createInfoCount,
                                                                                  const VkComputePipelineCreateInfo* pCreateInfos,
                                                                                  const VkAllocationCallbacks*, VkPipeline* pPipelines)
    {
        VkResult overall = VK_SUCCESS;
        for (uint32_t i = 0; i < createInfoCount; ++i)
        {
            const VkComputePipelineCreateInfo& ci = pCreateInfos[i];

            const resolved_stage_source shader = resolve_stage_source(device, ci.stage);
            if (!shader.valid)
            {
                pPipelines[i] = VK_NULL_HANDLE;
                overall = VK_ERROR_INITIALIZATION_FAILED;
                continue;
            }

            gb::create_compute_pipeline_request request{};
            request.device = to_object_id(device);
            request.pipeline_cache = to_object_id(pipelineCache);
            request.pipeline_layout = to_object_id(ci.layout);
            request.shader = shader.wire;
            request.flags = static_cast<uint32_t>(ci.flags & ~VK_PIPELINE_CREATE_DERIVATIVE_BIT);

            gb::create_compute_pipeline_response response{};
            const bool ok = bridge_call(gb::ioctl_create_compute_pipeline, &request, sizeof(request), &response, sizeof(response));
            if (!ok || response.vk_result != VK_SUCCESS)
            {
                pPipelines[i] = VK_NULL_HANDLE;
                overall = ok ? static_cast<VkResult>(response.vk_result) : VK_ERROR_INITIALIZATION_FAILED;
            }
            else
            {
                pPipelines[i] = to_handle<VkPipeline>(response.pipeline);
            }

            if (shader.owned_module != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(device, shader.owned_module, nullptr);
            }
        }
        return overall;
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks*)
    {
        destroy_device_child(gb::ioctl_destroy_pipeline, device, pipeline);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                                                          const VkRenderPassBeginInfo* pRenderPassBegin,
                                                                          VkSubpassContents contents)
    {
        const VkSubpassBeginInfo begin{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, nullptr, contents};
        record_render_pass(commandBuffer, gb::command::cmd_begin_render_pass_full, pRenderPassBegin, &begin);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                                                                       VkPipeline pipeline)
    {
        gb::cmd_bind_pipeline_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.pipeline = to_object_id(pipeline);
        request.bind_point = static_cast<uint32_t>(pipelineBindPoint);
        record_command(request.command_buffer, gb::command::cmd_bind_pipeline, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX,
                                                                   uint32_t groupCountY, uint32_t groupCountZ)
    {
        gb::cmd_dispatch_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.group_count_x = groupCountX;
        request.group_count_y = groupCountY;
        request.group_count_z = groupCountZ;
        record_command(request.command_buffer, gb::command::cmd_dispatch, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                           VkDeviceSize offset)
    {
        gb::cmd_dispatch_indirect_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(buffer);
        request.offset = offset;
        record_command(request.command_buffer, gb::command::cmd_dispatch_indirect, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
                                                               uint32_t firstVertex, uint32_t firstInstance)
    {
        gb::cmd_draw_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.vertex_count = vertexCount;
        request.instance_count = instanceCount;
        request.first_vertex = firstVertex;
        request.first_instance = firstInstance;
        record_command(request.command_buffer, gb::command::cmd_draw, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDrawMultiEXT(VkCommandBuffer commandBuffer, uint32_t drawCount,
                                                                        const VkMultiDrawInfoEXT* pVertexInfo,
                                                                        uint32_t instanceCount, uint32_t firstInstance,
                                                                        uint32_t stride)
    {
        const auto id = to_object_id(commandBuffer);
        constexpr size_t limit = 256 * 1024 * 1024;
        if ((drawCount && !pVertexInfo) ||
            (drawCount > 1 && (stride < sizeof(VkMultiDrawInfoEXT) || stride % 4 != 0)))
        {
            fail_recorded_command(id, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
        if (drawCount > (limit - sizeof(gb::cmd_draw_multi_request)) / sizeof(gb::multi_draw_info) ||
            (drawCount && static_cast<uint64_t>(drawCount - 1) * stride + sizeof(VkMultiDrawInfoEXT) > SIZE_MAX))
        {
            fail_recorded_command(id, VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
        }
        gb::cmd_draw_multi_request request{};
        request.command_buffer = id;
        request.draw_count = drawCount;
        request.instance_count = instanceCount;
        request.first_instance = firstInstance;
        std::vector<uint8_t> message(sizeof(request) + static_cast<size_t>(drawCount) * sizeof(gb::multi_draw_info));
        std::memcpy(message.data(), &request, sizeof(request));
        const auto* source = reinterpret_cast<const uint8_t*>(pVertexInfo);
        for (uint32_t i = 0; i < drawCount; ++i)
        {
            VkMultiDrawInfoEXT entry{};
            std::memcpy(&entry, source + static_cast<size_t>(i) * stride, sizeof(entry));
            const gb::multi_draw_info wire{entry.firstVertex, entry.vertexCount};
            std::memcpy(message.data() + sizeof(request) + static_cast<size_t>(i) * sizeof(wire), &wire, sizeof(wire));
        }
        record_command(id, gb::command::cmd_draw_multi, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding,
                                                                            uint32_t bindingCount, const VkBuffer* pBuffers,
                                                                            const VkDeviceSize* pOffsets)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_bind_vertex_buffers_request) +
                                     static_cast<size_t>(bindingCount) * sizeof(gb::vertex_buffer_binding));
        gb::cmd_bind_vertex_buffers_request header{};
        header.command_buffer = command_buffer;
        header.first_binding = firstBinding;
        header.binding_count = bindingCount;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < bindingCount; ++i)
        {
            gb::vertex_buffer_binding vb{};
            vb.buffer = to_object_id(pBuffers[i]);
            vb.offset = pOffsets ? pOffsets[i] : 0;
            std::memcpy(message.data() + sizeof(header) + i * sizeof(vb), &vb, sizeof(vb));
        }
        record_command(command_buffer, gb::command::cmd_bind_vertex_buffers, message.data(), message.size());
    }

    // Core 1.3 / VK_EXT_extended_dynamic_state: vertex binding with dynamic per-binding size and stride.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding,
                                                                             uint32_t bindingCount, const VkBuffer* pBuffers,
                                                                             const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes,
                                                                             const VkDeviceSize* pStrides)
    {
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_bind_vertex_buffers2_request) +
                                     static_cast<size_t>(bindingCount) * sizeof(gb::vertex_buffer_binding2));
        gb::cmd_bind_vertex_buffers2_request header{};
        header.command_buffer = command_buffer;
        header.first_binding = firstBinding;
        header.binding_count = bindingCount;
        header.has_sizes = pSizes ? 1u : 0u;
        header.has_strides = pStrides ? 1u : 0u;
        std::memcpy(message.data(), &header, sizeof(header));
        for (uint32_t i = 0; i < bindingCount; ++i)
        {
            gb::vertex_buffer_binding2 vb{};
            vb.buffer = to_object_id(pBuffers[i]);
            vb.offset = pOffsets ? pOffsets[i] : 0;
            vb.size = pSizes ? pSizes[i] : 0;
            vb.stride = pStrides ? pStrides[i] : 0;
            std::memcpy(message.data() + sizeof(header) + i * sizeof(vb), &vb, sizeof(vb));
        }
        record_command(command_buffer, gb::command::cmd_bind_vertex_buffers2, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                          VkDeviceSize offset, VkIndexType indexType)
    {
        gb::cmd_bind_index_buffer_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(buffer);
        request.offset = offset;
        request.index_type = static_cast<uint32_t>(indexType);
        record_command(request.command_buffer, gb::command::cmd_bind_index_buffer, &request, sizeof(request));
    }

    // VK_KHR_maintenance5 / core 1.4: like vkCmdBindIndexBuffer but with an explicit size. The bridge does
    // not model the size (the base bind covers the buffer from offset, which matches the usual VK_WHOLE_SIZE).
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer2KHR(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                              VkDeviceSize offset, VkDeviceSize /*size*/,
                                                                              VkIndexType indexType)
    {
        gb::cmd_bind_index_buffer_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(buffer);
        request.offset = offset;
        request.index_type = static_cast<uint32_t>(indexType);
        record_command(request.command_buffer, gb::command::cmd_bind_index_buffer, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount,
                                                                      uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
                                                                      uint32_t firstInstance)
    {
        gb::cmd_draw_indexed_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.index_count = indexCount;
        request.instance_count = instanceCount;
        request.first_index = firstIndex;
        request.vertex_offset = vertexOffset;
        request.first_instance = firstInstance;
        record_command(request.command_buffer, gb::command::cmd_draw_indexed, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDrawMultiIndexedEXT(
        VkCommandBuffer commandBuffer, uint32_t drawCount, const VkMultiDrawIndexedInfoEXT* pIndexInfo,
        uint32_t instanceCount, uint32_t firstInstance, uint32_t stride, const int32_t* pVertexOffset)
    {
        const auto id = to_object_id(commandBuffer);
        constexpr size_t limit = 256 * 1024 * 1024;
        if ((drawCount && !pIndexInfo) ||
            (drawCount > 1 && (stride < sizeof(VkMultiDrawIndexedInfoEXT) || stride % 4 != 0)))
        {
            fail_recorded_command(id, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
        if (drawCount > (limit - sizeof(gb::cmd_draw_multi_indexed_request)) / sizeof(gb::multi_draw_indexed_info) ||
            (drawCount && static_cast<uint64_t>(drawCount - 1) * stride + sizeof(VkMultiDrawIndexedInfoEXT) > SIZE_MAX))
        {
            fail_recorded_command(id, VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
        }
        gb::cmd_draw_multi_indexed_request request{};
        request.command_buffer = id;
        request.draw_count = drawCount;
        request.instance_count = instanceCount;
        request.first_instance = firstInstance;
        request.has_vertex_offset = pVertexOffset != nullptr;
        request.vertex_offset = pVertexOffset ? *pVertexOffset : 0;
        std::vector<uint8_t> message(sizeof(request) + static_cast<size_t>(drawCount) * sizeof(gb::multi_draw_indexed_info));
        std::memcpy(message.data(), &request, sizeof(request));
        const auto* source = reinterpret_cast<const uint8_t*>(pIndexInfo);
        for (uint32_t i = 0; i < drawCount; ++i)
        {
            VkMultiDrawIndexedInfoEXT entry{};
            std::memcpy(&entry, source + static_cast<size_t>(i) * stride, sizeof(entry));
            const gb::multi_draw_indexed_info wire{entry.firstIndex, entry.indexCount, entry.vertexOffset};
            std::memcpy(message.data() + sizeof(request) + static_cast<size_t>(i) * sizeof(wire), &wire, sizeof(wire));
        }
        record_command(id, gb::command::cmd_draw_multi_indexed, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                              VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
    {
        gb::cmd_draw_indexed_indirect_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(buffer);
        request.offset = offset;
        request.draw_count = drawCount;
        request.stride = stride;
        record_command(request.command_buffer, gb::command::cmd_draw_indexed_indirect, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                                   VkDeviceSize offset, VkBuffer countBuffer,
                                                                                   VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
                                                                                   uint32_t stride)
    {
        gb::cmd_draw_indexed_indirect_count_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(buffer);
        request.offset = offset;
        request.count_buffer = to_object_id(countBuffer);
        request.count_buffer_offset = countBufferOffset;
        request.max_draw_count = maxDrawCount;
        request.stride = stride;
        record_command(request.command_buffer, gb::command::cmd_draw_indexed_indirect_count, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCountKHR(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                                      VkDeviceSize offset, VkBuffer countBuffer,
                                                                                      VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
                                                                                      uint32_t stride)
    {
        vkCmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                                                                       uint32_t drawCount, uint32_t stride)
    {
        gb::cmd_draw_indirect_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(buffer);
        request.offset = offset;
        request.draw_count = drawCount;
        request.stride = stride;
        record_command(request.command_buffer, gb::command::cmd_draw_indirect, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                            VkDeviceSize offset, VkBuffer countBuffer,
                                                                            VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
                                                                            uint32_t stride)
    {
        gb::cmd_draw_indirect_count_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.buffer = to_object_id(buffer);
        request.offset = offset;
        request.count_buffer = to_object_id(countBuffer);
        request.count_buffer_offset = countBufferOffset;
        request.max_draw_count = maxDrawCount;
        request.stride = stride;
        record_command(request.command_buffer, gb::command::cmd_draw_indirect_count, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCountKHR(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                                               VkDeviceSize offset, VkBuffer countBuffer,
                                                                               VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
                                                                               uint32_t stride)
    {
        vkCmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                                                                             VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
                                                                             uint32_t firstSet, uint32_t descriptorSetCount,
                                                                             const VkDescriptorSet* pDescriptorSets,
                                                                             uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets)
    {
        // Forward the dynamic offsets: DXVK binds UNIFORM_BUFFER_DYNAMIC descriptors and indexes the current
        // frame's shader constants in a ring buffer through them. Dropping them reads stale constants.
        const gb::object_id command_buffer = to_object_id(commandBuffer);
        std::vector<uint8_t> message(sizeof(gb::cmd_bind_descriptor_sets_request) +
                                     static_cast<size_t>(descriptorSetCount) * sizeof(gb::object_id) +
                                     static_cast<size_t>(dynamicOffsetCount) * sizeof(uint32_t));
        gb::cmd_bind_descriptor_sets_request header{};
        header.command_buffer = command_buffer;
        header.pipeline_layout = to_object_id(layout);
        header.first_set = firstSet;
        header.set_count = descriptorSetCount;
        header.bind_point = static_cast<uint32_t>(pipelineBindPoint);
        header.dynamic_offset_count = dynamicOffsetCount;
        std::memcpy(message.data(), &header, sizeof(header));
        size_t cursor = sizeof(header);
        for (uint32_t i = 0; i < descriptorSetCount; ++i)
        {
            const gb::object_id id = to_object_id(pDescriptorSets[i]);
            std::memcpy(message.data() + cursor, &id, sizeof(id));
            cursor += sizeof(id);
        }
        for (uint32_t i = 0; i < dynamicOffsetCount; ++i)
        {
            std::memcpy(message.data() + cursor, &pDynamicOffsets[i], sizeof(uint32_t));
            cursor += sizeof(uint32_t);
        }
        record_command(command_buffer, gb::command::cmd_bind_descriptor_sets, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets2KHR(VkCommandBuffer commandBuffer,
                                                                                 const VkBindDescriptorSetsInfo* pBindDescriptorSetsInfo)
    {
        if (!pBindDescriptorSetsInfo)
        {
            return;
        }

        const auto bind = [&](const VkPipelineBindPoint bind_point) {
            vkCmdBindDescriptorSets(commandBuffer, bind_point, pBindDescriptorSetsInfo->layout, pBindDescriptorSetsInfo->firstSet,
                                    pBindDescriptorSetsInfo->descriptorSetCount, pBindDescriptorSetsInfo->pDescriptorSets,
                                    pBindDescriptorSetsInfo->dynamicOffsetCount, pBindDescriptorSetsInfo->pDynamicOffsets);
        };
        if (pBindDescriptorSetsInfo->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS)
        {
            bind(VK_PIPELINE_BIND_POINT_GRAPHICS);
        }
        if (pBindDescriptorSetsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        {
            bind(VK_PIPELINE_BIND_POINT_COMPUTE);
        }
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
    {
        gb::cmd_end_render_pass_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        record_command(request.command_buffer, gb::command::cmd_end_render_pass, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents)
    {
        gb::cmd_next_subpass_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        request.contents = static_cast<uint32_t>(contents);
        record_command(request.command_buffer, gb::command::cmd_next_subpass, &request, sizeof(request));
    }

    // Dynamic rendering (VK_KHR_dynamic_rendering / core 1.3): DXVK 2.x records draws inside this instead of
    // a render pass + framebuffer. Marshal the VkRenderingInfo and its attachment arrays across the bridge.
    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer commandBuffer,
                                                                         const VkRenderingInfo* pRenderingInfo)
    {
        const auto to_wire = [](const VkRenderingAttachmentInfo& a) {
            gb::rendering_attachment w{};
            w.image_view = to_object_id(a.imageView);
            w.resolve_image_view = to_object_id(a.resolveImageView);
            w.image_layout = static_cast<uint32_t>(a.imageLayout);
            w.resolve_image_layout = static_cast<uint32_t>(a.resolveImageLayout);
            w.resolve_mode = static_cast<uint32_t>(a.resolveMode);
            w.load_op = static_cast<uint32_t>(a.loadOp);
            w.store_op = static_cast<uint32_t>(a.storeOp);
            std::memcpy(w.clear_value.data(), &a.clearValue, sizeof(w.clear_value));
            return w;
        };

        const gb::object_id command_buffer = to_object_id(commandBuffer);
        const uint32_t color_count = pRenderingInfo->colorAttachmentCount;
        const bool has_depth = pRenderingInfo->pDepthAttachment != nullptr && pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE;
        const bool has_stencil =
            pRenderingInfo->pStencilAttachment != nullptr && pRenderingInfo->pStencilAttachment->imageView != VK_NULL_HANDLE;
        const uint32_t total = color_count + (has_depth ? 1u : 0u) + (has_stencil ? 1u : 0u);

        std::vector<uint8_t> message(sizeof(gb::cmd_begin_rendering_request) +
                                     static_cast<size_t>(total) * sizeof(gb::rendering_attachment));
        gb::cmd_begin_rendering_request header{};
        header.command_buffer = command_buffer;
        header.render_area_x = pRenderingInfo->renderArea.offset.x;
        header.render_area_y = pRenderingInfo->renderArea.offset.y;
        header.render_area_width = pRenderingInfo->renderArea.extent.width;
        header.render_area_height = pRenderingInfo->renderArea.extent.height;
        header.layer_count = pRenderingInfo->layerCount;
        header.view_mask = pRenderingInfo->viewMask;
        header.color_attachment_count = color_count;
        header.has_depth = has_depth ? 1u : 0u;
        header.has_stencil = has_stencil ? 1u : 0u;
        header.flags = static_cast<uint32_t>(pRenderingInfo->flags);
        std::memcpy(message.data(), &header, sizeof(header));

        size_t offset = sizeof(header);
        for (uint32_t i = 0; i < color_count; ++i)
        {
            const auto w = to_wire(pRenderingInfo->pColorAttachments[i]);
            std::memcpy(message.data() + offset, &w, sizeof(w));
            offset += sizeof(w);
        }
        if (has_depth)
        {
            const auto w = to_wire(*pRenderingInfo->pDepthAttachment);
            std::memcpy(message.data() + offset, &w, sizeof(w));
            offset += sizeof(w);
        }
        if (has_stencil)
        {
            const auto w = to_wire(*pRenderingInfo->pStencilAttachment);
            std::memcpy(message.data() + offset, &w, sizeof(w));
            offset += sizeof(w);
        }

        record_command(command_buffer, gb::command::cmd_begin_rendering, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer commandBuffer)
    {
        gb::cmd_end_rendering_request request{};
        request.command_buffer = to_object_id(commandBuffer);
        record_command(request.command_buffer, gb::command::cmd_end_rendering, &request, sizeof(request));
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                                                                        VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size,
                                                                        const void* pValues)
    {
        std::vector<uint8_t> message(sizeof(gb::cmd_push_constants_request) + size);
        gb::cmd_push_constants_request header{};
        header.command_buffer = to_object_id(commandBuffer);
        header.pipeline_layout = to_object_id(layout);
        header.stage_flags = stageFlags;
        header.offset = offset;
        header.size = size;
        std::memcpy(message.data(), &header, sizeof(header));
        if (size > 0 && pValues)
        {
            std::memcpy(message.data() + sizeof(header), pValues, size);
        }
        record_command(header.command_buffer, gb::command::cmd_push_constants, message.data(), message.size());
    }

    __declspec(dllexport) VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants2KHR(VkCommandBuffer commandBuffer,
                                                                            const VkPushConstantsInfo* pPushConstantsInfo)
    {
        if (!pPushConstantsInfo)
        {
            return;
        }

        vkCmdPushConstants(commandBuffer, pPushConstantsInfo->layout, pPushConstantsInfo->stageFlags, pPushConstantsInfo->offset,
                           pPushConstantsInfo->size, pPushConstantsInfo->pValues);
    }

    __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char* pName);

    // The bridge cannot hand a host memory export to the guest: a guest HANDLE is an emulator object with no
    // meaning to the host driver. DXVK asks for the export unconditionally once a resource requests sharing,
    // even after its own capability check failed, so answer with a clean failure instead of a null jump.
    __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
    vkGetMemoryWin32HandleKHR(VkDevice /*device*/, const VkMemoryGetWin32HandleInfoKHR* /*pGetWin32HandleInfo*/, HANDLE* pHandle)
    {
        if (pHandle)
        {
            *pHandle = nullptr;
        }

        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName)
    {
        if (!pName)
        {
            return nullptr;
        }

        // Outside the emulator: hand back the real loader's pointers so the app talks to the real driver.
        if (passthrough_active())
        {
            return g_real_get_instance_proc_addr(instance, pName);
        }

        struct entry
        {
            const char* name;
            PFN_vkVoidFunction func;
        };

        static const entry table[] = {
            {.name = "vkGetInstanceProcAddr", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr)},
            {.name = "vkGetDeviceProcAddr", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr)},
            {.name = "vkCreateInstance", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance)},
            {.name = "vkDestroyInstance", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance)},
            {.name = "vkEnumerateInstanceVersion", .func = reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceVersion)},
            {.name = "vkEnumerateInstanceLayerProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceLayerProperties)},
            {.name = "vkEnumerateDeviceLayerProperties", .func = reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateDeviceLayerProperties)},
            {.name = "vkEnumerateInstanceExtensionProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceExtensionProperties)},
            {.name = "vkEnumeratePhysicalDevices", .func = reinterpret_cast<PFN_vkVoidFunction>(vkEnumeratePhysicalDevices)},
            {.name = "vkEnumerateDeviceExtensionProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateDeviceExtensionProperties)},
            {.name = "vkGetPhysicalDeviceProperties", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties)},
            {.name = "vkGetPhysicalDeviceProperties2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties2)},
            {.name = "vkGetPhysicalDeviceFeatures", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFeatures)},
            {.name = "vkGetPhysicalDeviceFeatures2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFeatures2)},
            {.name = "vkGetPhysicalDeviceFormatProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFormatProperties)},
            {.name = "vkGetPhysicalDeviceFormatProperties2",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFormatProperties2)},
            {.name = "vkGetPhysicalDeviceImageFormatProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceImageFormatProperties)},
            {.name = "vkGetPhysicalDeviceImageFormatProperties2",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceImageFormatProperties2)},
            {.name = "vkGetPhysicalDeviceQueueFamilyProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceQueueFamilyProperties)},
            {.name = "vkGetPhysicalDeviceQueueFamilyProperties2",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceQueueFamilyProperties2)},
            {.name = "vkGetPhysicalDeviceMemoryProperties2",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceMemoryProperties2)},
            {.name = "vkGetPhysicalDeviceSurfaceSupportKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceSupportKHR)},
            {.name = "vkGetPhysicalDeviceSurfaceFormatsKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceFormatsKHR)},
            {.name = "vkGetPhysicalDeviceSurfacePresentModesKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfacePresentModesKHR)},
            {.name = "vkGetPhysicalDeviceWin32PresentationSupportKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceWin32PresentationSupportKHR)},
            {.name = "vkGetPhysicalDeviceExternalSemaphoreProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceExternalSemaphoreProperties)},
            {.name = "vkGetPhysicalDeviceSparseImageFormatProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSparseImageFormatProperties)},
            {.name = "vkGetPhysicalDeviceSparseImageFormatProperties2",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSparseImageFormatProperties2)},
            {.name = "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)},
            {.name = "vkGetPhysicalDeviceFragmentShadingRatesKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFragmentShadingRatesKHR)},
            {.name = "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)},
            {.name = "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)},
            {.name = "vkGetCalibratedTimestampsKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetCalibratedTimestampsKHR)},
            {.name = "vkGetCalibratedTimestampsEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetCalibratedTimestampsEXT)},
            {.name = "vkGetPhysicalDeviceSurfaceCapabilities2KHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceCapabilities2KHR)},
            {.name = "vkGetPhysicalDeviceSurfaceFormats2KHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceFormats2KHR)},
            {.name = "vkGetPhysicalDeviceSurfacePresentModes2EXT",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfacePresentModes2EXT)},
            {.name = "vkReleaseSwapchainImagesEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkReleaseSwapchainImagesEXT)},
            {.name = "vkCreateDevice", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice)},
            {.name = "vkDestroyDevice", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice)},
            {.name = "vkGetDeviceQueue", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue)},
            {.name = "vkCreateCommandPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateCommandPool)},
            {.name = "vkDestroyCommandPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyCommandPool)},
            {.name = "vkAllocateCommandBuffers", .func = reinterpret_cast<PFN_vkVoidFunction>(vkAllocateCommandBuffers)},
            {.name = "vkFreeCommandBuffers", .func = reinterpret_cast<PFN_vkVoidFunction>(vkFreeCommandBuffers)},
            {.name = "vkBeginCommandBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBeginCommandBuffer)},
            {.name = "vkEndCommandBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkEndCommandBuffer)},
            {.name = "vkResetCommandPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetCommandPool)},
            {.name = "vkResetCommandBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetCommandBuffer)},
            {.name = "vkQueueWaitIdle", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueueWaitIdle)},
            {.name = "vkDeviceWaitIdle", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDeviceWaitIdle)},
            {.name = "vkCreateEvent", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateEvent)},
            {.name = "vkDestroyEvent", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyEvent)},
            {.name = "vkGetEventStatus", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetEventStatus)},
            {.name = "vkSetEvent", .func = reinterpret_cast<PFN_vkVoidFunction>(vkSetEvent)},
            {.name = "vkResetEvent", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetEvent)},
            {.name = "vkCmdSetEvent", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetEvent)},
            {.name = "vkCmdResetEvent", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResetEvent)},
            {.name = "vkCmdWaitEvents", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdWaitEvents)},
            {.name = "vkCmdSetEvent2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetEvent2)},
            {.name = "vkCmdSetEvent2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetEvent2KHR)},
            {.name = "vkCmdResetEvent2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResetEvent2)},
            {.name = "vkCmdResetEvent2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResetEvent2KHR)},
            {.name = "vkCmdWaitEvents2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdWaitEvents2)},
            {.name = "vkCmdWaitEvents2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdWaitEvents2KHR)},
            {.name = "vkCreateFence", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateFence)},
            {.name = "vkCreateSemaphore", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateSemaphore)},
            {.name = "vkDestroySemaphore", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroySemaphore)},
            {.name = "vkGetSemaphoreCounterValue", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetSemaphoreCounterValue)},
            {.name = "vkGetSemaphoreCounterValueKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetSemaphoreCounterValue)},
            {.name = "vkSignalSemaphore", .func = reinterpret_cast<PFN_vkVoidFunction>(vkSignalSemaphore)},
            {.name = "vkSignalSemaphoreKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkSignalSemaphore)},
            {.name = "vkWaitSemaphores", .func = reinterpret_cast<PFN_vkVoidFunction>(vkWaitSemaphores)},
            {.name = "vkWaitSemaphoresKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkWaitSemaphores)},
            {.name = "vkGetBufferDeviceAddress", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferDeviceAddress)},
            {.name = "vkGetBufferDeviceAddressKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferDeviceAddress)},
            {.name = "vkGetBufferDeviceAddressEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferDeviceAddress)},
            {.name = "vkDestroyFence", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyFence)},
            {.name = "vkResetFences", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetFences)},
            {.name = "vkGetFenceStatus", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetFenceStatus)},
            {.name = "vkQueueSubmit", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit)},
            {.name = "vkQueueSubmit2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit2)},
            {.name = "vkQueueSubmit2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit2)},
            {.name = "vkWaitForFences", .func = reinterpret_cast<PFN_vkVoidFunction>(vkWaitForFences)},
            {.name = "vkGetPhysicalDeviceMemoryProperties",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceMemoryProperties)},
            {.name = "vkSetDeviceMemoryPriorityEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkSetDeviceMemoryPriorityEXT)},
            {.name = "vkAllocateMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkAllocateMemory)},
            {.name = "vkFreeMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkFreeMemory)},
            {.name = "vkMapMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkMapMemory)},
            {.name = "vkUnmapMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkUnmapMemory)},
            {.name = "vkCreateBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateBuffer)},
            {.name = "vkDestroyBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyBuffer)},
            {.name = "vkCreateBufferView", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateBufferView)},
            {.name = "vkDestroyBufferView", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyBufferView)},
            {.name = "vkCmdCopyBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBuffer)},
            {.name = "vkCmdCopyBuffer2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBuffer2)},
            {.name = "vkCmdCopyBuffer2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBuffer2)},
            {.name = "vkCreateQueryPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateQueryPool)},
            {.name = "vkDestroyQueryPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyQueryPool)},
            {.name = "vkResetQueryPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetQueryPool)},
            {.name = "vkResetQueryPoolEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetQueryPool)},
            {.name = "vkGetQueryPoolResults", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetQueryPoolResults)},
            {.name = "vkCmdResetQueryPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResetQueryPool)},
            {.name = "vkCmdBeginQuery", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginQuery)},
            {.name = "vkCmdBeginConditionalRenderingEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginConditionalRenderingEXT)},
            {.name = "vkCmdEndConditionalRenderingEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndConditionalRenderingEXT)},
            {.name = "vkCmdBeginQueryIndexedEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginQueryIndexedEXT)},
            {.name = "vkCmdEndQuery", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndQuery)},
            {.name = "vkCmdCopyQueryPoolResults", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyQueryPoolResults)},
            {.name = "vkCmdEndQueryIndexedEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndQueryIndexedEXT)},
            {.name = "vkCmdBindTransformFeedbackBuffersEXT",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindTransformFeedbackBuffersEXT)},
            {.name = "vkCmdBeginTransformFeedbackEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginTransformFeedbackEXT)},
            {.name = "vkCmdEndTransformFeedbackEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndTransformFeedbackEXT)},
            {.name = "vkCmdDrawIndirectByteCountEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawIndirectByteCountEXT)},
            {.name = "vkCmdWriteTimestamp", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdWriteTimestamp)},
            {.name = "vkCmdWriteTimestamp2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdWriteTimestamp2)},
            {.name = "vkCmdWriteTimestamp2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdWriteTimestamp2KHR)},
            {.name = "vkFlushMappedMemoryRanges", .func = reinterpret_cast<PFN_vkVoidFunction>(vkFlushMappedMemoryRanges)},
            {.name = "vkInvalidateMappedMemoryRanges", .func = reinterpret_cast<PFN_vkVoidFunction>(vkInvalidateMappedMemoryRanges)},
            {.name = "vkGetBufferMemoryRequirements", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferMemoryRequirements)},
            {.name = "vkGetBufferMemoryRequirements2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferMemoryRequirements2)},
            {.name = "vkGetBufferMemoryRequirements2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetBufferMemoryRequirements2)},
            {.name = "vkBindBufferMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindBufferMemory)},
            {.name = "vkBindBufferMemory2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindBufferMemory2)},
            {.name = "vkBindBufferMemory2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindBufferMemory2)},
            {.name = "vkBindImageMemory2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindImageMemory2)},
            {.name = "vkBindImageMemory2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindImageMemory2)},
            {.name = "vkCmdFillBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdFillBuffer)},
            {.name = "vkCmdWriteBufferMarkerAMD", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdWriteBufferMarkerAMD)},
            {.name = "vkCmdWriteBufferMarker2AMD", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdWriteBufferMarker2AMD)},
            {.name = "vkCreateImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateImage)},
            {.name = "vkDestroyImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImage)},
            {.name = "vkGetImageMemoryRequirements", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageMemoryRequirements)},
            {.name = "vkGetImageSubresourceLayout", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageSubresourceLayout)},
            {.name = "vkGetImageSubresourceLayout2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageSubresourceLayout2KHR)},
            {.name = "vkGetImageSubresourceLayout2EXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageSubresourceLayout2KHR)},
            {.name = "vkGetDeviceImageSubresourceLayout",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceImageSubresourceLayoutKHR)},
            {.name = "vkGetDeviceImageSubresourceLayoutKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceImageSubresourceLayoutKHR)},
            {.name = "vkGetImageMemoryRequirements2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageMemoryRequirements2)},
            {.name = "vkGetImageMemoryRequirements2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageMemoryRequirements2)},
            {.name = "vkGetDeviceBufferMemoryRequirements",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceBufferMemoryRequirements)},
            {.name = "vkGetDeviceBufferMemoryRequirementsKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceBufferMemoryRequirements)},
            {.name = "vkGetDeviceImageMemoryRequirements",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceImageMemoryRequirements)},
            {.name = "vkGetDeviceImageMemoryRequirementsKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceImageMemoryRequirements)},
            {.name = "vkBindImageMemory", .func = reinterpret_cast<PFN_vkVoidFunction>(vkBindImageMemory)},
            {.name = "vkCmdPipelineBarrier", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier)},
            {.name = "vkCmdPipelineBarrier2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier2)},
            {.name = "vkCmdPipelineBarrier2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier2KHR)},
            {.name = "vkCmdClearColorImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdClearColorImage)},
            {.name = "vkCmdClearAttachments", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdClearAttachments)},
            {.name = "vkCmdClearDepthStencilImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdClearDepthStencilImage)},
            {.name = "vkCmdCopyImageToBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImageToBuffer)},
            {.name = "vkCmdCopyImageToBuffer2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImageToBuffer2)},
            {.name = "vkCmdCopyImageToBuffer2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImageToBuffer2KHR)},
            {.name = "vkCmdCopyBufferToImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBufferToImage)},
            {.name = "vkCmdCopyBufferToImage2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBufferToImage2)},
            {.name = "vkCmdCopyBufferToImage2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyBufferToImage2)},
            {.name = "vkCmdCopyImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage)},
            {.name = "vkCmdCopyImage2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage2)},
            {.name = "vkCmdCopyImage2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage2)},
            {.name = "vkCmdUpdateBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdUpdateBuffer)},
            {.name = "vkCmdResolveImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage)},
            {.name = "vkCmdResolveImage2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage2)},
            {.name = "vkCmdResolveImage2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage2)},
            {.name = "vkCreateSampler", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateSampler)},
            {.name = "vkDestroySampler", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroySampler)},
            {.name = "vkCreateWin32SurfaceKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateWin32SurfaceKHR)},
            {.name = "vkDestroySurfaceKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroySurfaceKHR)},
            {.name = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)},
            {.name = "vkCreateSwapchainKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR)},
            {.name = "vkDestroySwapchainKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroySwapchainKHR)},
            {.name = "vkGetSwapchainImagesKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetSwapchainImagesKHR)},
            {.name = "vkAcquireNextImageKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImageKHR)},
            {.name = "vkAcquireNextImage2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImage2KHR)},
            {.name = "vkQueuePresentKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR)},
            {.name = "vkCreateShaderModule", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateShaderModule)},
            {.name = "vkDestroyShaderModule", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyShaderModule)},
            {.name = "vkGetShaderModuleCreateInfoIdentifierEXT",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetShaderModuleCreateInfoIdentifierEXT)},
            {.name = "vkGetShaderModuleIdentifierEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetShaderModuleIdentifierEXT)},
            {.name = "vkCreateImageView", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateImageView)},
            {.name = "vkDestroyImageView", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImageView)},
            {.name = "vkCreateRenderPass", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateRenderPass)},
            {.name = "vkCreateRenderPass2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateRenderPass2)},
            {.name = "vkCreateRenderPass2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateRenderPass2KHR)},
            {.name = "vkCmdBeginRenderPass2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderPass2)},
            {.name = "vkCmdBeginRenderPass2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderPass2KHR)},
            {.name = "vkCmdNextSubpass2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdNextSubpass2)},
            {.name = "vkCmdNextSubpass2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdNextSubpass2KHR)},
            {.name = "vkCmdEndRenderPass2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderPass2)},
            {.name = "vkCmdEndRenderPass2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderPass2KHR)},
            {.name = "vkGetRenderAreaGranularity", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetRenderAreaGranularity)},
            {.name = "vkGetRenderingAreaGranularity", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetRenderingAreaGranularity)},
            {.name = "vkGetRenderingAreaGranularityKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetRenderingAreaGranularityKHR)},
            {.name = "vkDestroyRenderPass", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyRenderPass)},
            {.name = "vkCreateFramebuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateFramebuffer)},
            {.name = "vkDestroyFramebuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyFramebuffer)},
            {.name = "vkCreatePipelineLayout", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreatePipelineLayout)},
            {.name = "vkDestroyPipelineLayout", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyPipelineLayout)},
            {.name = "vkCreateDescriptorSetLayout", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateDescriptorSetLayout)},
            {.name = "vkDestroyDescriptorSetLayout", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDescriptorSetLayout)},
            {.name = "vkCreateDescriptorPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateDescriptorPool)},
            {.name = "vkDestroyDescriptorPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDescriptorPool)},
            {.name = "vkResetDescriptorPool", .func = reinterpret_cast<PFN_vkVoidFunction>(vkResetDescriptorPool)},
            {.name = "vkAllocateDescriptorSets", .func = reinterpret_cast<PFN_vkVoidFunction>(vkAllocateDescriptorSets)},
            {.name = "vkFreeDescriptorSets", .func = reinterpret_cast<PFN_vkVoidFunction>(vkFreeDescriptorSets)},
            {.name = "vkGetDescriptorSetLayoutSupport", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDescriptorSetLayoutSupport)},
            {.name = "vkGetDescriptorSetLayoutSupportKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDescriptorSetLayoutSupportKHR)},
            {.name = "vkGetDeviceImageSparseMemoryRequirements",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceImageSparseMemoryRequirements)},
            {.name = "vkGetDeviceMemoryCommitment", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceMemoryCommitment)},
            {.name = "vkGetImageSparseMemoryRequirements",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageSparseMemoryRequirements)},
            {.name = "vkGetImageSparseMemoryRequirements2",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageSparseMemoryRequirements2)},
            {.name = "vkGetImageSparseMemoryRequirements2KHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetImageSparseMemoryRequirements2KHR)},
            {.name = "vkQueueBindSparse", .func = reinterpret_cast<PFN_vkVoidFunction>(vkQueueBindSparse)},
            {.name = "vkUpdateDescriptorSets", .func = reinterpret_cast<PFN_vkVoidFunction>(vkUpdateDescriptorSets)},
            {.name = "vkCreateDescriptorUpdateTemplate", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateDescriptorUpdateTemplate)},
            {.name = "vkCreateDescriptorUpdateTemplateKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateDescriptorUpdateTemplate)},
            {.name = "vkDestroyDescriptorUpdateTemplate", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDescriptorUpdateTemplate)},
            {.name = "vkDestroyDescriptorUpdateTemplateKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDescriptorUpdateTemplate)},
            {.name = "vkUpdateDescriptorSetWithTemplate", .func = reinterpret_cast<PFN_vkVoidFunction>(vkUpdateDescriptorSetWithTemplate)},
            {.name = "vkUpdateDescriptorSetWithTemplateKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkUpdateDescriptorSetWithTemplate)},
            {.name = "vkCmdBindDescriptorSets", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindDescriptorSets)},
            {.name = "vkCreatePipelineCache", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreatePipelineCache)},
            {.name = "vkDestroyPipelineCache", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyPipelineCache)},
            {.name = "vkGetPipelineCacheData", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPipelineCacheData)},
            {.name = "vkMergePipelineCaches", .func = reinterpret_cast<PFN_vkVoidFunction>(vkMergePipelineCaches)},
            {.name = "vkCreateGraphicsPipelines", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateGraphicsPipelines)},
            {.name = "vkCreateComputePipelines", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCreateComputePipelines)},
            {.name = "vkDestroyPipeline", .func = reinterpret_cast<PFN_vkVoidFunction>(vkDestroyPipeline)},
            {.name = "vkCmdBeginRenderPass", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderPass)},
            {.name = "vkCmdBeginRendering", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRendering)},
            {.name = "vkCmdBeginRenderingKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRendering)},
            {.name = "vkCmdEndRendering", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRendering)},
            {.name = "vkCmdEndRenderingKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRendering)},
            {.name = "vkCmdExecuteCommands", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdExecuteCommands)},
            {.name = "vkCmdBindPipeline", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindPipeline)},
            {.name = "vkCmdDraw", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDraw)},
            {.name = "vkCmdDrawMultiEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawMultiEXT)},
            {.name = "vkCmdDrawMultiIndexedEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawMultiIndexedEXT)},
            {.name = "vkCmdDispatch", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDispatch)},
            {.name = "vkCmdDispatchIndirect", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDispatchIndirect)},
            {.name = "vkCmdBlitImage2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage2)},
            {.name = "vkCmdBlitImage", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage)},
            {.name = "vkCmdBlitImage2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage2KHR)},
            {.name = "vkCmdBindVertexBuffers", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindVertexBuffers)},
            {.name = "vkCmdBindVertexBuffers2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindVertexBuffers2)},
            {.name = "vkCmdBindVertexBuffers2EXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindVertexBuffers2)},
            {.name = "vkCmdBindDescriptorSets2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindDescriptorSets2KHR)},
            {.name = "vkCmdBindDescriptorSets2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindDescriptorSets2KHR)},
            {.name = "vkCmdBindIndexBuffer", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindIndexBuffer)},
            {.name = "vkCmdBindIndexBuffer2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindIndexBuffer2KHR)},
            {.name = "vkCmdBindIndexBuffer2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindIndexBuffer2KHR)},
            {.name = "vkCmdDrawIndexed", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawIndexed)},
            {.name = "vkCmdDrawIndexedIndirect", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawIndexedIndirect)},
            {.name = "vkCmdDrawIndexedIndirectCount", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawIndexedIndirectCount)},
            {.name = "vkCmdDrawIndexedIndirectCountKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawIndexedIndirectCountKHR)},
            {.name = "vkCmdDrawIndirect", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawIndirect)},
            {.name = "vkCmdDrawIndirectCount", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawIndirectCount)},
            {.name = "vkCmdDrawIndirectCountKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdDrawIndirectCountKHR)},
            {.name = "vkCmdEndRenderPass", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderPass)},
            {.name = "vkCmdNextSubpass", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdNextSubpass)},
            {.name = "vkCmdPushConstants", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPushConstants)},
            {.name = "vkCmdPushConstants2KHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPushConstants2KHR)},
            {.name = "vkCmdPushConstants2", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdPushConstants2KHR)},
            {.name = "vkCmdSetViewport", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetViewport)},
            {.name = "vkCmdSetViewportWithCount", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetViewportWithCount)},
            {.name = "vkCmdSetViewportWithCountEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetViewportWithCount)},
            {.name = "vkCmdSetScissor", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetScissor)},
            {.name = "vkCmdSetScissorWithCount", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetScissorWithCount)},
            {.name = "vkCmdSetScissorWithCountEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetScissorWithCount)},
            {.name = "vkCmdSetDepthBias", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBias)},
            {.name = "vkCmdSetBlendConstants", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetBlendConstants)},
            {.name = "vkCmdSetDepthBounds", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBounds)},
            {.name = "vkCmdSetLineWidth", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetLineWidth)},
            {.name = "vkCmdSetStencilCompareMask", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilCompareMask)},
            {.name = "vkCmdSetStencilWriteMask", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilWriteMask)},
            {.name = "vkCmdSetStencilReference", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilReference)},
            {.name = "vkCmdSetStencilOp", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilOp)},
            {.name = "vkCmdSetStencilOpEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilOp)},
            {.name = "vkCmdSetAttachmentFeedbackLoopEnableEXT",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetAttachmentFeedbackLoopEnableEXT)},
            {.name = "vkCmdSetCoverageModulationModeNV", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetCoverageModulationModeNV)},
            {.name = "vkCmdSetCoverageModulationTableEnableNV",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetCoverageModulationTableEnableNV)},
            {.name = "vkCmdSetCoverageReductionModeNV", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetCoverageReductionModeNV)},
            {.name = "vkCmdSetCoverageToColorEnableNV", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetCoverageToColorEnableNV)},
            {.name = "vkCmdSetCoverageToColorLocationNV", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetCoverageToColorLocationNV)},
            {.name = "vkCmdSetDepthClipNegativeOneToOneEXT",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthClipNegativeOneToOneEXT)},
            {.name = "vkCmdSetDiscardRectangleEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDiscardRectangleEnableEXT)},
            {.name = "vkCmdSetDiscardRectangleModeEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDiscardRectangleModeEXT)},
            {.name = "vkCmdSetLineStippleEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetLineStippleEnableEXT)},
            {.name = "vkCmdSetLogicOpEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetLogicOpEXT)},
            {.name = "vkCmdSetPatchControlPointsEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPatchControlPointsEXT)},
            {.name = "vkCmdSetPrimitiveRestartIndexEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPrimitiveRestartIndexEXT)},
            {.name = "vkCmdSetProvokingVertexModeEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetProvokingVertexModeEXT)},
            {.name = "vkCmdSetRayTracingPipelineStackSizeKHR",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetRayTracingPipelineStackSizeKHR)},
            {.name = "vkCmdSetRepresentativeFragmentTestEnableNV",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetRepresentativeFragmentTestEnableNV)},
            {.name = "vkCmdSetShadingRateImageEnableNV", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetShadingRateImageEnableNV)},
            {.name = "vkCmdSetViewportWScalingEnableNV", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetViewportWScalingEnableNV)},
            {.name = "vkCmdSetDeviceMask", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDeviceMask)},
            {.name = "vkCmdSetColorBlendAdvancedEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetColorBlendAdvancedEXT)},
            {.name = "vkCmdSetColorWriteEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetColorWriteEnableEXT)},
            {.name = "vkCmdSetCoverageModulationTableNV", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetCoverageModulationTableNV)},
            {.name = "vkCmdSetDiscardRectangleEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDiscardRectangleEXT)},
            {.name = "vkCmdSetExclusiveScissorEnableNV", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetExclusiveScissorEnableNV)},
            {.name = "vkCmdSetExclusiveScissorNV", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetExclusiveScissorNV)},
            {.name = "vkCmdSetViewportSwizzleNV", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetViewportSwizzleNV)},
            {.name = "vkCmdSetViewportWScalingNV", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetViewportWScalingNV)},
            {.name = "vkCmdSetLineStipple", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetLineStipple)},
            {.name = "vkCmdSetFragmentShadingRateKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetFragmentShadingRateKHR)},
            {.name = "vkCmdSetFragmentShadingRateEnumNV", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetFragmentShadingRateEnumNV)},
            {.name = "vkCmdSetDepthClampRangeEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthClampRangeEXT)},
            {.name = "vkCmdSetDeviceMaskKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDeviceMaskKHR)},
            {.name = "vkCmdSetLineStippleKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetLineStippleKHR)},
            {.name = "vkCmdSetLineStippleEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetLineStippleEXT)},
            {.name = "vkCmdSetAlphaToCoverageEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetAlphaToCoverageEnableEXT)},
            {.name = "vkCmdSetAlphaToOneEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetAlphaToOneEnableEXT)},
            {.name = "vkCmdSetDepthClampEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthClampEnableEXT)},
            {.name = "vkCmdSetLogicOpEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetLogicOpEnableEXT)},
            {.name = "vkCmdSetPolygonModeEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPolygonModeEXT)},
            {.name = "vkCmdSetRasterizationSamplesEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetRasterizationSamplesEXT)},
            {.name = "vkCmdSetRasterizationStreamEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetRasterizationStreamEXT)},
            {.name = "vkCmdSetConservativeRasterizationModeEXT",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetConservativeRasterizationModeEXT)},
            {.name = "vkCmdSetSampleLocationsEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetSampleLocationsEnableEXT)},
            {.name = "vkCmdSetLineRasterizationModeEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetLineRasterizationModeEXT)},
            {.name = "vkCmdSetTessellationDomainOriginEXT",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetTessellationDomainOriginEXT)},
            {.name = "vkCmdSetExtraPrimitiveOverestimationSizeEXT",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetExtraPrimitiveOverestimationSizeEXT)},
            {.name = "vkCmdSetColorBlendEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetColorBlendEnableEXT)},
            {.name = "vkCmdSetColorBlendEquationEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetColorBlendEquationEXT)},
            {.name = "vkCmdSetColorWriteMaskEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetColorWriteMaskEXT)},
            {.name = "vkCmdSetSampleMaskEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetSampleMaskEXT)},
            {.name = "vkGetPhysicalDeviceMultisamplePropertiesEXT",
             .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceMultisamplePropertiesEXT)},
            {.name = "vkCmdSetSampleLocationsEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetSampleLocationsEXT)},
            {.name = "vkCmdSetCullMode", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetCullMode)},
            {.name = "vkCmdSetCullModeEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetCullMode)},
            {.name = "vkCmdSetFrontFace", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetFrontFace)},
            {.name = "vkCmdSetFrontFaceEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetFrontFace)},
            {.name = "vkCmdSetPrimitiveTopology", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPrimitiveTopology)},
            {.name = "vkCmdSetPrimitiveTopologyEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPrimitiveTopology)},
            {.name = "vkCmdSetDepthTestEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthTestEnable)},
            {.name = "vkCmdSetDepthTestEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthTestEnable)},
            {.name = "vkCmdSetDepthWriteEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthWriteEnable)},
            {.name = "vkCmdSetDepthWriteEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthWriteEnable)},
            {.name = "vkCmdSetDepthCompareOp", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthCompareOp)},
            {.name = "vkCmdSetDepthCompareOpEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthCompareOp)},
            {.name = "vkCmdSetDepthBoundsTestEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBoundsTestEnable)},
            {.name = "vkCmdSetDepthBoundsTestEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBoundsTestEnable)},
            {.name = "vkCmdSetDepthClipEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthClipEnableEXT)},
            {.name = "vkCmdSetStencilTestEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilTestEnable)},
            {.name = "vkCmdSetStencilTestEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetStencilTestEnable)},
            {.name = "vkCmdSetRasterizerDiscardEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetRasterizerDiscardEnable)},
            {.name = "vkCmdSetRasterizerDiscardEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetRasterizerDiscardEnable)},
            {.name = "vkCmdSetDepthBiasEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBiasEnable)},
            {.name = "vkCmdSetDepthBiasEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetDepthBiasEnable)},
            {.name = "vkCmdSetPrimitiveRestartEnable", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPrimitiveRestartEnable)},
            {.name = "vkCmdSetPrimitiveRestartEnableEXT", .func = reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetPrimitiveRestartEnable)},
            {.name = "vkGetMemoryWin32HandleKHR", .func = reinterpret_cast<PFN_vkVoidFunction>(vkGetMemoryWin32HandleKHR)},
        };

        for (const auto& e : table)
        {
            if (std::strcmp(e.name, pName) == 0)
            {
                return e.func;
            }
        }

        // Not implemented: return null per the Vulkan contract (callers use null to detect absent
        // optional functions/extensions) but log the name so missing entry points stay visible.
        std::array<char, 256> message{};
        std::snprintf(message.data(), message.size(), "vulkan-shim: no implementation for Vulkan function: %s (returning null)\n", pName);
        shim_log(message.data());
        return nullptr;
    }

    // Device-level entry points resolve through the same table for this minimal shim.
    __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName)
    {
        if (passthrough_active())
        {
            return g_real_get_device_proc_addr ? g_real_get_device_proc_addr(device, pName)
                                               : g_real_get_instance_proc_addr(VK_NULL_HANDLE, pName);
        }

        if (pName && (std::strcmp(pName, "vkCmdWriteBufferMarkerAMD") == 0 || std::strcmp(pName, "vkCmdWriteBufferMarker2AMD") == 0))
        {
            std::lock_guard lock(g_buffer_marker_devices_mutex);
            if (!g_buffer_marker_devices.contains(to_object_id(device)) ||
                (std::strcmp(pName, "vkCmdWriteBufferMarker2AMD") == 0 && !g_buffer_marker2_devices.contains(to_object_id(device))))
            {
                return nullptr;
            }
        }
        if (pName && (std::strcmp(pName, "vkCmdDrawMultiEXT") == 0 ||
                      std::strcmp(pName, "vkCmdDrawMultiIndexedEXT") == 0))
        {
            std::lock_guard lock(g_multi_draw_devices_mutex);
            if (!g_multi_draw_devices.contains(to_object_id(device)))
            {
                return nullptr;
            }
        }
        return vkGetInstanceProcAddr(VK_NULL_HANDLE, pName);
    }

    // Some loaders / probes bootstrap through the ICD-style export instead.
    __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName)
    {
        return vkGetInstanceProcAddr(instance, pName);
    }
}
