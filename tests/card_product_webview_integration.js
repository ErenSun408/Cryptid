"use strict";

// This fixture deliberately keeps its native window hidden. Chromium can
// suspend animation frames there; use a timer for paint-yield scheduling.
// This exercises the real COM bridge, not visual rendering or frame timing.
window.requestAnimationFrame = callback => setTimeout(() => callback(performance.now()), 16);

chrome.webview.postMessage("driver-loaded");

(async () => {
  const scrollProbe = document.createElement("div");
  scrollProbe.style.cssText = "position:fixed;left:0;top:0;width:120px;height:80px;overflow:auto;visibility:hidden;";
  const scrollContent = document.createElement("div");
  scrollContent.style.cssText = "width:240px;height:160px";
  scrollProbe.appendChild(scrollContent);
  document.body.appendChild(scrollProbe);
  const overlay = scrollProbe.clientWidth === 120 && scrollProbe.clientHeight === 80;
  scrollProbe.scrollTop = 20;
  scrollProbe.scrollLeft = 20;
  const scrollable = scrollProbe.scrollTop === 20 && scrollProbe.scrollLeft === 20;
  scrollProbe.remove();
  if (!overlay || !scrollable) throw new Error("overlay scrollbars changed content dimensions or disabled scrolling");
  chrome.webview.postMessage("overlay-layout-passed");

  const surface = document.getElementById("card-session-surface");
  const password = document.getElementById("card-password");
  const passwordVisibility =
    document.getElementById("card-password-visibility");
  const confirmation = document.getElementById("card-password-confirmation");
  const confirmationVisibility =
    document.getElementById("card-password-confirmation-visibility");
  const confirmationField = document.getElementById("confirmation-field");
  const strength = document.getElementById("password-strength");
  const action = document.getElementById("card-session-action");
  const actionLabel = document.getElementById("card-session-action-label");
  const actionSpinner = document.getElementById("card-session-spinner");
  const status = document.getElementById("card-session-status");
  const error = document.getElementById("card-session-error");
  const dashboard = document.getElementById("authenticated-dashboard");
  const dashboardUnmount = document.getElementById("dashboard-unmount");
  const dashboardVolumeState =
    document.getElementById("dashboard-volume-state");
  const dashboardContainerCount =
    document.getElementById("dashboard-container-count");
  const containersNavigation =
    document.querySelector('[data-section="files"]');
  const containersSection =
    document.getElementById("dashboard-section-files");
  const containersCreate = containersSection.querySelector(
    "[data-create-container]");
  const containersList = containersSection.querySelector(
    "[data-container-list]");
  const settingsNavigation =
    document.querySelector('[data-section="settings"]');
  const settingsSection =
    document.getElementById("dashboard-section-settings");
  const historyNavigation =
    document.querySelector('[data-section="history"]');
  const historySection =
    document.getElementById("dashboard-section-history");
  const historyBody = document.getElementById("operation-history-body");
  const historyClear = document.getElementById("operation-history-clear");
  const historyRows = () => Array.from(historyBody.querySelectorAll("tr"));
  const historyCells = (row) =>
    Array.from(row.querySelectorAll("td")).map((cell) => cell.textContent);
  const containerRows = () =>
    Array.from(containersList.querySelectorAll(".container-row"));
  const containerFacts = (row) =>
    Array.from(row.querySelectorAll(".container-fact-value"))
      .map((fact) => fact.textContent);
  const creationDialog = document.getElementById("create-volume-dialog");
  const creationTitle = document.getElementById("create-volume-title");
  const creationCancel = document.getElementById("create-volume-cancel");
  const creationCancelLabel =
    document.getElementById("create-volume-cancel-label");
  const creationContinue = document.getElementById("create-volume-continue");
  const creationName = document.getElementById("create-container-name");
  const creationNameError =
    document.getElementById("create-container-name-error");
  const creationDirectory =
    document.getElementById("create-container-directory");
  const creationDirectoryError =
    document.getElementById("create-container-directory-error");
  const creationBrowse = document.getElementById("create-container-browse");
  const creationCapacity =
    document.getElementById("create-container-capacity");
  const standardPick = document.querySelector(
    '[data-create-kind="encrypted"]');
  const hiddenPick = document.querySelector('[data-create-kind="hidden"]');
  const typeStep = document.getElementById("create-type-step");
  const hiddenCapacity =
    document.getElementById("create-container-b-capacity");
  const hiddenName = document.getElementById("create-container-b-name");
  const hiddenDirectory =
    document.getElementById("create-container-b-directory");
  const hiddenBrowse = document.getElementById("create-container-b-browse");
  const hiddenInnerCapacity =
    document.getElementById("create-container-b-hidden-capacity");
  const hiddenSecond = document.getElementById("create-container-b-second");
  const hiddenSecondConfirmation =
    document.getElementById("create-container-b-second-confirmation");
  const hiddenError = document.getElementById("create-container-b-error");
  const hiddenAcknowledge =
    document.getElementById("create-container-b-acknowledge");
  const capacityUnit = document.getElementById("create-container-unit");
  const capacityUnitList = capacityUnit.parentElement
    .querySelector(".unit-list");
  const capacityUnitOption = (unit) => capacityUnit.parentElement
    .querySelector(`.unit-option[data-unit="${unit}"]`);
  const capacityError = document.getElementById("create-container-error");
  const creationStatus = document.getElementById("create-volume-status");
  const instruction = "请输入并确认日常密码，至少12字符。";

  const waitFor = (condition) => {
    if (condition()) {
      return Promise.resolve(true);
    }
    return new Promise((resolve) => {
      let settled = false;
      const observer = new MutationObserver(() => {
        if (condition()) {
          finish(true);
        }
      });
      const timeout = setTimeout(() => finish(false), 5000);
      const finish = (matched) => {
        if (settled) return;
        settled = true;
        clearTimeout(timeout);
        observer.disconnect();
        resolve(matched);
      };
      observer.observe(document.body, {
        attributes: true,
        childList: true,
        characterData: true,
        subtree: true,
      });
      if (condition()) finish(true);
    });
  };
  const waitForState = (expected) => waitFor(() =>
    surface.dataset.sessionState === expected &&
    surface.dataset.busy === "false");
  const request = (message) => chrome.webview.postMessage(message);

  if (!await waitForState("no-card") || !action.hidden) {
    request("fail-no-card");
    return;
  }

  request("request-initialization");
  if (!await waitForState("initialization-required") ||
      confirmationField.hidden || confirmation.disabled ||
      passwordVisibility.disabled || confirmationVisibility.disabled ||
      !strength.hidden || !action.disabled) {
    request("fail-initialization-route");
    return;
  }

  password.value = "abcdefghijk";
  password.dispatchEvent(new Event("input", { bubbles: true }));
  if (!await waitFor(() => !strength.hidden &&
      strength.dataset.level === "invalid" &&
      strength.dataset.score === "1" && action.disabled)) {
    request("fail-short-password-state");
    return;
  }
  passwordVisibility.click();
  confirmation.focus();
  if (password.type !== "text" ||
      passwordVisibility.getAttribute("aria-pressed") !== "true") {
    request("fail-password-visibility-persistence");
    return;
  }
  passwordVisibility.click();

  password.value = "abcdefghijkl";
  password.dispatchEvent(new Event("input", { bubbles: true }));
  confirmation.value = "abcdefghijkX";
  confirmation.dispatchEvent(new Event("input", { bubbles: true }));
  action.click();
  if (!await waitFor(() => surface.dataset.sessionState ===
      "initialization-required" && surface.dataset.busy === "false" &&
      !error.hidden &&
      error.textContent === "初始化失败，两次密码不一致。" &&
      status.textContent === instruction &&
      password.value === "abcdefghijkl" &&
      confirmation.value === "abcdefghijkX")) {
    request("fail-initialization-mismatch");
    return;
  }
  confirmation.value = "abcdefghijkl";
  confirmation.dispatchEvent(new Event("input", { bubbles: true }));
  if (!error.hidden || error.textContent !== "" ||
      status.textContent !== instruction) {
    request("fail-initialization-error-clear");
    return;
  }

  password.value = "abcdefghijke\u0301";
  password.dispatchEvent(new Event("input", { bubbles: true }));
  confirmation.value = "abcdefghijk\u00e9";
  confirmation.dispatchEvent(new Event("input", { bubbles: true }));
  confirmationVisibility.click();
  password.focus();
  if (confirmation.type !== "text" ||
      confirmationVisibility.getAttribute("aria-pressed") !== "true") {
    request("fail-confirmation-visibility-persistence");
    return;
  }
  confirmationVisibility.click();
  if (!await waitFor(() => !strength.hidden &&
      strength.dataset.level === "acceptable" &&
      strength.dataset.score === "2" && !action.disabled)) {
    request("fail-strength");
    return;
  }
  action.click();
  if (!await waitForState("authenticated") ||
      password.value !== "" || confirmation.value !== "" || !error.hidden ||
      password.type !== "password" || confirmation.type !== "password" ||
      passwordVisibility.getAttribute("aria-pressed") !== "false" ||
      confirmationVisibility.getAttribute("aria-pressed") !== "false" ||
      !action.hidden || !surface.hidden || dashboard.hidden ||
      dashboardUnmount.hidden || dashboardUnmount.disabled || dashboardVolumeState.textContent !== "未挂载" ||
      dashboardContainerCount.textContent !== "0" ||
      containersCreate.hidden || containerRows().length !== 0) {
    request("fail-initialization-submit");
    return;
  }

  containersNavigation.click();
  if (!containersNavigation.classList.contains("active") ||
      !containersSection.classList.contains("active")) {
    request("fail-dashboard-navigation");
    return;
  }

  settingsNavigation.click();
  if (!settingsSection.classList.contains("active") ||
      containersSection.classList.contains("active")) {
    request("fail-settings-navigation");
    return;
  }
  // Stored switches settle after native readback; the locked one stays on.
  const autoMount = settingsSection.querySelector('[data-setting="auto-mount"]');
  const rememberContainers =
    settingsSection.querySelector('[data-setting="remember-containers"]');
  const mountImported =
    settingsSection.querySelector('[data-setting="mount-imported"]');
  const lockedUnmount = settingsSection.querySelector('[aria-disabled="true"]');
  if (autoMount.getAttribute("aria-checked") !== "false" ||
      rememberContainers.getAttribute("aria-checked") !== "true" ||
      mountImported.getAttribute("aria-checked") !== "false" ||
      lockedUnmount.getAttribute("aria-checked") !== "true") {
    request("fail-settings-defaults");
    return;
  }
  autoMount.click();
  lockedUnmount.click();
  if (!await waitFor(() => autoMount.getAttribute("aria-checked") === "true" && autoMount.getAttribute("aria-disabled") !== "true") ||
      lockedUnmount.getAttribute("aria-checked") !== "true") {
    request("fail-settings-switch");
    return;
  }
  autoMount.click();
  if (!await waitFor(() => autoMount.getAttribute("aria-checked") === "false" && autoMount.getAttribute("aria-disabled") !== "true")) {
    request("fail-settings-switch");
    return;
  }
  // 记住安全盘位置 is the one switch with a store behind it. The click carries an
  // instruction to the native side and the position comes back from there, so
  // it settles a turn later rather than in place.
  rememberContainers.click();
  if (!await waitFor(
      () => rememberContainers.getAttribute("aria-checked") === "false" && rememberContainers.getAttribute("aria-disabled") !== "true")) {
    request("fail-settings-remember");
    return;
  }
  rememberContainers.click();
  if (!await waitFor(
      () => rememberContainers.getAttribute("aria-checked") === "true" && rememberContainers.getAttribute("aria-disabled") !== "true")) {
    request("fail-settings-remember");
    return;
  }
  // The import switch is the same shape, and starts from the other position.
  mountImported.click();
  if (!await waitFor(
      () => mountImported.getAttribute("aria-checked") === "true" && mountImported.getAttribute("aria-disabled") !== "true")) {
    request("fail-settings-remember");
    return;
  }
  containersNavigation.click();
  if (!containersSection.classList.contains("active")) {
    request("fail-settings-navigation");
    return;
  }
  // The single entry opens on the type step: both shapes are offered, the
  // standard one is preselected, and nothing is asked for until 下一步.
  containersCreate.click();
  document.querySelector('[data-create-storage="file"]').click();
  if (creationDialog.hidden || typeStep.hidden ||
      creationTitle.textContent !== "新建安全盘" ||
      creationContinue.textContent !== "下一步" ||
      creationCancelLabel.textContent !== "取消" ||
      standardPick.getAttribute("aria-checked") !== "true" ||
      hiddenPick.getAttribute("aria-checked") !== "false" ||
      creationContinue.disabled) {
    request("fail-creation-type-step");
    return;
  }
  const typeInto = (input, value) => {
    input.value = value;
    input.dispatchEvent(new Event("input", { bubbles: true }));
  };
  creationContinue.click();
  // The form opens empty and stays uncommittable until it is named and placed:
  // there is no dialog left to supply either.
  if (!typeStep.hidden || creationTitle.textContent !== "新建标准安全盘" ||
      creationCancelLabel.textContent !== "上一步" ||
      creationContinue.textContent !== "创建安全盘" ||
      creationName.value !== "" || creationDirectory.value !== "" ||
      creationCapacity.value !== "64" || capacityUnit.dataset.unit !== "mib" ||
      !creationContinue.disabled || creationNameError.hidden ||
      creationNameError.textContent !== "请填写名称。") {
    request("fail-creation-entry");
    return;
  }
  // 上一步 returns to the picker and clears everything it collected, the
  // location included.
  typeInto(creationName, "scratch");
  typeInto(creationCapacity, "128");
  creationCancel.click();
  if (typeStep.hidden || creationName.value !== "" ||
      creationDirectory.value !== "" || creationCapacity.value !== "64" ||
      creationTitle.textContent !== "新建安全盘") {
    request("fail-creation-back-step");
    return;
  }
  creationContinue.click();
  if (!typeStep.hidden || creationCapacity.value !== "64" ||
      !creationContinue.disabled) {
    request("fail-creation-entry");
    return;
  }
  // A name a file name cannot carry is named under its own field and never
  // reaches the bridge.
  typeInto(creationName, "sub\\integration");
  if (!creationContinue.disabled || creationNameError.hidden ||
      creationNameError.textContent !==
        "名称不能包含 \\ / : * ? \" < > | 或控制字符。") {
    request("fail-creation-location");
    return;
  }
  typeInto(creationName, "integration");
  if (!creationNameError.hidden) {
    request("fail-creation-location");
    return;
  }
  // So is a save location that is not a drive-absolute path.
  typeInto(creationDirectory, "Vault");
  if (!creationContinue.disabled || creationDirectoryError.hidden ||
      creationDirectoryError.textContent !==
        "需要本机盘符路径，例如 D:\\Vault。") {
    request("fail-creation-location");
    return;
  }
  // 浏览 only fills the field. The value it returns is checked exactly like a
  // typed one, and the run owns the directory it names.
  creationBrowse.click();
  if (!await waitFor(() => surface.dataset.busy === "false" &&
      /^[A-Za-z]:\\/.test(creationDirectory.value) &&
      creationDirectoryError.hidden && !creationContinue.disabled)) {
    request("fail-creation-browse");
    return;
  }
  typeInto(creationCapacity, "19");
  if (!creationContinue.disabled || capacityError.hidden ||
      capacityError.textContent !== "容量低于最小值。") {
    request("fail-creation-validation");
    return;
  }
  // 1 TiB is the largest container, so two of them in GiB is over the top.
  creationCapacity.value = "2048";
  creationCapacity.dispatchEvent(new Event("input", { bubbles: true }));
  capacityUnit.click();
  if (capacityUnitList.hidden) {
    request("fail-creation-unit");
    return;
  }
  capacityUnitOption("gib").click();
  if (!capacityUnitList.hidden || capacityUnit.dataset.unit !== "gib" ||
      capacityUnit.querySelector(".unit-name").textContent !== "GiB" ||
      !creationContinue.disabled || capacityError.hidden ||
      capacityError.textContent !== "容量超出最大值。") {
    request("fail-creation-unit");
    return;
  }
  // The same field in GiB has to reach the bridge multiplied by 1024.
  creationCapacity.value = "1";
  creationCapacity.dispatchEvent(new Event("input", { bubbles: true }));
  if (creationContinue.disabled || !capacityError.hidden) {
    request("fail-creation-unit");
    return;
  }
  capacityUnit.click();
  capacityUnitOption("mib").click();
  typeInto(creationCapacity, "64");
  creationContinue.click();
  // A failed creation clears the two location fields with everything else, so
  // the form is ready for another try but has to be filled in again.
  if (!await waitFor(() => surface.dataset.busy === "false" &&
      dashboardVolumeState.textContent === "未挂载" &&
      creationStatus.dataset.tone === "error" &&
      creationName.value === "" && creationDirectory.value === "" &&
      !creationName.disabled && !creationCapacity.disabled &&
      creationContinue.disabled && containerRows().length === 0)) {
    request("fail-creation-failure-feedback");
    return;
  }
  typeInto(creationName, "integration");
  creationBrowse.click();
  if (!await waitFor(() => surface.dataset.busy === "false" &&
      !creationContinue.disabled)) {
    request("fail-creation-browse");
    return;
  }
  creationContinue.click();
  if (!await waitFor(() => {
    if (surface.dataset.busy !== "false" ||
        dashboardVolumeState.textContent !== "已挂载" ||
        dashboardUnmount.hidden || !creationCapacity.disabled ||
        !creationContinue.disabled ||
        creationCancelLabel.textContent !== "完成" ||
        creationStatus.dataset.tone !== "success" ||
        dashboardContainerCount.textContent !== "1" ||
        containerRows().length !== 1) {
      return false;
    }
    const row = containerRows()[0];
    const facts = containerFacts(row);
    // 8.7.2: the creation entry stays visible once a container exists.
    return row.querySelector(".container-name").textContent ===
        "integration.hc" &&
      row.querySelector(".container-state").textContent === "已挂载" &&
      facts.length === 2 && facts[0] === "64 MiB" && facts[1] === "D:" &&
      !containersCreate.hidden;
  })) {
    request("fail-creation-submit");
    return;
  }
  creationCancel.click();
  if (!creationDialog.hidden) {
    request("fail-creation-close");
    return;
  }

  // 8.7.2: the hidden container is named and placed on exactly the same two
  // fields, then commits the card record and creates the volume through one
  // six-argument bridge call; the fakes verify every position and the path.
  containersCreate.click();
  document.querySelector('[data-create-storage="file"]').click();
  hiddenPick.click();
  if (hiddenPick.getAttribute("aria-checked") !== "true" ||
      standardPick.getAttribute("aria-checked") !== "false") {
    request("fail-hidden-creation");
    return;
  }
  creationContinue.click();
  if (hiddenName.value !== "" || hiddenDirectory.value !== "") {
    request("fail-hidden-creation");
    return;
  }
  typeInto(hiddenName, "handover");
  hiddenBrowse.click();
  if (!await waitFor(() => surface.dataset.busy === "false" &&
      /^[A-Za-z]:\\/.test(hiddenDirectory.value))) {
    request("fail-creation-browse");
    return;
  }
  typeInto(hiddenCapacity, "128");
  typeInto(hiddenInnerCapacity, "97");
  typeInto(hiddenSecond, "second-password-value");
  typeInto(hiddenSecondConfirmation, "second-password-value");
  hiddenAcknowledge.checked = true;
  hiddenAcknowledge.dispatchEvent(new Event("change", { bubbles: true }));
  // 97 of 128 is past three quarters, so the inner volume is refused before
  // anything reaches the bridge.
  if (!creationContinue.disabled ||
      hiddenError.textContent !== "内层容量过载。") {
    request("fail-hidden-capacity-rule");
    return;
  }
  typeInto(hiddenInnerCapacity, "96");
  if (creationContinue.disabled || !hiddenError.hidden) {
    request("fail-hidden-capacity-rule");
    return;
  }
  // The acknowledgement is the last gate: without it nothing is submitted.
  hiddenAcknowledge.checked = false;
  hiddenAcknowledge.dispatchEvent(new Event("change", { bubbles: true }));
  creationContinue.click();
  const hiddenReviewConfirm = document.getElementById("hidden-create-review-confirm");
  if (!hiddenReviewConfirm.disabled || document.getElementById("hidden-create-review").hidden) {
    request("fail-hidden-acknowledgement");
    return;
  }
  hiddenAcknowledge.checked = true;
  hiddenAcknowledge.dispatchEvent(new Event("change", { bubbles: true }));
  if (creationDialog.hidden || creationContinue.disabled) {
    request("fail-hidden-creation");
    return;
  }
  hiddenReviewConfirm.click();
  if (!await waitFor(() => surface.dataset.busy === "false" &&
      dashboardContainerCount.textContent === "2" &&
      containerRows().length === 2 && hiddenSecond.value === "" &&
      hiddenSecondConfirmation.value === "" && hiddenName.value === "" &&
      hiddenDirectory.value === "" &&
      creationStatus.dataset.tone === "success")) {
    request("fail-hidden-creation");
    return;
  }
  creationCancel.click();
  if (!creationDialog.hidden) {
    request("fail-creation-close");
    return;
  }

  historyNavigation.click();
  const recordedRows = historyRows();
  const expectedOperations = [
    ["挂载安全盘", "handover.hc → E:", "成功"],
    ["创建安全盘", "handover.hc", "成功"],
    ["挂载安全盘", "integration.hc → D:", "成功"],
    ["创建安全盘", "integration.hc", "成功"],
    ["创建安全盘", "integration.hc", "失败"],
  ];
  if (!historySection.classList.contains("active") ||
      recordedRows.length !== expectedOperations.length ||
      historyClear.disabled || expectedOperations.some((expected, index) => {
        const cells = historyCells(recordedRows[index]);
        return cells.length !== 4 || cells[0].length !== 19 ||
          cells[1] !== expected[0] || cells[2] !== expected[1] ||
          cells[3] !== expected[2];
      })) {
    request("fail-operation-history");
    return;
  }
  historyClear.click();
  if (!await waitFor(() => historyRows().length === 1 &&
      historyCells(historyRows()[0]).join("") ===
        "当前会话还没有操作记录。" && historyClear.disabled)) {
    request("fail-operation-clear");
    return;
  }
  containersNavigation.click();

  request("request-login");
  if (!await waitForState("login-required") ||
      !confirmationField.hidden || password.disabled || surface.hidden ||
      !dashboard.hidden || !creationDialog.hidden) {
    request("fail-login-route");
    return;
  }
  password.value = "must-be-cleared-on-card-change";
  request("request-login");
  if (!await waitFor(() => password.value === "" &&
      surface.dataset.sessionState === "login-required" &&
      surface.dataset.busy === "false")) {
    request("fail-external-input-clear");
    return;
  }
  password.value = "wrong-password";
  action.click();
  if (!await waitForState("login-required") || password.value !== "") {
    request("fail-login-retry");
    return;
  }

  password.value = "p\u0000ä";
  action.click();
  if (!await waitForState("authenticated") || password.value !== "" ||
      !action.hidden || !surface.hidden || dashboard.hidden ||
      dashboardUnmount.hidden ||
      dashboardVolumeState.textContent !== "已挂载" ||
      dashboardContainerCount.textContent !== "2" ||
      containerFacts(containerRows()[0]).join("/") !== "64 MiB/D:") {
    request("fail-login-success");
    return;
  }
  historyNavigation.click();
  if (!historySection.classList.contains("active") ||
      historyRows().length !== 1 ||
      historyCells(historyRows()[0]).slice(1).join("/") !==
        "打开会话/—/成功" || historyClear.disabled) {
    request("fail-operation-reset");
    return;
  }

  request("request-residual-mount");
  if (!await waitForState("unavailable") || action.hidden ||
      actionLabel.textContent !== "尝试一键卸载" || surface.hidden ||
      !dashboard.hidden) {
    request("fail-unavailable");
    return;
  }
  action.click();
  if (!await waitFor(() => surface.dataset.sessionState === "unavailable" &&
      surface.dataset.busy === "false" && action.hidden)) {
    request("fail-unmount");
    return;
  }

  request("request-no-card");
  if (!await waitForState("no-card") || !action.hidden) {
    request("fail-final-no-card");
    return;
  }
  request("request-login");
  if (!await waitForState("login-required")) throw new Error("reset login route");
  const resetElement = name => document.getElementById("forgot-password-" + name);
  resetElement("open").click();
  resetElement("acknowledge").checked = true;
  resetElement("acknowledge").dispatchEvent(new Event("change"));
  resetElement("confirm").click();
  if (!await waitFor(() => !resetElement("skip").hidden)) throw new Error("unknown mount skip missing");
  resetElement("skip").click();
  if (!await waitFor(() => resetElement("status").textContent.includes("正在关闭应用"))) throw new Error("reset not completed");
  request("pass");
})().catch(() => {
  chrome.webview.postMessage("fail-driver-exception");
});
