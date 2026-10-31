#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtUserQueryDisplayConfig(const syscall_context& c, UINT32 flags, emulator_object<UINT32> num_path_array_elements,
                                             emulator_pointer path_array, emulator_object<UINT32> current_topology_id,
                                             emulator_pointer reserved);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint8_t sentinel = 0xab;
        constexpr size_t internal_path_size = 200;
        constexpr uint32_t qdc_only_active_paths = 2;

        class DisplayConfigTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.version.load_from_registry(this->emu.registry, this->emu.log);
                if (!this->emu.version.is_build_within(19041, 19046))
                {
                    GTEST_SKIP() << "DISPLAYCONFIG_PATH_INFO_INTERNAL is 200 bytes on Windows 10 19041-19045";
                }

                this->paths = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                this->count = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            windows_emulator emu = create_empty_emulator();
            syscall_context context{
                .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            uint64_t paths{};
            uint64_t count{};
        };
    }

    TEST_F(DisplayConfigTest, WritesOneInternalPathOfTheBuildsSize)
    {
        this->emu.memory.set_memory(this->paths, sentinel, 0x1000);
        this->emu.memory.write_memory(this->count, uint32_t{1});

        const auto status = syscalls::handle_NtUserQueryDisplayConfig(this->context, qdc_only_active_paths, {this->emu.memory, this->count},
                                                                      this->paths, {this->emu.memory}, 0);

        ASSERT_EQ(STATUS_SUCCESS, status);
        EXPECT_EQ(1u, this->emu.memory.read_memory<uint32_t>(this->count));
        EXPECT_EQ(this->emu.process.display_width, this->emu.memory.read_memory<uint32_t>(this->paths + 0x7C));
        EXPECT_EQ(this->emu.process.display_height, this->emu.memory.read_memory<uint32_t>(this->paths + 0x80));

        std::array<uint8_t, 0x40> tail{};
        this->emu.memory.read_memory(this->paths + internal_path_size, tail.data(), tail.size());
        for (const auto value : tail)
        {
            EXPECT_EQ(sentinel, value);
        }
    }
}
