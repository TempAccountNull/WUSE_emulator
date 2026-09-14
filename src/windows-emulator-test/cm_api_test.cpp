#include "emulation_test_utils.hpp"
#include <devices/cm_api.hpp>
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtEnumerateKey(const syscall_context&, handle, ULONG, KEY_INFORMATION_CLASS, emulator_pointer, ULONG,
                                   emulator_object<ULONG>);
}

namespace sogen::test
{
    class CmApiTest : public testing::Test
    {
      protected:
        windows_emulator emu{create_empty_emulator()};
        uint64_t memory{};
        std::unique_ptr<io_device> device{create_cm_api({})};
        std::array<uint32_t, 12> request{48, 0, 3, 0, 0, 0, 0, KEY_ENUMERATE_SUB_KEYS, 2, 0, 16, 0};

        void SetUp() override
        {
            memory = emu.memory.allocate_memory(0x2000, memory_permission::read_write);
        }

        io_device_context context()
        {
            io_device_context c{emu.memory};
            c.io_status_block = {emu.memory, memory};
            c.io_control_code = 0x470863;
            c.input_buffer = memory + 0x100;
            c.input_buffer_length = 48;
            c.output_buffer = memory + 0x200;
            c.output_buffer_length = 16;
            return c;
        }

        NTSTATUS run()
        {
            emu.memory.write_memory(memory + 0x100, request.data(), sizeof(request));
            return device->execute_ioctl(emu, context());
        }

        uint64_t returned_key() const
        {
            return emu.memory.read_memory<uint64_t>(memory + 0x208);
        }

        NTSTATUS returned_status() const
        {
            return emu.memory.read_memory<NTSTATUS>(memory + 0x204);
        }

        void set_guid()
        {
            constexpr std::u16string_view guid = u"{4986b000-3543-4b84-9023-1df15a099fd8}";
            emu.memory.write_memory(memory + 0x300, guid.data(), (guid.size() + 1) * sizeof(char16_t));
            const auto address = memory + 0x300;
            memcpy(request.data() + 4, &address, sizeof(address));
            request[6] = 78;
        }
    };

    TEST_F(CmApiTest, ReturnsUsableGuestInterfaceRegistryHandle)
    {
        emu.memory.set_memory(memory + 0x200, 0xa5, 16);
        ASSERT_EQ(run(), STATUS_SUCCESS);
        ASSERT_EQ(returned_status(), STATUS_SUCCESS);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x200), 16u);
        EXPECT_EQ(emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(memory).Information, 16u);
        const auto* key = emu.process.registry_keys.get(returned_key());
        ASSERT_NE(key, nullptr);
        EXPECT_EQ(key->path.get().filename().u16string(), u"deviceclasses");
        auto& vcpu = emu.vcpu(0);
        const syscall_context c{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const auto count = emu.registry.get_sub_key_count(*key);
        EXPECT_EQ(syscalls::handle_NtEnumerateKey(c, make_handle(returned_key()), static_cast<ULONG>(count), KeyBasicInformation,
                                                  memory + 0x400, 512, {emu.memory, memory + 0x700}),
                  STATUS_NO_MORE_ENTRIES);
    }

    TEST_F(CmApiTest, OpensInstallerClassRoot)
    {
        request[2] = 2;
        ASSERT_EQ(run(), STATUS_SUCCESS);
        ASSERT_EQ(returned_status(), STATUS_SUCCESS);
        const auto* key = emu.process.registry_keys.get(returned_key());
        ASSERT_NE(key, nullptr);
        EXPECT_EQ(key->path.get().filename().u16string(), u"class");
    }

    TEST_F(CmApiTest, MissingClassReturnsOperationFailureWithoutStaleHandle)
    {
        set_guid();
        emu.memory.set_memory(memory + 0x200, 0xa5, 16);
        ASSERT_EQ(run(), STATUS_SUCCESS);
        EXPECT_EQ(returned_status(), STATUS_OBJECT_NAME_NOT_FOUND);
        EXPECT_EQ(returned_key(), 0u);
    }

    TEST_F(CmApiTest, OpenAlwaysCreatesOnlyGuestOverlayKey)
    {
        set_guid();
        request[8] = 1;
        ASSERT_EQ(run(), STATUS_SUCCESS);
        ASSERT_EQ(returned_status(), STATUS_SUCCESS);
        const auto* key = emu.process.registry_keys.get(returned_key());
        ASSERT_NE(key, nullptr);
        EXPECT_EQ(key->path.get().filename().u16string(), u"{4986b000-3543-4b84-9023-1df15a099fd8}");
        request[8] = 2;
        ASSERT_EQ(run(), STATUS_SUCCESS);
        EXPECT_EQ(returned_status(), STATUS_SUCCESS);
    }

    TEST_F(CmApiTest, Wow64RequestProducesSameHandleResultLayout)
    {
        device = create_cm_api({.is_32_bit = true});
        const std::array<uint32_t, 9> wow64{36, 0, 3, 0, 0, KEY_ENUMERATE_SUB_KEYS, 2, 0, 16};
        emu.memory.write_memory(memory + 0x100, wow64.data(), sizeof(wow64));
        auto c = context();
        c.input_buffer_length = sizeof(wow64);
        ASSERT_EQ(device->execute_ioctl(emu, c), STATUS_SUCCESS);
        ASSERT_EQ(returned_status(), STATUS_SUCCESS);
        EXPECT_NE(emu.process.registry_keys.get(returned_key()), nullptr);
    }

    TEST_F(CmApiTest, RejectsMalformedRequestAndShortOutput)
    {
        request[0] = 47;
        EXPECT_EQ(run(), STATUS_INVALID_PARAMETER);
        request[0] = 48;
        emu.memory.write_memory(memory + 0x100, request.data(), sizeof(request));
        auto c = context();
        c.output_buffer_length = 8;
        EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(emu.process.registry_keys.size(), 0u);
    }

    TEST_F(CmApiTest, RejectsUnsupportedOperationAndInvalidGuestPointers)
    {
        request[1] = 1;
        ASSERT_EQ(run(), STATUS_SUCCESS);
        EXPECT_EQ(returned_status(), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(returned_key(), 0u);
        auto c = context();
        c.input_buffer = 0x7fffffff0000;
        EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_ACCESS_VIOLATION);
        c = context();
        c.io_control_code = 0x470867;
        EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_NOT_SUPPORTED);
    }

    TEST_F(CmApiTest, FailedOutputWriteClosesNewHandle)
    {
        emu.memory.write_memory(memory + 0x100, request.data(), sizeof(request));
        auto c = context();
        c.output_buffer = 0x7fffffff0000;
        EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(emu.process.registry_keys.size(), 0u);
    }
}
