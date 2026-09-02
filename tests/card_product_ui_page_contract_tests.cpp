#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/security/kdf_v1.h"

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

#ifndef USB_VAULT_PRODUCT_VERSION
#error USB_VAULT_PRODUCT_VERSION must name the CMake product version
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

// Returns the markup of one dashboard section so a contract can be scoped to
// it instead of to the whole page.
[[nodiscard]] std::string dashboard_section(const std::string_view page,
                                            const std::string_view name) {
  const std::string opening =
      "<section id=\"dashboard-section-" + std::string(name) + "\"";
  const std::size_t start = page.find(opening);
  require(start != std::string_view::npos, "dashboard section is missing");
  const std::size_t end = page.find("</section>", start);
  require(end != std::string_view::npos, "dashboard section is unterminated");
  return std::string(page.substr(start, end - start));
}

[[nodiscard]] std::string lower_ascii(std::string value) {
  std::ranges::transform(value, value.begin(), [](const unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return value;
}

void test_page_has_route_specific_password_surfaces() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  // Two session inputs plus the two the hidden-volume creation form needs.
  // The password the session was opened under is not among them.
  require(count_occurrences(page, "type=\"password\"") == 4,
          "product page must have exactly the session and creation inputs");
  require(page.find("id=\"card-password\"") != std::string::npos &&
              page.find("id=\"card-password-confirmation\"") !=
                  std::string::npos,
          "product password controls do not use the fixed page contract");
  require(page.find("id=\"confirmation-field\" class=\"field\" hidden") !=
              std::string::npos,
          "confirmation must start hidden outside initialization");
  require(count_occurrences(page, "autocomplete=\"off\"") == 4 &&
              count_occurrences(page, "spellcheck=\"false\"") == 4,
          "every product password control must disable browser assistance");
  require(page.find("<form") == std::string::npos,
          "product password inputs must not use browser form submission");
  require(page.find(
              "<script "
              "src=\"card_session.js?v=p2-17e-operation-history\"></script>") !=
                  std::string::npos &&
              count_occurrences(page, "<script") == 1,
          "product page must load only its packaged controller");
  require(
      count_occurrences(page, "class=\"password-visibility\"") == 2 &&
          page.find("aria-controls=\"card-password\"") != std::string::npos &&
          page.find("aria-controls=\"card-password-confirmation\"") !=
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
              page.find("id=\"dashboard-section-containers\"") !=
                  std::string::npos,
          "authenticated dashboard shell is incomplete");
  // 8.7.5 forbids presenting containers by volume type, so there is one list
  // and one management view, each reached from one creation entry. The two
  // shapes are offered inside that entry instead, both always present: 8.7
  // requires it, because hiding one would tell an observer what this machine
  // holds.
  require(
      count_occurrences(page, "data-create-container") == 2 &&
          count_occurrences(page, "data-create-kind=\"encrypted\"") == 1 &&
          count_occurrences(page, "data-create-kind=\"hidden\"") == 1 &&
          count_occurrences(page, "data-container-list") == 2 &&
          page.find("id=\"dashboard-section-hidden\"") == std::string::npos &&
          page.find("id=\"dashboard-section-encrypted\"") == std::string::npos,
      "containers are still split by volume type");
  require(
      count_occurrences(page, "class=\"nav-item nav-item-disabled\"") == 2 &&
          count_occurrences(page, "data-tooltip=\"正在开发中\"") == 2 &&
          page.find("文件管理") != std::string::npos &&
          page.find("备份中心") != std::string::npos &&
          page.find("<button class=\"nav-item dashboard-nav\" type=\"button\""
                    " data-section=\"history\">") != std::string::npos &&
          page.find("操作日志") != std::string::npos,
      "unfinished navigation is not visibly present and unavailable");
  require(
      script.find("const showDashboard = currentState === "
                  "nativeState.authenticated;") != std::string::npos &&
          script.find("surface.hidden = showDashboard;") != std::string::npos &&
          script.find("dashboard.hidden = !showDashboard;") !=
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

void test_settings_page_is_static_and_matches_the_native_facts() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  const std::string settings = dashboard_section(page, "settings");

  require(page.find("<button class=\"nav-item dashboard-nav\" type=\"button\""
                    " data-section=\"settings\">") != std::string::npos &&
              count_occurrences(page, "data-section=\"") ==
                  count_occurrences(page, "id=\"dashboard-section-") &&
              settings.find("系统设置") != std::string::npos,
          "settings is not a reachable dashboard section like the others");

  // Three switches, of which only the card-removal unmount is a real product
  // guarantee: it is locked on because 8.4 treats the unmount as part of
  // deniability, so it must never be presented as something a user can turn
  // off. The other two are the switches requirements 8.7.5 and 8.7.4 define.
  require(count_occurrences(settings, "role=\"switch\"") == 3 &&
              count_occurrences(settings, "data-setting=\"") == 2 &&
              settings.find("data-setting=\"auto-mount\" "
                            "aria-label=\"登录后自动挂载全部容器\"") !=
                  std::string::npos &&
              settings.find("data-setting=\"remember-containers\" "
                            "aria-label=\"记住容器位置\"") != std::string::npos,
          "settings switches do not match the two defined settings");
  require(settings.find("<span class=\"settings-switch\" role=\"switch\" "
                        "aria-checked=\"true\" aria-disabled=\"true\"") !=
              std::string::npos &&
              count_occurrences(settings, "aria-disabled=\"true\"") == 1 &&
              settings.find("拔出安全卡时立即卸载全部卷") != std::string::npos,
          "the card-removal unmount is not a locked, unactuatable switch");
  require(settings.find("<input") == std::string::npos,
          "settings page must not use form inputs");

  // Neither remaining switch has a backing store yet: 8.7.5 needs the first
  // local configuration artifact and 8.7.4 needs the encrypted, layered
  // container list. Until those land the switches must stay page-local, so
  // nothing here may reach the native bridge or any browser storage.
  require(count_occurrences(script, "data-setting") == 1 &&
              script.find("control.setAttribute(\"aria-checked\", "
                          "enabled ? \"false\" : \"true\");") !=
                  std::string::npos,
          "a settings switch does more than flip its own state");
  for (const std::string_view store : {"localStorage", "sessionStorage",
                                       "indexedDB", "document.cookie"}) {
    require(page.find(store) == std::string::npos &&
                script.find(store) == std::string::npos,
            "the product page persists state in browser storage");
  }

  const std::string derivation =
      "Argon2id · m=" +
      std::to_string(usb_vault::security::kKdfV1MemoryKib / 1024U) +
      " MiB · t=" +
      std::to_string(usb_vault::security::kKdfV1Iterations) + " · p=" +
      std::to_string(usb_vault::security::kKdfV1Parallelism);
  const std::string record =
      std::to_string(usb_vault::card::kCardRecordV1Size) +
      " 字节 · AES-256-GCM + HMAC-SHA-256";
  require(settings.find(derivation) != std::string::npos &&
              settings.find(record) != std::string::npos &&
              settings.find("AES-256-XTS") != std::string::npos &&
              settings.find("HMAC-SHA-512") != std::string::npos,
          "about facts have drifted from the native cryptographic baseline");
  require(settings.find("id=\"settings-product-version\">" USB_VAULT_PRODUCT_VERSION
                        "<") != std::string::npos,
          "about version has drifted from the CMake product version");

  // The projection deliberately carries no hardware identity, so the page has
  // nothing truthful to print about the card itself, and code signing is a P3
  // deliverable that does not exist yet.
  require(settings.find("安全卡状态") == std::string::npos &&
              settings.find("代码签名") == std::string::npos &&
              settings.find("EV") == std::string::npos,
          "settings page claims a card identity or a signature it cannot have");
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
  require(script.find("await bridge.createContainerMib(sizeMib);") !=
              std::string::npos,
          "authenticated creation does not use the capacity-only bridge");
  require(script.find("await bridge.getContainerCount();") !=
                  std::string::npos &&
              script.find("await bridge.getContainerId(index);") !=
                  std::string::npos &&
              script.find("await bridge.getContainerName(index);") !=
                  std::string::npos &&
              script.find("await bridge.getContainerMounted(index);") !=
                  std::string::npos &&
              script.find("await bridge.getContainerSizeMib(index);") !=
                  std::string::npos &&
              script.find("await bridge.getContainerDriveLetter(index);") !=
                  std::string::npos,
          "authenticated dashboard does not query the live container list");
  require(script.find("bridge.mountContainer()") != std::string::npos &&
              script.find("bridge.mountAdoptedContainer(id)") !=
                  std::string::npos &&
              script.find("bridge.unmountContainer(id)") != std::string::npos,
          "the page cannot adopt, remount, or release one container");
  // The page asks; the native side chooses. A path must never be built,
  // stored, or sent by the page.
  require(script.find("getContainerAState") == std::string::npos &&
              script.find("getContainerASizeMib") == std::string::npos &&
              script.find("getContainerADriveLetter") == std::string::npos &&
              script.find("createContainerAMib") == std::string::npos,
          "the page still reads the retired single-container bridge");
  require(script.find("bridge.submitPasswordBytes") == std::string::npos,
          "product page reused the incompatible P1 submission method");
}

void test_operation_history_is_bounded_session_only_and_inert() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  const std::string history = dashboard_section(page, "history");

  require(
      history.find("操作日志") != std::string::npos &&
          history.find("仅保留当前安全会话，退出或重新认证后清除") !=
              std::string::npos &&
          history.find("id=\"operation-history-clear\"") != std::string::npos &&
          history.find("id=\"operation-history-body\"") != std::string::npos &&
          history.find("最近 30 天") == std::string::npos,
      "operation history does not state its current-session lifetime");
  require(history.find("密码尝试") != std::string::npos &&
              history.find("卡标识") != std::string::npos &&
              history.find("容器类型与结构") != std::string::npos &&
              history.find("完整路径") != std::string::npos,
          "operation history does not disclose its deliberate omissions");

  for (const std::string_view call :
       {"await bridge.getOperationCount();",
        "await bridge.getOperationCompletedAtUnixMs(index);",
        "await bridge.getOperationKind(index);",
        "await bridge.getOperationOutcome(index);",
        "await bridge.getOperationContainerName(index);",
        "await bridge.getOperationDriveLetter(index);",
        "() => bridge.clearOperationHistory()"}) {
    require(script.find(call) != std::string::npos,
            "operation history omits a required native bridge call");
  }
  require(
      script.find("operationCount > 128") != std::string::npos &&
          script.find("Number.isSafeInteger(completedAtUnixMs)") !=
              std::string::npos &&
          script.find("name.indexOf(\"/\") === -1") != std::string::npos &&
          script.find("name.indexOf(\"\\\\\") === -1") != std::string::npos &&
          script.find("throw new Error(\"invalid operation event\")") !=
              std::string::npos,
      "operation history accepts an unbounded or unsafe projection");
  require(script.find("target.textContent = operationTarget(event);") !=
                  std::string::npos &&
              script.find("outcome.textContent =") != std::string::npos &&
              script.find("innerHTML") == std::string::npos,
          "operation history renders native text as executable markup");
  for (const std::string_view store :
       {"localStorage", "sessionStorage", "indexedDB", "document.cookie"}) {
    require(page.find(store) == std::string::npos &&
                script.find(store) == std::string::npos,
            "operation history is persisted in browser storage");
  }
}

void test_container_collection_replaces_the_single_container_display() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(page.find("id=\"dashboard-container-count\"") !=
                  std::string::npos &&
              count_occurrences(page, "data-container-list") == 2 &&
              count_occurrences(page, "data-mount-existing") == 2 &&
              page.find("id=\"dashboard-status\"") != std::string::npos,
          "dashboard and management views lack the shared container list");
  require(count_occurrences(page, "data-container-a-title") == 0 &&
              count_occurrences(page, "data-container-a-size") == 0 &&
              count_occurrences(page, "data-container-a-drive") == 0,
          "the retired single-container placeholders are still rendered");
  require(script.find("currentContainerAState") == std::string::npos &&
              script.find("加密盘已创建") == std::string::npos,
          "the page still tracks one fixed container");
  require(script.find("let containers = []") != std::string::npos &&
              script.find("nextContainers.push({ id, name, mounted, sizeMib,"
                          " drive })") != std::string::npos &&
              script.find("throw new Error(\"invalid container status\")") !=
                  std::string::npos,
          "the container list is not validated before it is displayed");
  // A container name comes from a file the user picked. It is displayed as
  // text so a crafted file name cannot become markup.
  require(script.find("name.textContent = container.name;") !=
                  std::string::npos &&
              script.find("innerHTML") == std::string::npos,
          "container names are not rendered as inert text");
  // 8.7.2: an entry that disappears tells an observer what this machine holds.
  require(script.find("button.hidden = existingContainerA") ==
                  std::string::npos &&
              script.find("button.disabled = busy || !showDashboard;") !=
                  std::string::npos,
          "a creation entry is still hidden by what already exists");
  require(script.find("getContainerB") == std::string::npos &&
              script.find("containerBType") == std::string::npos &&
              script.find("volumeType") == std::string::npos,
          "the container display leaked B shape or volume type into the UI");
}

void test_hidden_container_form_is_symmetric_and_never_validated() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(page.find("id=\"create-container-b-form\"") != std::string::npos &&
              page.find("id=\"create-container-b-capacity\"") !=
                  std::string::npos &&
              page.find("id=\"create-container-b-hidden-capacity\"") !=
                  std::string::npos &&
              page.find("id=\"create-container-b-second\"") !=
                  std::string::npos &&
              page.find("id=\"create-container-b-second-confirmation\"") !=
                  std::string::npos &&
              page.find("id=\"create-container-b-acknowledge\"") !=
                  std::string::npos,
          "the hidden container form is incomplete");
  // The password the session was opened under is never asked for again: the
  // session still holds it, so the page has no field for it and the bridge
  // call has no argument for it.
  require(page.find("create-container-b-password") == std::string::npos &&
              script.find("containerBPassword") == std::string::npos,
          "the page still asks for the password it was opened with");
  // 8.7.2: the wording is fixed and never adapts to who is logged in.
  require(page.find("为该容器设置诱饵密码") != std::string::npos &&
              page.find("再次输入诱饵密码") != std::string::npos &&
              page.find("若您曾设置过诱饵密码，此处需与曾经的诱饵密码保持一致") !=
                  std::string::npos,
          "the second password prompt is not the fixed symmetric wording");
  // The card holds two passwords at once, so creating this container drops
  // whichever third one was recorded. That has to be acknowledged, and the
  // acknowledgement reads the same under either login.
  require(page.find("创建这个容器会重写安全卡记录") != std::string::npos &&
              script.find("containerBAcknowledge.checked") !=
                  std::string::npos,
          "the card record rewrite is committed without an acknowledgement");
  require(script.find("await bridge.createContainerBMib(sizes.outer, "
                      "sizes.hidden,\n                                         "
                      "secondBytes, confirmationBytes);") != std::string::npos,
          "the page does not create the hidden container through the bridge");
  // 8.7.3: entering it twice is the only permitted check. The page must not
  // claim the second password is right or wrong.
  require(script.find("containerBSecond.value === "
                      "containerBSecondConfirmation.value") !=
                  std::string::npos &&
              script.find("诱饵密码错误") == std::string::npos &&
              script.find("密码不正确") == std::string::npos,
          "the page judges the second password instead of only confirming it");
  require(script.find("creationContinueButton.title") == std::string::npos &&
              script.find("隐藏盘参数与卡记录事务将在后续切片接入") ==
                  std::string::npos,
          "the deferred hidden-volume placeholder is still present");
}

void test_container_creation_form_is_bounded() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(page.find("id=\"create-container-capacity\"") !=
              std::string::npos &&
              page.find("min=\"20\" max=\"1048576\" step=\"1\" value=\"64\"") !=
                  std::string::npos &&
              page.find("id=\"create-volume-status\"") !=
                  std::string::npos,
          "container A dialog lacks its fixed capacity and status controls");
  // A capacity is typed in whichever unit the picker shows and always reaches
  // the bridge in MiB, so the bounds are checked after the conversion.
  require(script.find("const kMinimumCapacityMib = 20;") != std::string::npos &&
              script.find("const kMaximumCapacityMib = 1024 * 1024;") !=
                  std::string::npos &&
              script.find("{ mib: 1, gib: 1024, tib: 1024 * 1024 }") !=
                  std::string::npos &&
              script.find("if (value > kMaximumCapacityMib) return "
                          "\"容量超出最大值。\";") != std::string::npos &&
              script.find("if (value < kMinimumCapacityMib) return "
                          "\"容量低于最小值。\";") != std::string::npos,
          "capacity bounds are not enforced on the converted value");
  // The outer volume must stay large enough to hold credible handover
  // content, so the inner one is capped at three quarters of the total.
  require(script.find("if (hidden * 4 > outer * 3) return \"内层容量过载。\";") !=
              std::string::npos,
          "the three-quarter rule for the inner volume is missing");
  require(script.find("const containerCapacityMib") != std::string::npos &&
              script.find("creationKind !== \"encrypted\"") !=
                  std::string::npos &&
              script.find("creationKind !== \"hidden\"") !=
                  std::string::npos,
          "creation routing does not bind each capacity to its own flow");
  require(script.find("请选择保存位置，随后确认管理员权限") !=
              std::string::npos &&
              script.find("创建成功，可以开始使用。") !=
                  std::string::npos &&
              script.find("已取消创建。") != std::string::npos &&
              script.find("若容器已完成但未挂载，请重新插卡后重试") !=
                  std::string::npos,
          "creation lacks busy, success, cancellation, or recovery feedback");
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
    test_settings_page_is_static_and_matches_the_native_facts();
    test_five_states_are_complete_and_vendor_neutral();
    test_bridge_calls_are_narrow_and_route_specific();
    test_operation_history_is_bounded_session_only_and_inert();
    test_container_creation_form_is_bounded();
    test_hidden_container_form_is_symmetric_and_never_validated();
    test_container_collection_replaces_the_single_container_display();
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
