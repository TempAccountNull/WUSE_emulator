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

    class CmInterfaceListTest : public CmApiTest
    {
      protected:
        std::array<uint32_t, 10> input{40, 0, 0x91827364, 0x4aabbbbb, 0x78675645, 0x1201f0ef, 0, 0, 0, 20};
        const std::u16string class_path =
            uR"(\Registry\Machine\System\CurrentControlSet\Control\DeviceClasses\{91827364-bbbb-4aab-4556-6778eff00112})";

        io_device_context list_context(uint32_t length = 512)
        {
            auto c = context();
            c.io_control_code = 0x470807;
            c.input_buffer_length = 40;
            c.output_buffer_length = length;
            return c;
        }

        NTSTATUS query(uint32_t length = 512)
        {
            emu.memory.write_memory(memory + 0x100, input.data(), sizeof(input));
            return device->execute_ioctl(emu, list_context(length));
        }

        void add_interface(std::u16string_view name, std::u16string_view reference, std::u16string_view id)
        {
            const auto path = class_path + u'\\' + std::u16string(name);
            auto key = emu.registry.create_key(path);
            ASSERT_TRUE(key);
            std::u16string value{id};
            value += u'\0';
            emu.registry.set_value(*key, "DeviceInstance", REG_SZ, std::as_bytes(std::span(value)));
            ASSERT_TRUE(emu.registry.create_key(path + u'\\' + std::u16string(reference)));
        }

        uint32_t required() const
        {
            return emu.memory.read_memory<uint32_t>(memory + 0x208);
        }

        uint64_t information() const
        {
            return emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(memory).Information;
        }

        std::u16string payload() const
        {
            std::u16string value(required() / 2, u'\0');
            emu.memory.read_memory(memory + 0x210, value.data(), required());
            return value;
        }
    };

    TEST_F(CmInterfaceListTest, EmptyClassReportsSizeThenReturnsTerminator)
    {
        ASSERT_EQ(query(20), STATUS_SUCCESS);
        EXPECT_EQ(returned_status(), STATUS_BUFFER_TOO_SMALL);
        EXPECT_EQ(required(), 2u);
        EXPECT_EQ(information(), 20u);
        ASSERT_EQ(query(22), STATUS_SUCCESS);
        EXPECT_EQ(returned_status(), STATUS_SUCCESS);
        EXPECT_EQ(payload(), std::u16string(1, u'\0'));
        EXPECT_EQ(information(), 22u);
    }

    TEST_F(CmInterfaceListTest, EnumeratesReferencesAsMultiSzAndPreservesOutputTail)
    {
        add_interface(u"##?#ROOT#SOGENTEST#1#{91827364-bbbb-4aab-4556-6778eff00112}", u"#", u"ROOT\\SOGENTEST\\1");
        add_interface(u"##?#ROOT#SOGENTEST#1#{91827364-bbbb-4aab-4556-6778eff00112}", u"#named", u"ROOT\\SOGENTEST\\1");
        emu.memory.set_memory(memory + 0x200, 0xa5, 512);
        ASSERT_EQ(query(), STATUS_SUCCESS);
        ASSERT_EQ(returned_status(), STATUS_SUCCESS);
        const std::u16string first = u"\\\\?\\ROOT#SOGENTEST#1#{91827364-bbbb-4aab-4556-6778eff00112}";
        const auto expected = first + u'\0' + first + u"\\named" + u'\0' + u'\0';
        EXPECT_EQ(payload(), expected);
        EXPECT_EQ(information(), 20u + expected.size() * 2);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x210 + required()), 0xa5a5a5a5u);
    }

    TEST_F(CmInterfaceListTest, ShortBufferReportsFullRequiredSizeWithoutPartialList)
    {
        add_interface(u"##?#ROOT#SOGENTEST#1#{91827364-bbbb-4aab-4556-6778eff00112}", u"#", u"ROOT\\SOGENTEST\\1");
        emu.memory.set_memory(memory + 0x200, 0xa5, 512);
        ASSERT_EQ(query(22), STATUS_SUCCESS);
        EXPECT_EQ(returned_status(), STATUS_BUFFER_TOO_SMALL);
        EXPECT_GT(required(), 2u);
        EXPECT_EQ(information(), 20u);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x210), 0u);
        EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x214), 0xa5a5a5a5u);
    }

    TEST_F(CmInterfaceListTest, FiltersByCaseInsensitiveDeviceInstance)
    {
        add_interface(u"##?#ROOT#SOGENTEST#1#{91827364-bbbb-4aab-4556-6778eff00112}", u"#", u"ROOT\\SOGENTEST\\1");
        add_interface(u"##?#ROOT#SOGENTEST#2#{91827364-bbbb-4aab-4556-6778eff00112}", u"#", u"ROOT\\SOGENTEST\\2");
        const std::u16string id = u"root\\sogentest\\2";
        const auto address = memory + 0x500;
        emu.memory.write_memory(address, id.c_str(), (id.size() + 1) * 2);
        memcpy(input.data() + 6, &address, 8);
        input[8] = static_cast<uint32_t>((id.size() + 1) * 2);
        ASSERT_EQ(query(), STATUS_SUCCESS);
        EXPECT_EQ(payload(), std::u16string(u"\\\\?\\ROOT#SOGENTEST#2#{91827364-bbbb-4aab-4556-6778eff00112}") + u'\0' + u'\0');
    }

    TEST_F(CmInterfaceListTest, SavedRegistrationDoesNotActivateGuestHardware)
    {
        add_interface(u"##?#ROOT#SOGENTEST#1#{91827364-bbbb-4aab-4556-6778eff00112}", u"#", u"ROOT\\SOGENTEST\\1");
        input[1] = 0x10000;
        ASSERT_EQ(query(), STATUS_SUCCESS);
        EXPECT_EQ(returned_status(), STATUS_SUCCESS);
        EXPECT_EQ(payload(), std::u16string(1, u'\0'));
    }

    TEST_F(CmInterfaceListTest, Wow64DecodesPointerAndResultSizeSeparately)
    {
        device = create_cm_api({.is_32_bit = true});
        const std::array<uint32_t, 9> wow64{36, 0, input[2], input[3], input[4], input[5], 0, 0, 20};
        emu.memory.write_memory(memory + 0x100, wow64.data(), sizeof(wow64));
        auto c = list_context(22);
        c.input_buffer_length = 36;
        ASSERT_EQ(device->execute_ioctl(emu, c), STATUS_SUCCESS);
        EXPECT_EQ(returned_status(), STATUS_SUCCESS);
        EXPECT_EQ(information(), 22u);
    }

    TEST_F(CmInterfaceListTest, InvalidFlagsAreOperationFailureWithTransportSuccess)
    {
        input[1] = 2;
        ASSERT_EQ(query(), STATUS_SUCCESS);
        EXPECT_EQ(returned_status(), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(required(), 0u);
        EXPECT_EQ(information(), 20u);
    }

    TEST_F(CmInterfaceListTest, ValidatesHeadersPointersAlignmentAndLengths)
    {
        input[0] = 39;
        EXPECT_EQ(query(), STATUS_INVALID_PARAMETER);
        input[0] = 40;
        input[9] = 19;
        EXPECT_EQ(query(), STATUS_INVALID_PARAMETER);
        input[9] = 20;
        EXPECT_EQ(query(19), STATUS_INVALID_PARAMETER);
        input[8] = 2;
        EXPECT_EQ(query(), STATUS_INVALID_PARAMETER);
        input[6] = 1;
        EXPECT_EQ(query(), STATUS_DATATYPE_MISALIGNMENT);
        input[6] = 0xffff0000;
        input[7] = 0x7fff;
        EXPECT_EQ(query(), STATUS_ACCESS_VIOLATION);
        input[6] = input[7] = input[8] = 0;
        ASSERT_EQ(query(), STATUS_SUCCESS);
        auto c = list_context();
        c.input_buffer++;
        EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_DATATYPE_MISALIGNMENT);
        c = list_context();
        c.output_buffer++;
        EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_DATATYPE_MISALIGNMENT);
        c.output_buffer = 0x7fffffff0000;
        EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_ACCESS_VIOLATION);
    }
}
