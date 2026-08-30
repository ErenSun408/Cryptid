#include "usb_vault/ui/webview_password_array.h"

#include "usb_vault/security/password_normalization.h"

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
  [[nodiscard]] bool force_unmount_active() override {
    throw std::logic_error(
        "forced unmount is outside this contract");
  }
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
    size_ = bytes.size();
  }

  ByteArrayVariant(const std::size_t size, const std::byte value) {
    ::VariantInit(&value_);
    SAFEARRAY* const array = ::SafeArrayCreateVector(
        VT_UI1, 0, static_cast<ULONG>(size));
    if (array == nullptr) {
      throw std::runtime_error("could not allocate sized test SAFEARRAY");
    }
    value_.vt = VT_ARRAY | VT_UI1;
    value_.parray = array;
    size_ = size;

    void* raw_data = nullptr;
    require(SUCCEEDED(::SafeArrayAccessData(value_.parray, &raw_data)),
            "could not access sized test SAFEARRAY");
    auto* data = static_cast<std::byte*>(raw_data);
    for (std::size_t index = 0; index < size_; ++index) {
      data[index] = value;
    }
    require(SUCCEEDED(::SafeArrayUnaccessData(value_.parray)),
            "could not release sized test SAFEARRAY");
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
    for (std::size_t index = 0; index < size_; ++index) {
      require(data[index] == std::byte{0}, "SAFEARRAY was not wiped");
    }
    require(SUCCEEDED(::SafeArrayUnaccessData(value_.parray)),
            "could not release inspected SAFEARRAY");
  }

 private:
  VARIANT value_{};
  std::size_t size_{0};
};

class IntegerVariantArray final {
 public:
  explicit IntegerVariantArray(const std::array<std::byte, 4>& bytes,
                               const LONG lower_bound = 0) {
    ::VariantInit(&value_);
    SAFEARRAY* const array = ::SafeArrayCreateVector(
        VT_VARIANT, lower_bound, static_cast<ULONG>(bytes.size()));
    if (array == nullptr) {
      throw std::runtime_error("could not allocate variant test SAFEARRAY");
    }
    value_.vt = VT_ARRAY | VT_VARIANT;
    value_.parray = array;
    lower_bound_ = lower_bound;
    size_ = bytes.size();

    for (std::size_t index = 0; index < bytes.size(); ++index) {
      set_integer(index, static_cast<LONG>(
                             std::to_integer<unsigned char>(bytes[index])));
    }
  }

  IntegerVariantArray(const std::size_t size, const LONG value) {
    ::VariantInit(&value_);
    SAFEARRAY* const array = ::SafeArrayCreateVector(
        VT_VARIANT, 0, static_cast<ULONG>(size));
    if (array == nullptr) {
      throw std::runtime_error(
          "could not allocate sized variant test SAFEARRAY");
    }
    value_.vt = VT_ARRAY | VT_VARIANT;
    value_.parray = array;
    size_ = size;
    for (std::size_t index = 0; index < size_; ++index) {
      set_integer(index, value);
    }
  }

  ~IntegerVariantArray() noexcept {
    static_cast<void>(::VariantClear(&value_));
  }

  IntegerVariantArray(const IntegerVariantArray&) = delete;
  IntegerVariantArray& operator=(const IntegerVariantArray&) = delete;

  [[nodiscard]] VARIANT& get() noexcept { return value_; }

  void set_integer(const std::size_t index, const LONG value) {
    VARIANT element{};
    ::VariantInit(&element);
    element.vt = VT_I4;
    element.lVal = value;
    put(index, element);
  }

  void set_unsigned_byte(const std::size_t index, const BYTE value) {
    VARIANT element{};
    ::VariantInit(&element);
    element.vt = VT_UI1;
    element.bVal = value;
    put(index, element);
  }

  void require_i4_values_wiped() const {
    for (std::size_t index = 0; index < size_; ++index) {
      VARIANT element{};
      ::VariantInit(&element);
      LONG array_index = lower_bound_ + static_cast<LONG>(index);
      require(SUCCEEDED(
                  ::SafeArrayGetElement(value_.parray, &array_index, &element)),
              "could not inspect variant test SAFEARRAY");
      if (element.vt == VT_I4) {
        require(element.lVal == 0,
                "variant SAFEARRAY integer was not wiped");
      }
      static_cast<void>(::VariantClear(&element));
    }
  }

 private:
  void put(const std::size_t index, VARIANT& element) {
    LONG array_index = lower_bound_ + static_cast<LONG>(index);
    require(SUCCEEDED(
                ::SafeArrayPutElement(value_.parray, &array_index, &element)),
            "could not populate variant test SAFEARRAY");
  }

  VARIANT value_{};
  LONG lower_bound_{0};
  std::size_t size_{0};
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

void test_variant_integer_array_is_forwarded_and_wiped() {
  FakeSession session;
  UiSessionController controller(session);
  IntegerVariantArray password(session.expected, 11);

  submit_webview_password_array(password.get(), controller);

  password.require_i4_values_wiped();
  require(session.submit_calls == 1,
          "variant SAFEARRAY submit was not forwarded");
  require(controller.mounted(),
          "successful variant SAFEARRAY submit was not mounted");
}

void test_variant_session_failure_still_wipes_array() {
  FakeSession session;
  session.reject = true;
  UiSessionController controller(session);
  IntegerVariantArray password(session.expected);

  try {
    submit_webview_password_array(password.get(), controller);
  } catch (const std::runtime_error&) {
    password.require_i4_values_wiped();
    require(!controller.mounted(),
            "failed variant SAFEARRAY submit reported mounted");
    return;
  }
  throw std::runtime_error("variant SAFEARRAY session failure was not propagated");
}

void test_variant_element_type_is_rejected_and_i4_values_are_wiped() {
  FakeSession session;
  UiSessionController controller(session);
  IntegerVariantArray password(session.expected);
  password.set_unsigned_byte(2, 0xC3);

  try {
    submit_webview_password_array(password.get(), controller);
  } catch (const std::invalid_argument&) {
    password.require_i4_values_wiped();
    require(session.submit_calls == 0,
            "mixed variant SAFEARRAY reached the session");
    return;
  }
  throw std::runtime_error("mixed variant SAFEARRAY was accepted");
}

void test_variant_integer_range_is_enforced_and_wiped() {
  for (const LONG invalid_value : {-1L, 256L}) {
    FakeSession session;
    UiSessionController controller(session);
    IntegerVariantArray password(session.expected);
    password.set_integer(1, invalid_value);

    try {
      submit_webview_password_array(password.get(), controller);
    } catch (const std::invalid_argument&) {
      password.require_i4_values_wiped();
      require(session.submit_calls == 0,
              "out-of-range variant SAFEARRAY reached the session");
      continue;
    }
    throw std::runtime_error("out-of-range variant integer was accepted");
  }
}

void test_oversized_arrays_are_rejected_and_wiped() {
  constexpr std::size_t oversized =
      usb_vault::security::kMaximumPasswordUtf8InputBytes + 1;
  {
    FakeSession session;
    UiSessionController controller(session);
    ByteArrayVariant password(oversized, std::byte{'a'});
    bool rejected = false;
    try {
      submit_webview_password_array(password.get(), controller);
    } catch (const std::length_error&) {
      rejected = true;
      password.require_wiped();
      require(session.submit_calls == 0,
              "oversized byte SAFEARRAY reached the session");
    }
    require(rejected && session.submit_calls == 0,
            "oversized byte SAFEARRAY was accepted");
  }

  {
    FakeSession session;
    UiSessionController controller(session);
    IntegerVariantArray password(oversized, static_cast<LONG>('a'));
    bool rejected = false;
    try {
      submit_webview_password_array(password.get(), controller);
    } catch (const std::length_error&) {
      rejected = true;
      password.require_i4_values_wiped();
      require(session.submit_calls == 0,
              "oversized variant SAFEARRAY reached the session");
    }
    require(rejected && session.submit_calls == 0,
            "oversized variant SAFEARRAY was accepted");
  }
}

void test_direct_integer_safearray_is_rejected() {
  FakeSession session;
  UiSessionController controller(session);
  VARIANT invalid{};
  ::VariantInit(&invalid);
  invalid.vt = VT_ARRAY | VT_I4;
  invalid.parray = ::SafeArrayCreateVector(VT_I4, 0, 1);
  require(invalid.parray != nullptr,
          "could not allocate direct integer SAFEARRAY");

  bool rejected = false;
  try {
    submit_webview_password_array(invalid, controller);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  static_cast<void>(::VariantClear(&invalid));
  require(rejected, "direct integer SAFEARRAY was accepted");
  require(session.submit_calls == 0,
          "direct integer SAFEARRAY reached the session");
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
  test_variant_integer_array_is_forwarded_and_wiped();
  test_variant_session_failure_still_wipes_array();
  test_variant_element_type_is_rejected_and_i4_values_are_wiped();
  test_variant_integer_range_is_enforced_and_wiped();
  test_oversized_arrays_are_rejected_and_wiped();
  test_direct_integer_safearray_is_rejected();
  test_non_array_variant_is_rejected();

  std::cout << "webview_password_array_tests: PASS\n";
  return 0;
}
