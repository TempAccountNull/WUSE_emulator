#include "emulation_test_utils.hpp"
#include <platform/window.hpp>
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtUserGetDisplayConfigBufferSizes(const syscall_context&, UINT32, emulator_object<UINT32>, emulator_object<UINT32>);
    NTSTATUS handle_NtUserQueryDisplayConfig(const syscall_context&, UINT32, emulator_object<UINT32>, emulator_pointer,
                                             emulator_object<UINT32>, emulator_pointer);
}

namespace sogen::test
{
    class DisplayConfigTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t memory{};

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(memory, 0u);
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }
    };

    TEST_F(DisplayConfigTest, InternalPathMatchesGuestUser32StrideAndPreservesGuard)
    {
        constexpr size_t guard_size = 16;
        const auto count = emulator_object<UINT32>{emu.memory, memory};
        const auto path = memory + 0x100;
        count.write(1);

        std::array<uint8_t, sizeof(EMU_DISPLAYCONFIG_PATH_INFO_INTERNAL) + guard_size> initialized{};
        initialized.fill(0xA5);
        emu.memory.write_memory(path, initialized.data(), initialized.size());

        EXPECT_EQ(syscalls::handle_NtUserQueryDisplayConfig(context(), 0x2, count, path, emulator_object<UINT32>{emu.memory, 0}, 0),
                  STATUS_SUCCESS);
        EXPECT_EQ(count.read(), 1u);

        const auto internal = emu.memory.read_memory<EMU_DISPLAYCONFIG_PATH_INFO_INTERNAL>(path);
        EXPECT_EQ(internal.adapterId.LowPart, 0x1000u);
        EXPECT_EQ(internal.sourceWidth, 1920u);
        EXPECT_EQ(internal.sourceHeight, 1080u);
        EXPECT_EQ(internal.filterStatus, 0u);

        const auto guard = emu.memory.read_memory<std::array<uint8_t, guard_size>>(path + sizeof(EMU_DISPLAYCONFIG_PATH_INFO_INTERNAL));
        EXPECT_TRUE(std::ranges::all_of(guard, [](const uint8_t value) { return value == 0xA5; }));
    }

    TEST_F(DisplayConfigTest, RejectsMissingPathStorageWithoutChangingCount)
    {
        const auto count = emulator_object<UINT32>{emu.memory, memory};
        count.write(1);
        EXPECT_EQ(syscalls::handle_NtUserQueryDisplayConfig(context(), 0x2, count, 0, emulator_object<UINT32>{emu.memory, 0}, 0),
                  STATUS_INVALID_PARAMETER);
        EXPECT_EQ(count.read(), 1u);
    }

    TEST_F(DisplayConfigTest, VirtualModeSizingIncludesDesktopImageMode)
    {
        const auto paths = emulator_object<UINT32>{emu.memory, memory};
        const auto modes = emulator_object<UINT32>{emu.memory, memory + 4};
        EXPECT_EQ(syscalls::handle_NtUserGetDisplayConfigBufferSizes(context(), 0x2, paths, modes), STATUS_SUCCESS);
        EXPECT_EQ(paths.read(), 1u);
        EXPECT_EQ(modes.read(), 2u);
        EXPECT_EQ(syscalls::handle_NtUserGetDisplayConfigBufferSizes(context(), 0x12, paths, modes), STATUS_SUCCESS);
        EXPECT_EQ(paths.read(), 1u);
        EXPECT_EQ(modes.read(), 3u);
    }
}
