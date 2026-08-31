#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef USB_VAULT_CARD_PRODUCT_UI_PAGE
#error USB_VAULT_CARD_PRODUCT_UI_PAGE must name the product UI page
#endif

#ifndef USB_VAULT_CARD_PRODUCT_UI_SCRIPT
#error USB_VAULT_CARD_PRODUCT_UI_SCRIPT must name the product UI script
#endif

namespace {

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] std::size_t count_occurrences(const std::string_view text,
                                            const std::string_view needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = text.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

[[nodiscard]] std::string read_source(const char* const path) {
  std::ifstream input(path, std::ios::binary);
  require(input.is_open(), "could not open a packaged product UI source");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string lower_ascii(std::string value) {
  std::ranges::transform(value, value.begin(), [](const unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return value;
}

void test_page_has_route_specific_password_surfaces() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  require(count_occurrences(page, "type=\"password\"") == 2,
          "product page must have exactly password and confirmation inputs");
  require(page.find("id=\"card-password\"") != std::string::npos &&
              page.find("id=\"card-password-confirmation\"") !=
                  std::string::npos,
          "product password controls do not use the fixed page contract");
  require(page.find("id=\"confirmation-field\" class=\"field\" hidden") !=
              std::string::npos,
          "confirmation must start hidden outside initialization");
  require(count_occurrences(page, "autocomplete=\"off\"") == 2 &&
              count_occurrences(page, "spellcheck=\"false\"") == 2,
          "both product password controls must disable browser assistance");
  require(page.find("<form") == std::string::npos,
          "product password inputs must not use browser form submission");
  require(page.find(
              "<script src=\"card_session.js?v=p2-15c-five-state\"></script>") !=
              std::string::npos &&
              count_occurrences(page, "<script") == 1,
          "product page must load only its packaged controller");
}

void test_five_states_are_complete_and_vendor_neutral() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script =
      read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  const std::string combined = lower_ascii(page + script);

  for (const std::string_view state : {
           "no-card", "initialization-required", "login-required",
           "authenticated", "unavailable"}) {
    require(combined.find(state) != std::string::npos,
            "product page omits a native UI state");
  }
  for (const std::string_view marker : {
           "hardwareid", "credentialrole", "passwordrole", "volumetype",
           "container-shape", "containerlayout", "rootsecret",
           "localstorage", "sessionstorage", "json.stringify",
           "submitpasswordbytes"}) {
    require(combined.find(marker) == std::string::npos,
            "product page exposed a forbidden or legacy state marker");
  }
  require(combined.find("忘记密码") == std::string::npos &&
              combined.find("遗忘后果") == std::string::npos,
          "deferred password-forgetting confirmation leaked into the page");
  require(page.find("default-src 'none'") != std::string::npos &&
              page.find("form-action 'none'") != std::string::npos &&
              page.find("script-src 'self'") != std::string::npos &&
              page.find("script-src 'self' 'unsafe-inline'") ==
                  std::string::npos,
          "product page CSP does not preserve the local-only boundary");
  require(script.find("console.") == std::string::npos,
          "product UI must not log runtime state or secrets");
}

void test_bridge_calls_are_narrow_and_route_specific() {
  const std::string script =
      read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(script.find("await bridge.getSessionState();") !=
              std::string::npos &&
              script.find("await bridge.hasActiveSession();") !=
                  std::string::npos,
          "product page does not query the complete neutral state");
  require(script.find("await bridge.submitLoginPasswordBytes(passwordBytes);") !=
              std::string::npos,
          "login page does not use the P2 single-input bridge");
  require(script.find(
              "await bridge.submitInitializationPasswordBytes(\n"
              "          passwordBytes, confirmationBytes);") !=
              std::string::npos,
          "initialization page does not use the independent P2 inputs");
  require(script.find("await bridge.unmount();") != std::string::npos,
          "product page lacks unified cleanup");
  require(script.find("bridge.submitPasswordBytes") == std::string::npos,
          "product page reused the incompatible P1 submission method");
}

void test_all_password_copies_have_fallback_wipes() {
  const std::string script =
      read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(count_occurrences(script, "passwordInput.value = \"\";") >= 2 &&
              script.find("confirmationInput.value = \"\";") !=
                  std::string::npos,
          "DOM password values are not cleared on both submission paths");
  require(count_occurrences(script, "wipe(encodedPasswordBytes);") >= 4 &&
              count_occurrences(script, "wipe(passwordBytes);") >= 4 &&
              count_occurrences(script, "wipe(encodedConfirmationBytes);") >=
                  2 &&
              count_occurrences(script, "wipe(confirmationBytes);") >= 2,
          "one or more JavaScript byte copies lack a fallback wipe");
  const std::size_t first_guard =
      script.find("passwordInput.value = \"\";",
                  script.find("const submitInitialization"));
  const std::size_t second_guard =
      script.find("confirmationInput.value = \"\";", first_guard);
  const std::size_t bridge_call =
      script.find("bridge.submitInitializationPasswordBytes", second_guard);
  require(first_guard != std::string::npos &&
              second_guard != std::string::npos &&
              bridge_call != std::string::npos &&
              first_guard < second_guard && second_guard < bridge_call,
          "initialization DOM inputs survive until the native call");
}

void test_strength_feedback_matches_the_native_length_baseline() {
  const std::string script =
      read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(script.find("passwordInput.value.normalize(\"NFC\")") !=
              std::string::npos,
          "strength feedback does not count the native NFC form");
  require(script.find("count < 12") != std::string::npos &&
              script.find("count <= 17") != std::string::npos &&
              script.find("count <= 128") != std::string::npos &&
              script.find("强度：可接受") != std::string::npos &&
              script.find("强度：较强") != std::string::npos,
          "strength feedback diverges from the 12/18/128 policy boundary");
}

void test_actions_are_serialized_and_external_changes_coalesce() {
  const std::string script =
      read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(script.find("if (busy)") != std::string::npos &&
              script.find("actionButton.disabled = busy") !=
                  std::string::npos,
          "product page does not serialize asynchronous actions");
  require(script.find(
              "window.addEventListener(\"usb-vault-session-changed\"") !=
              std::string::npos &&
              script.find("externalRefreshPending = true") !=
                  std::string::npos &&
              script.find("clearDomInputs();") != std::string::npos &&
              script.find("while (externalRefreshPending)") !=
                  std::string::npos,
          "native state changes are not cleared and coalesced");
  require(script.find("nextState !== nativeState.authenticated") !=
              std::string::npos &&
              script.find("? nativeState.unavailable") != std::string::npos,
          "a residual mount does not override a raced action route");
}

}  // namespace

int main() {
  test_page_has_route_specific_password_surfaces();
  test_five_states_are_complete_and_vendor_neutral();
  test_bridge_calls_are_narrow_and_route_specific();
  test_all_password_copies_have_fallback_wipes();
  test_strength_feedback_matches_the_native_length_baseline();
  test_actions_are_serialized_and_external_changes_coalesce();
  std::cout << "card_product_ui_page_contract_tests: PASS\n";
  return 0;
}
