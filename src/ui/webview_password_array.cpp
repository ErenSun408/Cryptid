#include "usb_vault/ui/webview_password_array.h"

#include "usb_vault/security/password_normalization.h"
#include "usb_vault/security/secure_buffer.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>

namespace usb_vault::ui {
namespace {

class SafeArrayAccess final {
 public:
  explicit SafeArrayAccess(SAFEARRAY& array) : array_(array) {
    void* raw_data = nullptr;
    const HRESULT result = ::SafeArrayAccessData(&array_, &raw_data);
    if (FAILED(result)) {
      throw std::system_error(result, std::system_category(),
                              "access WebView2 password byte array");
    }
    data_ = raw_data;
  }

  ~SafeArrayAccess() noexcept {
    static_cast<void>(::SafeArrayUnaccessData(&array_));
  }

  SafeArrayAccess(const SafeArrayAccess&) = delete;
  SafeArrayAccess& operator=(const SafeArrayAccess&) = delete;

  [[nodiscard]] void* data() const noexcept { return data_; }

 private:
  SAFEARRAY& array_;
  void* data_{nullptr};
};

class PasswordVariantWiper final {
 public:
  explicit PasswordVariantWiper(VARIANT& value) noexcept : value_(value) {}
  ~PasswordVariantWiper() noexcept { wipe_webview_password_array(value_); }

  PasswordVariantWiper(const PasswordVariantWiper&) = delete;
  PasswordVariantWiper& operator=(const PasswordVariantWiper&) = delete;

 private:
  VARIANT& value_;
};

class VariantPasswordAccess final {
 public:
  VariantPasswordAccess(SAFEARRAY& array, const std::size_t size)
      : access_(array),
        elements_(static_cast<VARIANT*>(access_.data())),
        size_(size) {
    if (size_ != 0 && elements_ == nullptr) {
      throw std::invalid_argument("WebView2 password array has no data");
    }
  }

  ~VariantPasswordAccess() noexcept { wipe(); }

  VariantPasswordAccess(const VariantPasswordAccess&) = delete;
  VariantPasswordAccess& operator=(const VariantPasswordAccess&) = delete;

  [[nodiscard]] VARIANT& operator[](const std::size_t index) noexcept {
    return elements_[index];
  }

  void wipe() noexcept {
    if (wiped_) {
      return;
    }
    for (std::size_t index = 0; index < size_; ++index) {
      if (elements_[index].vt == VT_I4) {
        static_cast<void>(
            ::SecureZeroMemory(&elements_[index].lVal, sizeof(LONG)));
      }
    }
    wiped_ = true;
  }

 private:
  SafeArrayAccess access_;
  VARIANT* elements_{nullptr};
  std::size_t size_{0};
  bool wiped_{false};
};

struct ArrayInfo final {
  std::size_t size;
  VARTYPE element_type;
};

[[nodiscard]] ArrayInfo inspect_array(SAFEARRAY& array) {
  if (::SafeArrayGetDim(&array) != 1) {
    throw std::invalid_argument(
        "WebView2 password must be a one-dimensional array");
  }

  VARTYPE element_type = VT_EMPTY;
  const HRESULT type_result = ::SafeArrayGetVartype(&array, &element_type);
  if (FAILED(type_result)) {
    throw std::system_error(type_result, std::system_category(),
                            "inspect WebView2 password array type");
  }
  LONG lower_bound = 0;
  LONG upper_bound = 0;
  const HRESULT lower_result = ::SafeArrayGetLBound(&array, 1, &lower_bound);
  const HRESULT upper_result = ::SafeArrayGetUBound(&array, 1, &upper_bound);
  if (FAILED(lower_result) || FAILED(upper_result)) {
    const HRESULT result = FAILED(lower_result) ? lower_result : upper_result;
    throw std::system_error(result, std::system_category(),
                            "inspect WebView2 password array bounds");
  }
  if (upper_bound < lower_bound) {
    return {0, element_type};
  }
  return {static_cast<std::size_t>(static_cast<long long>(upper_bound) -
                                   static_cast<long long>(lower_bound) + 1LL),
          element_type};
}

[[nodiscard]] security::SecureBuffer consume_byte_array(
    SAFEARRAY& array, const std::size_t size) {
  if (array.cbElements != sizeof(std::byte)) {
    throw std::invalid_argument(
        "WebView2 byte array has an invalid element width");
  }
  SafeArrayAccess access(array);
  auto* const bytes = static_cast<std::byte*>(access.data());
  if (size != 0 && bytes == nullptr) {
    throw std::invalid_argument("WebView2 password array has no data");
  }
  security::SecureBuffer result(size);
  std::ranges::copy(std::span<const std::byte>(bytes, size),
                    result.bytes().begin());
  return result;
}

[[nodiscard]] security::SecureBuffer consume_variant_byte_array(
    SAFEARRAY& array, const std::size_t size) {
  if (array.cbElements != sizeof(VARIANT)) {
    throw std::invalid_argument(
        "WebView2 variant array has an invalid element width");
  }

  VariantPasswordAccess access(array, size);
  if (size > security::kMaximumPasswordUtf8InputBytes) {
    throw std::length_error("WebView2 password array is too long");
  }
  for (std::size_t index = 0; index < size; ++index) {
    const VARIANT& element = access[index];
    if (element.vt != VT_I4 || element.lVal < 0 || element.lVal > 255) {
      throw std::invalid_argument(
          "WebView2 variant password array must contain byte-range integers");
    }
  }
  security::SecureBuffer result(size);
  for (std::size_t index = 0; index < size; ++index) {
    result.bytes()[index] =
        static_cast<std::byte>(static_cast<unsigned char>(access[index].lVal));
  }
  access.wipe();
  return result;
}

[[nodiscard]] std::size_t array_element_count(SAFEARRAY& array) noexcept {
  const UINT dimensions = ::SafeArrayGetDim(&array);
  if (dimensions == 0) return 0;

  std::size_t result = 1;
  for (UINT index = 0; index < dimensions; ++index) {
    const std::size_t count = array.rgsabound[index].cElements;
    if (count != 0 &&
        result > std::numeric_limits<std::size_t>::max() / count) {
      return 0;
    }
    result *= count;
  }
  return result;
}

}  // namespace

void wipe_dispatch_text_arguments(DISPPARAMS* const parameters) noexcept {
  if (parameters == nullptr || parameters->rgvarg == nullptr) return;
  for (UINT index = 0; index < parameters->cArgs; ++index) {
    VARIANT& value = parameters->rgvarg[index];
    if (value.vt != VT_BSTR || value.bstrVal == nullptr) continue;
    const UINT length = ::SysStringLen(value.bstrVal);
    if (length == 0) continue;
    static_cast<void>(
        ::SecureZeroMemory(value.bstrVal, length * sizeof(wchar_t)));
  }
}

void wipe_webview_password_array(VARIANT& password_bytes) noexcept {
  if (password_bytes.parray == nullptr ||
      (password_bytes.vt != (VT_ARRAY | VT_UI1) &&
       password_bytes.vt != (VT_ARRAY | VT_VARIANT))) {
    return;
  }

  SAFEARRAY& array = *password_bytes.parray;
  const std::size_t count = array_element_count(array);
  if (count == 0) return;

  void* raw_data = nullptr;
  if (FAILED(::SafeArrayAccessData(&array, &raw_data)) || raw_data == nullptr) {
    return;
  }
  if (password_bytes.vt == (VT_ARRAY | VT_UI1) &&
      array.cbElements == sizeof(std::byte)) {
    static_cast<void>(::SecureZeroMemory(raw_data, count));
  } else if (password_bytes.vt == (VT_ARRAY | VT_VARIANT) &&
             array.cbElements == sizeof(VARIANT)) {
    auto* const elements = static_cast<VARIANT*>(raw_data);
    for (std::size_t index = 0; index < count; ++index) {
      if (elements[index].vt == VT_I4) {
        static_cast<void>(
            ::SecureZeroMemory(&elements[index].lVal, sizeof(LONG)));
      }
    }
  }
  static_cast<void>(::SafeArrayUnaccessData(&array));
}

security::SecureBuffer consume_webview_password_array(
    VARIANT& password_bytes) {
  PasswordVariantWiper source_wiper(password_bytes);
  if (password_bytes.parray == nullptr ||
      (password_bytes.vt != (VT_ARRAY | VT_UI1) &&
       password_bytes.vt != (VT_ARRAY | VT_VARIANT))) {
    throw std::invalid_argument(
        "WebView2 password must use a supported SAFEARRAY");
  }

  SAFEARRAY& array = *password_bytes.parray;
  const ArrayInfo info = inspect_array(array);
  const VARTYPE expected_element_type =
      password_bytes.vt == (VT_ARRAY | VT_UI1)
          ? static_cast<VARTYPE>(VT_UI1)
          : static_cast<VARTYPE>(VT_VARIANT);
  if (info.element_type != expected_element_type) {
    throw std::invalid_argument(
        "WebView2 password SAFEARRAY type does not match its VARIANT");
  }
  if (info.size > security::kMaximumPasswordUtf8InputBytes) {
    throw std::length_error("WebView2 password array is too long");
  }

  if (expected_element_type == static_cast<VARTYPE>(VT_UI1)) {
    return consume_byte_array(array, info.size);
  }
  return consume_variant_byte_array(array, info.size);
}

}  // namespace usb_vault::ui
