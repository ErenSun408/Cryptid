#include "volume/partition_write_policy.h"
#include "volume/partition_catalog_detail.h"
#include <wbemidl.h>
#include <wrl/client.h>
#include <chrono>
#include <stdexcept>
#include <string>

namespace usb_vault::volume::detail {
namespace {
using Microsoft::WRL::ComPtr;
constexpr long kQueryTimeoutMs = 5000;
void require(bool ok) { if (!ok) throw std::runtime_error("partition policy unavailable"); }
class Apartment final {
 public:
  Apartment() : result_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
    require(SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE);
  }
  ~Apartment() { if (SUCCEEDED(result_)) ::CoUninitialize(); }
  Apartment(const Apartment&) = delete;
  Apartment& operator=(const Apartment&) = delete;
 private:
  HRESULT result_;
};
class Text final {
 public:
  explicit Text(const wchar_t* text) : value_(::SysAllocString(text)) { require(value_ != nullptr); }
  ~Text() { ::SysFreeString(value_); }
  Text(const Text&) = delete;
  Text& operator=(const Text&) = delete;
  BSTR get() const { return value_; }
 private:
  BSTR value_;
};
class Property final {
 public:
  Property(IWbemClassObject* object, const wchar_t* name) {
    ::VariantInit(&value_);
    const HRESULT hr = object->Get(name, 0, &value_, &type_, nullptr);
    if (FAILED(hr)) { ::VariantClear(&value_); require(false); }
  }
  ~Property() { ::VariantClear(&value_); }
  Property(const Property&) = delete;
  Property& operator=(const Property&) = delete;
  bool boolean() const {
    require(type_ == CIM_BOOLEAN && value_.vt == VT_BOOL &&
        (value_.boolVal == VARIANT_FALSE || value_.boolVal == VARIANT_TRUE));
    return value_.boolVal == VARIANT_TRUE;
  }
  std::uint32_t number() const {
    require(type_ == CIM_UINT32 && value_.vt == VT_I4);
    return static_cast<std::uint32_t>(value_.lVal);
  }
  std::wstring string() const {
    require(type_ == CIM_STRING && value_.vt == VT_BSTR && value_.bstrVal != nullptr);
    const UINT size = ::SysStringLen(value_.bstrVal);
    require(size > 0 && size <= 1024);
    std::wstring text(value_.bstrVal, size);
    require(text.find(L'\0') == std::wstring::npos);
    return text;
  }
 private:
  VARIANT value_{};
  CIMTYPE type_{};
};
void secure_proxy(IUnknown* proxy) {
  require(SUCCEEDED(::CoSetProxyBlanket(proxy, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
      nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE)));
}
ComPtr<IWbemServices> connect(IWbemLocator* locator, const wchar_t* name) {
  Text space(name);
  ComPtr<IWbemServices> service;
  require(SUCCEEDED(locator->ConnectServer(space.get(), nullptr, nullptr, nullptr,
      WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &service)) && service);
  secure_proxy(service.Get());
  return service;
}
// Fixed queries only. Device IDs are compared as data, never inserted into WQL.
ComPtr<IWbemClassObject> find_volume(IWbemServices* service, const wchar_t* query,
                                   const std::wstring& volume) {
  Text language(L"WQL"), text(query);
  ComPtr<IEnumWbemClassObject> rows;
  require(SUCCEEDED(service->ExecQuery(language.get(), text.get(),
      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &rows)) && rows);
  secure_proxy(rows.Get());
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(kQueryTimeoutMs);
  ComPtr<IWbemClassObject> match;
  for (unsigned i = 0; i <= 4096; ++i) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - std::chrono::steady_clock::now()).count();
    require(remaining > 0);
    ComPtr<IWbemClassObject> row;
    ULONG count{};
    const HRESULT hr = rows->Next(static_cast<long>(remaining), 1, &row, &count);
    if (hr == static_cast<HRESULT>(WBEM_S_FALSE) && count == 0) { require(match != nullptr); return match; }
    require(hr == S_OK && count == 1 && row && i < 4096);
    const auto id = Property(row.Get(), L"DeviceID").string();
    if (id.size() == volume.size() && ::CompareStringOrdinal(id.c_str(), static_cast<int>(id.size()),
        volume.c_str(), static_cast<int>(volume.size()), TRUE) == CSTR_EQUAL) {
      require(match == nullptr); // Ambiguous inventory fails closed.
      match = row;
    }
  }
  require(false);
  return {};
}
ComPtr<IWbemClassObject> call_volume_method(IWbemServices* service, IWbemClassObject* row, const wchar_t* method_name) {
  const auto relative = Property(row, L"__RELPATH").string();
  require(relative.starts_with(L"Win32_EncryptableVolume.DeviceID="));
  Text object(relative.c_str()), method(method_name);
  ComPtr<IWbemCallResult> pending;
  require(SUCCEEDED(service->ExecMethod(object.get(), method.get(), WBEM_FLAG_RETURN_IMMEDIATELY,
      nullptr, nullptr, nullptr, &pending)) && pending);
  secure_proxy(pending.Get());
  LONG status{};
  require(pending->GetCallStatus(kQueryTimeoutMs, &status) == WBEM_S_NO_ERROR && status == WBEM_S_NO_ERROR);
  ComPtr<IWbemClassObject> result;
  require(pending->GetResultObject(0, &result) == WBEM_S_NO_ERROR && result);
  require(Property(result.Get(), L"ReturnValue").number() == 0);
  return result;
}
PartitionWriteFacts read_facts(const std::wstring& volume, PartitionBitLockerStatus* bitlocker = nullptr, const std::function<bool()>& decrypt_allowed = {}) {
  Apartment apartment;
  ComPtr<IWbemLocator> locator;
  require(SUCCEEDED(::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(&locator))) && locator);
  auto service = connect(locator.Get(), L"ROOT\\CIMV2");
  auto row = find_volume(service.Get(),
      L"SELECT DeviceID, BootVolume, SystemVolume, PageFilePresent FROM Win32_Volume", volume);
  PartitionWriteFacts facts;
  facts.boot_volume = Property(row.Get(), L"BootVolume").boolean();
  facts.system_volume = Property(row.Get(), L"SystemVolume").boolean();
  facts.page_file_present = Property(row.Get(), L"PageFilePresent").boolean();
  if (*facts.boot_volume || *facts.system_volume || *facts.page_file_present) return facts;
  auto encryption = connect(locator.Get(), L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption");
  auto encrypted_row = find_volume(encryption.Get(), L"SELECT * FROM Win32_EncryptableVolume", volume);
  auto result = call_volume_method(encryption.Get(), encrypted_row.Get(), L"GetConversionStatus");
  facts.conversion_status = Property(result.Get(), L"ConversionStatus").number();
  if (bitlocker) {
    *bitlocker = {*facts.conversion_status, Property(result.Get(), L"EncryptionPercentage").number()};
    require(valid_bitlocker_status(*bitlocker));
  }
  if (decrypt_allowed && facts.conversion_status && *facts.conversion_status != 0 && *facts.conversion_status != 3) {
    require(decrypt_allowed());
    static_cast<void>(call_volume_method(encryption.Get(), encrypted_row.Get(), L"Decrypt"));
  }
  return facts;
}
}  // namespace

bool permits_partition_write(const PartitionWriteFacts& facts) noexcept {
  return facts.boot_volume.has_value() && !*facts.boot_volume &&
      facts.system_volume.has_value() && !*facts.system_volume &&
      facts.page_file_present.has_value() && !*facts.page_file_present &&
      facts.conversion_status.has_value() && *facts.conversion_status == 0;
}
PartitionBitLockerStatus inspect_partition_bitlocker(HANDLE handle, const PartitionCandidate& expected) {
  require(matches_partition_handle_identity(handle, expected) &&
      matches_partition_snapshot(expected, enumerate_partition_candidates()));
  PartitionBitLockerStatus status{6, 0};
  const auto facts = read_facts(expected.volume_path, &status);
  require(facts.boot_volume && !*facts.boot_volume && facts.system_volume && !*facts.system_volume &&
      facts.page_file_present && !*facts.page_file_present && valid_bitlocker_status(status));
  require(matches_partition_handle_identity(handle, expected) &&
      matches_partition_snapshot(expected, enumerate_partition_candidates()));
  return status;
}
void begin_partition_bitlocker_decryption(HANDLE handle, const PartitionCandidate& expected,
    const std::function<bool()>& allowed) {
  require(allowed && allowed());
  static_cast<void>(inspect_partition_bitlocker(handle, expected));
  PartitionBitLockerStatus status{6,0};
  const auto facts=read_facts(expected.volume_path, &status, [&] {
    return allowed() && matches_partition_handle_identity(handle, expected) &&
        matches_partition_snapshot(expected, enumerate_partition_candidates()) && allowed();
  });
  require(facts.conversion_status && valid_bitlocker_status(status));
}
bool check_partition_write_policy(HANDLE handle, const PartitionCandidate& expected) noexcept {
  try {
    // Do not query by an unverified path or allow a changed target during WMI.
    if (!matches_partition_handle_identity(handle, expected)) return false;
    const auto inventory = enumerate_partition_candidates();
    if (!matches_partition_snapshot(expected, inventory)) return false;
    if (!permits_partition_write(read_facts(expected.volume_path))) return false;
    return matches_partition_handle_identity(handle, expected) &&
        matches_partition_snapshot(expected, enumerate_partition_candidates());
  } catch (...) { return false; }
}
bool check_partition_resume_policy(HANDLE handle, const PartitionLocation& expected) noexcept {
  try {
    if (!matches_partition_location_handle(handle, expected)) return false;
    const auto before=resolve_partition_location(expected.identity,enumerate_partition_locations());
    if (!before || *before!=expected) return false;
    // A paused volume deliberately has no usable NTFS filesystem. WMI exposes
    // null filesystem-derived flags here, so the initial NTFS write gate cannot
    // establish a resume permit. Require RAW plus the structural exclusions;
    // the writer must ALSO authenticate the in-place backup header under the
    // current card key, require a durably paused record and take an exclusive
    // lock. This function alone is never authorization for arbitrary RAW writes.
    wchar_t filesystem[32]{};
    if (!::GetVolumeInformationByHandleW(handle,nullptr,0,nullptr,nullptr,nullptr,filesystem,32) ||
        ::CompareStringOrdinal(filesystem,-1,L"RAW",-1,TRUE)!=CSTR_EQUAL) return false;
    const auto after=resolve_partition_location(expected.identity,enumerate_partition_locations());
    return after && *after==expected && matches_partition_location_handle(handle,expected);
  } catch (...) { return false; }
}
}  // namespace usb_vault::volume::detail
