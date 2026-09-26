#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <utils/finally.hpp>
#include <array>
#include <string>

namespace sogen::test
{
    TEST(GuestCxxThrowProbe, ExactVcruntimeExportCapturesBoundedArgumentsAndUnloads)
    {
        const char* prior = std::getenv("SOGEN_GUEST_CXX_THROW_PROBE");
        const std::string saved = prior ? prior : "";
        const auto restore = utils::finally([&] { _putenv_s("SOGEN_GUEST_CXX_THROW_PROBE", saved.c_str()); });
        ASSERT_EQ(_putenv_s("SOGEN_GUEST_CXX_THROW_PROBE", "1"), 0);

        emulator_settings settings{};
        settings.emulation_root = get_emulator_root();
        settings.use_relative_time = false;
        settings.use_instruction_precision = false;
        settings.load_registry = false;
        emulator_interfaces interfaces{};
        interfaces.socket_factory = network::create_static_socket_factory();
        interfaces.ui = std::make_unique<null_ui_backend>();
        windows_emulator win_emu{icicle::create_x86_64_emulator(1), settings, {}, std::move(interfaces)};
        std::string log_lines;
        win_emu.log.set_silent(true);
        win_emu.log.set_sink([&](const color, const std::string_view line) { log_lines += line; });

        const auto code = win_emu.memory.allocate_memory(0x1000, memory_permission::all);
        const auto stack = win_emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto object = win_emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto throw_info = win_emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0U);
        ASSERT_NE(stack, 0U);
        ASSERT_NE(object, 0U);
        ASSERT_NE(throw_info, 0U);
        const std::array<uint8_t, 2> nops{0x90, 0x90};
        const std::array<uint8_t, 4> object_head{0xab, 0xcd, 0xef, 0x12};
        const std::array<uint8_t, 4> info_head{0x11, 0x22, 0x33, 0x44};
        const auto return_address = code + 0x20;
        win_emu.emu().write_memory(code, nops.data(), nops.size());
        win_emu.emu().write_memory(stack + 0x100, &return_address, sizeof(return_address));
        win_emu.emu().write_memory(object, object_head.data(), object_head.size());
        win_emu.emu().write_memory(throw_info, info_head.data(), info_head.size());

        mapped_module runtime{};
        runtime.name = "VCRUNTIME140.DLL"; // case-insensitive matching
        runtime.image_base = code;
        runtime.size_of_image = 0x1000;
        runtime.exports.push_back(exported_symbol{.name = "_CxxThrowException", .address = code});
        win_emu.callbacks.on_module_load(runtime);

        auto& cpu = win_emu.emu().get_cpu(0);
        cpu.reg(x86_register::rsp, stack + 0x100);
        cpu.reg(x86_register::rcx, object);
        cpu.reg(x86_register::rdx, throw_info);

        cpu.reg(x86_register::rip, code + 1);
        cpu.start(1);
        EXPECT_EQ(log_lines.find("[GUESTCXXTHROW]"), std::string::npos) << "non-export address was hooked";

        cpu.reg(x86_register::rip, code);
        cpu.start(1);
        EXPECT_NE(log_lines.find("[GUESTCXXTHROW] n=1"), std::string::npos);
        EXPECT_NE(log_lines.find("ret_ok=1"), std::string::npos);
        EXPECT_NE(log_lines.find("object_bytes=abcdef12"), std::string::npos);
        EXPECT_NE(log_lines.find("throw_info_bytes=11223344"), std::string::npos);
        EXPECT_NE(log_lines.find("object_readable=32"), std::string::npos);
        EXPECT_NE(log_lines.find("info_readable=16"), std::string::npos);

        win_emu.callbacks.on_module_unload(runtime);
        cpu.reg(x86_register::rip, code);
        cpu.start(1);
        const auto first = log_lines.find("[GUESTCXXTHROW]");
        ASSERT_NE(first, std::string::npos);
        EXPECT_EQ(log_lines.find("[GUESTCXXTHROW]", first + 1), std::string::npos)
            << "unloaded runtime left an execution hook behind";
    }

    TEST(GuestCxxThrowProbe, ExplicitDxvkStaticRvaRequiresMatchingImageAndSignature)
    {
        const char* prior_probe = std::getenv("SOGEN_GUEST_CXX_THROW_PROBE");
        const char* prior_rva = std::getenv("SOGEN_GUEST_CXX_THROW_RVA");
        const std::string saved_probe = prior_probe ? prior_probe : "";
        const std::string saved_rva = prior_rva ? prior_rva : "";
        const auto restore = utils::finally([&] {
            _putenv_s("SOGEN_GUEST_CXX_THROW_PROBE", saved_probe.c_str());
            _putenv_s("SOGEN_GUEST_CXX_THROW_RVA", saved_rva.c_str());
        });
        ASSERT_EQ(_putenv_s("SOGEN_GUEST_CXX_THROW_PROBE", "1"), 0);
        ASSERT_EQ(_putenv_s("SOGEN_GUEST_CXX_THROW_RVA", "0x4eeb68"), 0);

        emulator_settings settings{};
        settings.emulation_root = get_emulator_root();
        settings.use_relative_time = false;
        settings.use_instruction_precision = false;
        settings.load_registry = false;
        emulator_interfaces interfaces{};
        interfaces.socket_factory = network::create_static_socket_factory();
        interfaces.ui = std::make_unique<null_ui_backend>();
        windows_emulator win_emu{icicle::create_x86_64_emulator(1), settings, {}, std::move(interfaces)};
        std::string log_lines;
        win_emu.log.set_silent(true);
        win_emu.log.set_sink([&](const color, const std::string_view line) { log_lines += line; });

        constexpr uint64_t throw_rva = 0x4eeb68;
        constexpr uint64_t image_size = 0x794000;
        const auto image = win_emu.memory.allocate_memory(image_size, memory_permission::all);
        const auto stack = win_emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(image, 0U);
        ASSERT_NE(stack, 0U);
        const auto entry = image + throw_rva;
        const auto return_address = image + 0x1000;
        win_emu.emu().write_memory(stack + 0x100, &return_address, sizeof(return_address));
        const uint8_t nop = 0x90;
        win_emu.emu().write_memory(entry, &nop, sizeof(nop));

        mapped_module runtime{};
        runtime.name = "D3D11.DLL";
        runtime.image_base = image;
        runtime.size_of_image = image_size;
        win_emu.callbacks.on_module_load(runtime); // signature mismatch
        auto& cpu = win_emu.emu().get_cpu(0);
        cpu.reg(x86_register::rsp, stack + 0x100);
        cpu.reg(x86_register::rip, entry);
        cpu.start(1);
        EXPECT_EQ(log_lines.find("[GUESTCXXTHROW]"), std::string::npos);
        EXPECT_NE(log_lines.find("[GUESTCXXPROBE]"), std::string::npos);
        EXPECT_NE(log_lines.find("signature_read=1 signature_match=0 installed=0 reason=signature_mismatch"),
                  std::string::npos);

        constexpr std::array<uint8_t, 16> signature{
            0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74,
            0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48};
        win_emu.emu().write_memory(entry, signature.data(), signature.size());
        runtime.size_of_image = image_size - 0x1000;
        win_emu.callbacks.on_module_load(runtime); // image-size mismatch
        cpu.reg(x86_register::rip, entry);
        cpu.start(1);
        EXPECT_EQ(log_lines.find("[GUESTCXXTHROW]"), std::string::npos);
        EXPECT_NE(log_lines.find("signature_read=0 signature_match=0 installed=0 reason=image_size"),
                  std::string::npos);
        constexpr uint64_t terminate_rva = 0x5334f4;
        constexpr uint64_t abort_rva = 0x536904;
        constexpr std::array<uint8_t, 8> terminate_signature{
            0x48, 0x83, 0xec, 0x28, 0xe8, 0x43, 0xc4, 0x00};
        constexpr std::array<uint8_t, 8> abort_signature{
            0x48, 0x83, 0xec, 0x28, 0xe8, 0x8f, 0x13, 0x01};
        constexpr std::array<uint8_t, 10> join_signature{
            0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x70};
        constexpr std::array<uint8_t, 5> join_throw_call{0xe8, 0x5f, 0x8b, 0x1c, 0x00};
        win_emu.emu().write_memory(image + terminate_rva, terminate_signature.data(), terminate_signature.size());
        win_emu.emu().write_memory(image + abort_rva, abort_signature.data(), abort_signature.size());
        win_emu.emu().write_memory(image + 0x325f10, join_signature.data(), join_signature.size());
        win_emu.emu().write_memory(image + 0x326004, join_throw_call.data(), join_throw_call.size());
        const auto join_throw_return = image + 0x326009;
        const auto queue_finish_return = image + 0x2009ba;
        const auto join_this = stack + 0x200;
        const auto join_data = stack + 0x208;
        constexpr uint64_t join_handle = 0x4800013;
        win_emu.emu().write_memory(stack + 0x100, &join_throw_return, sizeof(join_throw_return));
        win_emu.emu().write_memory(stack + 0x180, &queue_finish_return, sizeof(queue_finish_return));
        win_emu.emu().write_memory(join_this, &join_data, sizeof(join_data));
        win_emu.emu().write_memory(join_data, &join_handle, sizeof(join_handle));
        runtime.size_of_image = image_size;
        win_emu.callbacks.on_module_load(runtime);
        cpu.reg(x86_register::rdi, join_this);
        cpu.reg(x86_register::rip, entry);
        cpu.start(1);
        EXPECT_NE(log_lines.find("[GUESTCXXTHROW] n=1"), std::string::npos);
        EXPECT_NE(log_lines.find("signature_read=1 signature_match=1 installed="), std::string::npos);
        EXPECT_NE(log_lines.find("reason=installed"), std::string::npos);
        EXPECT_NE(log_lines.find("runtime=D3D11.DLL"), std::string::npos);
        EXPECT_NE(log_lines.find("ret_ok=1"), std::string::npos);
        EXPECT_NE(log_lines.find("[GUESTDXVKJOINFAIL]"), std::string::npos);
        EXPECT_NE(log_lines.find("handle=0x4800013 handle_read=1"), std::string::npos);
        EXPECT_NE(log_lines.find("join_layout_match=1"), std::string::npos);
        EXPECT_NE(log_lines.find("outer_ret_read=1 outer_caller=D3D11.DLL+0x2009ba"), std::string::npos);
        cpu.reg(x86_register::rsp, stack + 0x100);
        cpu.reg(x86_register::rip, image + terminate_rva);
        cpu.start(1);
        cpu.reg(x86_register::rsp, stack + 0x100);
        cpu.reg(x86_register::rip, image + abort_rva);
        cpu.start(1);
        EXPECT_NE(log_lines.find("[GUESTCXXTERM] n=1 kind=terminate"), std::string::npos);
        EXPECT_NE(log_lines.find("[GUESTCXXTERM] n=2 kind=abort"), std::string::npos);
        EXPECT_NE(log_lines.find("stack_readable=32"), std::string::npos);
        win_emu.callbacks.on_module_unload(runtime);
        const auto before_unloaded_run = log_lines.size();
        cpu.reg(x86_register::rsp, stack + 0x100);
        cpu.reg(x86_register::rip, image + terminate_rva);
        cpu.start(1);
        EXPECT_EQ(log_lines.find("[GUESTCXXTERM]", before_unloaded_run), std::string::npos)
            << "unloaded DXVK left a terminal execution hook behind";
    }

    TEST(GuestCxxThrowProbe, MatchingDxgiStaticCrtCapturesConstructorThrow)
    {
        const char* prior = std::getenv("SOGEN_GUEST_CXX_THROW_PROBE");
        const std::string saved = prior ? prior : "";
        const auto restore = utils::finally([&] { _putenv_s("SOGEN_GUEST_CXX_THROW_PROBE", saved.c_str()); });
        ASSERT_EQ(_putenv_s("SOGEN_GUEST_CXX_THROW_PROBE", "1"), 0);

        emulator_settings settings{};
        settings.emulation_root = get_emulator_root();
        settings.use_relative_time = false;
        settings.use_instruction_precision = false;
        settings.load_registry = false;
        emulator_interfaces interfaces{};
        interfaces.socket_factory = network::create_static_socket_factory();
        interfaces.ui = std::make_unique<null_ui_backend>();
        windows_emulator win_emu{icicle::create_x86_64_emulator(1), settings, {}, std::move(interfaces)};
        std::string log_lines;
        win_emu.log.set_silent(true);
        win_emu.log.set_sink([&](const color, const std::string_view line) { log_lines += line; });

        constexpr uint64_t throw_rva = 0x36296c;
        constexpr uint64_t image_size = 0x512000;
        const auto image = win_emu.memory.allocate_memory(image_size, memory_permission::all);
        const auto stack = win_emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(image, 0U);
        ASSERT_NE(stack, 0U);
        constexpr std::array<uint8_t, 16> signature{
            0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74,
            0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48};
        const auto entry = image + throw_rva;
        win_emu.emu().write_memory(entry, signature.data(), signature.size());
        const auto return_address = image + 0x2000;
        win_emu.emu().write_memory(stack + 0x100, &return_address, sizeof(return_address));

        mapped_module runtime{};
        runtime.name = "DXGI.DLL";
        runtime.image_base = image;
        runtime.size_of_image = image_size - 0x1000;
        win_emu.callbacks.on_module_load(runtime);
        auto& cpu = win_emu.emu().get_cpu(0);
        cpu.reg(x86_register::rsp, stack + 0x100);
        cpu.reg(x86_register::rip, entry);
        cpu.start(1);
        EXPECT_EQ(log_lines.find("[GUESTCXXTHROW]"), std::string::npos);
        EXPECT_NE(log_lines.find("reason=image_size"), std::string::npos);

        runtime.size_of_image = image_size;
        win_emu.callbacks.on_module_load(runtime);
        cpu.reg(x86_register::rsp, stack + 0x100);
        cpu.reg(x86_register::rip, entry);
        cpu.start(1);
        EXPECT_NE(log_lines.find("[GUESTCXXPROBE] module=DXGI.DLL"), std::string::npos);
        EXPECT_NE(log_lines.find("requested_rva=0x36296c"), std::string::npos);
        EXPECT_NE(log_lines.find("reason=installed"), std::string::npos);
        EXPECT_NE(log_lines.find("[GUESTCXXTHROW] n=1"), std::string::npos);
        EXPECT_NE(log_lines.find("runtime=DXGI.DLL"), std::string::npos);
        win_emu.callbacks.on_module_unload(runtime);
        const auto before_unloaded_run = log_lines.size();
        cpu.reg(x86_register::rsp, stack + 0x100);
        cpu.reg(x86_register::rip, entry);
        cpu.start(1);
        EXPECT_EQ(log_lines.find("[GUESTCXXTHROW]", before_unloaded_run), std::string::npos);
    }
}
