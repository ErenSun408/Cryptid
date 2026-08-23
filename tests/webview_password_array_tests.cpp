#include "usb_vault/ui/webview_password_array.h"

#include <Windows.h>
#include <OleAuto.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace {

using usb_vault::application::IApplicationSessionActions;
using usb_vault::security::SecureBuffer;
using usb_vault::ui::UiSessionController;
using usb_vault::ui::submit_webview_password_array;

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class FakeSession final : public IApplicationSessionActions {
 public:
  void authenticate_and_mount(SecureBuffer password_utf8) override {
    ++submit_calls;
    require(password_utf8.size() == expected.size(),
            "SAFEARRAY password size changed");
    for (std::size_t index = 0; index < expected.size(); ++index) {
      require(password_utf8.bytes()[index] == expected[index],
              "SAFEARRAY password bytes changed");
    }
    password_utf8.clear();
    if (reject) {
      throw std::runtime_error("mount rejected");
    }
    is_mounted = true;
  }

  void unmount_active() override { is_mounted = false; }
  [[nodiscard]] bool mounted() const noexcept override { return is_mounted; }

  std::array<std::byte, 4> expected{
      std::byte{'p'}, std::byte{0x00}, std::byte{0xC3}, std::byte{0xA4}};
  bool reject{false};
  bool is_mounted{false};
  int submit_calls{0};
};

class ByteArrayVariant final {
 public:
  explicit ByteArrayVariant(const std::array<std::byte, 4>& bytes,
                            const LONG lower_bound = 0) {
    ::VariantInit(&value_);
    SAFEARRAY* const array = ::SafeArrayCreateVector(
        VT_UI1, lower_bound, static_cast<ULONG>(bytes.size()));
    if (array == nullptr) {
      throw std::runtime_error("could not allocate test SAFEARRAY");
    }
    value_.vt = VT_ARRAY | VT_UI1;
    value_.parray = array;

    void* raw_data = nullptr;
    require(SUCCEEDED(::SafeArrayAccessData(value_.parray, &raw_data)),
            "could not access test SAFEARRAY");
    auto* data = static_cast<std::byte*>(raw_data);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      data[index] = bytes[index];
    }
    require(SUCCEEDED(::SafeArrayUnaccessData(value_.parray)),
            "could not release test SAFEARRAY");
  }

  ~ByteArrayVariant() noexcept { static_cast<void>(::VariantClear(&value_)); }

  ByteArrayVariant(const ByteArrayVariant&) = delete;
  ByteArrayVariant& operator=(const ByteArrayVariant&) = delete;

  [[nodiscard]] VARIANT& get() noexcept { return value_; }

  void require_wiped() const {
    void* raw_data = nullptr;
    require(SUCCEEDED(::SafeArrayAccessData(value_.parray, &raw_data)),
            "could not inspect test SAFEARRAY");
    const auto* data = static_cast<const std::byte*>(raw_data);
    for (std::size_t index = 0; index < 4; ++index) {
      require(data[index] == std::byte{0}, "SAFEARRAY was not wiped");
    }
    require(SUCCEEDED(::SafeArrayUnaccessData(value_.parray)),
            "could not release inspected SAFEARRAY");
  }

 private:
  VARIANT value_{};
};

void test_array_is_forwarded_and_wiped() {
  FakeSession session;
  UiSessionController controller(session);
  ByteArrayVariant password(session.expected, 7);

  submit_webview_password_array(password.get(), controller);

  password.require_wiped();
  require(session.submit_calls == 1, "SAFEARRAY submit was not forwarded");
  require(controller.mounted(), "successful SAFEARRAY submit was not mounted");
}

void test_session_failure_still_wipes_array() {
  FakeSession session;
  session.reject = true;
  UiSessionController controller(session);
  ByteArrayVariant password(session.expected);

  try {
    submit_webview_password_array(password.get(), controller);
  } catch (const std::runtime_error&) {
    password.require_wiped();
    require(!controller.mounted(), "failed SAFEARRAY submit reported mounted");
    return;
  }
  throw std::runtime_error("session failure was not propagated");
}

void test_non_array_variant_is_rejected() {
  FakeSession session;
  UiSessionController controller(session);
  VARIANT invalid{};
  ::VariantInit(&invalid);
  invalid.vt = VT_I4;
  invalid.lVal = 42;

  try {
    submit_webview_password_array(invalid, controller);
  } catch (const std::invalid_argument&) {
    require(session.submit_calls == 0, "invalid variant reached the session");
    return;
  }
  throw std::runtime_error("non-array password variant was accepted");
}

}  // namespace

int main() {
  test_array_is_forwarded_and_wiped();
  test_session_failure_still_wipes_array();
  test_non_array_variant_is_rejected();

  std::cout << "webview_password_array_tests: PASS\n";
  return 0;
}
