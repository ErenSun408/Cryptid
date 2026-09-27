#include "usb_vault/ui/card_configuration_webview_host_object.h"
#include "page_event_test_support.h"

#include <Windows.h>
#include <OleAuto.h>
#include <wrl.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using usb_vault::ui::ConfigurationCredentialStatus;
using usb_vault::ui::ConfigurationOutcome;
using usb_vault::ui::create_card_configuration_webview_host_object;
using usb_vault::ui::IConfigurationHost;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value) {
  std::vector<std::byte> result;
  result.reserve(value.size());
  for (const char character : value) {
    result.push_back(static_cast<std::byte>(character));
  }
  return result;
}

[[nodiscard]] std::string text(const std::span<const std::byte> value) {
  std::string result;
  result.reserve(value.size());
  for (const std::byte item : value) {
    result.push_back(static_cast<char>(item));
  }
  return result;
}

class FakeConfigurationHost final : public IConfigurationHost {
 public:
  ConfigurationCredentialStatus status{ConfigurationCredentialStatus::missing};
  ConfigurationOutcome outcome{ConfigurationOutcome::saved};
  bool allow_continue{true};
  unsigned int save_calls{};
  unsigned int continue_calls{};
  std::string saved_user_sn;
  std::string saved_api_key;
  std::string saved_user_pin;

  ConfigurationCredentialStatus credential_status() noexcept override {
    return status;
  }

  ConfigurationOutcome save_credentials(
      const std::span<const std::byte> user_sn,
      const std::span<const std::byte> api_key,
      const std::span<const std::byte> user_pin) noexcept override {
    ++save_calls;
    try {
      saved_user_sn = text(user_sn);
      saved_api_key = text(api_key);
      saved_user_pin = text(user_pin);
    } catch (...) {
      return ConfigurationOutcome::storage_failure;
    }
    return outcome;
  }

  bool continue_to_session() noexcept override {
    ++continue_calls;
    return allow_continue;
  }
};

void initialize_byte_array(VARIANTARG& value,
                           const std::span<const std::byte> input) {
  ::VariantInit(&value);
  SAFEARRAY* const array =
      ::SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(input.size()));
  if (array == nullptr) throw std::runtime_error("SAFEARRAY allocation failed");
  value.vt = VT_ARRAY | VT_UI1;
  value.parray = array;

  void* raw = nullptr;
  require(SUCCEEDED(::SafeArrayAccessData(array, &raw)),
          "SAFEARRAY access failed");
  auto* const data = static_cast<std::byte*>(raw);
  for (std::size_t index = 0; index < input.size(); ++index) {
    data[index] = input[index];
  }
  require(SUCCEEDED(::SafeArrayUnaccessData(array)),
          "SAFEARRAY release failed");
}

void require_array_wiped(const VARIANTARG& value, const std::size_t size,
                         const char* const message) {
  void* raw = nullptr;
  require(SUCCEEDED(::SafeArrayAccessData(value.parray, &raw)),
          "SAFEARRAY inspection failed");
  const auto* const data = static_cast<const std::byte*>(raw);
  for (std::size_t index = 0; index < size; ++index) {
    require(data[index] == std::byte{0}, message);
  }
  require(SUCCEEDED(::SafeArrayUnaccessData(value.parray)),
          "SAFEARRAY inspection release failed");
}

// The three values the page sends. COM stores positional arguments in reverse
// order, so the array below is deliberately built back to front.
class SaveArguments final {
 public:
  SaveArguments(const std::string_view user_sn, const std::string_view api_key,
                const std::string_view user_pin) {
    initialize_byte_array(values_[2], bytes(user_sn));
    initialize_byte_array(values_[1], bytes(api_key));
    initialize_byte_array(values_[0], bytes(user_pin));
    sizes_ = {user_pin.size(), api_key.size(), user_sn.size()};
  }

  ~SaveArguments() {
    for (VARIANTARG& value : values_) {
      static_cast<void>(::VariantClear(&value));
    }
  }

  SaveArguments(const SaveArguments&) = delete;
  SaveArguments& operator=(const SaveArguments&) = delete;

  [[nodiscard]] DISPPARAMS parameters() noexcept {
    return DISPPARAMS{values_.data(), nullptr, 3, 0};
  }

  void replace_with_bstr(const std::size_t index) {
    static_cast<void>(::VariantClear(&values_[index]));
    values_[index].vt = VT_BSTR;
    values_[index].bstrVal = ::SysAllocString(L"0123456789abcdef");
    require(values_[index].bstrVal != nullptr, "test BSTR allocation failed");
    arrays_[index] = false;
  }

  void require_all_wiped(const char* const message) {
    for (std::size_t index = 0; index < values_.size(); ++index) {
      if (!arrays_[index]) continue;
      require_array_wiped(values_[index], sizes_[index], message);
    }
  }

  void require_text_wiped(const char* const message) {
    for (const VARIANTARG& value : values_) {
      if (value.vt != VT_BSTR || value.bstrVal == nullptr) continue;
      const UINT length = ::SysStringLen(value.bstrVal);
      for (UINT index = 0; index < length; ++index) {
        require(value.bstrVal[index] == L'\0', message);
      }
    }
  }

 private:
  std::array<VARIANTARG, 3> values_{};
  std::array<std::size_t, 3> sizes_{};
  std::array<bool, 3> arrays_{true, true, true};
};

[[nodiscard]] DISPID member_id(IDispatch& object, const wchar_t* const name) {
  auto* mutable_name = const_cast<wchar_t*>(name);
  DISPID id = DISPID_UNKNOWN;
  require(SUCCEEDED(object.GetIDsOfNames(IID_NULL, &mutable_name, 1,
                                         LOCALE_USER_DEFAULT, &id)),
          "the setup bridge did not resolve a documented member");
  return id;
}

[[nodiscard]] HRESULT call(IDispatch& object, const wchar_t* const name,
                           DISPPARAMS& parameters, VARIANT* const result) {
  return object.Invoke(member_id(object, name), IID_NULL, LOCALE_USER_DEFAULT,
                       DISPATCH_METHOD, &parameters, result, nullptr, nullptr);
}

[[nodiscard]] ComPtr<IDispatch> make_bridge(FakeConfigurationHost& host) {
  ComPtr<IDispatch> object;
  require(SUCCEEDED(create_card_configuration_webview_host_object(
              host, object.GetAddressOf())),
          "the setup bridge could not be created");
  return object;
}

const std::string kUserSn(32, '1');
const std::string kApiKey(128, 'a');
const std::string kUserPin = "11111111";

void test_startup_view_names_setup() {
  FakeConfigurationHost host;
  auto object = make_bridge(host);
  DISPPARAMS parameters{};
  VARIANT result{};
  require(SUCCEEDED(call(*object.Get(), L"getStartupView", parameters, &result)),
          "the setup bridge did not report its startup view");
  require(result.vt == VT_BSTR, "the startup view was not a string");
  require(std::wstring_view(result.bstrVal) == L"configuration",
          "the setup bridge reported a view other than configuration");
  static_cast<void>(::VariantClear(&result));
}

void test_credential_status_is_reported_verbatim() {
  FakeConfigurationHost host;
  auto object = make_bridge(host);
  for (const auto status : {ConfigurationCredentialStatus::ready,
                            ConfigurationCredentialStatus::missing,
                            ConfigurationCredentialStatus::unreadable}) {
    host.status = status;
    require(page_event_tests::send(*object.Get()) == S_OK, "credential state blocked page reporting");
    DISPPARAMS parameters{};
    VARIANT result{};
    require(SUCCEEDED(call(*object.Get(), L"getCredentialStatus", parameters, &result)),
            "the setup bridge did not report the credential status");
    require(result.vt == VT_UI4 &&
                result.ulVal == static_cast<ULONG>(
                                    static_cast<unsigned int>(status)),
            "the setup bridge changed the credential status it was given");
  }
}

void test_save_passes_the_set_in_order_and_wipes_it() {
  FakeConfigurationHost host;
  auto object = make_bridge(host);
  SaveArguments arguments(kUserSn, kApiKey, kUserPin);
  DISPPARAMS parameters = arguments.parameters();
  VARIANT result{};
  require(SUCCEEDED(call(*object.Get(), L"saveCredentialBytes", parameters, &result)),
          "a well-formed credential set was rejected");
  require(result.vt == VT_UI4 &&
              result.ulVal == static_cast<ULONG>(
                                  static_cast<unsigned int>(
                                      ConfigurationOutcome::saved)),
          "the setup bridge did not return the host outcome");
  require(host.save_calls == 1, "the host was not asked to save exactly once");
  require(host.saved_user_sn == kUserSn && host.saved_api_key == kApiKey &&
              host.saved_user_pin == kUserPin,
          "the setup bridge reordered or altered the credential set");
  arguments.require_all_wiped(
      "the setup bridge left a saved credential in the page array");
}

void test_rejected_save_is_reported_without_a_failure_code() {
  FakeConfigurationHost host;
  host.outcome = ConfigurationOutcome::invalid_format;
  auto object = make_bridge(host);
  SaveArguments arguments(kUserSn, kApiKey, "short");
  DISPPARAMS parameters = arguments.parameters();
  VARIANT result{};
  require(SUCCEEDED(call(*object.Get(), L"saveCredentialBytes", parameters, &result)),
          "a rejected credential set did not reach the page as an outcome");
  require(result.vt == VT_UI4 &&
              result.ulVal == static_cast<ULONG>(
                                  static_cast<unsigned int>(
                                      ConfigurationOutcome::invalid_format)),
          "the setup bridge hid the rejection reason");
  arguments.require_all_wiped(
      "the setup bridge left a rejected credential in the page array");
}

void test_unsupported_value_still_wipes_the_other_two() {
  FakeConfigurationHost host;
  auto object = make_bridge(host);
  SaveArguments arguments(kUserSn, kApiKey, kUserPin);
  // The API key is the middle positional argument, so this fails after the
  // first value has already been read.
  arguments.replace_with_bstr(1);
  DISPPARAMS parameters = arguments.parameters();
  VARIANT result{};
  require(call(*object.Get(), L"saveCredentialBytes", parameters, &result) ==
              DISP_E_TYPEMISMATCH,
          "a non-array credential value was accepted");
  require(host.save_calls == 0, "a rejected set reached the host");
  arguments.require_all_wiped(
      "a failed save left another credential in the page array");
}

void test_wrong_argument_count_wipes_what_arrived() {
  FakeConfigurationHost host;
  auto object = make_bridge(host);
  SaveArguments arguments(kUserSn, kApiKey, kUserPin);
  DISPPARAMS parameters = arguments.parameters();
  parameters.cArgs = 2;
  VARIANT result{};
  require(call(*object.Get(), L"saveCredentialBytes", parameters, &result) ==
              DISP_E_BADPARAMCOUNT,
          "an incomplete credential set was accepted");
  require(host.save_calls == 0, "an incomplete set reached the host");
  require_array_wiped(parameters.rgvarg[0], kUserPin.size(),
                      "an incomplete call left a credential in the page array");
  require_array_wiped(parameters.rgvarg[1], kApiKey.size(),
                      "an incomplete call left a credential in the page array");
}

void test_a_string_argument_is_cleared_before_the_caller_frees_it() {
  FakeConfigurationHost host;
  auto object = make_bridge(host);
  SaveArguments arguments(kUserSn, kApiKey, kUserPin);
  arguments.replace_with_bstr(0);
  DISPPARAMS parameters = arguments.parameters();
  VARIANT result{};
  require(call(*object.Get(), L"saveCredentialBytes", parameters, &result) ==
              DISP_E_TYPEMISMATCH,
          "a credential sent as a string was accepted");
  require(host.save_calls == 0, "a string credential reached the host");
  arguments.require_text_wiped(
      "a credential sent as a string was left readable for the caller to free");
}

void test_a_query_clears_anything_attached_to_it() {
  FakeConfigurationHost host;
  auto object = make_bridge(host);
  VARIANTARG argument{};
  initialize_byte_array(argument, bytes(kUserPin));
  DISPPARAMS parameters{&argument, nullptr, 1, 0};
  VARIANT result{};
  require(call(*object.Get(), L"getCredentialStatus", parameters, &result) ==
              DISP_E_BADPARAMCOUNT,
          "a query with an argument was accepted");
  require_array_wiped(argument, kUserPin.size(),
                      "a rejected query left a credential in the page array");
  static_cast<void>(::VariantClear(&argument));
}

void test_a_set_is_not_stored_when_the_page_cannot_be_told() {
  FakeConfigurationHost host;
  auto object = make_bridge(host);
  SaveArguments arguments(kUserSn, kApiKey, kUserPin);
  DISPPARAMS parameters = arguments.parameters();
  require(call(*object.Get(), L"saveCredentialBytes", parameters, nullptr) ==
              E_POINTER,
          "a save with nowhere to report reported something else");
  require(host.save_calls == 0,
          "a credential set was stored although the outcome could not be told");
  arguments.require_all_wiped(
      "an unreportable save left a credential in the page array");
}

void test_continue_needs_the_host_to_agree() {
  FakeConfigurationHost host;
  host.allow_continue = false;
  auto object = make_bridge(host);
  DISPPARAMS parameters{};
  VARIANT result{};
  require(call(*object.Get(), L"continueToSession", parameters, &result) ==
              E_ACCESSDENIED,
          "the setup bridge left setup although the host refused");
  require(host.continue_calls == 1, "the host was not consulted");

  // A refusal must not retire the page: the user has to be able to fix the
  // credentials and try again.
  host.allow_continue = true;
  DISPPARAMS retry{};
  VARIANT retry_result{};
  require(SUCCEEDED(call(*object.Get(), L"continueToSession", retry, &retry_result)),
          "a refused continue permanently disabled setup");
  require(retry_result.vt == VT_BOOL && retry_result.boolVal == VARIANT_TRUE,
          "an accepted continue did not report success");
}

void test_retired_page_cannot_store_credentials() {
  FakeConfigurationHost host;
  auto object = make_bridge(host);
  DISPPARAMS parameters{};
  VARIANT result{};
  require(SUCCEEDED(call(*object.Get(), L"continueToSession", parameters, &result)),
          "an accepted continue was reported as a failure");

  SaveArguments arguments(kUserSn, kApiKey, kUserPin);
  DISPPARAMS save_parameters = arguments.parameters();
  VARIANT save_result{};
  require(call(*object.Get(), L"saveCredentialBytes", save_parameters,
               &save_result) == E_PENDING,
          "a retired setup page stored a credential set");
  require(host.save_calls == 0, "a retired setup page reached the host");
  arguments.require_all_wiped(
      "a retired setup page left a credential in the page array");

  DISPPARAMS status_parameters{};
  VARIANT status_result{};
  require(call(*object.Get(), L"getCredentialStatus", status_parameters,
               &status_result) == E_PENDING,
          "a retired setup page kept answering queries");
  require(page_event_tests::send(*object.Get()) == S_OK, "retired configuration page blocked reporting");
}

void test_product_members_are_absent() {
  FakeConfigurationHost host;
  auto object = make_bridge(host);
  page_event_tests::verify_contract(*object.Get());
  for (const wchar_t* const name :
       {L"submitLoginPasswordBytes", L"getSessionState", L"beginMountBatch",
        L"createContainerMib", L"unmount", L"openConfiguration",
        L"requestClose"}) {
    auto* mutable_name = const_cast<wchar_t*>(name);
    DISPID id = DISPID_UNKNOWN;
    require(object->GetIDsOfNames(IID_NULL, &mutable_name, 1,
                                  LOCALE_USER_DEFAULT, &id) ==
                DISP_E_UNKNOWNNAME,
            "the setup bridge exposed a product member");
    require(id == DISPID_UNKNOWN, "an unknown member returned an id");
  }
}

}  // namespace

int main() {
  try {
    test_startup_view_names_setup();
    test_credential_status_is_reported_verbatim();
    test_save_passes_the_set_in_order_and_wipes_it();
    test_rejected_save_is_reported_without_a_failure_code();
    test_unsupported_value_still_wipes_the_other_two();
    test_wrong_argument_count_wipes_what_arrived();
    test_a_string_argument_is_cleared_before_the_caller_frees_it();
    test_a_query_clears_anything_attached_to_it();
    test_a_set_is_not_stored_when_the_page_cannot_be_told();
    test_continue_needs_the_host_to_agree();
    test_retired_page_cannot_store_credentials();
    test_product_members_are_absent();
    std::cout << "card_configuration_webview_host_object_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_configuration_webview_host_object_tests failed: "
              << error.what() << "\n";
    return 1;
  }
}
