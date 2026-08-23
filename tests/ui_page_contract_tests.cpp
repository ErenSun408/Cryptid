#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef USB_VAULT_UI_PAGE
#error USB_VAULT_UI_PAGE must name the UI contract page
#endif

#ifndef USB_VAULT_UI_SCRIPT
#error USB_VAULT_UI_SCRIPT must name the UI controller script
#endif

namespace {

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
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
  require(input.is_open(), "could not open a packaged UI source");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void test_single_password_surface_is_bridge_controlled() {
  const std::string page = read_source(USB_VAULT_UI_PAGE);

  require(count_occurrences(page, "<input ") == 1,
          "UI must contain exactly one input");
  require(page.find("type=\"password\"") != std::string::npos,
          "the sole input must be a password control");
  require(page.find("autocomplete=\"off\"") != std::string::npos,
          "password autocomplete must be disabled");
  require(page.find("spellcheck=\"false\"") != std::string::npos,
          "password spellcheck must be disabled");
  require(page.find("id=\"session-password\"") != std::string::npos,
          "password input must use the bridge contract id");
  require(page.find("id=\"session-action\" type=\"button\" disabled") !=
              std::string::npos,
          "action must start inert before native bridge readiness");
  require(count_occurrences(page, "<script") == 1 &&
              page.find("<script src=\"session.js\"></script>") !=
                  std::string::npos,
          "page must load only the packaged session controller");
}

void test_page_does_not_expose_disallowed_state() {
  const std::string page = read_source(USB_VAULT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_UI_SCRIPT);
  const std::string combined = page + script;
  constexpr std::string_view disallowed[] = {
      "username", "user-name", "mode", "volume-type", "hidden-volume",
      "localStorage", "sessionStorage", "JSON.stringify"};
  for (const std::string_view marker : disallowed) {
    require(combined.find(marker) == std::string::npos,
            "UI exposed a disallowed state marker");
  }

  require(page.find("default-src 'none'") != std::string::npos,
          "page CSP must deny resources by default");
  require(page.find("form-action 'none'") != std::string::npos,
          "page CSP must deny form submission");
  require(page.find("script-src 'self'") != std::string::npos,
          "page CSP must limit script to packaged resources");
  require(page.find("script-src 'self' 'unsafe-inline'") == std::string::npos,
          "product page must not allow inline script");
  require(script.find("console.") == std::string::npos,
          "UI controller must not log runtime state");
}

void test_password_is_cleared_at_bridge_settlement() {
  const std::string script = read_source(USB_VAULT_UI_SCRIPT);
  const std::size_t encode = script.find(
      "passwordBytes = new TextEncoder().encode(passwordInput.value)");
  const std::size_t clear_dom =
      script.find("passwordInput.value = \"\";", encode);
  const std::size_t submit = script.find(
      "await bridge.submitPasswordBytes(passwordBytes);", clear_dom);
  const std::size_t wipe = script.find("passwordBytes.fill(0);", submit);

  require(encode != std::string::npos && clear_dom != std::string::npos &&
              submit != std::string::npos && wipe != std::string::npos,
          "password submission clearing contract is incomplete");
  require(encode < clear_dom && clear_dom < submit && submit < wipe,
          "password copies are not cleared at their safe lifetime boundaries");
  require(count_occurrences(script, "passwordBytes.fill(0);") >= 2,
          "password byte array must have a fallback wipe");
  require(script.find("shouldPassTypedArraysAsArrays = true") !=
              std::string::npos,
          "typed-array projection must be explicitly enabled");
}

void test_session_actions_are_serialized_and_recoverable() {
  const std::string script = read_source(USB_VAULT_UI_SCRIPT);
  require(script.find("if (busy)") != std::string::npos &&
              script.find("actionButton.disabled = busy") !=
                  std::string::npos,
          "UI actions must reject asynchronous reentry");
  require(script.find("bridge.hasActiveSession()") != std::string::npos,
          "UI must recover unified active-session state");
  require(script.find("await bridge.unmount()") != std::string::npos,
          "UI must expose active-session unmounting");
}

}  // namespace

int main() {
  test_single_password_surface_is_bridge_controlled();
  test_page_does_not_expose_disallowed_state();
  test_password_is_cleared_at_bridge_settlement();
  test_session_actions_are_serialized_and_recoverable();

  std::cout << "ui_page_contract_tests: PASS\n";
  return 0;
}
