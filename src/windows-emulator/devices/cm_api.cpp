#include "cm_api.hpp"
#include "../std_include.hpp"
#include "../windows_emulator.hpp"
#include <utils/finally.hpp>

#include <chrono>
#include <cstdlib> // opt-in CMApi logging and profiling gates

namespace sogen {
namespace {
struct registry_request {
  uint32_t size;
  uint32_t operation;
  uint32_t key_type;
  uint32_t padding;
  uint64_t name;
  uint32_t name_bytes;
  uint32_t desired_access;
  uint32_t disposition;
  uint32_t flags;
  uint32_t result_size;
  uint32_t reserved;
};

struct interface_list_request {
  uint32_t size;
  uint32_t flags;
  GUID interface_class;
  uint64_t device_id;
  uint32_t device_id_bytes;
  uint32_t result_size;
};

static_assert(sizeof(interface_list_request) == 40);

struct device_node_request {
  uint32_t size;
  uint32_t operation;
  uint32_t object_type;
  uint32_t reserved;
  uint64_t device_id;
  uint32_t device_id_bytes;
  uint32_t flags;
  uint32_t result_size;
  uint32_t padding;
};

static_assert(sizeof(device_node_request) == 40);

struct property_key {
  GUID format;
  uint32_t id;
};

struct object_property_request {
  uint32_t size;
  uint32_t reserved;
  uint32_t object_type;
  uint32_t padding;
  uint64_t object_name;
  uint32_t object_name_bytes;
  property_key key;
  uint64_t locale;
  uint64_t machine;
  uint32_t flags;
  uint32_t result_size;
};

struct property_descriptor {
  uint32_t type;
  std::string_view registry_name;
  bool named{};
};

struct property_value {
  uint32_t type{};
  std::vector<std::byte> data{};
};

struct object_property_context {
  std::u16string key_path;
  std::u16string properties_path;
  std::u16string name;
  std::u16string reference;
  GUID interface_class{};
};

static_assert(sizeof(property_key) == 20);
static_assert(sizeof(object_property_request) == 72);

constexpr uint32_t devprop_type_empty = 0x00;
constexpr uint32_t devprop_type_int32 = 0x06;
constexpr uint32_t devprop_type_uint32 = 0x07;
constexpr uint32_t devprop_type_uint64 = 0x09;
constexpr uint32_t devprop_type_guid = 0x0D;
constexpr uint32_t devprop_type_filetime = 0x10;
constexpr uint32_t devprop_type_boolean = 0x11;
constexpr uint32_t devprop_type_string = 0x12;
constexpr uint32_t devprop_type_security_descriptor = 0x13;
constexpr uint32_t devprop_type_security_descriptor_string = 0x14;
constexpr uint32_t devprop_type_ntstatus = 0x18;
constexpr uint32_t devprop_type_string_list = 0x2012;
constexpr uint32_t devprop_type_binary = 0x1003;

constexpr GUID property_name_format{
    0xB725F130,
    0x47EF,
    0x101A,
    {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}};
constexpr GUID device_legacy_format{
    0xA45C254E,
    0xDF1C,
    0x4EFD,
    {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}};
constexpr GUID device_instance_format{
    0x78C34FC8,
    0x104A,
    0x4ACA,
    {0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57}};
constexpr GUID device_state_format{
    0x4340A6C5,
    0x93FA,
    0x4706,
    {0x97, 0x2C, 0x7B, 0x64, 0x80, 0x08, 0xA5, 0xA7}};
constexpr GUID device_reported_format{
    0x80497100,
    0x8C73,
    0x48B9,
    {0xAA, 0xD9, 0xCE, 0x38, 0x7E, 0x19, 0xC5, 0x6E}};
constexpr GUID device_container_format{
    0x8C7ED206,
    0x3F8A,
    0x4827,
    {0xB3, 0xAB, 0xAE, 0x9E, 0x1F, 0xAE, 0xFC, 0x6C}};
constexpr GUID device_attributes_format{
    0x80D81EA6,
    0x7473,
    0x4B0C,
    {0x82, 0x16, 0xEF, 0xC1, 0x1A, 0x2C, 0x4C, 0x8B}};
constexpr GUID device_extended_format{
    0x540B947E,
    0x8B40,
    0x45BC,
    {0xA8, 0xA2, 0x6A, 0x0B, 0x89, 0x4C, 0xBD, 0xA2}};
constexpr GUID device_dates_format{
    0x83DA6326,
    0x97A6,
    0x4088,
    {0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29}};
constexpr GUID device_driver_format{
    0xA8B865DD,
    0x2E3D,
    0x4094,
    {0xAD, 0x97, 0xE5, 0x93, 0xA7, 0x0C, 0x75, 0xD6}};
constexpr GUID device_removal_format{
    0xAFD97640,
    0x86A3,
    0x4210,
    {0xB6, 0x7C, 0x28, 0x9C, 0x41, 0xAA, 0xBE, 0x55}};
constexpr GUID class_legacy_format{
    0x4321918B,
    0xF69E,
    0x470D,
    {0xA5, 0xDE, 0x4D, 0x88, 0xC7, 0x5A, 0xD2, 0x4B}};
constexpr GUID class_info_format{
    0x259ABFFC,
    0x50A7,
    0x47CE,
    {0xAF, 0x08, 0x68, 0xC9, 0xA7, 0xD7, 0x33, 0x66}};
constexpr GUID class_rebalance_format{
    0xD14D3EF3,
    0x66CF,
    0x4BA2,
    {0x9D, 0x38, 0x0D, 0xDB, 0x37, 0xAB, 0x47, 0x01}};
constexpr GUID class_coinstallers_format{
    0x713D1703,
    0xA2E2,
    0x49F5,
    {0x92, 0x14, 0x56, 0x47, 0x2E, 0xF3, 0xDA, 0x5C}};
constexpr GUID interface_format{
    0x026E516E,
    0xB814,
    0x414B,
    {0x83, 0xCD, 0x85, 0x6D, 0x6F, 0xEF, 0x48, 0x22}};

bool same_guid(const GUID &left, const GUID &right) {
  return memcmp(&left, &right, sizeof(GUID)) == 0;
}

std::u16string guid_name(const GUID &guid) {
  return u8_to_u16(utils::string::va(
      "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", guid.Data1,
      guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
      guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6],
      guid.Data4[7]));
}

std::optional<uint64_t> parse_hex(const std::u16string_view value,
                                  const size_t offset, const size_t length) {
  uint64_t result = 0;
  for (size_t i = 0; i < length; ++i) {
    const auto c = value[offset + i];
    uint8_t digit{};
    if (c >= u'0' && c <= u'9') {
      digit = static_cast<uint8_t>(c - u'0');
    } else if (c >= u'a' && c <= u'f') {
      digit = static_cast<uint8_t>(c - u'a' + 10);
    } else if (c >= u'A' && c <= u'F') {
      digit = static_cast<uint8_t>(c - u'A' + 10);
    } else {
      return std::nullopt;
    }
    result = (result << 4) | digit;
  }
  return result;
}

std::optional<GUID> parse_guid(const std::u16string_view value) {
  if (value.size() != 38 || value[0] != u'{' || value[9] != u'-' ||
      value[14] != u'-' || value[19] != u'-' || value[24] != u'-' ||
      value[37] != u'}') {
    return std::nullopt;
  }
  const auto data1 = parse_hex(value, 1, 8);
  const auto data2 = parse_hex(value, 10, 4);
  const auto data3 = parse_hex(value, 15, 4);
  const auto data4_0 = parse_hex(value, 20, 2);
  const auto data4_1 = parse_hex(value, 22, 2);
  const auto data4_2 = parse_hex(value, 25, 2);
  const auto data4_3 = parse_hex(value, 27, 2);
  const auto data4_4 = parse_hex(value, 29, 2);
  const auto data4_5 = parse_hex(value, 31, 2);
  const auto data4_6 = parse_hex(value, 33, 2);
  const auto data4_7 = parse_hex(value, 35, 2);
  if (!data1 || !data2 || !data3 || !data4_0 || !data4_1 || !data4_2 ||
      !data4_3 || !data4_4 || !data4_5 || !data4_6 || !data4_7) {
    return std::nullopt;
  }
  return GUID{static_cast<uint32_t>(*data1),
              static_cast<uint16_t>(*data2),
              static_cast<uint16_t>(*data3),
              {static_cast<uint8_t>(*data4_0), static_cast<uint8_t>(*data4_1),
               static_cast<uint8_t>(*data4_2), static_cast<uint8_t>(*data4_3),
               static_cast<uint8_t>(*data4_4), static_cast<uint8_t>(*data4_5),
               static_cast<uint8_t>(*data4_6), static_cast<uint8_t>(*data4_7)}};
}

property_descriptor named_property(const uint32_t type,
                                   const std::string_view name) {
  return {.type = type, .registry_name = name, .named = true};
}

property_descriptor stored_property(const uint32_t type) {
  return {.type = type};
}

std::optional<property_descriptor>
describe_device_legacy_property(const uint32_t id) {
  switch (id) {
  case 2:
    return named_property(devprop_type_string, "DeviceDesc");
  case 3:
    return named_property(devprop_type_string_list, "HardwareId");
  case 4:
    return named_property(devprop_type_string_list, "CompatibleIDs");
  case 6:
    return named_property(devprop_type_string, "Service");
  case 9:
    return named_property(devprop_type_string, "Class");
  case 10:
    return named_property(devprop_type_guid, "ClassGuid");
  case 11:
    return named_property(devprop_type_string, "Driver");
  case 12:
    return named_property(devprop_type_uint32, "ConfigFlags");
  case 13:
    return named_property(devprop_type_string, "Mfg");
  case 14:
    return named_property(devprop_type_string, "FriendlyName");
  case 15:
    return named_property(devprop_type_string, "LocationInformation");
  case 16:
    return named_property(devprop_type_string, "PDOName");
  case 17:
    return named_property(devprop_type_uint32, "Capabilities");
  case 18:
    return named_property(devprop_type_uint32, "UINumber");
  case 19:
    return named_property(devprop_type_string_list, "UpperFilters");
  case 20:
    return named_property(devprop_type_string_list, "LowerFilters");
  case 21:
    return named_property(devprop_type_guid, "BusTypeGuid");
  case 22:
    return named_property(devprop_type_uint32, "LegacyBusType");
  case 23:
    return named_property(devprop_type_uint32, "BusNumber");
  case 24:
    return named_property(devprop_type_string, "EnumeratorName");
  case 25:
    return named_property(devprop_type_security_descriptor, "Security");
  case 26:
    return named_property(devprop_type_security_descriptor_string,
                          "SecuritySDS");
  case 27:
    return named_property(devprop_type_uint32, "DevType");
  case 28:
    return named_property(devprop_type_boolean, "Exclusive");
  case 29:
    return named_property(devprop_type_uint32, "Characteristics");
  case 30:
    return named_property(devprop_type_uint32, "Address");
  case 31:
    return named_property(devprop_type_string, "UINumberDescFormat");
  case 32:
    return named_property(devprop_type_binary, "PowerData");
  case 33:
    return named_property(devprop_type_uint32, "RemovalPolicy");
  case 34:
    return named_property(devprop_type_uint32, "RemovalPolicyDefault");
  case 35:
    return named_property(devprop_type_uint32, "RemovalPolicyOverride");
  case 36:
    return named_property(devprop_type_uint32, "InstallState");
  case 37:
    return named_property(devprop_type_string_list, "LocationPaths");
  case 38:
    return named_property(devprop_type_guid, "BaseContainerId");
  default:
    return std::nullopt;
  }
}

std::optional<property_descriptor>
describe_device_property(const property_key &key) {
  if (same_guid(key.format, device_legacy_format)) {
    return describe_device_legacy_property(key.id);
  }
  if (same_guid(key.format, device_instance_format)) {
    if (key.id == 256 || key.id == 39) {
      return stored_property(devprop_type_string);
    }
  } else if (same_guid(key.format, device_state_format)) {
    switch (key.id) {
    case 2:
    case 3:
      return stored_property(devprop_type_uint32);
    case 4:
    case 5:
    case 6:
    case 7:
    case 9:
    case 10:
    case 11:
      return stored_property(devprop_type_string_list);
    case 8:
      return stored_property(devprop_type_string);
    case 12:
      return stored_property(devprop_type_ntstatus);
    default:
      break;
    }
  } else if (same_guid(key.format, device_reported_format) &&
             (key.id == 2 || key.id == 3)) {
    return stored_property(devprop_type_boolean);
  } else if (same_guid(key.format, device_container_format)) {
    if (key.id == 2) {
      return named_property(devprop_type_guid, "ContainerId");
    }
    if (key.id == 4) {
      return stored_property(devprop_type_boolean);
    }
  } else if (same_guid(key.format, device_attributes_format)) {
    switch (key.id) {
    case 2:
      return stored_property(devprop_type_guid);
    case 3:
    case 4:
      return stored_property(devprop_type_uint32);
    case 5:
    case 7:
    case 8:
      return stored_property(devprop_type_boolean);
    case 6:
      return stored_property(devprop_type_int32);
    default:
      break;
    }
  } else if (same_guid(key.format, device_extended_format)) {
    switch (key.id) {
    case 1:
    case 2:
    case 3:
    case 8:
    case 12:
      return stored_property(devprop_type_uint32);
    case 4:
    case 7:
    case 10:
    case 11:
    case 18:
    case 19:
      return stored_property(devprop_type_string);
    case 5:
    case 6:
    case 13:
    case 16:
    case 22:
      return stored_property(devprop_type_boolean);
    case 9:
      return stored_property(devprop_type_binary);
    case 14:
    case 15:
    case 20:
    case 21:
      return stored_property(devprop_type_string_list);
    case 17:
      return stored_property(devprop_type_filetime);
    case 23:
      return stored_property(devprop_type_uint64);
    default:
      break;
    }
  } else if (same_guid(key.format, device_dates_format)) {
    if (key.id == 6) {
      return stored_property(devprop_type_uint32);
    }
    if (key.id >= 100 && key.id <= 103) {
      return stored_property(devprop_type_filetime);
    }
  } else if (same_guid(key.format, device_driver_format)) {
    switch (key.id) {
    case 2:
      return named_property(devprop_type_filetime, "DriverDateData");
    case 3:
      return named_property(devprop_type_string, "DriverVersion");
    case 4:
      return named_property(devprop_type_string, "DriverDesc");
    case 5:
      return named_property(devprop_type_string, "InfPath");
    case 6:
      return named_property(devprop_type_string, "InfSection");
    case 7:
      return named_property(devprop_type_string, "InfSectionExt");
    case 8:
      return named_property(devprop_type_string, "MatchingDeviceId");
    case 9:
      return named_property(devprop_type_string, "ProviderName");
    case 10:
      return named_property(devprop_type_string, "EnumPropPages32");
    case 11:
      return named_property(devprop_type_string_list, "CoInstallers32");
    case 12:
      return named_property(devprop_type_string, "ResourcePickerTags");
    case 13:
      return named_property(devprop_type_string, "ResourcePickerExceptions");
    case 14:
    case 15:
      return stored_property(devprop_type_uint32);
    case 17:
    case 18:
    case 19:
      return stored_property(devprop_type_boolean);
    default:
      break;
    }
  } else if (same_guid(key.format, device_removal_format) &&
             (key.id == 2 || key.id == 3)) {
    return stored_property(devprop_type_boolean);
  }
  return std::nullopt;
}

std::optional<property_descriptor>
describe_class_property(const property_key &key) {
  if (same_guid(key.format, property_name_format) && key.id == 10) {
    return named_property(devprop_type_string, "");
  }
  if (same_guid(key.format, class_legacy_format)) {
    switch (key.id) {
    case 19:
      return named_property(devprop_type_string_list, "UpperFilters");
    case 20:
      return named_property(devprop_type_string_list, "LowerFilters");
    case 25:
      return named_property(devprop_type_security_descriptor, "Security");
    case 26:
      return named_property(devprop_type_security_descriptor_string,
                            "SecuritySDS");
    case 27:
      return named_property(devprop_type_uint32, "DevType");
    case 28:
      return named_property(devprop_type_boolean, "Exclusive");
    case 29:
      return named_property(devprop_type_uint32, "Characteristics");
    default:
      return std::nullopt;
    }
  }
  if (same_guid(key.format, class_info_format)) {
    switch (key.id) {
    case 2:
      return named_property(devprop_type_string, "");
    case 3:
      return named_property(devprop_type_string, "Class");
    case 4:
      return stored_property(devprop_type_string);
    case 5:
      return named_property(devprop_type_string, "Installer32");
    case 6:
      return named_property(devprop_type_string, "EnumPropPages32");
    case 7:
      return named_property(devprop_type_boolean, "NoInstallClass");
    case 8:
      return named_property(devprop_type_boolean, "NoDisplayClass");
    case 9:
      return named_property(devprop_type_boolean, "SilentInstall");
    case 10:
      return named_property(devprop_type_boolean, "NoUseClass");
    case 11:
      return named_property(devprop_type_string, "Default Service");
    case 12:
      return named_property(devprop_type_string_list, "IconPath");
    default:
      return std::nullopt;
    }
  }
  if (same_guid(key.format, class_rebalance_format) && key.id == 2) {
    return stored_property(devprop_type_boolean);
  }
  if (same_guid(key.format, class_coinstallers_format) && key.id == 2) {
    return stored_property(devprop_type_string_list);
  }
  return std::nullopt;
}

std::optional<property_descriptor>
describe_interface_property(const property_key &key) {
  if (same_guid(key.format, interface_format)) {
    switch (key.id) {
    case 2:
      return named_property(devprop_type_string, "FriendlyName");
    case 3:
      return stored_property(devprop_type_boolean);
    case 4:
      return stored_property(devprop_type_guid);
    case 5:
      return stored_property(devprop_type_string);
    case 6:
      return stored_property(devprop_type_boolean);
    case 8:
      return stored_property(devprop_type_string_list);
    case 9:
      return stored_property(devprop_type_string);
    default:
      break;
    }
  }
  if (same_guid(key.format, device_instance_format) && key.id == 256) {
    return named_property(devprop_type_string, "DeviceInstance");
  }
  if (same_guid(key.format, device_container_format) && key.id == 2) {
    return stored_property(devprop_type_guid);
  }
  return std::nullopt;
}

property_value string_property(const std::u16string_view value) {
  property_value result{.type = devprop_type_string};
  result.data.resize((value.size() + 1) * sizeof(char16_t));
  memcpy(result.data.data(), value.data(), value.size() * sizeof(char16_t));
  return result;
}

property_value binary_property(const uint32_t type, const void *data,
                               const size_t size) {
  property_value result{.type = type};
  result.data.resize(size);
  if (size) {
    memcpy(result.data.data(), data, size);
  }
  return result;
}

std::optional<property_value>
convert_registry_property(const registry_value &value,
                          const uint32_t expected_type) {
  auto type = expected_type;
  if (!type) {
    if ((value.type & 0xFFFF0000u) != 0xFFFF0000u) {
      return std::nullopt;
    }
    type = value.type & 0xFFFFu;
  }
  if (type == devprop_type_guid) {
    if (value.data.size() == sizeof(GUID)) {
      return binary_property(type, value.data.data(), value.data.size());
    }
    const auto text = value.as_string();
    const auto guid = text ? parse_guid(*text) : std::nullopt;
    return guid ? std::optional{binary_property(type, &*guid, sizeof(*guid))}
                : std::nullopt;
  }
  if (type == devprop_type_boolean) {
    uint8_t boolean{};
    if (value.data.size() == 1) {
      boolean =
          *reinterpret_cast<const uint8_t *>(value.data.data()) ? 0xFF : 0;
    } else if (const auto dword = value.as_dword()) {
      boolean = *dword ? 0xFF : 0;
    } else {
      return std::nullopt;
    }
    return binary_property(type, &boolean, sizeof(boolean));
  }
  return binary_property(type, value.data.data(), value.data.size());
}

std::optional<property_value>
read_property_store(windows_emulator &win_emu,
                    const std::u16string_view properties_path,
                    const property_key &key, const uint32_t expected_type) {
  auto path = std::u16string(properties_path);
  path += u'\\';
  path += guid_name(key.format);
  path += u'\\';
  path += u8_to_u16(utils::string::va("%04X", key.id));
  win_emu.callbacks.on_generic_access("Registry key", path);
  const auto property = win_emu.registry.get_key({path});
  if (!property) {
    return std::nullopt;
  }
  const auto value = win_emu.registry.get_value(*property, "");
  return value ? convert_registry_property(*value, expected_type)
               : std::nullopt;
}

struct handle_result {
  uint32_t size{16};
  NTSTATUS status{};
  uint64_t key{};
};

struct status_result {
  uint32_t size{8};
  NTSTATUS status{};
};

static_assert(sizeof(registry_request) == 48);
static_assert(sizeof(handle_result) == 16);
static_assert(sizeof(status_result) == 8);

bool is_class_guid(const std::u16string_view value) {
  if (value.size() != 38 || value.front() != u'{' || value.back() != u'}') {
    return false;
  }
  for (size_t i = 1; i < 37; ++i) {
    if (i == 9 || i == 14 || i == 19 || i == 24) {
      if (value[i] != u'-') {
        return false;
      }
    } else if (!((value[i] >= u'0' && value[i] <= u'9') ||
                 (value[i] >= u'a' && value[i] <= u'f') ||
                 (value[i] >= u'A' && value[i] <= u'F'))) {
      return false;
    }
  }
  return true;
}

bool is_legal_device_id(const std::u16string_view value) {
  if (value.empty() || value.size() > 200) {
    return false;
  }
  size_t components = 1;
  size_t component_length = 0;
  for (const auto character : value) {
    if (character < 0x21 || character > 0x7F || character == u',') {
      return false;
    }
    if (character == u'\\') {
      if (!component_length) {
        return false;
      }
      ++components;
      component_length = 0;
    } else {
      ++component_length;
    }
  }
  return components == 3 && component_length != 0;
}

struct cm_api : stateless_device {
  bool is_32_bit;

  explicit cm_api(const bool wow64) : is_32_bit(wow64) {}

  NTSTATUS
  read_object_property_request(windows_emulator &win_emu,
                               const io_device_context &c,
                               object_property_request &request) const {
    const auto required = this->is_32_bit ? 56u : 72u;
    if (!c.input_buffer || c.input_buffer_length < required) {
      return STATUS_INVALID_PARAMETER;
    }
    if (c.input_buffer % (this->is_32_bit ? 4 : 8)) {
      return STATUS_DATATYPE_MISALIGNMENT;
    }
    if (this->is_32_bit) {
      std::array<uint32_t, 14> input{};
      if (!win_emu.memory.try_read_memory(c.input_buffer, input.data(),
                                          sizeof(input))) {
        return STATUS_ACCESS_VIOLATION;
      }
      if (input[0] != required) {
        return STATUS_INVALID_PARAMETER;
      }
      request.size = 72;
      request.reserved = input[1];
      request.object_type = input[2];
      request.object_name = input[3];
      request.object_name_bytes = input[4];
      memcpy(&request.key, input.data() + 5, sizeof(request.key));
      request.locale = input[10];
      request.machine = input[11];
      request.flags = input[12];
      request.result_size = input[13];
    } else if (!win_emu.memory.try_read_memory(c.input_buffer, &request,
                                               sizeof(request))) {
      return STATUS_ACCESS_VIOLATION;
    }
    if (request.size != 72 || request.reserved || request.padding ||
        request.locale || request.machine || request.flags ||
        request.key.id < 2) {
      return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
  }

  static NTSTATUS read_object_name(windows_emulator &win_emu,
                                   const object_property_request &request,
                                   std::u16string &name) {
    if (!request.object_name || request.object_name_bytes < 2 ||
        request.object_name_bytes > 0xFFFE ||
        request.object_name_bytes % sizeof(char16_t)) {
      return STATUS_INVALID_PARAMETER;
    }
    if (request.object_name % alignof(char16_t)) {
      return STATUS_DATATYPE_MISALIGNMENT;
    }
    name.resize(request.object_name_bytes / sizeof(char16_t));
    if (!win_emu.memory.try_read_memory(request.object_name, name.data(),
                                        request.object_name_bytes)) {
      return STATUS_ACCESS_VIOLATION;
    }
    const auto terminator = name.find(u'\0');
    if (terminator == std::u16string::npos || terminator + 1 != name.size()) {
      return STATUS_INVALID_PARAMETER;
    }
    name.resize(terminator);
    return name.empty() ? STATUS_INVALID_PARAMETER : STATUS_SUCCESS;
  }

  static NTSTATUS resolve_property_object(windows_emulator &win_emu,
                                          const uint32_t object_type,
                                          std::u16string name,
                                          object_property_context &result) {
    result.name = std::move(name);
    if (object_type == 1) {
      if (result.name.front() == u'\\' ||
          result.name.find(u'\\') == std::u16string::npos) {
        return STATUS_OBJECT_NAME_INVALID;
      }
      result.key_path =
          uR"(\Registry\Machine\System\CurrentControlSet\Enum\)" + result.name;
      result.properties_path = result.key_path + uR"(\Properties)";
    } else if (object_type == 2 || object_type == 3) {
      if (!parse_guid(result.name)) {
        return STATUS_OBJECT_NAME_INVALID;
      }
      result.key_path =
          uR"(\Registry\Machine\System\CurrentControlSet\Control\)";
      result.key_path += object_type == 2 ? u"Class\\" : u"DeviceClasses\\";
      result.key_path += result.name;
      result.properties_path = result.key_path + uR"(\Properties)";
    } else if (object_type == 4) {
      if (!result.name.starts_with(u"\\\\?\\")) {
        return STATUS_OBJECT_NAME_INVALID;
      }
      const auto reference_separator = result.name.find(u'\\', 4);
      const auto symbolic_name =
          result.name.substr(4, reference_separator == std::u16string::npos
                                    ? std::u16string::npos
                                    : reference_separator - 4);
      const auto guid_separator = symbolic_name.rfind(u'#');
      if (guid_separator == std::u16string::npos) {
        return STATUS_OBJECT_NAME_INVALID;
      }
      const auto interface_guid =
          parse_guid(symbolic_name.substr(guid_separator + 1));
      if (!interface_guid) {
        return STATUS_OBJECT_NAME_INVALID;
      }
      result.interface_class = *interface_guid;
      result.reference = u"#";
      if (reference_separator != std::u16string::npos) {
        const auto reference = result.name.substr(reference_separator + 1);
        if (reference.empty() ||
            reference.find(u'\\') != std::u16string::npos) {
          return STATUS_OBJECT_NAME_INVALID;
        }
        result.reference += reference;
      }
      result.key_path =
          uR"(\Registry\Machine\System\CurrentControlSet\Control\DeviceClasses\)";
      result.key_path += guid_name(*interface_guid);
      result.key_path += u"\\##?#";
      result.key_path += symbolic_name;
      result.properties_path =
          result.key_path + u'\\' + result.reference + uR"(\Properties)";
    } else {
      return STATUS_INVALID_PARAMETER;
    }
    win_emu.callbacks.on_generic_access("Registry key", result.key_path);
    return win_emu.registry.get_key({result.key_path})
               ? STATUS_SUCCESS
               : STATUS_OBJECT_NAME_NOT_FOUND;
  }

  static std::optional<property_value>
  query_named_property(windows_emulator &win_emu,
                       const object_property_context &object,
                       const property_descriptor &descriptor) {
    const auto key = win_emu.registry.get_key({object.key_path});
    if (!key) {
      return std::nullopt;
    }
    const auto value =
        win_emu.registry.get_value(*key, descriptor.registry_name);
    return value ? convert_registry_property(*value, descriptor.type)
                 : std::nullopt;
  }

  static std::optional<property_value>
  query_dynamic_property(windows_emulator &win_emu, const uint32_t object_type,
                         const object_property_context &object,
                         const property_key &key) {
    if (object_type == 1 && same_guid(key.format, device_instance_format) &&
        key.id == 256) {
      return string_property(object.name);
    }
    if (object_type == 1 && same_guid(key.format, device_legacy_format) &&
        key.id == 24) {
      const auto separator = object.name.find(u'\\');
      if (separator != std::u16string::npos) {
        return string_property(object.name.substr(0, separator));
      }
    }
    if (object_type != 4) {
      return std::nullopt;
    }
    if (same_guid(key.format, interface_format) && key.id == 3) {
      const auto control = win_emu.registry.get_key(
          {object.key_path + u'\\' + object.reference + uR"(\Control)"});
      const auto linked = control
                              ? win_emu.registry.get_value(*control, "Linked")
                              : std::nullopt;
      const uint8_t enabled =
          linked && linked->as_dword().value_or(0) ? 0xFF : 0;
      return binary_property(devprop_type_boolean, &enabled, sizeof(enabled));
    }
    if (same_guid(key.format, interface_format) && key.id == 4) {
      return binary_property(devprop_type_guid, &object.interface_class,
                             sizeof(object.interface_class));
    }
    if (same_guid(key.format, interface_format) && key.id == 5) {
      return string_property(
          object.reference.size() > 1
              ? std::u16string_view(object.reference).substr(1)
              : u"");
    }
    return std::nullopt;
  }

  static std::optional<property_value>
  query_object_property(windows_emulator &win_emu, const uint32_t object_type,
                        const object_property_context &object,
                        const property_key &key) {
    if (auto dynamic =
            query_dynamic_property(win_emu, object_type, object, key)) {
      return dynamic;
    }
    std::optional<property_descriptor> descriptor;
    if (object_type == 1) {
      descriptor = describe_device_property(key);
    } else if (object_type == 2) {
      descriptor = describe_class_property(key);
    } else if (object_type == 4) {
      descriptor = describe_interface_property(key);
    }
    if (descriptor && descriptor->named) {
      if (auto value = query_named_property(win_emu, object, *descriptor)) {
        return value;
      }
    }
    return read_property_store(win_emu, object.properties_path, key,
                               descriptor ? descriptor->type
                                          : devprop_type_empty);
  }

  NTSTATUS get_object_property(windows_emulator &win_emu,
                               const io_device_context &c) const {
    object_property_request request{};
    auto status = this->read_object_property_request(win_emu, c, request);
    if (status != STATUS_SUCCESS) {
      return status;
    }
    if (!c.output_buffer || c.output_buffer_length < 20 ||
        request.result_size != 20) {
      return STATUS_INVALID_PARAMETER;
    }
    if (c.output_buffer % 4) {
      return STATUS_DATATYPE_MISALIGNMENT;
    }
    std::u16string name;
    status = read_object_name(win_emu, request, name);
    object_property_context object{};
    if (status == STATUS_SUCCESS) {
      status = resolve_property_object(win_emu, request.object_type,
                                       std::move(name), object);
    }
    std::optional<property_value> value;
    if (status == STATUS_SUCCESS) {
      value = query_object_property(win_emu, request.object_type, object,
                                    request.key);
      if (!value) {
        status = STATUS_OBJECT_NAME_NOT_FOUND;
      }
    }
    uint32_t required{};
    uint32_t type{};
    if (value) {
      if (value->data.size() > UINT32_MAX) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        value.reset();
      } else {
        required = static_cast<uint32_t>(value->data.size());
        type = value->type;
        if (c.output_buffer_length - 20 < required) {
          status = STATUS_BUFFER_TOO_SMALL;
        }
      }
    }
    const auto copied = status == STATUS_SUCCESS ? required : 0u;
    std::vector<std::byte> result(std::max(20u, 16u + copied));
    const std::array<uint32_t, 4> header{20, static_cast<uint32_t>(status),
                                         required, type};
    memcpy(result.data(), header.data(), sizeof(header));
    if (copied) {
      memcpy(result.data() + 16, value->data.data(), copied);
    }
    if (!win_emu.memory.try_write_memory(c.output_buffer, result.data(),
                                         result.size())) {
      return STATUS_ACCESS_VIOLATION;
    }
    if (c.io_status_block) {
      c.io_status_block.access(
          [&](auto &block) { block.Information = 20u + copied; });
    }
    win_emu.log.info("CMApi property type %u %s %s\\%04X: %u bytes, "
                     "DEVPROPTYPE 0x%X, status 0x%08X\n",
                     request.object_type, u16_to_u8(object.name).c_str(),
                     u16_to_u8(guid_name(request.key.format)).c_str(),
                     request.key.id, required, type,
                     static_cast<uint32_t>(status));
    return STATUS_SUCCESS;
  }

  NTSTATUS read_device_node_request(windows_emulator &win_emu,
                                    const io_device_context &c,
                                    device_node_request &request) const {
    const auto required = this->is_32_bit ? 28u : 40u;
    if (!c.input_buffer || c.input_buffer_length < required) {
      return STATUS_INVALID_PARAMETER;
    }
    if (c.input_buffer % (this->is_32_bit ? 4 : 8)) {
      return STATUS_DATATYPE_MISALIGNMENT;
    }
    if (this->is_32_bit) {
      std::array<uint32_t, 7> input{};
      if (!win_emu.memory.try_read_memory(c.input_buffer, input.data(),
                                          sizeof(input))) {
        return STATUS_ACCESS_VIOLATION;
      }
      if (input[0] != required) {
        return STATUS_INVALID_PARAMETER;
      }
      request = {.size = 40,
                 .operation = input[1],
                 .object_type = input[2],
                 .reserved = 0,
                 .device_id = input[3],
                 .device_id_bytes = input[4],
                 .flags = input[5],
                 .result_size = input[6],
                 .padding = 0};
    } else if (!win_emu.memory.try_read_memory(c.input_buffer, &request,
                                               sizeof(request))) {
      return STATUS_ACCESS_VIOLATION;
    }
    if (request.size != 40 || (request.operation != 1 && request.operation != 2) ||
        request.object_type != 1 || request.reserved || request.flags ||
        request.result_size != sizeof(status_result) || request.padding) {
      return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
  }

  static NTSTATUS read_device_id(windows_emulator &win_emu,
                                 const device_node_request &request,
                                 std::u16string &device_id) {
    if (!request.device_id || request.device_id_bytes < 2 ||
        request.device_id_bytes > 402 ||
        request.device_id_bytes % sizeof(char16_t)) {
      return STATUS_INVALID_PARAMETER;
    }
    if (request.device_id % alignof(char16_t)) {
      return STATUS_DATATYPE_MISALIGNMENT;
    }
    device_id.resize(request.device_id_bytes / sizeof(char16_t));
    if (!win_emu.memory.try_read_memory(request.device_id, device_id.data(),
                                        request.device_id_bytes)) {
      return STATUS_ACCESS_VIOLATION;
    }
    const auto terminator = device_id.find(u'\0');
    if (terminator == std::u16string::npos ||
        terminator + 1 != device_id.size()) {
      return STATUS_INVALID_PARAMETER;
    }
    device_id.resize(terminator);
    return is_legal_device_id(device_id) ? STATUS_SUCCESS
                                         : STATUS_OBJECT_NAME_INVALID;
  }

  static NTSTATUS validate_device_node(windows_emulator &win_emu,
                                       const device_node_request &request,
                                       const std::u16string_view device_id) {
    const auto path =
        uR"(\Registry\Machine\System\CurrentControlSet\Enum\)" +
        std::u16string(device_id);
    win_emu.callbacks.on_generic_access("Registry key", path);
    const auto key = win_emu.registry.get_key({path});
    if (!key) {
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    const auto phantom = win_emu.registry.get_value(*key, "Phantom");
    if (phantom && phantom->as_dword().value_or(0)) {
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    if (request.operation == 1) {
      // The volatile Control key is Sogen's registry-backed projection of a
      // live PnP devnode. A saved Enum entry without it remains phantom.
      win_emu.callbacks.on_generic_access("Registry key", path + uR"(\Control)");
      if (!win_emu.registry.get_key({path + uR"(\Control)"})) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
      }
    }
    return STATUS_SUCCESS;
  }

  NTSTATUS locate_device_node(windows_emulator &win_emu,
                              const io_device_context &c) const {
    device_node_request request{};
    auto status = this->read_device_node_request(win_emu, c, request);
    if (status != STATUS_SUCCESS) {
      return status;
    }
    if (!c.output_buffer || c.output_buffer_length < sizeof(status_result)) {
      return STATUS_INVALID_PARAMETER;
    }
    if (c.output_buffer % alignof(uint32_t)) {
      return STATUS_DATATYPE_MISALIGNMENT;
    }

    std::u16string device_id;
    status = read_device_id(win_emu, request, device_id);
    if (status == STATUS_SUCCESS) {
      status = validate_device_node(win_emu, request, device_id);
    }

    status_result result{};
    result.status = status;
    if (!win_emu.memory.try_write_memory(c.output_buffer, &result,
                                         sizeof(result))) {
      return STATUS_ACCESS_VIOLATION;
    }
    if (c.io_status_block) {
      c.io_status_block.access(
          [](auto &block) { block.Information = sizeof(status_result); });
    }
    win_emu.log.info("CMApi locate devnode %s (%s): status 0x%08X\n",
                     u16_to_u8(device_id).c_str(),
                     request.operation == 1 ? "normal" : "phantom",
                     static_cast<uint32_t>(status));
    return STATUS_SUCCESS;
  }

  NTSTATUS read_request(windows_emulator &win_emu, const io_device_context &c,
                        registry_request &request) const {
    const auto required = this->is_32_bit ? 36u : 48u;
    if (c.input_buffer == 0 || c.input_buffer_length < required) {
      return STATUS_INVALID_PARAMETER;
    }
    if (c.input_buffer % (this->is_32_bit ? 4 : 8) != 0) {
      return STATUS_DATATYPE_MISALIGNMENT;
    }
    if (this->is_32_bit) {
      std::array<uint32_t, 9> input{};
      if (!win_emu.memory.try_read_memory(c.input_buffer, input.data(),
                                          sizeof(input))) {
        return STATUS_ACCESS_VIOLATION;
      }
      if (input[0] != required) {
        return STATUS_INVALID_PARAMETER;
      }
      request = {.size = 48,
                 .operation = input[1],
                 .key_type = input[2],
                 .padding = 0,
                 .name = input[3],
                 .name_bytes = input[4],
                 .desired_access = input[5],
                 .disposition = input[6],
                 .flags = input[7],
                 .result_size = input[8],
                 .reserved = 0};
    } else if (!win_emu.memory.try_read_memory(c.input_buffer, &request,
                                               sizeof(request))) {
      return STATUS_ACCESS_VIOLATION;
    }
    return request.size == 48 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
  }

  static NTSTATUS open_class_key(windows_emulator &win_emu,
                                 const registry_request &request,
                                 uint64_t &key_handle) {
    if (request.operation != 0 || request.flags != 0 ||
        (request.key_type != 2 && request.key_type != 3)) {
      return STATUS_INVALID_PARAMETER;
    }
    std::u16string path =
        uR"(\Registry\Machine\System\CurrentControlSet\Control\)";
    path += request.key_type == 3 ? u"DeviceClasses" : u"Class";
    if (request.name) {
      if (request.name_bytes != 78 || request.name % 2 != 0) {
        return STATUS_INVALID_PARAMETER;
      }
      std::array<char16_t, 39> name{};
      if (!win_emu.memory.try_read_memory(request.name, name.data(),
                                          sizeof(name))) {
        return STATUS_ACCESS_VIOLATION;
      }
      const std::u16string_view guid{name.data(), 38};
      if (!is_class_guid(guid)) {
        return STATUS_INVALID_PARAMETER;
      }
      path += u'\\';
      path += guid;
    } else if (request.name_bytes != 0) {
      return STATUS_INVALID_PARAMETER;
    }

    win_emu.callbacks.on_generic_access("Registry key", path);
    auto key = win_emu.registry.get_key({path});
    if (!key && request.name && request.disposition == 1) {
      key = win_emu.registry.create_key({path});
    }
    if (!key) {
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    const auto opened = win_emu.process.registry_keys.store(std::move(*key));
    key_handle = opened.bits;
    // This open can repeat thousands of times during package registration. Keep the
    // detailed line available for a targeted diagnosis without formatting it on every call.
    static const bool log_class_key = [] {
      const char *value = std::getenv("SOGEN_LOG_CMAPI_CLASS_KEY");
      return value && *value && *value != '0';
    }();
    if (log_class_key) {
      win_emu.log.info("CMApi class key %s -> 0x%" PRIx64 " (access: 0x%X)\n",
                       u16_to_u8(path).c_str(), key_handle,
                       request.desired_access);
    }
    return STATUS_SUCCESS;
  }

  NTSTATUS read_interface_request(windows_emulator &win_emu,
                                  const io_device_context &c,
                                  interface_list_request &request) const {
    const auto required = this->is_32_bit ? 36u : 40u;
    if (!c.input_buffer || c.input_buffer_length < required) {
      return STATUS_INVALID_PARAMETER;
    }
    if (c.input_buffer % (this->is_32_bit ? 4 : 8) != 0) {
      return STATUS_DATATYPE_MISALIGNMENT;
    }
    if (this->is_32_bit) {
      std::array<uint32_t, 9> input{};
      if (!win_emu.memory.try_read_memory(c.input_buffer, input.data(),
                                          sizeof(input))) {
        return STATUS_ACCESS_VIOLATION;
      }
      if (input[0] != required) {
        return STATUS_INVALID_PARAMETER;
      }
      request.size = 40;
      request.flags = input[1];
      memcpy(&request.interface_class, input.data() + 2, sizeof(GUID));
      request.device_id = input[6];
      request.device_id_bytes = input[7];
      request.result_size = input[8];
    } else if (!win_emu.memory.try_read_memory(c.input_buffer, &request,
                                               sizeof(request))) {
      return STATUS_ACCESS_VIOLATION;
    }
    return request.size == 40 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
  }

  static std::u16string
  registered_interfaces(windows_emulator &win_emu,
                        const interface_list_request &request,
                        const std::u16string &device_id) {
    std::u16string list;
    // Enabled interfaces are PnP runtime objects, not the devices saved in an
    // offline SYSTEM hive. Sogen currently has no interface activation path;
    // only ALL_DEVICES includes saved registrations.
    if (request.flags == 0) {
      const auto path =
          uR"(\Registry\Machine\System\CurrentControlSet\Control\DeviceClasses\)" +
          guid_name(request.interface_class);
      const auto interface_class = win_emu.registry.get_key({path});
      if (interface_class) {
        const auto count = win_emu.registry.get_sub_key_count(*interface_class);
        for (size_t index = 0; index < count; ++index) {
          const auto name =
              win_emu.registry.get_sub_key_name(*interface_class, index);
          if (!name || !name->starts_with("##?#")) {
            continue;
          }
          const auto instance_name = u8_to_u16(*name);
          auto instance_path = path;
          instance_path += u'\\';
          instance_path += instance_name;
          const auto instance = win_emu.registry.get_key({instance_path});
          if (!instance) {
            continue;
          }
          if (!device_id.empty()) {
            const auto value =
                win_emu.registry.get_value(*instance, "DeviceInstance");
            const auto id = value ? value->as_string() : std::nullopt;
            if (!id || utils::string::to_lower(*id) !=
                           utils::string::to_lower(device_id)) {
              continue;
            }
          }
          const auto references = win_emu.registry.get_sub_key_count(*instance);
          for (size_t reference = 0; reference < references; ++reference) {
            const auto ref =
                win_emu.registry.get_sub_key_name(*instance, reference);
            if (!ref || !ref->starts_with('#')) {
              continue;
            }
            auto symbolic_link = u"\\\\?\\" + instance_name.substr(4);
            if (ref->size() > 1) {
              symbolic_link += u'\\';
              symbolic_link += u8_to_u16(ref->substr(1));
            }
            list += symbolic_link;
            list += u'\0';
          }
        }
      }
    }
    list += u'\0';
    return list;
  }

  NTSTATUS get_interface_list(windows_emulator &win_emu,
                              const io_device_context &c) const {
    const bool profile = win_emu.cmapi_interface_profile.enabled();
    const auto started = profile ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
    std::optional<uint32_t> profiled_flags;
    std::array<uint8_t, 16> profiled_guid{};
    const auto publish_profile = utils::finally([&] {
      if (!profile) return;
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started);
      win_emu.cmapi_interface_profile.record(profiled_flags, profiled_guid,
                                              static_cast<uint64_t>(elapsed.count()));
    });
    interface_list_request request{};
    auto status = this->read_interface_request(win_emu, c, request);
    if (status != STATUS_SUCCESS) {
      return status;
    }
    if (profile) {
      profiled_flags = request.flags;
      std::memcpy(profiled_guid.data(), &request.interface_class,
                  profiled_guid.size());
    }
    std::u16string device_id;
    if (request.device_id) {
      if (request.device_id_bytes < 2 || request.device_id_bytes > 0xfffe ||
          request.device_id_bytes % 2) {
        return STATUS_INVALID_PARAMETER;
      }
      if (request.device_id % 2) {
        return STATUS_DATATYPE_MISALIGNMENT;
      }
      device_id.resize(request.device_id_bytes / 2);
      if (!win_emu.memory.try_read_memory(request.device_id, device_id.data(),
                                          request.device_id_bytes)) {
        return STATUS_ACCESS_VIOLATION;
      }
      device_id.back() = u'\0';
      device_id.resize(device_id.find(u'\0'));
    } else if (request.device_id_bytes) {
      return STATUS_INVALID_PARAMETER;
    }
    if (!c.output_buffer || c.output_buffer_length < 20 ||
        request.result_size != 20) {
      return STATUS_INVALID_PARAMETER;
    }
    if (c.output_buffer % 4) {
      return STATUS_DATATYPE_MISALIGNMENT;
    }
    std::u16string list;
    if (request.flags & ~0x10000u) {
      status = STATUS_INVALID_PARAMETER;
    } else {
      list = registered_interfaces(win_emu, request, device_id);
    }
    const auto bytes = list.size() * sizeof(char16_t);
    if (bytes > UINT32_MAX - 20) {
      status = STATUS_INSUFFICIENT_RESOURCES;
      list.clear();
    }
    const auto required = static_cast<uint32_t>(list.size() * sizeof(char16_t));
    if (status == STATUS_SUCCESS && c.output_buffer_length - 20 < required) {
      status = STATUS_BUFFER_TOO_SMALL;
    }
    const auto copied = status == STATUS_SUCCESS ? required : 0u;
    std::vector<std::byte> result(std::max(20u, 16u + copied));
    const std::array<uint32_t, 4> header{20, static_cast<uint32_t>(status),
                                         required, 0};
    memcpy(result.data(), header.data(), sizeof(header));
    if (copied) {
      // PiCMReturnBufferResultData starts payload at 16 but includes 20 header
      // bytes in Information.
      memcpy(result.data() + 16, list.data(), copied);
    }
    if (!win_emu.memory.try_write_memory(c.output_buffer, result.data(),
                                         result.size())) {
      return STATUS_ACCESS_VIOLATION;
    }
    if (c.io_status_block) {
      c.io_status_block.access(
          [&](auto &block) { block.Information = 20u + copied; });
    }
    // The interface-list query fires thousands of times during device enumeration (each guest
    // re-scan walks every class), so this per-query line floods the console and costs log I/O on
    // the hot path. Suppress it by default; opt back in with SOGEN_LOG_CMAPI_INTERFACE_LIST=1
    // (the panel's "Log CMApi interface list" checkbox). Every other CMApi log is unaffected.
    static const bool log_interface_list = [] {
      const char *value = std::getenv("SOGEN_LOG_CMAPI_INTERFACE_LIST");
      return value && *value && *value != '0';
    }();
    if (log_interface_list) {
      win_emu.log.info("CMApi interface list %s (%s): %u bytes, status 0x%08X\n",
                       u16_to_u8(guid_name(request.interface_class)).c_str(),
                       request.flags == 0 ? "all registered" : "active", required,
                       static_cast<uint32_t>(status));
    }
    return STATUS_SUCCESS;
  }

  NTSTATUS io_control(windows_emulator &win_emu,
                      const io_device_context &c) override {
    if (c.io_control_code == 0x470807) {
      return get_interface_list(win_emu, c);
    }
    if (c.io_control_code == 0x470813) {
      return get_object_property(win_emu, c);
    }
    if (c.io_control_code == 0x470843) {
      return locate_device_node(win_emu, c);
    }
    if (c.io_control_code != 0x470863) {
      win_emu.log.warn("Unsupported CMApi ioctl: 0x%X\n", c.io_control_code);
      return STATUS_NOT_SUPPORTED;
    }
    registry_request request{};
    const auto status = this->read_request(win_emu, c, request);
    if (status != STATUS_SUCCESS) {
      return status;
    }
    if (c.output_buffer == 0 ||
        c.output_buffer_length < sizeof(handle_result) ||
        request.result_size != sizeof(handle_result)) {
      return STATUS_INVALID_PARAMETER;
    }
    if (c.output_buffer % 4 != 0) {
      return STATUS_DATATYPE_MISALIGNMENT;
    }

    // PiCMReturnHandleResultData returns transport success with the operation's
    // NTSTATUS in the output.
    handle_result result{};
    result.status = open_class_key(win_emu, request, result.key);
    if (!win_emu.memory.try_write_memory(c.output_buffer, &result,
                                         sizeof(result))) {
      if (result.key) {
        win_emu.process.registry_keys.erase(result.key);
      }
      return STATUS_ACCESS_VIOLATION;
    }
    if (c.io_status_block) {
      c.io_status_block.access(
          [](auto &block) { block.Information = sizeof(handle_result); });
    }
    return STATUS_SUCCESS;
  }
};
} // namespace

std::unique_ptr<io_device>
create_cm_api(const device_creation_context &context) {
  return std::make_unique<cm_api>(context.is_32_bit);
}
} // namespace sogen
