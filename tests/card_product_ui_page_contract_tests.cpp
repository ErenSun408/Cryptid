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

#ifndef USB_VAULT_CARD_PRODUCT_UI_FIXTURE_PAGE
#error USB_VAULT_CARD_PRODUCT_UI_FIXTURE_PAGE must name the integration page
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
  std::string content{std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>()};
  // Whether these files arrive with CRLF or LF is a checkout setting, not a
  // fact about the page. Every contract below quotes source text, and some of
  // those quotes span lines, so they would silently stop matching on a
  // machine that checks out the other way. Normalizing here keeps the
  // contracts about the page rather than about git configuration.
  std::erase(content, '\r');
  return content;
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
  // The two password controls, the two the creation forms add, the four
  // name/save-location fields, and the file manager's name field. A remembered
  // container path is exactly what section 8.4 forbids, so browser assistance
  // is off on those too; a file name inside a container is the content of an
  // encrypted volume, which is at least as good a reason.
  require(count_occurrences(page, "autocomplete=\"off\"") == 10 &&
              count_occurrences(page, "spellcheck=\"false\"") == 10,
          "every product text control must disable browser assistance");
  require(page.find("<form") == std::string::npos,
          "product password inputs must not use browser form submission");
  require(page.find(
              "<script "
              "src=\"card_session.js?v=p2-20-create-form\"></script>") !=
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
              page.find("id=\"dashboard-section-files\"") !=
                  std::string::npos,
          "authenticated dashboard shell is incomplete");
  require(page.find("data-section=\"containers\"") == std::string::npos &&
              page.find("id=\"dashboard-section-containers\"") == std::string::npos &&
              page.find("id=\"file-container-select\"") == std::string::npos,
          "the old container route or duplicate container picker remains");
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
  // Both 数据管理 entries are real as of P3.15, so nothing in the sidebar is a
  // placeholder any more. The disabled styling stays defined for whatever
  // comes next; what must not come back is a placeholder nobody removed.
  require(
      count_occurrences(page, "class=\"nav-item nav-item-disabled\"") == 0 &&
          count_occurrences(page, "data-tooltip=\"正在开发中\"") == 0 &&
          page.find("容器管理") != std::string::npos &&
          page.find("导出与迁移") != std::string::npos &&
          page.find("<button class=\"nav-item dashboard-nav\" type=\"button\""
                    " data-section=\"files\">") != std::string::npos &&
          page.find("<button class=\"nav-item dashboard-nav\" type=\"button\""
                    " data-section=\"backup\">") != std::string::npos &&
          page.find("<button class=\"nav-item dashboard-nav\" type=\"button\""
                    " data-section=\"history\">") != std::string::npos &&
          page.find("操作日志") != std::string::npos,
      "the navigation still carries a placeholder or lost a real entry");
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

  // Four switches, of which only the card-removal unmount is a real product
  // guarantee: it is locked on because 8.4 treats the unmount as part of
  // deniability, so it must never be presented as something a user can turn
  // off. The four editable switches include the user-requested logging toggle.
  require(count_occurrences(settings, "role=\"switch\"") == 5 &&
              count_occurrences(settings, "data-setting=\"") == 4 &&
              settings.find("data-setting=\"operation-logging\" "
                            "aria-label=\"开启操作日志\"") != std::string::npos &&
              settings.find("data-setting=\"auto-mount\" "
                            "aria-label=\"登录后自动挂载全部容器\"") !=
                  std::string::npos &&
              settings.find("data-setting=\"remember-containers\" "
                            "aria-label=\"记住容器位置\"") != std::string::npos &&
              settings.find("data-setting=\"mount-imported\" "
                            "aria-label=\"自动尝试挂载导入的容器\"") !=
                  std::string::npos,
          "settings switches do not match the four defined settings");
  require(settings.find("<span class=\"settings-switch\" role=\"switch\" "
                        "aria-checked=\"true\" aria-disabled=\"true\"") !=
              std::string::npos &&
              count_occurrences(settings, "aria-disabled=\"true\"") == 1 &&
              settings.find("拔出安全卡时立即卸载全部卷") != std::string::npos,
          "the card-removal unmount is not a locked, unactuatable switch");
  require(settings.find("<input") == std::string::npos,
          "settings page must not use form inputs");

  // 8.7.4 has landed, so remember-containers now has a store and reaches the
  // bridge; what that must look like is pinned separately. Auto-mount does
  // not: 8.7.5 first requires a cap on how many volumes an automatic mount may
  // attach, or a renegotiated five-second budget. Until that lands it stays
  // page-local, so nothing here may carry it to the native side.
  require(script.find("control.setAttribute(\"aria-checked\", "
                      "enabled ? \"false\" : \"true\");") != std::string::npos,
          "a switch with no store behind it no longer flips its own state");
  require(script.find("autoMount") == std::string::npos &&
              script.find("AutoMount") == std::string::npos,
          "the auto-mount switch reached the native side before 8.7.5 landed");
  // Browser storage stays forbidden for every switch, store or no store.
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
  require(script.find(
              "await bridge.createContainerMib(name, directory, sizeMib);") !=
              std::string::npos,
          "authenticated creation does not name the container and location");
  require(script.find("await bridge.chooseContainerDirectory();") !=
              std::string::npos,
          "the save-location field has no browse button behind it");
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
  // Six arguments and no more: the name, the location, the two capacities and
  // the new password twice. The login input is still not among them.
  require(script.find("await bridge.createContainerBMib(name, directory, "
                      "sizes.outer,") != std::string::npos &&
              count_occurrences(script, "bridge.createContainerBMib(") ==
                  1 &&
              script.find("secondBytes,") != std::string::npos &&
              script.find("confirmationBytes);") != std::string::npos,
          "the page does not create the hidden container through the bridge");
  // 8.7.2: the location is named on this form exactly as it is on the
  // standard one, so both carry the same two fields with the same wording.
  require(count_occurrences(page, "<label for=\"create-container-name\">"
                                  "名称</label>") == 1 &&
              count_occurrences(page, "<label for=\"create-container-b-name\">"
                                      "名称</label>") == 1 &&
              count_occurrences(page, "路径</label>") == 2 &&
              count_occurrences(page, "class=\"creation-text\"") == 2 &&
              count_occurrences(page, "class=\"creation-text mono\"") == 2,
          "the two creation forms do not ask for the location identically");
  // Neither parameter step carries a description. The one they used to share
  // ended on a dialog that no longer opens, and a sentence kept on one shape
  // only would be exactly the visible difference 8.7.2 forbids.
  require(script.find("接着会让你选择保存位置") == std::string::npos &&
              count_occurrences(script, "setCreationDescription(\"\")") == 1,
          "a creation parameter step still describes a location dialog");
  // Nothing about a location survives the call that carried it, on either
  // flow, so a failure asks for the fields again rather than keeping them.
  require(count_occurrences(script, "containerName.value = \"\";") >= 1 &&
              count_occurrences(script, "containerBName.value = \"\";") >= 1 &&
              script.find("重试前需要重新填写名称、路径与诱饵密码") !=
                  std::string::npos,
          "the hidden container flow keeps its location after a failure");
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

void test_dropping_a_container_is_confirmed_and_says_what_it_keeps() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(page.find("id=\"forget-container-dialog\"") != std::string::npos &&
              page.find("id=\"forget-container-confirm\"") !=
                  std::string::npos &&
              page.find("id=\"forget-container-cancel\"") != std::string::npos,
          "dropping a container has no confirmation step");
  // The one thing a user could fear here is losing the data. The dialog has to
  // say plainly that it does not touch the file, or the action is unusable.
  require(page.find("不会删除容器文件") != std::string::npos &&
              page.find("重新添加至列表") != std::string::npos,
          "the drop confirmation does not say the container file survives");
  require(script.find("bridge.forgetContainer(id)") != std::string::npos,
          "the page cannot drop a container off the list");
  // The action is reached only through the dialog: a row button that called
  // the bridge directly would be a one-click removal.
  require(script.find("openForgetFlow(id") != std::string::npos &&
              script.find("pendingForgetId") != std::string::npos,
          "the drop button does not go through the confirmation");
}

void test_the_import_switch_is_stored_and_defaults_to_mounting_nothing() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  // Importing a container must not open it unless the user asked for that, so
  // the switch ships off. The page holds the same position the native default
  // does, or the first paint would contradict the store it is about to read.
  const std::size_t control = page.find("data-setting=\"mount-imported\"");
  require(control != std::string::npos, "the import switch is missing");
  const std::size_t row = page.rfind("<div class=\"settings-row\"", control);
  require(row != std::string::npos &&
              page.substr(row, control - row).find(
                  "aria-checked=\"false\"") != std::string::npos,
          "the import switch does not ship turned off");
  require(script.find("bridge.getMountImportedContainers()") !=
              std::string::npos &&
              script.find("bridge.setMountImportedContainers(value)") !=
                  std::string::npos,
          "the import switch does not reach the native store");
}

void test_the_remember_switch_shows_what_the_native_side_holds() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(page.find("data-setting=\"remember-containers\"") !=
              std::string::npos,
          "the remember-containers switch is missing");
  require(script.find("bridge.getRememberContainerPaths()") !=
              std::string::npos &&
              script.find("bridge.setRememberContainerPaths(value)") !=
                  std::string::npos,
          "the switch does not reach the native store");
  // Every stored switch is a view of the setting, never of the click. A page
  // that set one optimistically would show a choice that was never saved, so
  // the write is followed by a re-read for all of them.
  require(script.find("const refreshStoredSwitches") != std::string::npos &&
              script.find("await stored.write(!enabled);") !=
                  std::string::npos &&
              count_occurrences(script, "await refreshStoredSwitches();") == 2,
          "the switch position is not re-read from the native side");
}

void test_the_integration_fixture_page_carries_every_element_the_script_takes() {
  // The WebView2 integration fixture has its own stripped-down page, and the
  // product script runs against it unchanged. An element added to the product
  // page and forgotten there is not a cosmetic difference: the script looks
  // every one of these up at startup and wires listeners to them, so a missing
  // one throws before the page ever reports a state, and the fixture then
  // fails at its first step for a reason that says nothing about the cause.
  //
  // That is exactly what happened to 从列表移除 (P2.26), and it went unseen
  // because the fixture is built and run by name rather than by CTest. This
  // case is the cheap half of that lesson: the expensive half stays manual.
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  const std::string fixture =
      read_source(USB_VAULT_CARD_PRODUCT_UI_FIXTURE_PAGE);
  constexpr std::string_view lookup = "document.getElementById(\"";
  std::size_t offset = 0;
  std::size_t checked = 0;
  while ((offset = script.find(lookup, offset)) != std::string::npos) {
    offset += lookup.size();
    const std::size_t end = script.find('"', offset);
    require(end != std::string::npos, "an element lookup is not terminated");
    const std::string identifier = script.substr(offset, end - offset);
    require(fixture.find("id=\"" + identifier + "\"") != std::string::npos,
            "the integration fixture page is missing an element the product "
            "script looks up");
    ++checked;
    offset = end;
  }
  require(checked > 40, "the element lookups were not found in the script");
}

void test_card_reset_is_gated_and_states_its_cost() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  require(page.find("id=\"reset-card-open\"") != std::string::npos &&
              page.find("id=\"reset-card-dialog\"") != std::string::npos &&
              page.find("id=\"reset-card-acknowledge\"") !=
                  std::string::npos &&
              page.find("id=\"reset-card-confirm\"") != std::string::npos,
          "the card reset entry or its confirmation is incomplete");
  // The operation destroys the only copy of the root secret. The page has to
  // say what that costs before it can be started at all.
  require(page.find("销毁安全卡上的认证密钥") != std::string::npos &&
              page.find("现有容器文件不会被删除，但将永久无法打开") !=
                  std::string::npos &&
              page.find("此操作不可撤销") != std::string::npos,
          "the reset confirmation does not state what it destroys");
  require(script.find("resetAcknowledge.checked") != std::string::npos &&
              script.find("await bridge.resetCardRecord();") !=
                  std::string::npos,
          "the reset is committed without an acknowledgement");
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
  // Cancellation is no longer one of the outcomes: the form settles every
  // field before the create button commits, so the only ways out are success
  // and failure, and failure says which fields have to be filled in again.
  require(script.find("正在创建，请确认管理员权限。") != std::string::npos &&
              script.find("创建成功，可以开始使用。") !=
                  std::string::npos &&
              script.find("重试前需要重新填写名称与路径") !=
                  std::string::npos &&
              script.find("若容器已完成但未挂载，请重新插卡后重试") !=
                  std::string::npos,
          "creation lacks busy, success, or recovery feedback");
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

void test_the_file_manager_browses_a_container_without_naming_a_volume() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  const std::string section = dashboard_section(page, "files");

  require(section.find("id=\"management-containers\"") != std::string::npos &&
              section.find("id=\"management-files\"") != std::string::npos &&
              section.find("id=\"file-breadcrumb\"") != std::string::npos &&
              section.find("id=\"file-list-body\"") != std::string::npos &&
              section.find("id=\"file-list-status\"") != std::string::npos,
          "the management view is missing a level, the breadcrumb, the list, "
          "or its status line");

  // The compact file view routes exports through the existing export centre;
  // removing its standing explanatory copy must not add an export shortcut.
  require(script.find("offerEntryForExport(act.dataset.fileName);") != std::string::npos,
          "the browse view does not hand exports to the export section");

  // The page names a container and a directory inside it. A drive letter would
  // let it name any volume on the machine, so it is never given one.
  require(script.find("await bridge.listContainerDirectory(\n"
                      "        container.id, filePath.join(\"/\"));") !=
              std::string::npos,
          "the listing is not requested by container identifier");

  // One call brings a whole directory back, separated by the two control
  // characters no name off a mounted volume can hold.
  //
  // There are two callers and no more: this one, which walks wherever the
  // user browses, and the export panel, which reads a container root and
  // never anything below it. The second is held to the root by its own case,
  // so a third caller here would be a third way to walk a volume.
  require(script.find("const listingRecordSeparator = \"\\u001e\";") !=
              std::string::npos &&
              script.find("const listingFieldSeparator = \"\\u001f\";") !=
                  std::string::npos &&
              count_occurrences(script, "bridge.listContainerDirectory") == 2,
          "the packed listing is not read the way the bridge writes it");
  require(script.find("if (records[0] !== \"0\" && records[0] !== \"1\") {") !=
              std::string::npos &&
              script.find("if (fields.length !== 5) throw new Error(") !=
                  std::string::npos,
          "the page draws a listing it has not fully checked");

  // Every state the browse view has to be able to say, including the two that
  // look alike on screen and mean opposite things.
  require(script.find("这个容器没有挂载。") != std::string::npos &&
              script.find("这个文件夹是空的。") != std::string::npos &&
              script.find("还没有容器。先新建或导入一个容器") !=
                  std::string::npos,
          "the browse view cannot say unmounted, empty, or no containers yet");
  require(script.find("这个文件夹里的条目太多") != std::string::npos,
          "a truncated listing would be presented as a whole one");
}

void test_deleting_is_permanent_and_asked_once_and_renaming_is_one_name() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  const std::string section = dashboard_section(page, "files");

  require(section.find("<th>操作</th>") != std::string::npos &&
              section.find("id=\"file-create-directory\"") != std::string::npos,
          "the browse view has nowhere to act on an entry from");

  // The user's compact table revision removes the standing notice. Preserve
  // the permanent-delete explanation on the icon and the existing semantics.
  require(script.find("永久删除，不进回收站") != std::string::npos &&
              page.find("id=\"file-context-rename\"") != std::string::npos,
          "the compact file actions lost their deletion hint or rename menu");
  require(page.find("id=\"file-delete-dialog\"") == std::string::npos,
          "a delete confirmation appeared without the decision being revisited");
  require(script.find("() => bridge.deleteContainerEntry(") !=
              std::string::npos &&
              script.find("container.id, filePath.join(\"/\"), name),") !=
                  std::string::npos,
          "the delete does not name the entry in the directory being shown");

  // Renaming and making a directory ask for a name in the same dialog, because
  // they are the same question with two answers.
  require(page.find("id=\"file-name-dialog\"") != std::string::npos &&
              page.find("id=\"file-name-input\"") != std::string::npos &&
              page.find("id=\"file-name-confirm\"") != std::string::npos &&
              page.find("id=\"file-name-cancel\"") != std::string::npos &&
              page.find("id=\"file-name-status\"") != std::string::npos,
          "the name dialog is incomplete");
  require(script.find("() => bridge.renameContainerEntry(\n"
                      "          container.id, filePath.join(\"/\"), target, "
                      "value),") != std::string::npos,
          "the rename does not carry both the old and the new name");
  require(script.find("() => bridge.createContainerDirectory(\n"
                      "        container.id, filePath.join(\"/\"), value),") !=
              std::string::npos,
          "the new directory is not made where the page is looking");

  // A name that is only a name: 255 is the limit every file system a mounted
  // volume can carry imposes, and the native side refuses the rest.
  require(page.find("id=\"file-name-input\" class=\"file-name-text\" type=\"text\"") !=
              std::string::npos &&
              page.find("maxlength=\"255\"") != std::string::npos,
          "the name field does not match the native name limit");
}

void test_a_drag_stays_inside_the_containers_and_says_where_out_lives() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  const std::string section = dashboard_section(page, "files");

  require(section.find("id=\"file-drop-targets\"") != std::string::npos,
          "there is nowhere to drop an entry into another container");
  require(script.find("row.draggable = !busy;") != std::string::npos &&
              script.find("row.dataset.fileDrag = entry.name;") !=
                  std::string::npos &&
              script.find("row.dataset.fileDropDirectory = entry.name;") !=
                  std::string::npos,
          "the listing rows neither travel nor receive");

  // 🔴 The drag carries a marker and nothing else. A page that put a file on
  // the drag would have made a plaintext export out of a gesture, with none of
  // the confirmations Q-04 requires standing in front of it.
  require(script.find("event.dataTransfer.setData(\"text/plain\", \"\");") !=
              std::string::npos &&
              script.find("DownloadURL") == std::string::npos &&
              script.find("dataTransfer.files") == std::string::npos &&
              script.find("dataTransfer.items.add") == std::string::npos,
          "the drag hands something other than a marker to the shell");

  // 🔴 And when it lands nowhere this page owns, the page says where out of
  // the volume actually lives instead of quietly doing nothing.
  require(script.find("要把文件拿出加密卷，那是明文导出，请到「导出与迁移」做。") !=
              std::string::npos &&
              script.find("if (!dropped) reportFileDropRefused();") !=
                  std::string::npos,
          "a drag that left the page is not answered with the backup centre");

  // Both ends of a transfer, and the modifier that makes it a copy.
  require(script.find("? bridge.copyContainerEntry(\n"
                      "          container.id, here, name, destinationId, "
                      "destinationPath)") != std::string::npos &&
              script.find(": bridge.moveContainerEntry(\n"
                          "          container.id, here, name, destinationId, "
                          "destinationPath)") != std::string::npos,
          "a transfer does not name both of its ends");
  require(script.find("event.dataTransfer.dropEffect = event.ctrlKey ? "
                      "\"copy\" : \"move\";") != std::string::npos,
          "the drag does not distinguish a copy from a move");

  // A transfer between containers reads and writes every byte on the thread
  // that draws this page. The disabled state has to reach the screen first.
  require(script.find("await new Promise((resolve) => { setTimeout(resolve, "
                      "0); });") != std::string::npos,
          "a blocking transfer starts before the page looks busy");
}

// 🔴 The plaintext export happens in one place, under one notice, and the file
// manager only ever hands an entry over to it.
void test_the_backup_centre_owns_the_plaintext_export() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  const std::string section = dashboard_section(page, "backup");
  const std::string files = dashboard_section(page, "files");

  require(section.find("id=\"export-container-list\"") != std::string::npos &&
              section.find("id=\"backup-export-location\"") !=
                  std::string::npos &&
              section.find("id=\"backup-export-browse\"") !=
                  std::string::npos &&
              section.find("id=\"backup-export-confirm\"") !=
                  std::string::npos &&
              section.find("id=\"backup-export-status\"") != std::string::npos,
          "the backup centre cannot pick a file, a location, or export");

  // The panel picks by ticking containers open, not by naming one entry. Every
  // ticked entry goes through the one entry point that may leave a volume, so
  // a selection is many calls to it rather than a second, bulk way out.
  require(section.find("class=\"xlist\"") != std::string::npos &&
              script.find("const exportSelection = new Map();") !=
                  std::string::npos &&
              script.find("head.dataset.exportToggle = String(container.id);") !=
                  std::string::npos,
          "the export panel does not select through container cards");

  require(script.find("row.dataset.exportPick = entry.name;") !=
              std::string::npos &&
              script.find("tail.dataset.exportSelectAll = String(container.id);") !=
                  std::string::npos,
          "the export panel cannot tick one entry or a whole container");

  // Each container row carries its own mount control and a drag handle, and
  // shows only the drive letter: capacity and a spelled-out state were both
  // saying what the icon and the button already say.
  require(script.find("grab.dataset.reorderGrip = String(container.id);") !=
              std::string::npos &&
              script.find("mount.dataset.rowMount = String(container.id);") !=
                  std::string::npos &&
              script.find("factCell(\"盘符\"") != std::string::npos &&
              script.find("factCell(\"容量\"") == std::string::npos &&
              script.find("factCell(\"状态\"") == std::string::npos,
          "a container row does not carry its own handle and mount control");

  // 🔴 Only the handle starts a reorder, so the row body keeps the meaning
  // it has, and the order is page-local: nothing about it is written down.
  require(script.find("beginContainerReorder(event, row, container.id);") !=
              std::string::npos &&
              script.find("let containerOrder = [];") != std::string::npos,
          "the container order is not dragged from the handle alone");

  // A card reads its container's root and only its root. Anything deeper is
  // the file manager's job, and a second way to walk a volume from here would
  // be a second thing to keep inside the boundary.
  require(script.find("const readContainerRoot = async (id) => {") !=
              std::string::npos,
          "an opened card reads more than the container root");

  // 🔴 What succeeded leaves the selection as it goes, so a partial failure
  // leaves exactly the unfinished entries ticked and says how many.
  require(script.find("exportSelection.delete(\n"
                      "          exportKey(request.id, request.directory, "
                      "request.name));") != std::string::npos &&
              script.find("exportLocation = \"\";\n    setBackupStatus(") !=
                  std::string::npos,
          "the page kept the exported entry or its location afterwards");

  // A run that half fails says how many landed and how many did not, and the
  // ones that did not stay ticked -- the page never reports a whole export
  // for a partial one.
  require(script.find("failed.push(request);") != std::string::npos &&
              script.find("项没有完成，仍勾选着。") != std::string::npos,
          "a partly failed export does not say what is left to do");

  // The migration wording sits beside the title, where it is read before the
  // panels rather than inside the one it describes.
  require(section.find("class=\"section-aside\"") != std::string::npos &&
              section.find("迁移只复制容器文件本身") != std::string::npos &&
              section.find("不导出根密钥、密码或任何恢复材料") !=
                  std::string::npos,
          "the section does not carry the migration wording by its title");

  // Every call that copies bytes on the drawing thread paints the disabled
  // state first: a transfer between containers, a plaintext export, and a
  // whole-container migration. The count is here so a fourth one cannot be
  // added without deciding about this.
  require(count_occurrences(
              script, "await new Promise((resolve) => { setTimeout(resolve, "
                      "0); });") == 3,
          "a copy starts before the page looks busy");
}

// The migration export: a whole container, still encrypted, for a user moving
// to another machine.
void test_the_migration_export_says_what_it_does_and_does_not_do() {
  const std::string page = read_source(USB_VAULT_CARD_PRODUCT_UI_PAGE);
  const std::string script = read_source(USB_VAULT_CARD_PRODUCT_UI_SCRIPT);
  const std::string section = dashboard_section(page, "backup");

  require(section.find("id=\"migrate-container-list\"") != std::string::npos &&
              section.find("id=\"backup-migrate-location\"") !=
                  std::string::npos &&
              section.find("id=\"backup-migrate-browse\"") !=
                  std::string::npos &&
              section.find("id=\"backup-migrate-confirm\"") !=
                  std::string::npos &&
              section.find("id=\"backup-migrate-status\"") !=
                  std::string::npos,
          "the migration export cannot pick a container, a location, or run");

  // 🔴 What a migrating user has to understand, in Q-04's own terms: this is
  // not a key backup and not a recovery channel, and a lost card is still a
  // lost vault.
  require(section.find("原样复制容器文件本身") != std::string::npos &&
              section.find("不导出根密钥、密码或任何恢复材料") !=
                  std::string::npos &&
              section.find("仍需原卡与原密码才能打开") !=
                  std::string::npos,
          "the migration export does not say what it is and is not");

  // 🔴 The plaintext export asks before it runs, and the question cannot be
  // clicked through: the user types the words. Q-04 acceptance item 1 asks
  // for a non-skippable confirmation, and with nothing about an export
  // written down anywhere this screen is the only place the user is told
  // what is about to happen. It came back on 2026-09-10, after the
  // 2026-09-09 decision had removed both screens Q-04 asked for.
  require(page.find("id=\"export-confirm-dialog\"") != std::string::npos &&
              page.find("请键入 <b>明文导出</b> 以确认") != std::string::npos &&
              script.find("const kExportConfirmPhrase = \"明文导出\";") !=
                  std::string::npos &&
              script.find(
                  "exportConfirmPhrase.value.trim() !== kExportConfirmPhrase") !=
                  std::string::npos,
          "the plaintext export can be confirmed without typing");

  // 🔴 Only the dialog runs it. The section button opens the question, so no
  // path reaches an export that was never asked about.
  require(script.find("backupExportConfirm.addEventListener(\"click\", () => { openExportConfirm(); });") !=
              std::string::npos &&
              script.find("closeExportConfirm();") != std::string::npos,
          "an export can start without the confirmation");

  require(count_occurrences(script, "bridge.exportContainerFile") == 1 &&
              script.find("await bridge.exportContainerFile(id, location);") !=
                  std::string::npos,
          "the migration export is not reached in exactly one place");

  // A mounted container is offered and says why it cannot go yet, rather than
  // being missing from a list the user expects to be complete.
  require(script.find("container.mounted ? \"已挂载\" : \"未挂载\"") !=
              std::string::npos &&
              script.find("container.id === migrateContainerId && !container.mounted") !=
                  std::string::npos,
          "a mounted container is hidden rather than explained");

  require(script.find("migrateLocation = \"\";\n    setMigrateStatus(") !=
              std::string::npos,
          "the page kept the migration location afterwards");
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
    test_dropping_a_container_is_confirmed_and_says_what_it_keeps();
    test_the_remember_switch_shows_what_the_native_side_holds();
    test_the_import_switch_is_stored_and_defaults_to_mounting_nothing();
    test_the_integration_fixture_page_carries_every_element_the_script_takes();
    test_card_reset_is_gated_and_states_its_cost();
    test_container_creation_form_is_bounded();
    test_hidden_container_form_is_symmetric_and_never_validated();
    test_container_collection_replaces_the_single_container_display();
    test_all_password_copies_have_fallback_wipes();
    test_strength_feedback_matches_the_native_length_baseline();
    test_initialization_feedback_is_stable_and_recoverable();
    test_visibility_controls_persist_until_sensitive_inputs_are_cleared();
    test_actions_are_serialized_and_external_changes_coalesce();
    test_the_file_manager_browses_a_container_without_naming_a_volume();
    test_deleting_is_permanent_and_asked_once_and_renaming_is_one_name();
    test_a_drag_stays_inside_the_containers_and_says_where_out_lives();
    test_the_backup_centre_owns_the_plaintext_export();
    test_the_migration_export_says_what_it_does_and_does_not_do();
    std::cout << "card_product_ui_page_contract_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_product_ui_page_contract_tests: FAIL: "
              << error.what() << '\n';
    return 1;
  }
}
