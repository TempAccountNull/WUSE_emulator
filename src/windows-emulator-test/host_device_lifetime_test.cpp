#include "emulation_test_utils.hpp"
#include <gpu_bridge_protocol.hpp>
#include <syscall_utils.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <span>

namespace sogen::syscalls
{
    NTSTATUS handle_NtMapViewOfSection(const syscall_context&, handle, handle, emulator_object<uint64_t>, uint64_t, uint64_t,
                                       emulator_object<LARGE_INTEGER>, emulator_object<uint64_t>, SECTION_INHERIT, ULONG, ULONG);
    NTSTATUS handle_NtUnmapViewOfSection(const syscall_context&, handle, uint64_t);
    NTSTATUS handle_NtClose(const syscall_context&, handle);
}

namespace sogen::test
{
    class HostDeviceLifetimeTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            emulator_interfaces interfaces{};
            interfaces.audio = std::make_unique<null_audio_backend>();
            return create_sample_emulator(std::move(settings), {}, {}, std::move(interfaces));
        }();
        uint64_t arguments{};
        static constexpr uint64_t view = 0x73000000;
        static constexpr size_t page = 0x1000;

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            ASSERT_FALSE(emu.process.is_wow64_process);
            arguments = emu.memory.allocate_memory(0x10000, memory_permission::read_write);
            ASSERT_NE(arguments, 0u);
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        NTSTATUS map_section(const handle section_handle)
        {
            emu.emu().write_memory<uint64_t>(arguments, view);
            emu.emu().write_memory<uint64_t>(arguments + 8, 0);
            emu.emu().write_memory<uint64_t>(arguments + 16, 0);
            return syscalls::handle_NtMapViewOfSection(context(), section_handle, CURRENT_PROCESS, {emu.memory, arguments}, 0, 0,
                                                       {emu.memory, arguments + 16}, {emu.memory, arguments + 8}, ViewShare, 0,
                                                       PAGE_READWRITE);
        }
    };

    class AudioHostLifetimeTest : public HostDeviceLifetimeTest
    {
      protected:
        handle port_handle{};
        handle section_handle{};
        uint64_t backing{};
        uint64_t section_size{};
        std::weak_ptr<void> storage;

        lpc_request_result request(const std::span<const uint8_t> bytes)
        {
            emu.emu().write_memory(arguments + 0x1000, bytes.data(), bytes.size());
            return emu.process.ports.get(port_handle)
                ->handle_request(emu, {.send_buffer = arguments + 0x1000,
                                       .send_buffer_length = static_cast<ULONG>(bytes.size()),
                                       .recv_buffer = arguments + 0x2000,
                                       .recv_buffer_length = 0x1000});
        }

        lpc_request_result rpc(const uint32_t procedure, const std::span<const uint8_t> payload = {})
        {
            std::vector<uint8_t> bytes(0x40 + payload.size());
            std::memcpy(bytes.data() + 20, &procedure, sizeof(procedure));
            if (!payload.empty())
            {
                std::memcpy(bytes.data() + 0x40, payload.data(), payload.size());
            }
            return request(bytes);
        }

        void open_stream()
        {
            port_handle = emu.process.ports.store(port_container{u"\\RPC Control\\Audiosrv", emu, {}});
            std::array<uint8_t, 36> bind{};
            bind[0] = 1;
            bind[32] = 3;
            constexpr std::array<uint8_t, 16> audio_client = {0x11, 0xd1, 0x74, 0xd5, 0x26, 0x61, 0xd7, 0x49,
                                                              0x9b, 0x86, 0x4d, 0xe6, 0xb6, 0x50, 0xd4, 0xfc};
            std::memcpy(bind.data() + 12, audio_client.data(), audio_client.size());
            ASSERT_EQ(request(bind).status, STATUS_SUCCESS);

            struct wave_format
            {
                uint16_t tag{0xfffe};
                uint16_t channels{2};
                uint32_t sample_rate{48000};
                uint32_t average_bytes_per_second{192000};
                uint16_t block_align{4};
                uint16_t bits_per_sample{16};
                uint16_t extra_size{22};
                uint16_t valid_bits_per_sample{16};
                uint32_t channel_mask{3};
                uint32_t subtype{1};
                std::array<uint8_t, 12> subtype_tail{0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
            };

            static_assert(sizeof(wave_format) == 40);
            const wave_format format{};
            // The fixture is x64; the existing audio RPC implementation reads NDR64's deferred format at +152.
            std::array<uint8_t, 192> open{};
            std::memcpy(open.data() + 152, &format, sizeof(format));
            ASSERT_EQ(rpc(4, open).status, STATUS_SUCCESS);
            std::array<uint8_t, 32> create{};
            constexpr uint64_t duration_hns = 100000;
            std::memcpy(create.data() + 24, &duration_hns, sizeof(duration_hns));
            const auto result = rpc(7, create);
            ASSERT_EQ(result.status, STATUS_SUCCESS);
            ASSERT_EQ(result.handles.size(), 1u);
            section_handle.bits = result.handles.front().handle;
            const auto* section = emu.process.sections.get(section_handle);
            ASSERT_NE(section, nullptr);
            backing = section->backing_address;
            section_size = section->maximum_size;
            ASSERT_NE(backing, 0u);
            const auto token = emu.memory.host_memory_backing_at(backing);
            ASSERT_NE(token, nullptr);
            storage = token->storage;
            ASSERT_FALSE(storage.expired());
            ASSERT_EQ(map_section(section_handle), STATUS_SUCCESS);
        }

        void verify_view_survives_producer_teardown(const bool close_port)
        {
            ASSERT_NO_FATAL_FAILURE(open_stream());
            if (close_port)
            {
                ASSERT_EQ(syscalls::handle_NtClose(context(), port_handle), STATUS_SUCCESS);
            }
            else
            {
                ASSERT_EQ(rpc(13).status, STATUS_SUCCESS);
            }
            ASSERT_FALSE(storage.expired());
            EXPECT_EQ(emu.process.sections.get(section_handle)->backing_address, backing);
            EXPECT_EQ(emu.memory.shared_view_source(view), backing);
            EXPECT_FALSE(emu.memory.host_memory_backing_at(view)->snapshot_reconstructible);
            emu.emu().write_memory<uint64_t>(view + 0x400, 0x123456789abcdef0);
            EXPECT_EQ(emu.emu().read_memory<uint64_t>(backing + 0x400), 0x123456789abcdef0u);
            ASSERT_EQ(syscalls::handle_NtClose(context(), section_handle), STATUS_SUCCESS);
            EXPECT_FALSE(storage.expired());
            EXPECT_EQ(emu.emu().read_memory<uint64_t>(view + 0x400), 0x123456789abcdef0u);
            ASSERT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, view), STATUS_SUCCESS);
            EXPECT_FALSE(emu.memory.get_region_info(view).is_reserved);
            EXPECT_FALSE(emu.memory.get_region_info(backing).is_reserved);
            EXPECT_TRUE(storage.expired());
        }
    };

    TEST_F(AudioHostLifetimeTest, DestroyStreamKeepsSectionAndViewUntilLastUnmap)
    {
        verify_view_survives_producer_teardown(false);
    }

    TEST_F(AudioHostLifetimeTest, FinalPortCloseKeepsSectionAndViewUntilLastUnmap)
    {
        verify_view_survives_producer_teardown(true);
    }

    TEST_F(AudioHostLifetimeTest, ClosedSectionAndViewDoNotLetProducerTeardownRevokeReusedAddress)
    {
        ASSERT_NO_FATAL_FAILURE(open_stream());
        ASSERT_EQ(syscalls::handle_NtClose(context(), section_handle), STATUS_SUCCESS);
        ASSERT_EQ(syscalls::handle_NtUnmapViewOfSection(context(), CURRENT_PROCESS, view), STATUS_SUCCESS);
        EXPECT_FALSE(emu.memory.get_region_info(backing).is_reserved);
        EXPECT_FALSE(storage.expired());
        ASSERT_TRUE(emu.memory.allocate_memory(backing, static_cast<size_t>(section_size), memory_permission::read_write));
        emu.emu().write_memory<uint64_t>(backing, 0x1122334455667788);
        ASSERT_EQ(rpc(13).status, STATUS_SUCCESS);
        EXPECT_TRUE(storage.expired());
        EXPECT_TRUE(emu.memory.get_region_info(backing).is_committed);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(backing), 0x1122334455667788u);
    }

    class GpuHostLifetimeTest : public HostDeviceLifetimeTest
    {
      protected:
        handle device_handle{};

        NTSTATUS ioctl(const ULONG code, const void* input, const size_t input_size, const size_t output_size)
        {
            if (input_size)
            {
                emu.emu().write_memory(arguments + 0x1000, input, input_size);
            }
            io_device_context request{emu.memory};
            request.io_control_code = code;
            request.io_status_block = {emu.memory, arguments};
            request.input_buffer = input_size ? arguments + 0x1000 : 0;
            request.input_buffer_length = static_cast<ULONG>(input_size);
            request.output_buffer = arguments + 0x2000;
            request.output_buffer_length = static_cast<ULONG>(output_size);
            request.vcpu = &emu.vcpu(0);
            return emu.process.devices.get(device_handle)->io_control(emu, request);
        }

        NTSTATUS create_instance()
        {
            constexpr gpu_bridge::create_instance_request request{.magic = gpu_bridge::create_instance_request_magic};
            return ioctl(gpu_bridge::ioctl_create_instance, &request, sizeof(request), sizeof(gpu_bridge::create_instance_response));
        }

        template <typename T>
        T output(const size_t offset = 0)
        {
            return emu.emu().read_memory<T>(arguments + 0x2000 + offset);
        }
    };

    TEST_F(GpuHostLifetimeTest, FinalDeviceHandleCloseRevokesDirectMappingAndSharedAlias)
    {
        device_handle = emu.process.devices.store(io_device_container{u"SogenGpu", emu, {}});
        ASSERT_EQ(create_instance(), STATUS_SUCCESS);
        const auto instance = output<gpu_bridge::create_instance_response>();
        if (instance.vk_result != 0)
        {
            GTEST_SKIP() << "Host Vulkan instance unavailable: " << instance.vk_result;
        }
        const gpu_bridge::enumerate_physical_devices_request enumerate{.instance = instance.instance, .max_count = 32};
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_enumerate_physical_devices, &enumerate, sizeof(enumerate), 8 + 32 * 8), STATUS_SUCCESS);
        const auto devices = output<gpu_bridge::enumerate_physical_devices_response>();
        ASSERT_EQ(devices.vk_result, 0);
        if (!devices.count)
        {
            GTEST_SKIP() << "Host Vulkan has no physical device";
        }
        const auto physical = output<uint64_t>(sizeof(devices));
        const gpu_bridge::get_queue_family_properties_request families{.physical_device = physical, .max_count = 32};
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_get_queue_family_properties, &families, sizeof(families),
                        sizeof(gpu_bridge::get_queue_family_properties_response) + 32 * sizeof(gpu_bridge::queue_family_properties)),
                  STATUS_SUCCESS);
        const auto family_count = output<gpu_bridge::get_queue_family_properties_response>().count;
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < std::min(family_count, uint32_t{32}); ++i)
        {
            if (output<gpu_bridge::queue_family_properties>(8 + i * sizeof(gpu_bridge::queue_family_properties)).queue_count)
            {
                family = i;
                break;
            }
        }
        ASSERT_NE(family, UINT32_MAX);

        struct device_request
        {
            gpu_bridge::create_device_request header{};
            gpu_bridge::device_queue_create_entry queue{};
        } create;

        static_assert(offsetof(device_request, queue) == sizeof(gpu_bridge::create_device_request));
        create.header.physical_device = physical;
        create.header.queue_create_count = 1;
        create.queue.queue_family_index = family;
        create.queue.queue_count = 1;
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_create_device, &create, sizeof(create), sizeof(gpu_bridge::create_device_response)),
                  STATUS_SUCCESS);
        const auto device = output<gpu_bridge::create_device_response>();
        ASSERT_EQ(device.vk_result, 0);
        ASSERT_NE(device.device, 0u);

        const gpu_bridge::get_physical_device_memory_properties_request properties{.physical_device = physical};
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_get_physical_device_memory_properties, &properties, sizeof(properties), 1024), STATUS_SUCCESS);
        // The wire uses VkPhysicalDeviceMemoryProperties: count followed by 32 {propertyFlags, heapIndex} pairs.
        const auto type_count = output<uint32_t>();
        ASSERT_LE(type_count, 32u);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < type_count; ++i)
        {
            constexpr uint32_t host_visible_coherent = 0x2 | 0x4;
            if ((output<uint32_t>(4 + 8 * i) & host_visible_coherent) == host_visible_coherent)
            {
                type = i;
                break;
            }
        }
        ASSERT_NE(type, UINT32_MAX);
        const gpu_bridge::allocate_memory_request allocation{.device = device.device, .size = page, .memory_type_index = type};
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_allocate_memory, &allocation, sizeof(allocation), sizeof(gpu_bridge::allocate_memory_response)),
                  STATUS_SUCCESS);
        const auto memory = output<gpu_bridge::allocate_memory_response>();
        ASSERT_EQ(memory.vk_result, 0);
        ASSERT_NE(memory.memory, 0u);
        const gpu_bridge::map_memory_direct_request map{.device = device.device, .memory = memory.memory, .offset = 0, .size = page};
        ASSERT_EQ(ioctl(gpu_bridge::ioctl_map_memory_direct, &map, sizeof(map), sizeof(gpu_bridge::map_memory_direct_response)),
                  STATUS_SUCCESS);
        const auto mapped = output<gpu_bridge::map_memory_direct_response>();
        ASSERT_EQ(mapped.vk_result, 0);
        ASSERT_NE(mapped.guest_address, 0u) << "This test requires a backend with direct host mapping support";
        ASSERT_TRUE(emu.memory.allocate_shared_view(view, mapped.guest_address, page, memory_permission::read_write));
        std::weak_ptr<memory_manager::host_memory_backing> backing = emu.memory.host_memory_backing_at(mapped.guest_address);
        emu.emu().write_memory<uint64_t>(view, 0x123456789abcdef0);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(mapped.guest_address), 0x123456789abcdef0u);

        // A nonfinal handle close must leave both the producer and aliases usable.
        const auto duplicate = emu.process.devices.duplicate(device_handle);
        ASSERT_TRUE(duplicate.has_value());
        ASSERT_EQ(syscalls::handle_NtClose(context(), device_handle), STATUS_SUCCESS);
        EXPECT_FALSE(backing.expired());
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(view), 0x123456789abcdef0u);
        ASSERT_EQ(syscalls::handle_NtClose(context(), *duplicate), STATUS_SUCCESS);
        EXPECT_EQ(emu.process.devices.get(*duplicate), nullptr);
        EXPECT_FALSE(emu.memory.get_region_info(mapped.guest_address).is_reserved);
        EXPECT_FALSE(emu.memory.get_region_info(view).is_reserved);
        EXPECT_EQ(emu.memory.shared_view_source(view), 0u);
        EXPECT_TRUE(backing.expired());
        uint64_t value{};
        EXPECT_FALSE(emu.memory.try_read_memory(mapped.guest_address, &value, sizeof(value)));
        EXPECT_FALSE(emu.memory.try_read_memory(view, &value, sizeof(value)));
    }

    TEST_F(GpuHostLifetimeTest, InstanceDestructionPreservesOtherInstanceMappingsAndAliases)
    {
        device_handle = emu.process.devices.store(io_device_container{u"SogenGpu", emu, {}});

        struct mapped_device
        {
            uint64_t instance{};
            uint64_t device{};
            uint64_t memory{};
            uint64_t address{};
            uint64_t alias{};
            std::weak_ptr<memory_manager::host_memory_backing> backing;
        };

        std::array<mapped_device, 2> mappings{};
        for (size_t index = 0; index < mappings.size(); ++index)
        {
            auto& mapping = mappings[index];
            const auto alias = view + index * 0x10000;
            ASSERT_EQ(create_instance(), STATUS_SUCCESS);
            const auto instance = output<gpu_bridge::create_instance_response>();
            if (index == 0 && instance.vk_result != 0)
            {
                GTEST_SKIP() << "Host Vulkan instance unavailable: " << instance.vk_result;
            }
            ASSERT_EQ(instance.vk_result, 0);
            const gpu_bridge::enumerate_physical_devices_request enumerate{.instance = instance.instance, .max_count = 32};
            ASSERT_EQ(ioctl(gpu_bridge::ioctl_enumerate_physical_devices, &enumerate, sizeof(enumerate), 8 + 32 * 8), STATUS_SUCCESS);
            const auto devices = output<gpu_bridge::enumerate_physical_devices_response>();
            ASSERT_EQ(devices.vk_result, 0);
            if (index == 0 && !devices.count)
            {
                GTEST_SKIP() << "Host Vulkan has no physical device";
            }
            ASSERT_GT(devices.count, 0u);
            const auto physical = output<uint64_t>(sizeof(devices));
            const gpu_bridge::get_queue_family_properties_request families{.physical_device = physical, .max_count = 32};
            ASSERT_EQ(ioctl(gpu_bridge::ioctl_get_queue_family_properties, &families, sizeof(families),
                            sizeof(gpu_bridge::get_queue_family_properties_response) + 32 * sizeof(gpu_bridge::queue_family_properties)),
                      STATUS_SUCCESS);
            const auto family_count = output<gpu_bridge::get_queue_family_properties_response>().count;
            uint32_t family = UINT32_MAX;
            for (uint32_t i = 0; i < std::min(family_count, uint32_t{32}); ++i)
            {
                if (output<gpu_bridge::queue_family_properties>(8 + i * sizeof(gpu_bridge::queue_family_properties)).queue_count)
                {
                    family = i;
                    break;
                }
            }
            ASSERT_NE(family, UINT32_MAX);

            struct device_request
            {
                gpu_bridge::create_device_request header{};
                gpu_bridge::device_queue_create_entry queue{};
            } create;

            static_assert(offsetof(device_request, queue) == sizeof(gpu_bridge::create_device_request));
            create.header.physical_device = physical;
            create.header.queue_create_count = 1;
            create.queue.queue_family_index = family;
            create.queue.queue_count = 1;
            ASSERT_EQ(ioctl(gpu_bridge::ioctl_create_device, &create, sizeof(create), sizeof(gpu_bridge::create_device_response)),
                      STATUS_SUCCESS);
            const auto device = output<gpu_bridge::create_device_response>();
            ASSERT_EQ(device.vk_result, 0);
            ASSERT_NE(device.device, 0u);

            const gpu_bridge::get_physical_device_memory_properties_request properties{.physical_device = physical};
            ASSERT_EQ(ioctl(gpu_bridge::ioctl_get_physical_device_memory_properties, &properties, sizeof(properties), 1024),
                      STATUS_SUCCESS);
            // The wire uses VkPhysicalDeviceMemoryProperties: count followed by 32 {propertyFlags, heapIndex} pairs.
            const auto type_count = output<uint32_t>();
            ASSERT_LE(type_count, 32u);
            uint32_t type = UINT32_MAX;
            for (uint32_t i = 0; i < type_count; ++i)
            {
                constexpr uint32_t host_visible_coherent = 0x2 | 0x4;
                if ((output<uint32_t>(4 + 8 * i) & host_visible_coherent) == host_visible_coherent)
                {
                    type = i;
                    break;
                }
            }
            ASSERT_NE(type, UINT32_MAX);
            const gpu_bridge::allocate_memory_request allocation{.device = device.device, .size = page, .memory_type_index = type};
            ASSERT_EQ(
                ioctl(gpu_bridge::ioctl_allocate_memory, &allocation, sizeof(allocation), sizeof(gpu_bridge::allocate_memory_response)),
                STATUS_SUCCESS);
            const auto memory = output<gpu_bridge::allocate_memory_response>();
            ASSERT_EQ(memory.vk_result, 0);
            ASSERT_NE(memory.memory, 0u);
            const gpu_bridge::map_memory_direct_request map{.device = device.device, .memory = memory.memory, .offset = 0, .size = page};
            ASSERT_EQ(ioctl(gpu_bridge::ioctl_map_memory_direct, &map, sizeof(map), sizeof(gpu_bridge::map_memory_direct_response)),
                      STATUS_SUCCESS);
            const auto mapped = output<gpu_bridge::map_memory_direct_response>();
            ASSERT_EQ(mapped.vk_result, 0);
            ASSERT_NE(mapped.guest_address, 0u) << "This test requires a backend with direct host mapping support";
            ASSERT_TRUE(emu.memory.allocate_shared_view(alias, mapped.guest_address, page, memory_permission::read_write));
            mapping.instance = instance.instance;
            mapping.device = device.device;
            mapping.memory = memory.memory;
            mapping.address = mapped.guest_address;
            mapping.alias = alias;
            mapping.backing = emu.memory.host_memory_backing_at(mapped.guest_address);
        }
        const auto& first = mappings[0];
        const auto& second = mappings[1];

        const auto expect_mapping_usable = [&](const mapped_device& mapping, const uint64_t value) -> void {
            ASSERT_FALSE(mapping.backing.expired());
            ASSERT_TRUE(emu.memory.get_region_info(mapping.address).is_committed);
            ASSERT_TRUE(emu.memory.get_region_info(mapping.alias).is_committed);
            EXPECT_EQ(emu.memory.host_memory_backing_at(mapping.address), mapping.backing.lock());
            EXPECT_EQ(emu.memory.host_memory_backing_at(mapping.alias), mapping.backing.lock());
            emu.emu().write_memory<uint64_t>(mapping.alias, value);
            EXPECT_EQ(emu.emu().read_memory<uint64_t>(mapping.address), value);
            emu.emu().write_memory<uint64_t>(mapping.address + 8, ~value);
            EXPECT_EQ(emu.emu().read_memory<uint64_t>(mapping.alias + 8), ~value);
        };

        const auto expect_mapping_revoked = [&](const mapped_device& mapping) -> void {
            EXPECT_TRUE(mapping.backing.expired());
            EXPECT_FALSE(emu.memory.get_region_info(mapping.address).is_reserved);
            EXPECT_FALSE(emu.memory.get_region_info(mapping.alias).is_reserved);
            EXPECT_EQ(emu.memory.shared_view_source(mapping.alias), 0u);
            uint64_t value{};
            EXPECT_FALSE(emu.memory.try_read_memory(mapping.address, &value, sizeof(value)));
            EXPECT_FALSE(emu.memory.try_read_memory(mapping.alias, &value, sizeof(value)));
        };

        const auto expect_mapping_contents = [&](const mapped_device& mapping, const uint64_t value) -> void {
            ASSERT_FALSE(mapping.backing.expired());
            ASSERT_TRUE(emu.memory.get_region_info(mapping.address).is_committed);
            ASSERT_TRUE(emu.memory.get_region_info(mapping.alias).is_committed);
            EXPECT_EQ(emu.emu().read_memory<uint64_t>(mapping.address), value);
            EXPECT_EQ(emu.emu().read_memory<uint64_t>(mapping.alias), value);
            EXPECT_EQ(emu.emu().read_memory<uint64_t>(mapping.address + 8), ~value);
            EXPECT_EQ(emu.emu().read_memory<uint64_t>(mapping.alias + 8), ~value);
        };

        const auto destroy_instance = [&](const uint64_t instance) {
            const gpu_bridge::destroy_instance_request request{.instance = instance};
            return ioctl(gpu_bridge::ioctl_destroy_instance, &request, sizeof(request), 0);
        };
        ASSERT_NE(first.instance, second.instance);
        ASSERT_NE(first.device, second.device);
        ASSERT_NE(first.memory, second.memory);
        ASSERT_NE(first.address, second.address);
        ASSERT_NO_FATAL_FAILURE(expect_mapping_usable(first, 0x1111222233334444));
        ASSERT_NO_FATAL_FAILURE(expect_mapping_usable(second, 0xaaaabbbbccccdddd));

        ASSERT_EQ(destroy_instance(UINT64_MAX), STATUS_SUCCESS);
        ASSERT_NO_FATAL_FAILURE(expect_mapping_contents(first, 0x1111222233334444));
        ASSERT_NO_FATAL_FAILURE(expect_mapping_contents(second, 0xaaaabbbbccccdddd));
        ASSERT_NO_FATAL_FAILURE(expect_mapping_usable(first, 0x1122334455667788));
        ASSERT_NO_FATAL_FAILURE(expect_mapping_usable(second, 0x8877665544332211));

        ASSERT_EQ(destroy_instance(first.instance), STATUS_SUCCESS);
        expect_mapping_revoked(first);
        ASSERT_NO_FATAL_FAILURE(expect_mapping_contents(second, 0x8877665544332211));
        ASSERT_NO_FATAL_FAILURE(expect_mapping_usable(second, 0x123456789abcdef0));
        // A repeated destruction must remain a no-op for the other live instance.
        ASSERT_EQ(destroy_instance(first.instance), STATUS_SUCCESS);
        ASSERT_NO_FATAL_FAILURE(expect_mapping_contents(second, 0x123456789abcdef0));
        ASSERT_NO_FATAL_FAILURE(expect_mapping_usable(second, 0x0fedcba987654321));

        ASSERT_EQ(syscalls::handle_NtClose(context(), device_handle), STATUS_SUCCESS);
        EXPECT_EQ(emu.process.devices.get(device_handle), nullptr);
        expect_mapping_revoked(second);
    }
}
