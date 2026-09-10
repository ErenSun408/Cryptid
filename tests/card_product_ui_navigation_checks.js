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
      $(manager + " .container-row [data-container-action=browse]"),
      "Removed detail actions stay absent and row opening belongs to management");
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

    $("[data-section=files]").click();
    check(!$(manager).hidden && $("#management-files").hidden, "Management navigation starts at containers");
    action(manager, 1, "browse").click();
    await wait(() => $("[data-file-enter]"));
    check($(manager).hidden && !$("#management-files").hidden, "Open enters the file level");
    $("[data-file-enter]").click();
    await tick();
    check(window.navigationFixture.calls.some(([id, dir]) => id === 1 && dir === "文档"), "Nested browsing uses container ID and relative directory");
    $("[data-file-back]").click();
    check(!$(manager).hidden && $("#management-files").hidden, "Breadcrumb returns to container list");
    action(manager, 2, "details").click();
    action(manager, 2, "browse").click();
    await tick();
    check($("#file-list-body").textContent.includes("这个容器没有挂载"), "Unmounted containers remain reachable");
    $("[data-file-back]").click();

    window.navigationFixture.delayListing = true;
    action(manager, 1, "browse").click();
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
