#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQuerySystemInformationEx(const syscall_context&, uint32_t, uint64_t, uint32_t, uint64_t, uint32_t,
                                               emulator_object<uint32_t>);
}

namespace sogen::test
{
    class ProcessorTopologyTest : public testing::Test
    {
      protected:
        windows_emulator emu{create_empty_emulator()};
        uint64_t memory{};

        void SetUp() override
        {
            memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            emu.process.kusd.access([](KUSER_SHARED_DATA64& data) { data.ActiveProcessorCount = 8; });
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        NTSTATUS query(const LOGICAL_PROCESSOR_RELATIONSHIP relationship, const uint32_t size)
        {
            emu.emu().write_memory(memory, relationship);
            return syscalls::handle_NtQuerySystemInformationEx(context(), SystemLogicalProcessorAndGroupInformation, memory,
                                                               sizeof(relationship), memory + 0x100, size, {emu.memory, memory + 0x800});
        }

        uint32_t returned_size()
        {
            return emu.emu().read_memory<uint32_t>(memory + 0x800);
        }

        EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64 record(const uint32_t offset = 0)
        {
            return emu.emu().read_memory<EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64>(memory + 0x100 + offset);
        }
    };

    TEST_F(ProcessorTopologyTest, PackageMatchesNativeBufferAndMask)
    {
        constexpr auto package_size = static_cast<uint32_t>(offsetof(EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64, Processor) +
                                                            sizeof(EMU_PROCESSOR_RELATIONSHIP64));
        ASSERT_EQ(package_size, 48u);

        emu.emu().set_memory(memory + 0x100, 0xA5, package_size);
        ASSERT_EQ(query(RelationProcessorPackage, 0), STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_EQ(returned_size(), package_size);
        ASSERT_EQ(query(RelationProcessorPackage, package_size - 1), STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_EQ(returned_size(), package_size);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(memory + 0x100), 0xA5A5A5A5A5A5A5A5u);

        ASSERT_EQ(query(RelationProcessorPackage, package_size), STATUS_SUCCESS);
        EXPECT_EQ(returned_size(), package_size);
        const auto package = record();
        EXPECT_EQ(package.Relationship, RelationProcessorPackage);
        EXPECT_EQ(package.Size, package_size);
        EXPECT_EQ(package.Processor.GroupCount, 1);
        EXPECT_EQ(package.Processor.GroupMask[0].Mask, 0xFFu);
        EXPECT_EQ(package.Processor.GroupMask[0].Group, 0);
    }

    TEST_F(ProcessorTopologyTest, UnsupportedRelationshipReturnsStatusWithoutChangingOutput)
    {
        emu.emu().write_memory<uint32_t>(memory + 0x800, 0xDEADBEEFu);
        emu.emu().set_memory(memory + 0x100, 0xA5, 48);
        EXPECT_EQ(query(RelationCache, 48), STATUS_NOT_SUPPORTED);
        EXPECT_EQ(returned_size(), 0xDEADBEEFu);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(memory + 0x100), 0xA5A5A5A5A5A5A5A5u);
    }

    TEST_F(ProcessorTopologyTest, AllIncludesPackageCoresNumaAndGroup)
    {
        ASSERT_EQ(query(RelationAll, 0), STATUS_INFO_LENGTH_MISMATCH);
        const auto total_size = returned_size();
        EXPECT_EQ(total_size, 48u + 8u * 48u + 48u + 80u);

        ASSERT_EQ(query(RelationAll, total_size), STATUS_SUCCESS);
        EXPECT_EQ(returned_size(), total_size);
        uint32_t offset = 0;
        const auto package = record(offset);
        ASSERT_EQ(package.Relationship, RelationProcessorPackage);
        EXPECT_EQ(package.Processor.GroupMask[0].Mask, 0xFFu);
        offset += package.Size;

        for (uint32_t processor = 0; processor < 8; ++processor)
        {
            const auto core = record(offset);
            ASSERT_EQ(core.Relationship, RelationProcessorCore);
            EXPECT_EQ(core.Processor.GroupMask[0].Mask, uint64_t{1} << processor);
            offset += core.Size;
        }

        const auto numa = record(offset);
        ASSERT_EQ(numa.Relationship, RelationNumaNode);
        EXPECT_EQ(numa.NumaNode.GroupCount, 0);
        EXPECT_EQ(numa.NumaNode.GroupMask.Mask, 0xFFu);
        offset += numa.Size;

        const auto group = record(offset);
        ASSERT_EQ(group.Relationship, RelationGroup);
        EXPECT_EQ(group.Group.ActiveGroupCount, 1);
        EXPECT_EQ(group.Group.GroupInfo[0].ActiveProcessorMask, 0xFFu);
        offset += group.Size;
        EXPECT_EQ(offset, total_size);
    }
}
