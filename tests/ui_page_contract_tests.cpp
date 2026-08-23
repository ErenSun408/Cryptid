#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef USB_VAULT_UI_PAGE
#error USB_VAULT_UI_PAGE must name the UI contract page
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

[[nodiscard]] std::string page_source() {
  std::ifstream input(USB_VAULT_UI_PAGE, std::ios::binary);
  require(input.is_open(), "could not open the packaged UI source");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void test_single_password_surface_is_inert_until_bridge_ready() {
  const std::string page = page_source();

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
          "action must remain inert before native bridge readiness");
  require(page.find("<script") == std::string::npos,
          "inert UI slice must not execute script");
}

void test_page_does_not_expose_disallowed_state() {
  const std::string page = page_source();
  constexpr std::string_view disallowed[] = {
      "username", "user-name", "mode", "volume-type", "hidden-volume",
      "localStorage", "sessionStorage"};
  for (const std::string_view marker : disallowed) {
    require(page.find(marker) == std::string::npos,
            "UI exposed a disallowed state marker");
  }

  require(page.find("default-src 'none'") != std::string::npos,
          "page CSP must deny resources by default");
  require(page.find("form-action 'none'") != std::string::npos,
          "page CSP must deny form submission");
}

}  // namespace

int main() {
  test_single_password_surface_is_inert_until_bridge_ready();
  test_page_does_not_expose_disallowed_state();

  std::cout << "ui_page_contract_tests: PASS\n";
  return 0;
}
