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

    $('[data-create-container]').click();
    $('[data-create-kind="hidden"]').click();
    $('#create-volume-continue').click();
    const enterCreationValue = (id, value) => {
      $(id).value = value;
      $(id).dispatchEvent(new Event("input", { bubbles: true }));
    };
    enterCreationValue('#create-container-b-name', 'review-test');
    enterCreationValue('#create-container-b-directory', 'D:\\Containers');
    enterCreationValue('#create-container-b-second', 'test-decoy-password');
    enterCreationValue('#create-container-b-second-confirmation', 'test-decoy-password');
    $('#create-volume-continue').click();
    const review = $('#hidden-create-review');
    const reviewPassword = $('#hidden-create-review-password');
    const reviewConfirm = $('#hidden-create-review-confirm');
    check(!review.hidden && reviewConfirm.disabled && !window.navigationFixture.hiddenCreateCalls,
      "Hidden container creation opens acknowledgement before any native call");
    check(reviewPassword.type === "password" && reviewPassword.value === 'test-decoy-password',
      "Review shows the current decoy password masked by default");
    $('#hidden-create-review-visibility').click();
    check(reviewPassword.type === "text", "Review eye reveals the current password");
    $('#hidden-create-review-cancel').click();
    check(review.hidden && reviewPassword.value === "" && reviewPassword.type === "password",
      "Returning to edit clears and remasks the review password");
    $('#create-volume-continue').click();
    check(reviewConfirm.disabled, "Reopening review requires fresh acknowledgement");
    $('#create-container-b-acknowledge').click();
    reviewConfirm.click();
    await wait(() => window.navigationFixture.pendingCreation);
    check(!$('#creation-progress').hidden && $('#create-container-b-form').hidden &&
      $('#create-volume-dialog').dataset.running === "true",
      "Hidden creation replaces parameters with a running view");
    const initialElapsed = $('#creation-progress-time').textContent;
    check(!$('#creation-progress-authorize').classList.contains("done"),
      "Authorization is not marked complete before native confirmation");
    window.navigationFixture.creationProgress = "1,3,41,0,3";
    await wait(() => $('#creation-progress-work .step-tail').textContent === "41%");
    check($('#creation-progress-authorize').classList.contains("done") &&
      $('#creation-progress-allocate').classList.contains("done") &&
      $('#creation-progress [role=progressbar]').getAttribute("aria-valuenow") === "41",
      "Native authorization allocation and format percentage drive the steps");
    await new Promise((resolve) => setTimeout(resolve, 1100));
    check($('#creation-progress-time').textContent !== initialElapsed,
      "Creation elapsed time updates while native creation is pending");
    window.navigationFixture.failCancellation = true;
    $('#creation-cancel-work').click();
    await wait(() => !$('#creation-cancel-work').disabled);
    check($('#creation-cancel-work').textContent.includes('取消请求未确认'),
      "An unconfirmed cancel request can be retried without claiming cancellation");
    window.navigationFixture.failCancellation = false;
    $('#creation-cancel-work').click();
    await tick();
    check($('#creation-cancel-work').disabled &&
      $('#creation-cancel-work').textContent === '正在取消…' &&
      $('#create-volume-dialog').dataset.running === 'true',
      "An accepted cancel request waits for the actual creation outcome");
    window.navigationFixture.pendingCreation.reject(new Error("simulated creation failure"));
    window.navigationFixture.pendingCreation = null;
    await wait(() => window.navigationFixture.hiddenCreateCalls === 1 &&
      !$('#create-volume-close').disabled);
    check(review.hidden && reviewPassword.value === "" && $('#create-container-b-second').value === "" &&
      $('#create-container-b-second-confirmation').value === "",
      "Confirmed creation calls native once and clears passwords even on failure");
    check(!$('#creation-progress').hidden && $('#creation-progress').dataset.result === "error" &&
      $('#create-container-b-form').hidden && $('#create-volume-continue').hidden,
      "Failed creation keeps its result on the execution page");
    $('#create-volume-cancel').click();
    check($('#creation-progress').hidden && $('#creation-progress-time').textContent === "",
      "Failed creation stops and clears the running view");
    $('[data-create-container]').click();
    $('[data-create-kind="encrypted"]').click();
    $('#create-volume-continue').click();
    enterCreationValue('#create-container-name', 'standard-review');
    enterCreationValue('#create-container-directory', 'D:\\Containers');
    $('#create-volume-continue').click();
    await wait(() => window.navigationFixture.pendingCreation);
    check(!$('#creation-progress').hidden && $('#create-container-form').hidden,
      "Standard creation uses the same running view");
    check(!$('#creation-cancel-work').disabled,
      "A new creation restores cancellation after the previous request");
    window.navigationFixture.acceptCancellation = false;
    $('#creation-cancel-work').click();
    await tick();
    check($('#creation-cancel-work').disabled &&
      $('#creation-cancel-work').textContent.includes('已无法取消'),
      "A native rejection closes cancellation even before the next progress poll");
    window.navigationFixture.acceptCancellation = true;
    window.navigationFixture.creationProgress = "1,4,0,0,3";
    await wait(() => $('#creation-progress-check .step-tail').textContent === "挂载中");
    check(['authorize', 'allocate', 'work'].every((name) => {
      const step = $('#creation-progress-' + name);
      return step.className === "done" && !step.hasAttribute("aria-current") &&
        getComputedStyle(step.querySelector('.step-mark')).animation === "none";
    }) && $('#creation-progress-allocate .step-tail').textContent === "已分配" &&
      $('#creation-progress-work .step-tail').textContent === "已完成" &&
      $('#creation-progress-check').className === "current",
      "Fast creation skips directly to mounting with completed non-spinning prior steps");
    check($('#creation-cancel-work').disabled &&
      $('#creation-cancel-work').textContent.includes('已无法取消'),
      "Publication progress keeps cancellation disabled");
    window.navigationFixture.pendingCreation.resolve();
    window.navigationFixture.pendingCreation = null;
    await wait(() => $('#create-volume-status').dataset.tone === "success");
    check(!$('#creation-progress').hidden && $('#creation-progress').dataset.result === "success" &&
      $('#create-volume-dialog').dataset.running === "false" && $('#create-container-form').hidden &&
      $('#creation-progress-check .step-tail').textContent === "Z:" && $('#create-volume-continue').hidden,
      "Successful creation keeps mount results on the execution page");
    $('#create-volume-close').click();
    for (const kind of ['encrypted', 'hidden']) {
      // Reuse the same name and authenticated page; success immediately
      // after cancellation must not inherit its terminal state or controls.
      for (const status of [3, 4, 2, 5]) {
        $('[data-create-container]').click();
        $(`[data-create-kind="${kind}"]`).click();
        $('#create-volume-continue').click();
        const prefix = kind === 'hidden' ? '#create-container-b' : '#create-container';
        enterCreationValue(prefix + '-name', 'outcome-check');
        enterCreationValue(prefix + '-directory', 'D:\\Containers');
        if (kind === 'hidden') {
          enterCreationValue(prefix + '-second', 'test-decoy-password');
          enterCreationValue(prefix + '-second-confirmation', 'test-decoy-password');
        }
        $('#create-volume-continue').click();
        if (kind === 'hidden') {
          $('#create-container-b-acknowledge').click();
          $('#hidden-create-review-confirm').click();
        }
        await wait(() => window.navigationFixture.pendingCreation);
        window.navigationFixture.creationProgress = status === 2
          ? "2,5,100,90,3" : `${status},3,41,0,3`;
        window.navigationFixture.pendingCreation = null;
        await wait(() => !$('#create-volume-close').disabled);
        const outcome = $('#create-volume-status');
        check(outcome.dataset.tone === (status === 2 ? 'success' : status === 4 ? 'cancelled' : 'error') &&
          outcome.textContent.includes('没有留下容器文件') === (status === 4) &&
          outcome.textContent.includes('未能确认临时文件已清理') === (status === 5),
          `${kind} creation distinguishes failure, confirmed cancellation and cleanup failure (${status})`);
        if (status === 2) {
          check($('#creation-progress').dataset.result === 'success' &&
            $('#creation-progress-check .step-tail').textContent === 'Z:',
            `${kind} same-name retry after cancellation shows a fresh successful mount`);
        }
        $('#create-volume-close').click();
      }
    }
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
    window.navigationFixture.delayVolume = true;
    $("#file-mount-toggle").click();
    check($("#file-mount-toggle").disabled, "Mount controls disable while the action is running");
    check(!$("#volume-operation-dialog").hidden && $("#volume-operation-title").textContent === "正在挂载 影像存档 …" &&
      !$("#volume-operation-dialog [role=progressbar]").hasAttribute("aria-valuenow"), "Mount immediately opens indeterminate progress");
    await wait(() => window.navigationFixture.completeVolume);
    const starts = window.navigationFixture.volumeStarts;
    $("#file-mount-toggle").click();
    $("#volume-operation-dialog").dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
    $("#volume-operation-dialog").dispatchEvent(new MouseEvent("click", { bubbles: true }));
    check(window.navigationFixture.volumeStarts === starts && !$("#volume-operation-dialog").hidden,
      "Pending operation prevents duplicate dispatch, Escape, and outside-click dismissal");
    window.navigationFixture.completeVolume();
    window.navigationFixture.delayVolume = false;
    await wait(() => $("[data-file-enter]") && $("#file-mount-toggle").textContent === "卸载");
    check(window.navigationFixture.containers[1].mounted && $("#file-mount-toggle").classList.contains("danger") &&
      !$("#management-files").hidden, "Toolbar mounts the current container and stays in its file view");
    window.navigationFixture.failUnmount = true;
    check(!$("#volume-operation-dialog").hidden && $("#volume-operation-title").textContent === "挂载成功" &&
      $("#volume-operation-note").textContent.includes("影像存档") && !$("#volume-operation-confirm").hidden,
      "Successful mount shows its named result and explicit confirmation");
    $("#volume-operation-confirm").click();
    check($("#volume-operation-dialog").hidden, "Confirmation dismisses success immediately");
    $("#file-mount-toggle").click();
    check(!$("#volume-operation-dialog").hidden && $("#volume-operation-title").textContent === "正在卸载 影像存档 …",
      "Unmount immediately opens progress");
    await wait(() => !$("#file-mount-toggle").disabled);
    check($("#file-mount-toggle").textContent === "卸载" && window.navigationFixture.containers[1].mounted,
      "Refused unmount keeps the actual mounted state");
    check(!$("#volume-operation-dialog").hidden && $("#volume-operation-title").textContent === "卸载失败" &&
      $("#volume-operation-note").textContent.includes("占用"), "Failed unmount displays its native reason");
    await new Promise((resolve) => setTimeout(resolve, 3100));
    check(!$("#volume-operation-dialog").hidden, "Failure persists and an old success timer cannot dismiss it");
    $("#volume-operation-dialog").dispatchEvent(new MouseEvent("click", { bubbles: true }));
    check($("#volume-operation-dialog").hidden, "Clicking outside dismisses a failed result");
    for (const [code, reason] of [[5, "找不到"], [6, "访问被拒绝"], [7, "无法打开"], [8, "读取或写入"], [9, "驱动"], [3, "无法确定"]]) {
      window.navigationFixture.volumeFailureStatus = code;
      $("#file-mount-toggle").click();
      await wait(() => $("#volume-operation-dialog").dataset.result === "error");
      check($("#volume-operation-note").textContent.includes(reason), `Failure ${code} shows the bounded native reason`);
      check($("#volume-operation-force").hidden, `Failure ${code} cannot escalate to force`);
      $("#volume-operation-confirm").click();
    }
    window.navigationFixture.volumeFailureStatus = 0;
    $("#file-mount-toggle").click();
    await wait(() => $("#volume-operation-dialog").dataset.result === "error");
    check(!$("#volume-operation-force").hidden && $("#volume-operation-force").classList.contains("danger"),
      "Busy unmount exposes a red force button");
    $("#volume-operation-force").click();
    check($("#volume-operation-dialog").dataset.result === "confirm-force" &&
      $("#volume-operation-note").textContent.includes("文件系统可能损坏") && !window.navigationFixture.forceCalls,
      "Force requires separate risk confirmation before native work");
    $("#volume-operation-confirm").click();
    check(!window.navigationFixture.forceCalls && window.navigationFixture.containers[1].mounted,
      "Cancelling force never calls native cleanup");
    $("#file-mount-toggle").click();
    await wait(() => $("#volume-operation-dialog").dataset.result === "error");
    $("#volume-operation-force").click();
    window.navigationFixture.failForce = true;
    window.navigationFixture.volumeFailureStatus = 8;
    window.navigationFixture.delayVolume = true;
    window.navigationFixture.completeVolume = null;
    $("#volume-operation-force").click();
    await wait(() => window.navigationFixture.completeVolume);
    $("#volume-operation-force").click();
    check(window.navigationFixture.forceCalls === 1 && $("#volume-operation-dialog").dataset.result === "running",
      "Confirmed force runs once with progress");
    window.navigationFixture.completeVolume();
    window.navigationFixture.delayVolume = false;
    await wait(() => $("#volume-operation-dialog").dataset.result === "error");
    check(window.navigationFixture.containers[1].mounted && $("#volume-operation-note").textContent.includes("读取或写入"),
      "Failed force keeps mounted state and reports reason");
    $("#volume-operation-confirm").click();
    window.navigationFixture.failForce = false;
    window.navigationFixture.volumeFailureStatus = 0;
    $("#file-mount-toggle").click();
    await wait(() => $("#volume-operation-dialog").dataset.result === "error");
    $("#volume-operation-force").click();
    $("#volume-operation-force").click();
    await wait(() => $("#volume-operation-dialog").dataset.result === "success");
    check($("#volume-operation-title").textContent === "强制卸载成功" &&
      !window.navigationFixture.containers[1].mounted && window.navigationFixture.containers[0].mounted,
      "Successful force unmounts only the failed target");
    $("#volume-operation-confirm").click();
    $("#file-mount-toggle").click();
    await wait(() => $("#volume-operation-dialog").dataset.result === "success");
    $("#volume-operation-confirm").click();
    window.navigationFixture.failUnmount = false;
    $("#file-mount-toggle").click();
    await wait(() => $("#file-mount-toggle").textContent === "挂载" && !$("[data-file-enter]"));
    check(!window.navigationFixture.containers[1].mounted && window.navigationFixture.containers[0].mounted &&
      $("#file-refresh").disabled && !$("#management-files").hidden, "Toolbar unmounts only the current container and clears its listing");
    check($("#volume-operation-title").textContent === "卸载成功", "Unmount shows its own success label");
    $("#volume-operation-title").dispatchEvent(new MouseEvent("click", { bubbles: true }));
    check(!$("#volume-operation-dialog").hidden, "Clicking inside the result dialog keeps it open");
    await new Promise((resolve) => setTimeout(resolve, 2800));
    check(!$("#volume-operation-dialog").hidden && !$("#volume-operation-note").textContent.includes("关闭"),
      "Success remains visible before three seconds and never narrates its own timer");
    await wait(() => $("#volume-operation-dialog").hidden);
    check($("#volume-operation-title").textContent === "", "Success automatically closes and clears its name after three seconds");
    window.navigationFixture.failMount = true;
    $("#file-mount-toggle").click();
    await wait(() => $("#volume-operation-dialog").dataset.result === "error");
    check($("#volume-operation-title").textContent === "挂载失败" && !$("#volume-operation-retry").hidden &&
      $("#volume-operation-force").hidden, "Failed mount offers a retry and never the force button");
    window.navigationFixture.failMount = false;
    $("#volume-operation-retry").click();
    await wait(() => window.navigationFixture.containers[1].mounted);
    check($("#volume-operation-title").textContent === "挂载成功" && $("#volume-operation-retry").hidden,
      "Retry remounts the same container and withdraws itself on success");
    $("#volume-operation-confirm").click();
    $("#file-mount-toggle").click();
    await wait(() => !window.navigationFixture.containers[1].mounted);
    $("#volume-operation-confirm").click();
    $("[data-file-back]").click();

    window.navigationFixture.delayListing = true;
    $(manager + ' [data-row-container="1"]').click();
    await wait(() => window.navigationFixture.pending);
    $("[data-file-back]").click();
    window.navigationFixture.pending("0\u001e迟到文件\u001f0\u001f0\u001f1\u001f0");
    await tick();
    check(!$("#file-list-body").textContent.includes("迟到文件"), "Late directory result cannot restore departed view");
    window.navigationFixture.failUnmount = true;
    $("#dashboard-unmount").click();
    check(!$("#volume-operation-dialog").hidden, "Unified unmount shows the same progress dialog");
    await wait(() => $("#volume-operation-dialog").dataset.result === "error");
    check($("#dashboard-status").dataset.tone === "error" && window.navigationFixture.containers[0].mounted,
      "Unified unmount refusal is visible on the dashboard and preserves volumes");
    $("#volume-operation-force").click();
    check($("#volume-operation-title").textContent.includes("全部已挂载卷"), "Unified force names the full cleanup scope");
    const beforeCardChange = window.navigationFixture.forceCalls;
    window.dispatchEvent(new Event("usb-vault-session-changed"));
    await wait(() => $("#volume-operation-force").hidden);
    $("#volume-operation-force").click();
    check(window.navigationFixture.forceCalls === beforeCardChange, "Card change invalidates an open force confirmation");
    $("#volume-operation-confirm").click();
    await wait(() => !$("#dashboard-unmount").disabled);
    window.navigationFixture.failUnmount = false;
    window.navigationFixture.delayVolume = true;
    window.navigationFixture.completeVolume = null;
    $("#dashboard-unmount").click();
    await wait(() => window.navigationFixture.completeVolume);
    window.navigationFixture.state = 0;
    window.dispatchEvent(new Event("usb-vault-session-changed"));
    window.navigationFixture.completeVolume();
    await wait(() => $("#authenticated-dashboard").hidden);
    check($("#volume-operation-dialog").dataset.result === "error" &&
      !$("#volume-operation-note").textContent.includes("工作资料") && window.navigationFixture.containers.every((item) => !item.mounted),
      "Session change during unified unmount clears names and never claims success");
    $("#volume-operation-confirm").click();
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
