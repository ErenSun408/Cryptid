"use strict";
(async () => {
  const $ = (selector) => document.querySelector(selector);
  const all = (selector) => [...document.querySelectorAll(selector)];
  const tick = () => new Promise((resolve) => setTimeout(resolve, 0));
  const wait = async (condition) => {
    for (let i = 0; i < 100; i += 1) {
      if (condition()) return;
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    throw new Error("Timed out waiting for UI state");
  };
  const results = [];
  const check = (condition, message) => {
    if (!condition) throw new Error(message);
    results.push(message);
  };
  const home = "#dashboard-section-dashboard";
  const manager = "#management-containers";
  const action = (where, id, name) => $(
    `${where} [data-container-id="${id}"][data-container-action="${name}"]`);
  const order = (where) => all(`${where} [data-row-container]`).map((row) => row.dataset.rowContainer).join(",");
  try {
    await wait(() => !$("#authenticated-dashboard").hidden);
    check(!$("[data-section=containers]") && !$("#dashboard-section-containers"), "Old container route removed");
    check(order(home) === "1,2,3" && order(manager) === "1,2,3", "Shared container lists render");
    const firstContainerRow = $(home + " .container-row");
    check(firstContainerRow.lastElementChild === action(home, 1, "details") &&
      !action(home, 1, "details").classList.contains("shell-button"), "Disclosure is an unframed arrow at the end of the row");
    firstContainerRow.querySelector(".container-name").click();
    check(all(`${home} .container-details`).length === 1, "Details expand inline");
    $(home + " .container-row .container-facts").click();
    check(!$(home + " .container-details"), "Clicking row facts collapses details");
    $(home + " .container-row .container-grab").click();
    check(!$(home + " .container-details"), "Clicking the drag grip does not toggle details");
    check(action(home, 1, "forget").disabled && !action(home, 2, "forget").disabled,
      "Row removal is available only after unmounting");
    action(home, 2, "forget").click();
    check(!$("#forget-container-dialog").hidden && !$(home + " .container-details"),
      "Row removal opens its confirmation without toggling details");
    $("#forget-container-cancel").click();
    check(order(home) === "1,2,3", "Cancelling removal keeps the container listed");
    action(home, 1, "details").click();
    action(home, 2, "details").click();
    check(all(`${home} .container-details`).length === 1 && action(home, 1, "details").getAttribute("aria-expanded") === "false", "Only one details panel stays open");
    check($(home + " .container-details-grid").textContent.includes("AES-256-XTS") &&
      all(`${home} .container-details-grid .container-fact`).length === 6, "Details contain six allowed facts");
    check(!$(".container-details [data-container-action=unavailable]") &&
      !$(home + " .container-row [data-container-action=browse]") &&
      !$(manager + " [data-container-action=details]") && !$(manager + " [data-container-action=forget]"),
      "Management has no disclosure or removal actions");
    action(home, 2, "details").click();
    check(!$(home + " .container-details"), "Details collapse");

    const rows = all(`${home} .container-row`);
    const grip = rows[2].querySelector(".container-grab");
    grip.dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, button: 0, pointerId: 7 }));
    document.dispatchEvent(new PointerEvent("pointermove", { bubbles: true, pointerId: 7, clientY: rows[0].getBoundingClientRect().top - 1 }));
    document.dispatchEvent(new PointerEvent("pointerup", { bubbles: true, pointerId: 7 }));
    check(order(home) === "3,1,2" && order(manager) === "3,1,2", "Pointer reorder synchronizes both lists");
    $(home + " .container-grab").dispatchEvent(new KeyboardEvent("keydown", { bubbles: true, altKey: true, key: "ArrowDown" }));
    check(order(home) === "1,3,2" && order(manager) === "1,3,2", "Keyboard reorder shares the same order");
    const firstRow = $(home + " .container-row");
    firstRow.dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, button: 0, pointerId: 8 }));
    document.dispatchEvent(new PointerEvent("pointermove", { pointerId: 8, clientY: 999 }));
    document.dispatchEvent(new PointerEvent("pointerup", { pointerId: 8 }));
    check(order(home) === "1,3,2", "Row body does not start a reorder");

    $("[data-section=backup]").click();
    $("[data-section=settings]").click();
    const sizeSelect = $("#settings-window-size");
    const loggingSwitch = $('[data-setting="operation-logging"]');
    const historyNav = $('[data-section="history"]');
    check(loggingSwitch.getAttribute("aria-checked") === "true" && !historyNav.hidden,
      "Operation logging and its navigation are enabled by default");
    loggingSwitch.click();
    await wait(() => historyNav.hidden);
    historyNav.click();
    check(!window.navigationFixture.loggingEnabled &&
      $('#dashboard-section-settings').classList.contains("active"),
      "Disabling logging hides and blocks the history route");
    window.navigationFixture.failLogging = true;
    loggingSwitch.click();
    await tick();
    check(historyNav.hidden && loggingSwitch.getAttribute("aria-checked") === "false",
      "Failed logging preference change preserves native state");
    window.navigationFixture.failLogging = false;
    loggingSwitch.click();
    await wait(() => !historyNav.hidden);
    check(window.navigationFixture.loggingEnabled, "Re-enabling logging restores navigation");
    check(sizeSelect.value === "1", "Spacious is the default window preset");
    sizeSelect.value = "3";
    sizeSelect.dispatchEvent(new Event("change"));
    await tick();
    check(window.navigationFixture.windowSizes.at(-1) === 3,
      "Compact window preset reaches the native sink");
    sizeSelect.value = "1";
    sizeSelect.dispatchEvent(new Event("change"));
    check(sizeSelect.disabled, "Window preset disables during resize");
    await tick();
    check(window.navigationFixture.windowSizes.at(-1) === 1 && !sizeSelect.disabled,
      "Window size selection reaches the native preset sink");
    window.navigationFixture.failWindowSize = true;
    sizeSelect.value = "2";
    sizeSelect.dispatchEvent(new Event("change"));
    await tick();
    check(sizeSelect.value === "1" && $("#settings-window-size-status").textContent,
      "Failed resize restores the last applied preset and reports failure");
    window.navigationFixture.failWindowSize = false;
    $("[data-section=backup]").click();
    const selectAll = (id) => $(`[data-export-select-all="${id}"]`);
    selectAll(1).click();
    await wait(() => selectAll(1).classList.contains("on"));
    selectAll(3).click();
    await wait(() => selectAll(3).classList.contains("on"));
    selectAll(1).click();
    check(!selectAll(1).classList.contains("on") && selectAll(3).classList.contains("on"),
      "Collapsed select-all can clear only its own container");
    selectAll(1).click();
    await wait(() => selectAll(1).classList.contains("on"));
    $('[data-export-toggle="1"] .xrow-open').click();
    await wait(() => all("#export-container-list .xfile").length === 2);
    check(all("#export-container-list .xfile .cbx.on").length === 2,
      "Selecting again while collapsed selects all files");
    selectAll(1).click();
    check(!all("#export-container-list .xfile .cbx.on").length,
      "Expanded select-all still clears the file selection");
    $("#backup-export-location").click();
    await tick();
    check($("#backup-export-location").textContent === "D:\\Export" && window.navigationFixture.pickerCalls === 1,
      "Plain export location opens the directory picker");
    window.navigationFixture.chosenLocation = "";
    $("#backup-export-browse").click();
    await tick();
    check($("#backup-export-location").textContent === "D:\\Export",
      "Cancelling the picker preserves the selected location");
    $("#export-mode-migrate").click();
    window.navigationFixture.chosenLocation = "D:\\Migration";
    $("#backup-migrate-location").click();
    await tick();
    check($("#backup-migrate-location").textContent === "D:\\Migration" &&
      $("#backup-export-location").textContent === "D:\\Export" && window.navigationFixture.pickerCalls === 3,
      "Migration location uses the picker and stays separate from plain export");
    $("[data-section=files]").click();
    check(!$(manager).hidden && $("#management-files").hidden, "Management navigation starts at containers");
    $(manager + ' [data-row-container="1"] .container-name').click();
    await wait(() => $("[data-file-enter]"));
    check($(manager).hidden && !$("#management-files").hidden, "Open enters the file level");
    check($("#file-breadcrumb").closest(".section-header") &&
      $("#file-breadcrumb h2 [data-file-back]") &&
      !$("[data-file-depth='0']").disabled, "Title and clickable path share the header");
    check(all("#file-list-body [data-file-action]").every((button) =>
      ["export", "delete"].includes(button.dataset.fileAction) && button.textContent === "" &&
      button.getAttribute("aria-label") && button.querySelector("svg")), "File actions are accessible export and delete icons only");
    check($("#file-list-body .file-glyph.directory") && $("#file-list-body .file-glyph:not(.directory)"), "Folder and file icons appear beside names");
    const fileRow = $('#file-list-body [data-file-drag="报告.xlsx"]');
    fileRow.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, clientX: 20, clientY: 20 }));
    check(!$("#file-context-menu").hidden && all("#file-context-menu [role=menuitem]").length === 1, "Right click opens a rename-only menu");
    $("#file-context-rename").click();
    check(!$("#file-name-dialog").hidden && $("#file-name-input").value === "报告.xlsx", "Context rename targets the selected file");
    $("#file-name-cancel").click();
    fileRow.dispatchEvent(new KeyboardEvent("keydown", { bubbles: true, key: "F10", shiftKey: true }));
    document.dispatchEvent(new KeyboardEvent("keydown", { bubbles: true, key: "Escape" }));
    check($("#file-context-menu").hidden && document.activeElement === fileRow, "Keyboard menu dismisses and restores row focus");
    fileRow.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true }));
    $("[data-file-enter]").click();
    await tick();
    check(window.navigationFixture.calls.some(([id, dir]) => id === 1 && dir === "文档"), "Nested browsing uses container ID and relative directory");
    check($("#file-context-menu").hidden && !$("[data-file-depth='1']").disabled, "Navigation closes the menu and keeps every path segment clickable");
    window.navigationFixture.listingSuffix = "\u001e外部新增.txt\u001f0\u001f0\u001f10\u001f0";
    $("#file-refresh").click();
    check($("#file-refresh").disabled && $("#file-refresh").getAttribute("aria-busy") === "true", "Refresh is disabled while the directory is loading");
    await wait(() => $('#file-list-body [data-file-drag="外部新增.txt"]'));
    check(JSON.stringify(window.navigationFixture.calls.at(-1)) === '[1,"文档"]' &&
      $("#file-breadcrumb").textContent.endsWith(">文档"), "Manual refresh reads external changes in the current directory");
    window.navigationFixture.listingSuffix = "";
    $("#file-refresh").click();
    await wait(() => !$('#file-list-body [data-file-drag="外部新增.txt"]'));
    check(!$("#file-refresh").disabled, "Refresh removes externally deleted entries and becomes available again");
    $("[data-file-depth='0']").click();
    await wait(() => $("[data-file-enter]"));
    check($("#file-breadcrumb").textContent === "容器管理>工作资料", "Container breadcrumb returns to the root");
    $("[data-file-back]").click();
    check(!$(manager).hidden && $("#management-files").hidden, "Breadcrumb returns to container list");
    $(manager + ' [data-row-container="2"]').click();
    await tick();
    check($("#file-list-body").textContent.includes("这个容器没有挂载"), "Unmounted containers remain reachable");
    check($("#file-mount-toggle").textContent === "挂载" && $("#file-create-directory").disabled,
      "Unmounted file view offers mounting and disables file creation");
    $("#file-mount-toggle").click();
    check($("#file-mount-toggle").disabled, "Mount controls disable while the action is running");
    await wait(() => $("[data-file-enter]") && $("#file-mount-toggle").textContent === "卸载");
    check(window.navigationFixture.containers[1].mounted && $("#file-mount-toggle").classList.contains("danger") &&
      !$("#management-files").hidden, "Toolbar mounts the current container and stays in its file view");
    window.navigationFixture.failUnmount = true;
    $("#file-mount-toggle").click();
    await wait(() => !$("#file-mount-toggle").disabled);
    check($("#file-mount-toggle").textContent === "卸载" && window.navigationFixture.containers[1].mounted,
      "Refused unmount keeps the actual mounted state");
    window.navigationFixture.failUnmount = false;
    $("#file-mount-toggle").click();
    await wait(() => $("#file-mount-toggle").textContent === "挂载" && !$("[data-file-enter]"));
    check(!window.navigationFixture.containers[1].mounted && window.navigationFixture.containers[0].mounted &&
      $("#file-refresh").disabled && !$("#management-files").hidden, "Toolbar unmounts only the current container and clears its listing");
    $("[data-file-back]").click();

    window.navigationFixture.delayListing = true;
    $(manager + ' [data-row-container="1"]').click();
    await wait(() => window.navigationFixture.pending);
    $("[data-file-back]").click();
    window.navigationFixture.pending("0\u001e迟到文件\u001f0\u001f0\u001f1\u001f0");
    await tick();
    check(!$("#file-list-body").textContent.includes("迟到文件"), "Late directory result cannot restore departed view");
    window.navigationFixture.state = 0;
    window.dispatchEvent(new Event("usb-vault-session-changed"));
    await wait(() => $("#authenticated-dashboard").hidden);
    check(!$(".container-details") && !$("[data-file-enter]"), "Session loss clears details and file contents");
    window.navigationFixture.state = 3;
    window.dispatchEvent(new Event("usb-vault-session-changed"));
    await wait(() => !$("#authenticated-dashboard").hidden);
    check(order(home) === "1,2,3" && !$(manager).hidden, "New session resets navigation and page-local order");
    document.title = `PASS: ${results.length} navigation checks`;
  } catch (error) {
    document.title = `FAIL: ${error.message}`;
    results.push(`FAIL: ${error.stack}`);
  }
  const output = document.createElement("pre");
  output.id = "navigation-test-results";
  output.textContent = document.title + "\n" + results.join("\n");
  document.body.prepend(output);
})();
