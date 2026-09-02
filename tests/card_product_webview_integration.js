"use strict";

chrome.webview.postMessage("driver-loaded");

(async () => {
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
  const status = document.getElementById("card-session-status");
  const error = document.getElementById("card-session-error");
  const dashboard = document.getElementById("authenticated-dashboard");
  const dashboardUnmount = document.getElementById("dashboard-unmount");
  const dashboardVolumeState =
    document.getElementById("dashboard-volume-state");
  const dashboardContainerCount =
    document.getElementById("dashboard-container-count");
  const containersNavigation =
    document.querySelector('[data-section="containers"]');
  const containersSection =
    document.getElementById("dashboard-section-containers");
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
  const creationCapacity =
    document.getElementById("create-container-capacity");
  const standardPick = document.querySelector(
    '[data-create-kind="encrypted"]');
  const hiddenPick = document.querySelector('[data-create-kind="hidden"]');
  const typeStep = document.getElementById("create-type-step");
  const hiddenCapacity =
    document.getElementById("create-container-b-capacity");
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
      !dashboardUnmount.hidden || dashboardVolumeState.textContent !== "未挂载" ||
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
  // Both switches flip in place; the locked one is not actuatable at all.
  const autoMount = settingsSection.querySelector('[data-setting="auto-mount"]');
  const lockedUnmount = settingsSection.querySelector('[aria-disabled="true"]');
  if (autoMount.getAttribute("aria-checked") !== "false" ||
      lockedUnmount.getAttribute("aria-checked") !== "true") {
    request("fail-settings-defaults");
    return;
  }
  autoMount.click();
  lockedUnmount.click();
  if (autoMount.getAttribute("aria-checked") !== "true" ||
      lockedUnmount.getAttribute("aria-checked") !== "true") {
    request("fail-settings-switch");
    return;
  }
  autoMount.click();
  if (autoMount.getAttribute("aria-checked") !== "false") {
    request("fail-settings-switch");
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
  if (creationDialog.hidden || typeStep.hidden ||
      creationTitle.textContent !== "新建容器" ||
      creationContinue.textContent !== "下一步" ||
      creationCancelLabel.textContent !== "取消" ||
      standardPick.getAttribute("aria-checked") !== "true" ||
      hiddenPick.getAttribute("aria-checked") !== "false" ||
      creationContinue.disabled) {
    request("fail-creation-type-step");
    return;
  }
  creationContinue.click();
  if (!typeStep.hidden || creationTitle.textContent !== "新建标准容器" ||
      creationCancelLabel.textContent !== "上一步" ||
      creationContinue.textContent !== "创建容器" ||
      creationCapacity.value !== "64" || capacityUnit.dataset.unit !== "mib" ||
      creationContinue.disabled) {
    request("fail-creation-entry");
    return;
  }
  // 上一步 returns to the picker and clears the parameters it collected.
  creationCapacity.value = "128";
  creationCapacity.dispatchEvent(new Event("input", { bubbles: true }));
  creationCancel.click();
  if (typeStep.hidden || creationCapacity.value !== "64" ||
      creationTitle.textContent !== "新建容器") {
    request("fail-creation-back-step");
    return;
  }
  creationContinue.click();
  if (!typeStep.hidden || creationCapacity.value !== "64" ||
      creationContinue.disabled) {
    request("fail-creation-entry");
    return;
  }
  creationCapacity.value = "19";
  creationCapacity.dispatchEvent(new Event("input", { bubbles: true }));
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
  creationCapacity.value = "64";
  creationCapacity.dispatchEvent(new Event("input", { bubbles: true }));
  creationContinue.click();
  if (!await waitFor(() => surface.dataset.busy === "false" &&
      dashboardVolumeState.textContent === "未挂载" &&
      creationStatus.dataset.tone === "error" &&
      !creationCapacity.disabled && !creationContinue.disabled)) {
    request("fail-creation-failure-feedback");
    return;
  }
  // The second selection is cancelled by the native chooser: the page must
  // report it as an ordinary cancellation and stay ready for another try.
  creationContinue.click();
  if (!await waitFor(() => surface.dataset.busy === "false" &&
      creationStatus.textContent === "已取消创建。" &&
      creationStatus.dataset.tone === "" && !creationContinue.disabled &&
      containerRows().length === 0)) {
    request("fail-creation-cancelled");
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

  // The hidden container commits the card record and the volume through one
  // five-argument bridge call; the fakes verify every position.
  const typeInto = (input, value) => {
    input.value = value;
    input.dispatchEvent(new Event("input", { bubbles: true }));
  };
  containersCreate.click();
  hiddenPick.click();
  if (hiddenPick.getAttribute("aria-checked") !== "true" ||
      standardPick.getAttribute("aria-checked") !== "false") {
    request("fail-hidden-creation");
    return;
  }
  creationContinue.click();
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
  if (!creationContinue.disabled) {
    request("fail-hidden-acknowledgement");
    return;
  }
  hiddenAcknowledge.checked = true;
  hiddenAcknowledge.dispatchEvent(new Event("change", { bubbles: true }));
  if (creationDialog.hidden || creationContinue.disabled) {
    request("fail-hidden-creation");
    return;
  }
  creationContinue.click();
  if (!await waitFor(() => surface.dataset.busy === "false" &&
      dashboardContainerCount.textContent === "2" &&
      containerRows().length === 2 && hiddenSecond.value === "" &&
      hiddenSecondConfirmation.value === "" &&
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
    ["挂载容器", "hidden.hc → E:", "成功"],
    ["创建容器", "hidden.hc", "成功"],
    ["挂载容器", "integration.hc → D:", "成功"],
    ["创建容器", "integration.hc", "成功"],
    ["创建容器", "integration.hc", "失败"],
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
      action.textContent !== "尝试安全卸载" || surface.hidden ||
      !dashboard.hidden) {
    request("fail-unavailable");
    return;
  }
  action.click();
  if (!await waitForState("unavailable") || !action.hidden) {
    request("fail-unmount");
    return;
  }

  request("request-no-card");
  if (!await waitForState("no-card") || !action.hidden) {
    request("fail-final-no-card");
    return;
  }
  request("pass");
})().catch(() => {
  chrome.webview.postMessage("fail-driver-exception");
});
