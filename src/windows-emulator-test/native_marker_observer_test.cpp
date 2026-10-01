#include "emulation_test_utils.hpp"
#include "../windows-analyzer/native_marker_capture.hpp"
#include <platform/win_pefile.hpp>
#include <array>
#include <cstring>
#include <optional>
#include <vector>

namespace sogen::test
{
    class NativeMarkerObserver : public testing::Test
    {
      protected:
        windows_emulator win = [] {
            emulator_settings settings{.disable_logging = true};
            settings.emulation_root = get_emulator_root();
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            emulator_interfaces interfaces{};
            interfaces.socket_factory = network::create_static_socket_factory();
            interfaces.dns_lookup = create_sample_dns_lookup();
            interfaces.ui = std::make_unique<null_ui_backend>();
            return windows_emulator{
                create_x86_64_emulator(backend_type::whp, 2), get_sample_app_settings({}), settings, {}, std::move(interfaces)};
        }();

        static constexpr uint64_t image_base = 0x10000000000;
        static constexpr uint64_t image_size = 0x3000000;
        static constexpr uint64_t data_base = image_base + 0x4000000;
        static constexpr uint64_t owner_rsp = data_base + 0x2100;
        static constexpr uint64_t peer_rsp = data_base + 0x3100;
        static constexpr uint64_t guard_page = data_base + 0x4000;
        static constexpr uint64_t owner_rax = 0x8877665544332211;
        static constexpr uint64_t peer_rax = 0x1122334455667788;
        uint32_t owner_tid{};
        std::vector<uint64_t> plain_pages{};

        template <typename T>
        void write(const uint64_t address, const T& value)
        {
            win.memory.write_memory(address, &value, sizeof(value));
        }

        bool allocate_page(const uint64_t address)
        {
            if (!win.memory.allocate_memory(address, 0x1000, memory_permission::read_write))
            {
                return false;
            }
            plain_pages.push_back(address);
            return true;
        }

        void SetUp() override
        {
            win.setup_process_if_necessary();
            ASSERT_TRUE(allocate_page(image_base));
            PEDosHeader_t dos{};
            dos.e_magic = 0x5A4D;
            dos.e_lfanew = 0x80;
            write(image_base, dos);
            PENTHeaders_t<uint64_t> nt{};
            nt.Signature = 0x4550;
            nt.FileHeader.Machine = PEMachineType::AMD64;
            nt.FileHeader.SizeOfOptionalHeader = static_cast<uint16_t>(sizeof(nt.OptionalHeader));
            nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE;
            nt.OptionalHeader.Magic = 0x20B;
            nt.OptionalHeader.ImageBase = image_base;
            nt.OptionalHeader.SizeOfImage = 0x1000;
            nt.OptionalHeader.SizeOfHeaders = 0x1000;
            nt.OptionalHeader.SectionAlignment = 0x1000;
            nt.OptionalHeader.FileAlignment = 0x200;
            nt.OptionalHeader.NumberOfRvaAndSizes = 16;
            write(image_base + 0x80, nt);
            auto* module = win.mod_manager.map_memory_module(image_base, 0x1000, R"(C:\destiny2.exe)", win.log);
            ASSERT_NE(module, nullptr);
            module->size_of_image = image_size;
            ASSERT_EQ(module->machine, 0x8664U);

            for (const auto page : {image_base + 0xB44000, image_base + 0xFD1000, image_base + 0x1FB5000, image_base + 0x2742000, data_base,
                                    data_base + 0x2000, data_base + 0x3000})
            {
                ASSERT_TRUE(allocate_page(page));
            }
            constexpr std::array<uint8_t, 4> first_guard{0x41, 0xC6, 0x07, 0x02};
            constexpr std::array<uint8_t, 4> second_guard{0x83, 0x79, 0x0C, 0xFF};
            win.memory.write_memory(image_base + 0xB440DF, first_guard.data(), first_guard.size());
            win.memory.write_memory(image_base + 0xFD1A10, second_guard.data(), second_guard.size());
            constexpr std::array<uint8_t, 12> slot3{3, 4, 5, 6, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
            constexpr std::array<uint8_t, 12> slot6{6, 5, 4, 3, 0x80, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10};
            win.memory.write_memory(image_base + 0x1FB5F44, slot3.data(), slot3.size());
            win.memory.write_memory(image_base + 0x1FB5F68, slot6.data(), slot6.size());
            write(image_base + 0x1FB5F80, uint32_t{9});
            write(image_base + 0x2742FA0, guard_page);
            write(image_base + 0x2742FB0, data_base);
            write(data_base, image_base + 0x1BEB568);
            write(data_base + 8, uint32_t{2});
            write(data_base + 12, uint32_t{91});
            ASSERT_TRUE(win.memory.allocate_memory(guard_page, 0x1000, memory_permission::read_write));
            write(guard_page, uint64_t{0xAABBCCDDEEFF0011});
            ASSERT_TRUE(win.memory.protect_memory(guard_page, 0x1000, memory_permission::read_write | memory_permission_ext::guard));
            for (uint64_t index = 0; index < 16; ++index)
            {
                write(owner_rsp + index * 8, 0xA000000000000000ULL + index);
                write(peer_rsp + index * 8, 0xB000000000000000ULL + index);
            }

            auto& first = win.vcpu(0);
            first.cpu.reg(x86_register::rax, peer_rax);
            first.cpu.reg(x86_register::rsp, peer_rsp);
            const auto peer_registers = first.cpu.save_registers();
            const auto target_handle = win.process.create_thread(win.memory, win.mod_manager.executable->entry_point, 0, 0x10000, 0);
            auto* target = win.process.threads.get(target_handle);
            ASSERT_NE(target, nullptr);
            first.cpu.reg(x86_register::rax, owner_rax);
            first.cpu.reg(x86_register::rsp, owner_rsp);
            first.cpu.reg(x86_register::rip, image_base + 0xB440DF);
            target->last_registers = first.cpu.save_registers();
            target->setup_done = true;
            owner_tid = target->id;
            first.cpu.restore_registers(peer_registers);
            ASSERT_TRUE(win.activate_thread(win.vcpu(1), owner_tid));
            ASSERT_NE(win.vcpu(0).active_thread->id, owner_tid);
            win.vcpu(1).cpu.reg(x86_register::rax, owner_rax);
            win.vcpu(1).cpu.reg(x86_register::rsp, owner_rsp);
            win.vcpu(1).cpu.reg(x86_register::rip, image_base + 0xB440DF);
            ASSERT_FALSE(win.has_active_dispatch_context());
        }

        std::vector<std::array<uint8_t, 4096>> memory_snapshot()
        {
            std::vector<std::array<uint8_t, 4096>> result(plain_pages.size());
            for (size_t index = 0; index < plain_pages.size(); ++index)
            {
                win.memory.read_memory(plain_pages[index], result[index].data(), result[index].size());
            }
            return result;
        }

        std::optional<detail::native_marker_snapshot> dispatched_capture(native_marker_observer& observer, const std::string_view message)
        {
            return win.dispatch_on_cpu(win.vcpu(1).cpu, [&] {
                EXPECT_TRUE(win.has_active_dispatch_context());
                EXPECT_EQ(win.active_cpu().index(), 1U);
                EXPECT_EQ(win.current_thread().id, owner_tid);
                return observer.capture(win, message);
            });
        }
    };

    TEST_F(NativeMarkerObserver, DisabledFlagsReturnNoCaptureInsideValidSecondCpuDispatch)
    {
        const auto registers = win.vcpu(1).cpu.save_registers();
        const auto memory = memory_snapshot();
        for (const auto flag :
             {std::string_view{}, std::string_view{"0"}, std::string_view{"true"}, std::string_view{"01"}, std::string_view{"1 "}})
        {
            native_marker_observer observer{flag};
            EXPECT_FALSE(dispatched_capture(observer, detail::native_marker_capture_state::investment_marker));
        }
        EXPECT_EQ(win.vcpu(1).cpu.save_registers(), registers);
        EXPECT_EQ(memory_snapshot(), memory);
        EXPECT_TRUE(win.memory.get_region_info(guard_page).permissions.is_guarded());
    }

    TEST_F(NativeMarkerObserver, NoDispatchCannotSampleTheCpuZeroFallback)
    {
        native_marker_observer observer{"1"};
        const auto peer = win.vcpu(0).cpu.save_registers();
        const auto owner = win.vcpu(1).cpu.save_registers();
        EXPECT_FALSE(win.has_active_dispatch_context());
        EXPECT_FALSE(observer.capture(win, detail::native_marker_capture_state::investment_marker));
        EXPECT_EQ(win.vcpu(0).cpu.save_registers(), peer);
        EXPECT_EQ(win.vcpu(1).cpu.save_registers(), owner);
    }

    TEST_F(NativeMarkerObserver, BorrowedDispatchWithoutKernelLockDoesNotConsumeTheFirstMarker)
    {
        native_marker_observer observer{"1"};
        {
            const windows_emulator::scoped_dispatch dispatch{win, win.vcpu(1)};
            EXPECT_FALSE(win.has_active_dispatch_context());
            EXPECT_FALSE(observer.capture(win, detail::native_marker_capture_state::investment_marker));
        }
        const auto sample = dispatched_capture(observer, detail::native_marker_capture_state::investment_marker);
        ASSERT_TRUE(sample);
        EXPECT_EQ(sample->reservation.event, detail::native_marker_event::investment_entry);
        EXPECT_EQ(sample->reservation.sequence, 1U);
        EXPECT_EQ(sample->registers.actual_cpu, 1U);
        EXPECT_EQ(sample->registers.actual_tid, owner_tid);
    }

    TEST_F(NativeMarkerObserver, SecondCpuSnapshotPreservesGuestStateAndKeepsTheActualOwner)
    {
        native_marker_observer observer{"1"};
        const auto peer = win.vcpu(0).cpu.save_registers();
        const auto owner = win.vcpu(1).cpu.save_registers();
        const auto memory = memory_snapshot();
        const auto sample = dispatched_capture(observer, detail::native_marker_capture_state::investment_marker);
        ASSERT_TRUE(sample);
        EXPECT_EQ(sample->reservation.event, detail::native_marker_event::investment_entry);
        EXPECT_EQ(sample->identity.module_base, image_base);
        EXPECT_EQ(sample->identity.module_size, image_size);
        EXPECT_NE(sample->identity.pid, 0U);
        EXPECT_NE(sample->identity.birth, 0U);
        EXPECT_NE(sample->identity.generation, 0U);
        EXPECT_EQ(sample->registers.actual_cpu, 1U);
        EXPECT_EQ(sample->registers.actual_tid, owner_tid);
        const auto& context = sample->registers.context;
        EXPECT_EQ(context.stack_requested_words, 16U);
        if (context.available[0] & 1U)
        {
            EXPECT_EQ(context.gpr[0], owner_rax);
            EXPECT_NE(context.gpr[0], peer_rax);
        }
        if (context.available[0] & (uint64_t{1} << 7))
        {
            EXPECT_EQ(context.gpr[7], owner_rsp);
            EXPECT_EQ(context.stack_base, owner_rsp);
        }
        if (context.available[0] & (uint64_t{1} << 16))
        {
            EXPECT_EQ(context.rip, image_base + 0xB440DF);
        }
        for (uint32_t index = 0; index < 16; ++index)
        {
            if (context.stack_success[0] & (uint64_t{1} << index))
            {
                EXPECT_EQ(context.stack[index], 0xA000000000000000ULL + index);
            }
        }
        EXPECT_LE(sample->budget.reads, 23U);
        EXPECT_LE(sample->budget.bytes, 196U);
        EXPECT_LE(sample->qualification_reads, 2U);
        EXPECT_LE(sample->qualification_bytes, 8U);
        EXPECT_EQ(win.vcpu(0).cpu.save_registers(), peer);
        EXPECT_EQ(win.vcpu(1).cpu.save_registers(), owner);
        EXPECT_EQ(memory_snapshot(), memory);
        EXPECT_TRUE(win.memory.get_region_info(guard_page).permissions.is_guarded());
        EXPECT_FALSE(win.has_active_dispatch_context());
    }

    TEST_F(NativeMarkerObserver, FirstInvestmentMarkerIsAdmittedOnlyOnce)
    {
        native_marker_observer observer{"1"};
        EXPECT_FALSE(dispatched_capture(observer, detail::native_marker_capture_state::cleanup_marker));
        const auto first = dispatched_capture(observer, detail::native_marker_capture_state::investment_marker);
        ASSERT_TRUE(first);
        ASSERT_EQ(first->reservation.event, detail::native_marker_event::investment_entry);
        EXPECT_EQ(first->reservation.sequence, 1U);
        EXPECT_EQ(first->counts.capture_attempts, 1U);
        const auto registers = win.vcpu(1).cpu.save_registers();
        const auto memory = memory_snapshot();
        EXPECT_FALSE(dispatched_capture(observer, detail::native_marker_capture_state::investment_marker));
        EXPECT_FALSE(dispatched_capture(observer, detail::native_marker_capture_state::investment_marker));
        EXPECT_EQ(win.vcpu(1).cpu.save_registers(), registers);
        EXPECT_EQ(memory_snapshot(), memory);
        const auto cleanup = dispatched_capture(observer, detail::native_marker_capture_state::cleanup_marker);
        if (first->completion.reason == detail::native_marker_stop::none)
        {
            ASSERT_TRUE(cleanup);
            EXPECT_EQ(cleanup->reservation.event, detail::native_marker_event::cleanup_entry);
            EXPECT_EQ(cleanup->reservation.sequence, 2U);
            EXPECT_EQ(cleanup->registers.actual_cpu, 1U);
            EXPECT_EQ(cleanup->registers.actual_tid, owner_tid);
            EXPECT_EQ(cleanup->counts.capture_attempts, 2U);
        }
        else
        {
            EXPECT_FALSE(cleanup);
        }
        EXPECT_FALSE(dispatched_capture(observer, detail::native_marker_capture_state::cleanup_marker));
    }
}
