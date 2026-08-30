#include "usb_vault/ui/webview_password_array.h"

#include "usb_vault/security/password_normalization.h"
#include "usb_vault/security/secure_buffer.h"

#include <Windows.h>

#include <cstddef>
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

void submit_byte_array(SAFEARRAY& array, const std::size_t size,
                       UiSessionController& controller) {
  if (array.cbElements != sizeof(std::byte)) {
    throw std::invalid_argument(
        "WebView2 byte array has an invalid element width");
  }
  SafeArrayAccess access(array);
  auto* const bytes = static_cast<std::byte*>(access.data());
  if (size != 0 && bytes == nullptr) {
    throw std::invalid_argument("WebView2 password array has no data");
  }
  controller.submit_password(std::span<std::byte>(bytes, size));
}

void submit_variant_byte_array(SAFEARRAY& array, const std::size_t size,
                               UiSessionController& controller) {
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
  security::SecureBuffer normalized(size);
  for (std::size_t index = 0; index < size; ++index) {
    normalized.bytes()[index] =
        static_cast<std::byte>(static_cast<unsigned char>(access[index].lVal));
  }
  access.wipe();
  controller.submit_password(normalized.bytes());
}

}  // namespace

void submit_webview_password_array(VARIANT& password_bytes,
                                   UiSessionController& controller) {
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

  if (expected_element_type == static_cast<VARTYPE>(VT_UI1)) {
    submit_byte_array(array, info.size, controller);
    return;
  }
  submit_variant_byte_array(array, info.size, controller);
}

}  // namespace usb_vault::ui
