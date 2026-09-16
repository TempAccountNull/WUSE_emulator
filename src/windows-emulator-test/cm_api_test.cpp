#include "emulation_test_utils.hpp"
#include <devices/cm_api.hpp>
#include <syscall_utils.hpp>

namespace sogen::syscalls {
NTSTATUS handle_NtEnumerateKey(const syscall_context &, handle, ULONG,
                               KEY_INFORMATION_CLASS, emulator_pointer, ULONG,
                               emulator_object<ULONG>);
}

namespace sogen::test {
class CmApiTest : public testing::Test {
protected:
  windows_emulator emu{create_empty_emulator()};
  uint64_t memory{};
  std::unique_ptr<io_device> device{create_cm_api({})};
  std::array<uint32_t, 12> request{
      48, 0, 3, 0, 0, 0, 0, KEY_ENUMERATE_SUB_KEYS, 2, 0, 16, 0};

  void SetUp() override {
    memory = emu.memory.allocate_memory(0x2000, memory_permission::read_write);
  }

  io_device_context context() {
    io_device_context c{emu.memory};
    c.io_status_block = {emu.memory, memory};
    c.io_control_code = 0x470863;
    c.input_buffer = memory + 0x100;
    c.input_buffer_length = 48;
    c.output_buffer = memory + 0x200;
    c.output_buffer_length = 16;
    return c;
  }

  NTSTATUS run() {
    emu.memory.write_memory(memory + 0x100, request.data(), sizeof(request));
    return device->execute_ioctl(emu, context());
  }

  uint64_t returned_key() const {
    return emu.memory.read_memory<uint64_t>(memory + 0x208);
  }

  NTSTATUS returned_status() const {
    return emu.memory.read_memory<NTSTATUS>(memory + 0x204);
  }

  void set_guid() {
    constexpr std::u16string_view guid =
        u"{4986b000-3543-4b84-9023-1df15a099fd8}";
    emu.memory.write_memory(memory + 0x300, guid.data(),
                            (guid.size() + 1) * sizeof(char16_t));
    const auto address = memory + 0x300;
    memcpy(request.data() + 4, &address, sizeof(address));
    request[6] = 78;
  }
};

TEST_F(CmApiTest, ReturnsUsableGuestInterfaceRegistryHandle) {
  emu.memory.set_memory(memory + 0x200, 0xa5, 16);
  ASSERT_EQ(run(), STATUS_SUCCESS);
  ASSERT_EQ(returned_status(), STATUS_SUCCESS);
  EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x200), 16u);
  EXPECT_EQ(
      emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(memory)
          .Information,
      16u);
  const auto *key = emu.process.registry_keys.get(returned_key());
  ASSERT_NE(key, nullptr);
  EXPECT_EQ(key->path.get().filename().u16string(), u"deviceclasses");
  auto &vcpu = emu.vcpu(0);
  const syscall_context c{
      .win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
  const auto count = emu.registry.get_sub_key_count(*key);
  EXPECT_EQ(syscalls::handle_NtEnumerateKey(c, make_handle(returned_key()),
                                            static_cast<ULONG>(count),
                                            KeyBasicInformation, memory + 0x400,
                                            512, {emu.memory, memory + 0x700}),
            STATUS_NO_MORE_ENTRIES);
}

TEST_F(CmApiTest, OpensInstallerClassRoot) {
  request[2] = 2;
  ASSERT_EQ(run(), STATUS_SUCCESS);
  ASSERT_EQ(returned_status(), STATUS_SUCCESS);
  const auto *key = emu.process.registry_keys.get(returned_key());
  ASSERT_NE(key, nullptr);
  EXPECT_EQ(key->path.get().filename().u16string(), u"class");
}

TEST_F(CmApiTest, MissingClassReturnsOperationFailureWithoutStaleHandle) {
  set_guid();
  emu.memory.set_memory(memory + 0x200, 0xa5, 16);
  ASSERT_EQ(run(), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_OBJECT_NAME_NOT_FOUND);
  EXPECT_EQ(returned_key(), 0u);
}

TEST_F(CmApiTest, OpenAlwaysCreatesOnlyGuestOverlayKey) {
  set_guid();
  request[8] = 1;
  ASSERT_EQ(run(), STATUS_SUCCESS);
  ASSERT_EQ(returned_status(), STATUS_SUCCESS);
  const auto *key = emu.process.registry_keys.get(returned_key());
  ASSERT_NE(key, nullptr);
  EXPECT_EQ(key->path.get().filename().u16string(),
            u"{4986b000-3543-4b84-9023-1df15a099fd8}");
  request[8] = 2;
  ASSERT_EQ(run(), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_SUCCESS);
}

TEST_F(CmApiTest, Wow64RequestProducesSameHandleResultLayout) {
  device = create_cm_api({.is_32_bit = true});
  const std::array<uint32_t, 9> wow64{36, 0, 3, 0, 0, KEY_ENUMERATE_SUB_KEYS,
                                      2,  0, 16};
  emu.memory.write_memory(memory + 0x100, wow64.data(), sizeof(wow64));
  auto c = context();
  c.input_buffer_length = sizeof(wow64);
  ASSERT_EQ(device->execute_ioctl(emu, c), STATUS_SUCCESS);
  ASSERT_EQ(returned_status(), STATUS_SUCCESS);
  EXPECT_NE(emu.process.registry_keys.get(returned_key()), nullptr);
}

TEST_F(CmApiTest, RejectsMalformedRequestAndShortOutput) {
  request[0] = 47;
  EXPECT_EQ(run(), STATUS_INVALID_PARAMETER);
  request[0] = 48;
  emu.memory.write_memory(memory + 0x100, request.data(), sizeof(request));
  auto c = context();
  c.output_buffer_length = 8;
  EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_INVALID_PARAMETER);
  EXPECT_EQ(emu.process.registry_keys.size(), 0u);
}

TEST_F(CmApiTest, RejectsUnsupportedOperationAndInvalidGuestPointers) {
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

TEST_F(CmApiTest, FailedOutputWriteClosesNewHandle) {
  emu.memory.write_memory(memory + 0x100, request.data(), sizeof(request));
  auto c = context();
  c.output_buffer = 0x7fffffff0000;
  EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_ACCESS_VIOLATION);
  EXPECT_EQ(emu.process.registry_keys.size(), 0u);
}

class CmInterfaceListTest : public CmApiTest {
protected:
  std::array<uint32_t, 10> input{
      40, 0, 0x91827364, 0x4aabbbbb, 0x78675645, 0x1201f0ef, 0, 0, 0, 20};
  const std::u16string class_path =
      uR"(\Registry\Machine\System\CurrentControlSet\Control\DeviceClasses\{91827364-bbbb-4aab-4556-6778eff00112})";

  io_device_context list_context(uint32_t length = 512) {
    auto c = context();
    c.io_control_code = 0x470807;
    c.input_buffer_length = 40;
    c.output_buffer_length = length;
    return c;
  }

  NTSTATUS query(uint32_t length = 512) {
    emu.memory.write_memory(memory + 0x100, input.data(), sizeof(input));
    return device->execute_ioctl(emu, list_context(length));
  }

  void add_interface(std::u16string_view name, std::u16string_view reference,
                     std::u16string_view id) {
    const auto path = class_path + u'\\' + std::u16string(name);
    auto key = emu.registry.create_key(path);
    ASSERT_TRUE(key);
    std::u16string value{id};
    value += u'\0';
    emu.registry.set_value(*key, "DeviceInstance", REG_SZ,
                           std::as_bytes(std::span(value)));
    ASSERT_TRUE(
        emu.registry.create_key(path + u'\\' + std::u16string(reference)));
  }

  uint32_t required() const {
    return emu.memory.read_memory<uint32_t>(memory + 0x208);
  }

  uint64_t information() const {
    return emu.memory
        .read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(memory)
        .Information;
  }

  std::u16string payload() const {
    std::u16string value(required() / 2, u'\0');
    emu.memory.read_memory(memory + 0x210, value.data(), required());
    return value;
  }
};

TEST_F(CmInterfaceListTest, EmptyClassReportsSizeThenReturnsTerminator) {
  ASSERT_EQ(query(20), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_BUFFER_TOO_SMALL);
  EXPECT_EQ(required(), 2u);
  EXPECT_EQ(information(), 20u);
  ASSERT_EQ(query(22), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_SUCCESS);
  EXPECT_EQ(payload(), std::u16string(1, u'\0'));
  EXPECT_EQ(information(), 22u);
}

TEST_F(CmInterfaceListTest,
       EnumeratesReferencesAsMultiSzAndPreservesOutputTail) {
  add_interface(u"##?#ROOT#SOGENTEST#1#{91827364-bbbb-4aab-4556-6778eff00112}",
                u"#", u"ROOT\\SOGENTEST\\1");
  add_interface(u"##?#ROOT#SOGENTEST#1#{91827364-bbbb-4aab-4556-6778eff00112}",
                u"#named", u"ROOT\\SOGENTEST\\1");
  emu.memory.set_memory(memory + 0x200, 0xa5, 512);
  ASSERT_EQ(query(), STATUS_SUCCESS);
  ASSERT_EQ(returned_status(), STATUS_SUCCESS);
  const std::u16string first =
      u"\\\\?\\ROOT#SOGENTEST#1#{91827364-bbbb-4aab-4556-6778eff00112}";
  const auto expected = first + u'\0' + first + u"\\named" + u'\0' + u'\0';
  EXPECT_EQ(payload(), expected);
  EXPECT_EQ(information(), 20u + expected.size() * 2);
  EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x210 + required()),
            0xa5a5a5a5u);
}

TEST_F(CmInterfaceListTest,
       ShortBufferReportsFullRequiredSizeWithoutPartialList) {
  add_interface(u"##?#ROOT#SOGENTEST#1#{91827364-bbbb-4aab-4556-6778eff00112}",
                u"#", u"ROOT\\SOGENTEST\\1");
  emu.memory.set_memory(memory + 0x200, 0xa5, 512);
  ASSERT_EQ(query(22), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_BUFFER_TOO_SMALL);
  EXPECT_GT(required(), 2u);
  EXPECT_EQ(information(), 20u);
  EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x210), 0u);
  EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x214), 0xa5a5a5a5u);
}

TEST_F(CmInterfaceListTest, FiltersByCaseInsensitiveDeviceInstance) {
  add_interface(u"##?#ROOT#SOGENTEST#1#{91827364-bbbb-4aab-4556-6778eff00112}",
                u"#", u"ROOT\\SOGENTEST\\1");
  add_interface(u"##?#ROOT#SOGENTEST#2#{91827364-bbbb-4aab-4556-6778eff00112}",
                u"#", u"ROOT\\SOGENTEST\\2");
  const std::u16string id = u"root\\sogentest\\2";
  const auto address = memory + 0x500;
  emu.memory.write_memory(address, id.c_str(), (id.size() + 1) * 2);
  memcpy(input.data() + 6, &address, 8);
  input[8] = static_cast<uint32_t>((id.size() + 1) * 2);
  ASSERT_EQ(query(), STATUS_SUCCESS);
  EXPECT_EQ(
      payload(),
      std::u16string(
          u"\\\\?\\ROOT#SOGENTEST#2#{91827364-bbbb-4aab-4556-6778eff00112}") +
          u'\0' + u'\0');
}

TEST_F(CmInterfaceListTest, SavedRegistrationDoesNotActivateGuestHardware) {
  add_interface(u"##?#ROOT#SOGENTEST#1#{91827364-bbbb-4aab-4556-6778eff00112}",
                u"#", u"ROOT\\SOGENTEST\\1");
  input[1] = 0x10000;
  ASSERT_EQ(query(), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_SUCCESS);
  EXPECT_EQ(payload(), std::u16string(1, u'\0'));
}

TEST_F(CmInterfaceListTest, Wow64DecodesPointerAndResultSizeSeparately) {
  device = create_cm_api({.is_32_bit = true});
  const std::array<uint32_t, 9> wow64{36,       0, input[2], input[3], input[4],
                                      input[5], 0, 0,        20};
  emu.memory.write_memory(memory + 0x100, wow64.data(), sizeof(wow64));
  auto c = list_context(22);
  c.input_buffer_length = 36;
  ASSERT_EQ(device->execute_ioctl(emu, c), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_SUCCESS);
  EXPECT_EQ(information(), 22u);
}

TEST_F(CmInterfaceListTest,
       InvalidFlagsAreOperationFailureWithTransportSuccess) {
  input[1] = 2;
  ASSERT_EQ(query(), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_INVALID_PARAMETER);
  EXPECT_EQ(required(), 0u);
  EXPECT_EQ(information(), 20u);
}

TEST_F(CmInterfaceListTest, ValidatesHeadersPointersAlignmentAndLengths) {
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
class CmPropertyTest : public CmApiTest {
protected:
  struct test_property_key {
    GUID format;
    uint32_t id;
  };

  std::array<uint32_t, 18> input{72, 0, 1, 0, 0, 0, 0, 0, 0,
                                 0,  0, 0, 0, 0, 0, 0, 0, 20};
  std::u16string object_name = u"ROOT\\SOGENTEST\\0000";
  const std::u16string device_path =
      uR"(\Registry\Machine\System\CurrentControlSet\Enum\ROOT\SOGENTEST\0000)";
  const GUID custom_format{0x11223344,
                           0x5566,
                           0x7788,
                           {0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0, 0x12}};

  void SetUp() override {
    CmApiTest::SetUp();
    ASSERT_TRUE(emu.registry.create_key(device_path));
    set_name(object_name);
    set_key(custom_format, 2);
  }

  io_device_context property_context(const uint32_t output_length = 512) {
    auto c = context();
    c.io_control_code = 0x470813;
    c.input_buffer_length = 72;
    c.output_buffer_length = output_length;
    return c;
  }

  void set_name(const std::u16string_view value) {
    object_name.assign(value);
    const auto address = memory + 0x500;
    emu.memory.write_memory(address, object_name.c_str(),
                            (object_name.size() + 1) * sizeof(char16_t));
    memcpy(input.data() + 4, &address, sizeof(address));
    input[6] =
        static_cast<uint32_t>((object_name.size() + 1) * sizeof(char16_t));
  }

  void set_key(const GUID &format, const uint32_t id) {
    const test_property_key key{format, id};
    memcpy(input.data() + 7, &key, sizeof(key));
  }

  void store_generic(const std::u16string_view path,
                     const uint32_t registry_type,
                     const std::span<const std::byte> data) {
    const auto key = emu.registry.create_key(path);
    ASSERT_TRUE(key);
    emu.registry.set_value(*key, "", registry_type, data);
  }

  NTSTATUS query(const uint32_t output_length = 512) {
    emu.memory.write_memory(memory + 0x100, input.data(), sizeof(input));
    return device->execute_ioctl(emu, property_context(output_length));
  }

  uint32_t required() const {
    return emu.memory.read_memory<uint32_t>(memory + 0x208);
  }

  uint32_t property_type() const {
    return emu.memory.read_memory<uint32_t>(memory + 0x20C);
  }

  uint64_t information() const {
    return emu.memory
        .read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(memory)
        .Information;
  }
};

TEST_F(CmPropertyTest, ReadsTypedGenericDeviceProperty) {
  const std::u16string text = u"Sogen display adapter\0";
  store_generic(
      device_path +
          uR"(\Properties\{11223344-5566-7788-99aa-bbccddeef012}\0002)",
      0xFFFF0012, std::as_bytes(std::span(text)));
  ASSERT_EQ(query(), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_SUCCESS);
  EXPECT_EQ(property_type(), 0x12u);
  EXPECT_EQ(required(), text.size() * sizeof(char16_t));
  std::u16string result(text.size(), u'\0');
  emu.memory.read_memory(memory + 0x210, result.data(),
                         result.size() * sizeof(char16_t));
  EXPECT_EQ(result, text);
  EXPECT_EQ(information(), 20u + required());
}

TEST_F(CmPropertyTest, ShortPropertyBufferReportsMetadataWithoutPartialData) {
  const std::array<std::byte, 8> data{std::byte{1}, std::byte{2}, std::byte{3},
                                      std::byte{4}, std::byte{5}, std::byte{6},
                                      std::byte{7}, std::byte{8}};
  store_generic(
      device_path +
          uR"(\Properties\{11223344-5566-7788-99aa-bbccddeef012}\0002)",
      0xFFFF1003, data);
  emu.memory.set_memory(memory + 0x200, 0xA5, 64);
  ASSERT_EQ(query(20), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_BUFFER_TOO_SMALL);
  EXPECT_EQ(property_type(), 0x1003u);
  EXPECT_EQ(required(), data.size());
  EXPECT_EQ(information(), 20u);
  EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x210), 0u);
  EXPECT_EQ(emu.memory.read_memory<uint32_t>(memory + 0x214), 0xA5A5A5A5u);
}

TEST_F(CmPropertyTest, ConvertsNamedGuidAndReturnsFullInstanceIdentity) {
  const auto device_key = emu.registry.get_key({device_path});
  ASSERT_TRUE(device_key);
  const std::u16string class_guid = u"{4d36e968-e325-11ce-bfc1-08002be10318}\0";
  emu.registry.set_value(*device_key, "ClassGuid", REG_SZ,
                         std::as_bytes(std::span(class_guid)));
  const GUID expected{0x4D36E968,
                      0xE325,
                      0x11CE,
                      {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};
  set_key(GUID{0xA45C254E,
               0xDF1C,
               0x4EFD,
               {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}},
          10);
  ASSERT_EQ(query(), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_SUCCESS);
  EXPECT_EQ(property_type(), 0x0Du);
  const auto actual = emu.memory.read_memory<GUID>(memory + 0x210);
  EXPECT_EQ(memcmp(&actual, &expected, sizeof(GUID)), 0);

  set_key(GUID{0x78C34FC8,
               0x104A,
               0x4ACA,
               {0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57}},
          256);
  ASSERT_EQ(query(), STATUS_SUCCESS);
  std::u16string result(required() / sizeof(char16_t), u'\0');
  emu.memory.read_memory(memory + 0x210, result.data(), required());
  EXPECT_EQ(result, object_name + u'\0');
}

TEST_F(CmPropertyTest, ResolvesInterfaceStateClassAndReference) {
  constexpr GUID expected_class{
      0x91827364,
      0xBBBB,
      0x4AAB,
      {0x45, 0x56, 0x67, 0x78, 0xEF, 0xF0, 0x01, 0x12}};
  const std::u16string interface_path =
      uR"(\Registry\Machine\System\CurrentControlSet\Control\DeviceClasses\{91827364-bbbb-4aab-4556-6778eff00112}\##?#ROOT#SOGENTEST#0000#{91827364-bbbb-4aab-4556-6778eff00112})";
  ASSERT_TRUE(emu.registry.create_key(interface_path));
  const auto control =
      emu.registry.create_key(interface_path + uR"(\#render\Control)");
  ASSERT_TRUE(control);
  const uint32_t linked = 1;
  emu.registry.set_value(*control, "Linked", REG_DWORD,
                         std::as_bytes(std::span(&linked, 1)));
  input[2] = 4;
  set_name(u"\\\\?\\ROOT#SOGENTEST#0000#{91827364-bbbb-4aab-4556-6778eff00112}"
           u"\\render");
  const GUID format{0x026E516E,
                    0xB814,
                    0x414B,
                    {0x83, 0xCD, 0x85, 0x6D, 0x6F, 0xEF, 0x48, 0x22}};

  set_key(format, 3);
  ASSERT_EQ(query(), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_SUCCESS);
  EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x210), 0xFF);
  set_key(format, 4);
  ASSERT_EQ(query(), STATUS_SUCCESS);
  const auto actual = emu.memory.read_memory<GUID>(memory + 0x210);
  EXPECT_EQ(memcmp(&actual, &expected_class, sizeof(GUID)), 0);
  set_key(format, 5);
  ASSERT_EQ(query(), STATUS_SUCCESS);
  std::u16string reference(required() / sizeof(char16_t), u'\0');
  emu.memory.read_memory(memory + 0x210, reference.data(), required());
  EXPECT_EQ(reference, std::u16string(u"render\0", 7));
}

TEST_F(CmPropertyTest, MissingObjectsAndValuesRemainOperationFailures) {
  set_name(u"ROOT\\SOGENTEST\\missing");
  ASSERT_EQ(query(), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_OBJECT_NAME_NOT_FOUND);
  EXPECT_EQ(required(), 0u);
  set_name(u"ROOT\\SOGENTEST\\0000");
  ASSERT_EQ(query(), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_OBJECT_NAME_NOT_FOUND);
  EXPECT_EQ(property_type(), 0u);
}

TEST_F(CmPropertyTest, Wow64DecodesMatchedFiftySixBytePacket) {
  const std::array<uint32_t, 1> data{0x12345678};
  store_generic(
      device_path +
          uR"(\Properties\{11223344-5566-7788-99aa-bbccddeef012}\0002)",
      0xFFFF0007, std::as_bytes(std::span(data)));
  device = create_cm_api({.is_32_bit = true});
  constexpr uint64_t low_memory = 0x100000;
  ASSERT_TRUE(emu.memory.allocate_memory(low_memory, 0x2000,
                                         memory_permission::read_write));
  emu.memory.write_memory(low_memory + 0x500, object_name.c_str(),
                          (object_name.size() + 1) * sizeof(char16_t));
  std::array<uint32_t, 14> wow64{
      56,       0, 1, static_cast<uint32_t>(low_memory + 0x500),
      input[6], 0, 0, 0,
      0,        0, 0, 0,
      0,        20};
  const test_property_key key{custom_format, 2};
  memcpy(wow64.data() + 5, &key, sizeof(key));
  emu.memory.write_memory(low_memory + 0x100, wow64.data(), sizeof(wow64));
  io_device_context c{emu.memory};
  c.io_status_block = {emu.memory, low_memory};
  c.io_control_code = 0x470813;
  c.input_buffer = low_memory + 0x100;
  c.input_buffer_length = sizeof(wow64);
  c.output_buffer = low_memory + 0x200;
  c.output_buffer_length = 512;
  ASSERT_EQ(device->execute_ioctl(emu, c), STATUS_SUCCESS);
  EXPECT_EQ(emu.memory.read_memory<NTSTATUS>(low_memory + 0x204),
            STATUS_SUCCESS);
  EXPECT_EQ(emu.memory.read_memory<uint32_t>(low_memory + 0x20C), 0x07u);
  EXPECT_EQ(emu.memory.read_memory<uint32_t>(low_memory + 0x210), data[0]);
}

TEST_F(CmPropertyTest, RejectsMalformedPacketsAndNames) {
  input[17] = 19;
  EXPECT_EQ(query(), STATUS_INVALID_PARAMETER);
  input[17] = 20;
  input[1] = 1;
  EXPECT_EQ(query(), STATUS_INVALID_PARAMETER);
  input[1] = 0;
  input[6] -= 2;
  EXPECT_EQ(query(), STATUS_SUCCESS);
  EXPECT_EQ(returned_status(), STATUS_INVALID_PARAMETER);
  input[6] += 2;
  emu.memory.write_memory(memory + 0x100, input.data(), sizeof(input));
  auto c = property_context();
  c.output_buffer++;
  EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_DATATYPE_MISALIGNMENT);
}

} // namespace sogen::test
