#pragma once

#include <cstdint>

namespace usb_vault::card::vikey::sdk {

using Status = std::uint32_t;
using Index = std::uint16_t;
using Address = std::uint32_t;
using Length = std::uint32_t;
using Byte = std::uint8_t;
using ModuleIndex = std::uint16_t;
using ModuleValue = std::uint16_t;

constexpr Status kSuccess = 0;
constexpr Status kNoDevice = 0x80000001U;
constexpr Status kInvalidPassword = 0x80000002U;
constexpr Status kNeedFind = 0x80000003U;
constexpr Status kInvalidIndex = 0x80000004U;
constexpr Status kInvalidValue = 0x80000005U;
constexpr Status kNoChance = 0x80000009U;
constexpr Status kNoAuthority = 0x8000000AU;
constexpr Status kInvalidAddressOrSize = 0x8000000BU;
constexpr Status kConnection = 0x8000000EU;
constexpr Status kNoSupport = 0x80000016U;
constexpr Status kGetUsbData = 0x80000018U;
constexpr Status kSetUsbData = 0x80000019U;
constexpr Status kCommunication = 0x8000001DU;
constexpr Status kApiKey = 0x80000021U;
constexpr Status kUserSn = 0x80000023U;
constexpr Status kNotActive = 0x80000024U;
constexpr Status kUnsetApiKey = 0x80000025U;
constexpr Status kCheckApiKey = 0x80000028U;

#if defined(_MSC_VER)
#define USB_VAULT_VIKEY_CALL __stdcall
#else
#define USB_VAULT_VIKEY_CALL
#endif

using SetUserSn = Status(USB_VAULT_VIKEY_CALL*)(char* user_sn);
using FindEx = Status(USB_VAULT_VIKEY_CALL*)(std::uint32_t* count);
using SetApiKey = Status(USB_VAULT_VIKEY_CALL*)(Index index, char* api_key);
using Uninitialization = Status(USB_VAULT_VIKEY_CALL*)();
using GetHid =
    Status(USB_VAULT_VIKEY_CALL*)(Index index, std::uint32_t* hardware_id);
using GetLevel = Status(USB_VAULT_VIKEY_CALL*)(Index index, Byte* level);
using GetUserDataSize =
    Status(USB_VAULT_VIKEY_CALL*)(Index index, Length* bytes);
using GetUserPasswordAttempt = Status(USB_VAULT_VIKEY_CALL*)(
    Index index, Byte* current_attempt, Byte* maximum_attempt);
using UserLogin =
    Status(USB_VAULT_VIKEY_CALL*)(Index index, char* user_password);
using Logoff = Status(USB_VAULT_VIKEY_CALL*)(Index index);
using ReadData = Status(USB_VAULT_VIKEY_CALL*)(
    Index index, Address address, Length length, Byte* buffer);
using WriteData = Status(USB_VAULT_VIKEY_CALL*)(
    Index index, Address address, Length length, Byte* buffer);
using GetModule = Status(USB_VAULT_VIKEY_CALL*)(
    Index index, ModuleIndex module, ModuleValue* value);
using DecreaseModule =
    Status(USB_VAULT_VIKEY_CALL*)(Index index, ModuleIndex module);
using CheckModule = Status(USB_VAULT_VIKEY_CALL*)(
    Index index, ModuleIndex module, ModuleValue* is_zero,
    ModuleValue* can_decrease);

struct Functions final {
  SetUserSn set_user_sn{};
  FindEx find_ex{};
  SetApiKey set_api_key{};
  Uninitialization uninitialization{};
  GetHid get_hid{};
  GetLevel get_level{};
  GetUserDataSize get_user_data_size{};
  GetUserPasswordAttempt get_user_password_attempt{};
  UserLogin user_login{};
  Logoff logoff{};
  ReadData read_data{};
  WriteData write_data{};
  GetModule get_module{};
  DecreaseModule decrease_module{};
  CheckModule check_module{};
};

#undef USB_VAULT_VIKEY_CALL

static_assert(sizeof(Status) == 4);
static_assert(sizeof(Index) == 2);
static_assert(sizeof(Address) == 4);
static_assert(sizeof(Length) == 4);
static_assert(sizeof(Byte) == 1);
static_assert(sizeof(ModuleValue) == 2);

}  // namespace usb_vault::card::vikey::sdk
