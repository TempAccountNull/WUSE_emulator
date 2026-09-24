#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include "../windows-emulator/io_device.hpp"
#include <native_wsi_wire.hpp>
#include <windows_emulator.hpp>
#include <utils/finally.hpp>

#include <gtest/gtest.h>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace sogen::test
{
    namespace wire = gpu_bridge::native_wsi;

    TEST(GpuBridgePresentation, MultiVcpuNativeModesFallBackToReadback)
    {
        const char* prior = std::getenv("SOGEN_VULKAN_PRESENT");
        const std::string saved = prior ? prior : "";
        const auto restore = utils::finally([&] { _putenv_s("SOGEN_VULKAN_PRESENT", saved.c_str()); });

        for (const char* requested : {"native", "direct", "gpu-copy"})
        {
            ASSERT_EQ(_putenv_s("SOGEN_VULKAN_PRESENT", requested), 0);

            emulator_settings settings{.disable_logging = true};
            settings.use_instruction_precision = false;
            settings.use_relative_time = false;
            settings.load_registry = false;
            emulator_interfaces interfaces{};
            interfaces.ui = std::make_unique<null_ui_backend>();
            windows_emulator win{icicle::create_x86_64_emulator(2), settings, {}, std::move(interfaces)};
            const auto buffer = win.memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(buffer, 0U);

            // Device creation must not require the worker to own SDL's UI thread.
            io_device_container bridge{u"SogenGpu", win, {}};
            wire::request query{};
            query.op = wire::operation::enabled;
            const auto bytes = wire::encode(query);
            win.emu().write_memory(buffer, bytes.data(), bytes.size());

            io_device_context context{win.memory};
            context.vcpu = &win.vcpu(0);
            context.io_control_code = wire::ioctl;
            context.input_buffer = buffer;
            context.input_buffer_length = static_cast<ULONG>(bytes.size());
            context.output_buffer = buffer + 0x200;
            context.output_buffer_length = sizeof(wire::response);
            ASSERT_EQ(bridge.io_control(win, context), STATUS_SUCCESS) << "requested=" << requested;
            const auto reply = win.emu().read_memory<wire::response>(context.output_buffer);
            EXPECT_EQ(reply.result, 0) << "requested=" << requested;
            EXPECT_EQ(reply.count, 0U) << "requested=" << requested;
            EXPECT_FALSE(win.ui().native_presentation_active()) << "requested=" << requested;
        }

        // The one-vCPU native path still requires a real SDL UI owner.
        ASSERT_EQ(_putenv_s("SOGEN_VULKAN_PRESENT", "native"), 0);
        emulator_settings settings{.disable_logging = true};
        settings.load_registry = false;
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        windows_emulator win{icicle::create_x86_64_emulator(1), settings, {}, std::move(interfaces)};
        EXPECT_THROW((io_device_container{u"SogenGpu", win, {}}), std::runtime_error);
    }
}
