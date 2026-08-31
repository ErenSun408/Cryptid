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
              "<script src=\"card_session.js?v=p2-16d-container-a-status\"></script>") !=
              std::string::npos &&
              count_occurrences(page, "<script") == 1,
          "product page must load only its packaged controller");
  require(count_occurrences(page, "class=\"password-visibility\"") == 2 &&
              page.find("aria-controls=\"card-password\"") !=
                  std::string::npos &&
              page.find("aria-controls=\"card-password-confirmation\"") !=
                  std::string::npos &&
              page.find("input::-ms-reveal") != std::string::npos,
          "product page does not replace transient browser reveal controls");
  require(count_occurrences(page,
                            "class=\"password-strength-segment\"") == 3,
          "product page strength meter must contain exactly three segments");
}

void test_authenticated_dashboard_is_real_state_only() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  const std::string combined = lower_ascii(page + script);

  require(page.find("id=\"authenticated-dashboard\"") !=
              std::string::npos &&
              page.find("id=\"dashboard-section-dashboard\"") !=
                  std::string::npos &&
              page.find("id=\"dashboard-section-encrypted\"") !=
                  std::string::npos &&
              page.find("id=\"dashboard-section-hidden\"") !=
                  std::string::npos,
          "authenticated dashboard shell is incomplete");
  require(count_occurrences(page, "data-create-kind=\"encrypted\"") == 2 &&
              count_occurrences(page, "data-create-kind=\"hidden\"") == 2,
          "dashboard and management views lack both creation entries");
  require(count_occurrences(page, "aria-disabled=\"true\"") == 4 &&
              count_occurrences(page, "data-tooltip=\"正在开发中\"") == 4 &&
              page.find("文件管理") != std::string::npos &&
              page.find("备份中心") != std::string::npos &&
              page.find("操作历史") != std::string::npos &&
              page.find("系统设置") != std::string::npos,
          "unfinished navigation is not visibly present and unavailable");
  require(script.find(
              "const showDashboard = currentState === nativeState.authenticated;") !=
              std::string::npos &&
              script.find("surface.hidden = showDashboard;") !=
                  std::string::npos &&
              script.find("dashboard.hidden = !showDashboard;") !=
                  std::string::npos &&
              script.find("dashboardVolumeState.textContent = activeVolumes") !=
                  std::string::npos,
          "dashboard is not exclusively derived from native authenticated state");
  require(page.find("onclick=") == std::string::npos &&
              combined.find("admin") == std::string::npos &&
              combined.find("账号") == std::string::npos &&
              combined.find("用户名") == std::string::npos &&
              combined.find("encrypteddrives") == std::string::npos &&
              combined.find("backuptasks") == std::string::npos &&
              combined.find("historylogs") == std::string::npos,
          "prototype account, inline, or mock-data behavior reached the product page");
  for (const std::string_view emoji : {
           "🔐", "📊", "🔒", "👁️", "📁", "💾", "📋", "⚙️", "➕"}) {
    require(page.find(emoji) == std::string::npos,
            "product dashboard still contains an emoji icon");
  }
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
  require(script.find("await bridge.createContainerAMib(sizeMib);") !=
              std::string::npos,
          "authenticated A creation does not use the capacity-only bridge");
  require(script.find("await bridge.getContainerAState();") !=
                  std::string::npos &&
              script.find("await bridge.getContainerASizeMib();") !=
                  std::string::npos &&
              script.find("await bridge.getContainerADriveLetter();") !=
                  std::string::npos,
          "authenticated dashboard does not query live container A facts");
  require(script.find("bridge.submitPasswordBytes") == std::string::npos,
          "product page reused the incompatible P1 submission method");
}

void test_container_a_information_replaces_the_dashboard_placeholders() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(page.find("id=\"dashboard-container-a-state\"") !=
                  std::string::npos &&
              count_occurrences(page, "data-container-a-title") == 2 &&
              count_occurrences(page, "data-container-a-size") == 2 &&
              count_occurrences(page, "data-container-a-drive") == 2,
          "dashboard and A management page lack shared live A fields");
  require(page.find("建卷能力接入后，这里会显示加密盘") ==
                  std::string::npos &&
              page.find("本页已经接入认证会话；下一切片将连接真实建卷") ==
                  std::string::npos,
          "obsolete container A placeholders remain visible");
  require(script.find("currentContainerAState") != std::string::npos &&
              script.find("加密盘已挂载") != std::string::npos &&
              script.find("加密盘已创建") != std::string::npos &&
              script.find("未挂载，请重新插卡并登录后打开") !=
                  std::string::npos &&
              script.find("button.hidden = existingContainerA") !=
                  std::string::npos,
          "live A state does not drive its text, facts, and creation entry");
  require(script.find("getContainerB") == std::string::npos &&
              script.find("containerBType") == std::string::npos,
          "A display slice leaked B shape or volume type into the UI");
}

void test_container_a_creation_form_is_bounded_and_hidden_flow_stays_deferred() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(page.find("id=\"create-container-a-capacity\"") !=
              std::string::npos &&
              page.find("min=\"20\" max=\"1048576\" step=\"1\" value=\"64\"") !=
                  std::string::npos &&
              page.find("id=\"create-volume-status\"") !=
                  std::string::npos,
          "container A dialog lacks its fixed capacity and status controls");
  require(script.find("const containerACapacityMib") != std::string::npos &&
              script.find("value >= 20 && value <= 1048576") !=
                  std::string::npos &&
              script.find("creationKind !== \"encrypted\"") !=
                  std::string::npos &&
              script.find("creationContinueButton.title = encrypted ? \"\" : \"正在开发中\"") !=
                  std::string::npos,
          "creation routing does not bind A capacity or defer hidden volumes");
  require(script.find("正在请求管理员权限并创建加密盘") !=
              std::string::npos &&
              script.find("创建成功，可以开始使用加密盘") !=
                  std::string::npos &&
              script.find("若容器已完成但未挂载，请重新插卡后重试") !=
                  std::string::npos,
          "container A flow lacks busy, success, or recovery feedback");
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
              script.find("strengthOutput.dataset.score = \"1\"") !=
                  std::string::npos &&
              script.find("strengthOutput.dataset.score = \"2\"") !=
                  std::string::npos &&
              script.find("strengthOutput.dataset.score = \"3\"") !=
                  std::string::npos &&
              script.find("passwordInput.value.length === 0") !=
                  std::string::npos &&
              script.find("strengthOutput.hidden = true") !=
                  std::string::npos &&
              script.find("还需") == std::string::npos,
          "strength feedback diverges from the 12/18/128 policy boundary");
  require(script.find("initializationInputReady") != std::string::npos &&
              script.find("count >= 12 && count <= 128") !=
                  std::string::npos &&
              script.find("至少12字符") != std::string::npos,
          "initialization button does not enforce the visible minimum length");
}

void test_initialization_feedback_is_stable_and_recoverable() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script =
      read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(page.find("class=\"strength-slot\"") != std::string::npos &&
              page.find(".strength-slot { min-height:") != std::string::npos,
          "strength meter does not reserve its height before it appears");
  const std::size_t status_line = page.find("id=\"card-session-status\"");
  const std::size_t error_line = page.find("id=\"card-session-error\"");
  require(status_line != std::string::npos &&
              error_line != std::string::npos && status_line < error_line &&
              page.find("#card-session-error { margin-top: .5rem; color:") !=
                  std::string::npos,
          "failures do not appear as a red line under the standing guidance");
  require(script.find("statusOutput.textContent = message;") !=
              std::string::npos &&
              script.find("errorOutput.textContent = error;") !=
                  std::string::npos &&
              script.find("render(messageForCurrentState(), retryable") !=
                  std::string::npos,
          "an initialization failure hides the standing guidance line");
  require(script.find("初始化失败，两次密码不一致。") != std::string::npos &&
              script.find("无法初始化安全卡") == std::string::npos &&
              script.find("initializationInputsMatch") != std::string::npos &&
              script.find("password.normalize(\"NFC\") === "
                          "confirmation.normalize(\"NFC\")") !=
                  std::string::npos,
          "a mismatch is not named on the same terms the native route uses");
  require(script.find("passwordInput.value = retainedPassword;") !=
              std::string::npos &&
              script.find("confirmationInput.value = retainedConfirmation;") !=
                  std::string::npos &&
              script.find("!externalRefreshPending;") != std::string::npos,
          "a retryable initialization failure discards the typed inputs");
  require(count_occurrences(script, "retainedPassword = \"\";") >= 3 &&
              count_occurrences(script, "retainedConfirmation = \"\";") >= 3,
          "retained retry copies lack a success and fallback release");
}

void test_visibility_controls_persist_until_sensitive_inputs_are_cleared() {
  const std::string script =
      read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(script.find("const setPasswordVisibility") != std::string::npos &&
              script.find("input.type = visible ? \"text\" : \"password\"") !=
                  std::string::npos &&
              count_occurrences(script,
                                "getAttribute(\"aria-pressed\")") == 2,
          "custom password visibility controls are not stateful");
  require(script.find("const resetPasswordVisibility") != std::string::npos &&
              count_occurrences(script, "resetPasswordVisibility();") >= 3,
          "password visibility survives a submission or card-state clear");
}

void test_actions_are_serialized_and_external_changes_coalesce() {
  const std::string script =
      read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(script.find("if (busy)") != std::string::npos &&
              script.find("actionButton.disabled = busy ||") !=
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
  try {
    test_page_has_route_specific_password_surfaces();
    test_authenticated_dashboard_is_real_state_only();
    test_five_states_are_complete_and_vendor_neutral();
    test_bridge_calls_are_narrow_and_route_specific();
    test_container_a_creation_form_is_bounded_and_hidden_flow_stays_deferred();
    test_container_a_information_replaces_the_dashboard_placeholders();
    test_all_password_copies_have_fallback_wipes();
    test_strength_feedback_matches_the_native_length_baseline();
    test_initialization_feedback_is_stable_and_recoverable();
    test_visibility_controls_persist_until_sensitive_inputs_are_cleared();
    test_actions_are_serialized_and_external_changes_coalesce();
    std::cout << "card_product_ui_page_contract_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_product_ui_page_contract_tests: FAIL: "
              << error.what() << '\n';
    return 1;
  }
}
