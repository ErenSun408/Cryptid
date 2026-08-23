#include "usb_vault/ui/webview_password_array.h"

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
    data_ = static_cast<std::byte*>(raw_data);
  }

  ~SafeArrayAccess() noexcept {
    static_cast<void>(::SafeArrayUnaccessData(&array_));
  }

  SafeArrayAccess(const SafeArrayAccess&) = delete;
  SafeArrayAccess& operator=(const SafeArrayAccess&) = delete;

  [[nodiscard]] std::byte* data() const noexcept { return data_; }

 private:
  SAFEARRAY& array_;
  std::byte* data_{nullptr};
};

[[nodiscard]] std::size_t array_size(SAFEARRAY& array) {
  if (::SafeArrayGetDim(&array) != 1) {
    throw std::invalid_argument(
        "WebView2 password must be a one-dimensional byte array");
  }

  VARTYPE element_type = VT_EMPTY;
  const HRESULT type_result = ::SafeArrayGetVartype(&array, &element_type);
  if (FAILED(type_result)) {
    throw std::system_error(type_result, std::system_category(),
                            "inspect WebView2 password array type");
  }
  if (element_type != VT_UI1) {
    throw std::invalid_argument(
        "WebView2 password array must contain unsigned bytes");
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
    return 0;
  }
  return static_cast<std::size_t>(static_cast<long long>(upper_bound) -
                                  static_cast<long long>(lower_bound) + 1LL);
}

}  // namespace

void submit_webview_password_array(VARIANT& password_bytes,
                                   UiSessionController& controller) {
  if (password_bytes.vt != (VT_ARRAY | VT_UI1) ||
      password_bytes.parray == nullptr) {
    throw std::invalid_argument(
        "WebView2 password must use a byte SAFEARRAY");
  }

  SAFEARRAY& array = *password_bytes.parray;
  const std::size_t size = array_size(array);
  SafeArrayAccess access(array);
  controller.submit_password(std::span<std::byte>(access.data(), size));
}

}  // namespace usb_vault::ui
