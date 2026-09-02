(() => {
  "use strict";

  const nativeState = Object.freeze({
    noCard: 0,
    initializationRequired: 1,
    loginRequired: 2,
    authenticated: 3,
    unavailable: 4,
  });
  const stateName = Object.freeze([
    "no-card",
    "initialization-required",
    "login-required",
    "authenticated",
    "unavailable",
  ]);
  const surface = document.getElementById("card-session-surface");
  const stateTitle = document.getElementById("card-state-title");
  const passwordFields = document.getElementById("password-fields");
  const passwordLabel = document.getElementById("card-password-label");
  const passwordInput = document.getElementById("card-password");
  const passwordVisibilityButton =
    document.getElementById("card-password-visibility");
  const confirmationField = document.getElementById("confirmation-field");
  const confirmationInput =
    document.getElementById("card-password-confirmation");
  const confirmationVisibilityButton =
    document.getElementById("card-password-confirmation-visibility");
  const strengthOutput = document.getElementById("password-strength");
  const strengthDescription =
    document.getElementById("password-strength-description");
  const actionButton = document.getElementById("card-session-action");
  const actionLabel = document.getElementById("card-session-action-label");
  const actionSpinner = document.getElementById("card-session-spinner");
  const statusOutput = document.getElementById("card-session-status");
  const errorOutput = document.getElementById("card-session-error");
  const dashboard = document.getElementById("authenticated-dashboard");
  const dashboardUnmountButton = document.getElementById("dashboard-unmount");
  const dashboardVolumeState =
    document.getElementById("dashboard-volume-state");
  const dashboardContainerCount =
    document.getElementById("dashboard-container-count");
  const navContainerCount = document.getElementById("nav-container-count");
  const dashboardStatus = document.getElementById("dashboard-status");
  const containerLists =
    Array.from(document.querySelectorAll("[data-container-list]"));
  const mountExistingButtons =
    Array.from(document.querySelectorAll("[data-mount-existing]"));
  const dashboardNavigation =
    Array.from(document.querySelectorAll(".dashboard-nav"));
  const dashboardSections =
    Array.from(document.querySelectorAll(".dashboard-section"));
  const settingSwitches =
    Array.from(document.querySelectorAll("[data-setting]"));
  const operationHistoryBody =
    document.getElementById("operation-history-body");
  const operationHistoryClear =
    document.getElementById("operation-history-clear");
  const creationButtons =
    Array.from(document.querySelectorAll("[data-create-container]"));
  const creationTypeButtons =
    Array.from(document.querySelectorAll("[data-create-kind]"));
  const creationDialog = document.getElementById("create-volume-dialog");
  const creationTypeStep = document.getElementById("create-type-step");
  const creationTitle = document.getElementById("create-volume-title");
  const creationDescription =
    document.getElementById("create-volume-description");
  const creationCloseButton = document.getElementById("create-volume-close");
  const creationCancelButton = document.getElementById("create-volume-cancel");
  const creationCancelLabel =
    document.getElementById("create-volume-cancel-label");
  const creationBackIcon = document.getElementById("create-volume-back-icon");
  const creationContinueButton =
    document.getElementById("create-volume-continue");
  const containerForm = document.getElementById("create-container-form");
  const containerCapacity =
    document.getElementById("create-container-capacity");
  const containerError = document.getElementById("create-container-error");
  const creationStatus = document.getElementById("create-volume-status");
  const creationWorkflow = document.getElementById("create-volume-workflow");
  const creationTypeNote = document.getElementById("create-type-note");
  const containerBForm = document.getElementById("create-container-b-form");
  const containerBCapacity =
    document.getElementById("create-container-b-capacity");
  const containerBHiddenCapacity =
    document.getElementById("create-container-b-hidden-capacity");
  const containerBSecond =
    document.getElementById("create-container-b-second");
  const containerBSecondConfirmation =
    document.getElementById("create-container-b-second-confirmation");
  const containerBAcknowledge =
    document.getElementById("create-container-b-acknowledge");
  const containerBError =
    document.getElementById("create-container-b-error");
  const containerBInputs = [
    containerBCapacity, containerBHiddenCapacity, containerBSecond,
    containerBSecondConfirmation, containerBAcknowledge,
  ];

  // Capacities are entered in whichever unit the picker shows and always
  // reach the bridge in MiB. Nothing else in the page works in another unit.
  const kCapacityUnits = Object.freeze(
    { mib: 1, gib: 1024, tib: 1024 * 1024 });
  const kCapacityUnitNames = Object.freeze(
    { mib: "MiB", gib: "GiB", tib: "TiB" });
  const kMinimumCapacityMib = 20;
  const kMaximumCapacityMib = 1024 * 1024;
  const operationKind = Object.freeze({
    sessionOpened: 0,
    containerCreated: 1,
    containerMounted: 2,
    containerUnmounted: 3,
  });
  const operationOutcome = Object.freeze({ succeeded: 0, failed: 1 });

  const makeUnitPicker = (button, fields, echo = null) => ({
    button,
    echo,
    fields,
    list: button.parentElement.querySelector(".unit-list"),
    options: Array.from(button.parentElement.querySelectorAll(".unit-option")),
    name: button.querySelector(".unit-name"),
  });

  const containerUnit = makeUnitPicker(
    document.getElementById("create-container-unit"),
    [{
      input: containerCapacity,
      minimum: kMinimumCapacityMib,
      maximum: kMaximumCapacityMib,
    }]);
  // The two hidden-container capacities are compared against each other, so
  // they share one unit and can never be read in different ones.
  const containerBUnit = makeUnitPicker(
    document.getElementById("create-container-b-unit"),
    [{
      input: containerBCapacity,
      minimum: kMinimumCapacityMib,
      maximum: kMaximumCapacityMib,
    }, {
      input: containerBHiddenCapacity,
      minimum: 1,
      maximum: kMaximumCapacityMib - 1,
    }],
    document.getElementById("create-container-b-unit-echo"));
  const unitPickers = [containerUnit, containerBUnit];

  let bridge = null;
  let currentState = null;
  let activeVolumes = false;
  // Live projection of the containers this session has adopted. Nothing here
  // is stored: it is rebuilt from the native side on every refresh.
  let containers = [];
  // Bounded, native-owned facts from this authenticated session. Like the
  // container projection, this array is rebuilt and never persisted by JS.
  let operationEvents = [];
  let busy = true;
  let externalRefreshPending = false;
  let creationKind = null;
  // The dialog is one flow in two steps: "type" picks the container shape,
  // "params" collects what that shape needs.
  let creationStep = "type";
  let creationCompleted = false;

  const setVisible = (element, visible) => {
    element.hidden = !visible;
  };

  const acceptsLogin = () => currentState === nativeState.loginRequired;
  const acceptsInitialization = () =>
    currentState === nativeState.initializationRequired;

  const normalizedPasswordLength = () => {
    try {
      return Array.from(passwordInput.value.normalize("NFC")).length;
    } catch (_) {
      return null;
    }
  };

  // The native route compares both inputs only after NFC normalization, so a
  // failure is reported as a mismatch on exactly the same terms.
  const initializationInputsMatch = (password, confirmation) => {
    try {
      return password.normalize("NFC") === confirmation.normalize("NFC");
    } catch (_) {
      return password === confirmation;
    }
  };

  const initializationInputReady = () => {
    const count = normalizedPasswordLength();
    return count !== null && count >= 12 && count <= 128 &&
      confirmationInput.value.length !== 0;
  };

  const updateActionAvailability = () => {
    actionButton.disabled = busy ||
      (acceptsInitialization() && !initializationInputReady());
  };

  const setPasswordVisibility = (input, button, visible) => {
    input.type = visible ? "text" : "password";
    button.setAttribute("aria-pressed", visible ? "true" : "false");
    const confirmation = input === confirmationInput;
    button.setAttribute(
      "aria-label",
      `${visible ? "隐藏" : "显示"}${confirmation ? "确认密码" : "密码"}`);
  };

  const resetPasswordVisibility = () => {
    setPasswordVisibility(passwordInput, passwordVisibilityButton, false);
    setPasswordVisibility(
      confirmationInput, confirmationVisibilityButton, false);
  };

  const focusCurrentInput = () => {
    if (!busy && (acceptsLogin() || acceptsInitialization())) {
      passwordInput.focus();
    }
  };

  const closeUnitLists = () => {
    for (const picker of unitPickers) {
      picker.list.hidden = true;
      picker.button.setAttribute("aria-expanded", "false");
    }
  };

  // Switching the unit rewrites the reachable bounds of every field it
  // governs, so the spinner and the validation below never disagree.
  const applyCapacityUnit = (picker, unit) => {
    const factor = kCapacityUnits[unit] || 1;
    picker.button.dataset.unit = unit;
    picker.name.textContent = kCapacityUnitNames[unit];
    if (picker.echo !== null) {
      picker.echo.textContent = kCapacityUnitNames[unit];
    }
    for (const option of picker.options) {
      option.setAttribute(
        "aria-selected", option.dataset.unit === unit ? "true" : "false");
    }
    for (const field of picker.fields) {
      field.input.min = String(Math.max(1, Math.ceil(field.minimum / factor)));
      field.input.max =
        String(Math.max(1, Math.floor(field.maximum / factor)));
    }
  };

  const capacityMib = (input, picker) => {
    const text = input.value.trim();
    if (!/^[0-9]+$/.test(text)) return null;
    const value = Number(text) * (kCapacityUnits[picker.button.dataset.unit] ||
                                  1);
    return Number.isSafeInteger(value) ? value : null;
  };

  // Every rejection names the field it is about. An empty string means the
  // capacities are usable, which is also what enables the submit button.
  const containerCapacityProblem = () => {
    const value = capacityMib(containerCapacity, containerUnit);
    if (value === null) return "请输入整数容量。";
    if (value > kMaximumCapacityMib) return "容量超出最大值。";
    if (value < kMinimumCapacityMib) return "容量低于最小值。";
    return "";
  };

  const containerCapacityMib = () =>
    containerCapacityProblem() === ""
      ? capacityMib(containerCapacity, containerUnit)
      : null;

  const containerBProblem = () => {
    const outer = capacityMib(containerBCapacity, containerBUnit);
    const hidden = capacityMib(containerBHiddenCapacity, containerBUnit);
    if (outer === null || hidden === null) return "请输入整数容量。";
    if (outer > kMaximumCapacityMib) return "容量超出最大值。";
    if (outer < kMinimumCapacityMib) return "容量低于最小值。";
    if (hidden < 1) return "内层容量低于最小值。";
    // The outer volume has to hold enough handover content to stay credible,
    // so the inner one is capped at three quarters of the total.
    if (hidden * 4 > outer * 3) return "内层容量过载。";
    return "";
  };

  const containerBSizes = () => {
    if (containerBProblem() !== "") return null;
    return {
      outer: capacityMib(containerBCapacity, containerBUnit),
      hidden: capacityMib(containerBHiddenCapacity, containerBUnit),
    };
  };

  const showFieldProblem = (element, message) => {
    element.textContent = message;
    element.hidden = message === "";
  };

  const normalizedLength = (value) => {
    try {
      return Array.from(value.normalize("NFC")).length;
    } catch (_) {
      return null;
    }
  };

  const containerBInputReady = () => {
    const second = normalizedLength(containerBSecond.value);
    return containerBSizes() !== null && second !== null &&
      second >= 12 && second <= 128 &&
      containerBSecond.value === containerBSecondConfirmation.value &&
      containerBAcknowledge.checked;
  };

  const updateCreationAvailability = () => {
    const encrypted = creationKind === "encrypted";
    const hiddenVolume = creationKind === "hidden";
    const choosing = creationStep === "type";
    const authenticated = currentState === nativeState.authenticated;
    const ready = choosing
      ? encrypted || hiddenVolume
      : encrypted
        ? containerCapacityMib() !== null
        : hiddenVolume && containerBInputReady();
    creationDialog.dataset.step = creationStep;
    creationTypeStep.hidden = !choosing;
    containerForm.hidden = choosing || !encrypted;
    containerBForm.hidden = choosing || !hiddenVolume;
    // The step preview describes the standard flow, which picks a location.
    // The hidden container uses a fixed path, so it is not shown there.
    creationWorkflow.hidden = choosing || hiddenVolume;
    // 8.7.2: both shapes are offered on every visit, in the same order and
    // with the same weight. Neither entry is ever withheld.
    for (const button of creationTypeButtons) {
      button.setAttribute(
          "aria-checked",
          button.dataset.createKind === creationKind ? "true" : "false");
      button.disabled = busy || !authenticated || !choosing;
    }
    creationTypeNote.hidden = !choosing;
    containerCapacity.disabled = busy || !authenticated || choosing ||
      !encrypted || creationCompleted;
    containerUnit.button.disabled = containerCapacity.disabled;
    for (const input of containerBInputs) {
      input.disabled = busy || !authenticated || choosing || !hiddenVolume ||
        creationCompleted;
    }
    containerBUnit.button.disabled = containerBCapacity.disabled;
    if (containerUnit.button.disabled && containerBUnit.button.disabled) {
      closeUnitLists();
    }
    showFieldProblem(
      containerError,
      choosing || !encrypted || creationCompleted
        ? ""
        : containerCapacityProblem());
    showFieldProblem(
      containerBError,
      choosing || !hiddenVolume || creationCompleted
        ? ""
        : containerBProblem());
    creationCloseButton.disabled = busy;
    creationCancelButton.disabled = busy;
    creationContinueButton.disabled = busy || !authenticated ||
      creationCompleted || !ready;
    creationContinueButton.textContent = choosing ? "下一步" : "创建容器";
    creationCancelLabel.textContent = creationCompleted
      ? "完成"
      : choosing ? "取消" : "上一步";
    creationBackIcon.hidden = choosing || creationCompleted;
  };

  const kCreationTypeLead =
    "容器是一个加密文件，挂载后显示为一个普通磁盘。" +
    "请选择要创建的容器类型：";

  const applyCreationStepCopy = () => {
    if (creationStep === "type") {
      creationTitle.textContent = "新建容器";
      creationDescription.textContent = kCreationTypeLead;
      return;
    }
    const hiddenVolume = creationKind === "hidden";
    creationTitle.textContent = hiddenVolume
      ? "新建含隐藏卷的容器"
      : "新建标准容器";
    creationDescription.textContent = hiddenVolume
      ? "请设置总容量与内层容量，并为这个容器设置一个诱饵密码。"
      : "请设置容器容量。接着会让你选择保存位置。";
  };

  // Leaving a parameter step discards what was typed on it: a decoy password
  // must not outlive the step that asked for it.
  const resetCreationInputs = () => {
    applyCapacityUnit(containerUnit, "mib");
    applyCapacityUnit(containerBUnit, "mib");
    closeUnitLists();
    containerCapacity.value = "64";
    containerBCapacity.value = "128";
    containerBHiddenCapacity.value = "96";
    containerBSecond.value = "";
    containerBSecondConfirmation.value = "";
    containerBAcknowledge.checked = false;
  };

  const focusCreationType = () => {
    const selected = creationTypeButtons.find(
      (button) => button.dataset.createKind === creationKind);
    if (selected) selected.focus();
  };

  const closeCreationFlow = (force = false) => {
    if (busy && !force) return;
    resetCreationInputs();
    creationDialog.hidden = true;
    creationKind = null;
    creationStep = "type";
    creationDialog.dataset.step = "type";
    creationCompleted = false;
    creationStatus.textContent = "";
    creationStatus.dataset.tone = "";
    creationCancelLabel.textContent = "取消";
    creationBackIcon.hidden = true;
  };

  const showDashboardSection = (sectionName) => {
    for (const item of dashboardNavigation) {
      const selected = item.dataset.section === sectionName;
      item.classList.toggle("active", selected);
      if (selected) {
        item.setAttribute("aria-current", "page");
      } else {
        item.removeAttribute("aria-current");
      }
    }
    for (const section of dashboardSections) {
      section.classList.toggle(
        "active", section.id === `dashboard-section-${sectionName}`);
    }
  };

  const openCreationFlow = () => {
    if (busy || currentState !== nativeState.authenticated) {
      return;
    }
    // The flow always opens on the same step with the same preselection, so
    // opening it says nothing about which password is logged in.
    creationKind = "encrypted";
    creationStep = "type";
    creationCompleted = false;
    creationStatus.textContent = "";
    creationStatus.dataset.tone = "";
    resetCreationInputs();
    applyCreationStepCopy();
    creationDialog.hidden = false;
    updateCreationAvailability();
    focusCreationType();
  };

  const enterCreationParameters = () => {
    creationStep = "params";
    creationStatus.textContent = "";
    creationStatus.dataset.tone = "";
    applyCreationStepCopy();
    updateCreationAvailability();
    if (creationKind === "hidden") {
      containerBCapacity.focus();
      containerBCapacity.select();
    } else {
      containerCapacity.focus();
      containerCapacity.select();
    }
  };

  const returnToCreationTypes = () => {
    resetCreationInputs();
    creationStep = "type";
    creationStatus.textContent = "";
    creationStatus.dataset.tone = "";
    applyCreationStepCopy();
    updateCreationAvailability();
    focusCreationType();
  };

  const kSvgNamespace = "http://www.w3.org/2000/svg";
  const kLockPaths = Object.freeze([
    "M7 10.5h10a1.5 1.5 0 011.5 1.5v6a1.5 1.5 0 01-1.5 1.5H7A1.5 1.5 0 015.5" +
      " 18v-6A1.5 1.5 0 017 10.5z",
    "M8.2 10.5V7.6a3.8 3.8 0 017.6 0v2.9",
  ]);

  const setDashboardStatus = (message, tone = "") => {
    dashboardStatus.textContent = message;
    dashboardStatus.dataset.tone = tone;
  };

  const lockIcon = () => {
    const svg = document.createElementNS(kSvgNamespace, "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("aria-hidden", "true");
    for (const definition of kLockPaths) {
      const path = document.createElementNS(kSvgNamespace, "path");
      path.setAttribute("d", definition);
      svg.appendChild(path);
    }
    return svg;
  };

  const containerFact = (label, value) => {
    const fact = document.createElement("div");
    fact.className = "container-fact";
    const caption = document.createElement("div");
    caption.className = "container-fact-label";
    caption.textContent = label;
    const content = document.createElement("div");
    content.className = "container-fact-value";
    content.textContent = value;
    fact.append(caption, content);
    return fact;
  };

  // The name comes from a file the user picked, so it is only ever written as
  // text. Nothing on this row is built from markup.
  const containerRow = (container) => {
    const row = document.createElement("div");
    row.className = container.mounted
      ? "container-row mounted"
      : "container-row";

    const mark = document.createElement("span");
    mark.className = "container-mark";
    mark.appendChild(lockIcon());

    const identity = document.createElement("div");
    identity.className = "container-identity";
    const name = document.createElement("div");
    name.className = "container-name";
    name.textContent = container.name;
    name.title = container.name;
    const state = document.createElement("div");
    state.className = "container-state";
    state.textContent = container.mounted ? "已挂载" : "未挂载";
    identity.append(name, state);

    const facts = document.createElement("div");
    facts.className = "container-facts";
    facts.append(
      containerFact(
        "容量",
        container.sizeMib > 0
          ? `${container.sizeMib.toLocaleString("zh-CN")} MiB`
          : "不可用"),
      containerFact("盘符", container.mounted ? container.drive : "—"));

    const actions = document.createElement("div");
    actions.className = "container-actions";
    const toggle = document.createElement("button");
    toggle.type = "button";
    toggle.className = "shell-button";
    toggle.textContent = container.mounted ? "卸载" : "挂载";
    toggle.disabled = busy;
    toggle.dataset.containerId = String(container.id);
    toggle.dataset.containerAction = container.mounted ? "unmount" : "mount";
    actions.appendChild(toggle);

    row.append(mark, identity, facts, actions);
    return row;
  };

  const renderContainers = () => {
    for (const list of containerLists) {
      while (list.firstChild !== null) list.removeChild(list.firstChild);
      if (containers.length === 0) {
        const empty = document.createElement("p");
        empty.className = "container-empty";
        empty.textContent =
          "还没有容器。容器是一个加密文件，挂载后在系统里显示为一个普通磁盘。" +
          "新建一个，或导入已有的容器文件。";
        list.appendChild(empty);
        continue;
      }
      for (const container of containers) {
        list.appendChild(containerRow(container));
      }
    }
  };

  const twoDigits = (value) => String(value).padStart(2, "0");

  const formatOperationTime = (unixMs) => {
    const time = new Date(unixMs);
    return `${time.getFullYear()}-${twoDigits(time.getMonth() + 1)}-` +
      `${twoDigits(time.getDate())} ${twoDigits(time.getHours())}:` +
      `${twoDigits(time.getMinutes())}:${twoDigits(time.getSeconds())}`;
  };

  const operationEventName = (kind) => [
    "打开会话", "创建容器", "挂载容器", "卸载容器",
  ][kind];

  const operationTarget = (event) => {
    if (event.kind === operationKind.sessionOpened) return "—";
    return event.drive === ""
      ? event.name
      : `${event.name} → ${event.drive}`;
  };

  const renderOperationHistory = () => {
    while (operationHistoryBody.firstChild !== null) {
      operationHistoryBody.removeChild(operationHistoryBody.firstChild);
    }
    if (operationEvents.length === 0) {
      const row = document.createElement("tr");
      const cell = document.createElement("td");
      cell.colSpan = 4;
      cell.className = "operation-empty";
      cell.textContent = "当前会话还没有操作记录。";
      row.appendChild(cell);
      operationHistoryBody.appendChild(row);
      return;
    }

    for (let index = operationEvents.length - 1; index >= 0; index -= 1) {
      const event = operationEvents[index];
      const row = document.createElement("tr");
      const time = document.createElement("td");
      time.className = "operation-time";
      time.textContent = formatOperationTime(event.completedAtUnixMs);
      const kind = document.createElement("td");
      kind.className = "operation-event";
      kind.textContent = operationEventName(event.kind);
      const target = document.createElement("td");
      target.className = "operation-target";
      target.textContent = operationTarget(event);
      const outcome = document.createElement("td");
      outcome.className = "operation-result";
      outcome.dataset.outcome = event.outcome === operationOutcome.succeeded
        ? "succeeded"
        : "failed";
      outcome.textContent = event.outcome === operationOutcome.succeeded
        ? "成功"
        : "失败";
      row.append(time, kind, target, outcome);
      operationHistoryBody.appendChild(row);
    }
  };

  const updateProductSurface = () => {
    const showDashboard = currentState === nativeState.authenticated;
    surface.hidden = showDashboard;
    dashboard.hidden = !showDashboard;
    document.body.dataset.view = showDashboard ? "dashboard" : "session";
    dashboardVolumeState.textContent = activeVolumes ? "已挂载" : "未挂载";
    dashboardContainerCount.textContent = String(containers.length);
    navContainerCount.textContent = String(containers.length);
    renderContainers();
    renderOperationHistory();
    dashboardUnmountButton.hidden = !showDashboard || !activeVolumes;
    dashboardUnmountButton.disabled = busy || !showDashboard || !activeVolumes;
    // Every creation entry stays visible on every route the page can reach.
    // Hiding one would tell an observer which containers this machine holds.
    for (const button of creationButtons) {
      button.disabled = busy || !showDashboard;
    }
    for (const button of mountExistingButtons) {
      button.disabled = busy || !showDashboard;
    }
    operationHistoryClear.disabled =
      busy || !showDashboard || operationEvents.length === 0;
    if (!showDashboard) {
      closeCreationFlow(true);
      showDashboardSection("dashboard");
    }
    updateCreationAvailability();
  };

  const render = (message, error = "") => {
    surface.dataset.sessionState = currentState === null
      ? "initializing"
      : stateName[currentState];
    surface.dataset.busy = busy ? "true" : "false";

    const showPassword = acceptsLogin() || acceptsInitialization();
    const showConfirmation = acceptsInitialization();
    const showUnmount = activeVolumes &&
      currentState === nativeState.unavailable;
    setVisible(passwordFields, showPassword);
    setVisible(confirmationField, showConfirmation);
    setVisible(actionButton, showPassword || showUnmount);
    passwordInput.disabled = busy || !showPassword;
    confirmationInput.disabled = busy || !showConfirmation;
    passwordVisibilityButton.disabled = busy || !showPassword;
    confirmationVisibilityButton.disabled = busy || !showConfirmation;
    updateActionAvailability();

    switch (currentState) {
      case nativeState.noCard:
        stateTitle.textContent = "请插入安全卡";
        break;
      case nativeState.initializationRequired:
        stateTitle.textContent = "初始化安全卡";
        passwordLabel.textContent = "设置日常密码";
        actionLabel.textContent = "初始化安全卡";
        break;
      case nativeState.loginRequired:
        stateTitle.textContent = "打开安全会话";
        passwordLabel.textContent = "密码";
        actionLabel.textContent = "打开安全会话";
        break;
      case nativeState.authenticated:
        stateTitle.textContent = activeVolumes
          ? "安全会话已打开"
          : "安全卡已认证";
        actionLabel.textContent = "安全卸载";
        break;
      case nativeState.unavailable:
        stateTitle.textContent = "安全组件当前不可用";
        actionLabel.textContent = "尝试安全卸载";
        break;
      default:
        stateTitle.textContent = "正在连接安全组件";
        break;
    }
    // The card work behind this button takes seconds. Without a moving
    // indicator and a label that says what is running, the page reads as
    // frozen and the user retries or pulls the card.
    const working = busy && !actionButton.hidden;
    actionSpinner.hidden = !working;
    if (working) {
      actionLabel.textContent = acceptsInitialization()
        ? "正在初始化安全卡"
        : acceptsLogin()
          ? "正在验证安全卡"
          : "正在安全卸载";
    }
    statusOutput.textContent = message;
    errorOutput.textContent = error;
    errorOutput.hidden = error.length === 0;
    updateProductSurface();
  };

  const clearError = () => {
    errorOutput.textContent = "";
    errorOutput.hidden = true;
  };

  const updateStrength = () => {
    if (!acceptsInitialization() || passwordInput.value.length === 0) {
      strengthOutput.hidden = true;
      strengthOutput.dataset.level = "";
      strengthOutput.dataset.score = "0";
      strengthDescription.textContent = "";
      return;
    }

    strengthOutput.hidden = false;
    const count = normalizedPasswordLength();
    if (count === null) {
      strengthOutput.dataset.level = "invalid";
      strengthOutput.dataset.score = "1";
      strengthDescription.textContent = "密码强度：无法评估";
      return;
    }

    if (count < 12) {
      strengthOutput.dataset.level = "invalid";
      strengthOutput.dataset.score = "1";
      strengthDescription.textContent = "密码强度：未达到最低要求";
    } else if (count <= 17) {
      strengthOutput.dataset.level = "acceptable";
      strengthOutput.dataset.score = "2";
      strengthDescription.textContent = "密码强度：可接受";
    } else if (count <= 128) {
      strengthOutput.dataset.level = "strong";
      strengthOutput.dataset.score = "3";
      strengthDescription.textContent = "密码强度：较强";
    } else {
      strengthOutput.dataset.level = "invalid";
      strengthOutput.dataset.score = "3";
      strengthDescription.textContent = "密码长度超过 128 个字符";
    }
  };

  const wipe = (value) => {
    if (value !== null) {
      value.fill(0);
    }
  };

  const clearDomInputs = () => {
    passwordInput.value = "";
    confirmationInput.value = "";
    resetPasswordVisibility();
    updateStrength();
    updateActionAvailability();
  };

  const refreshNativeState = async () => {
    const nextState = await bridge.getSessionState();
    const nextActiveVolumes = await bridge.hasActiveSession();
    if (!Number.isInteger(nextState) || nextState < nativeState.noCard ||
        nextState > nativeState.unavailable ||
        (nextActiveVolumes !== true && nextActiveVolumes !== false)) {
      throw new Error("invalid native session state");
    }
    const nextContainers = [];
    const nextOperationEvents = [];
    if (nextState === nativeState.authenticated) {
      const count = await bridge.getContainerCount();
      if (!Number.isInteger(count) || count < 0 || count > 4096) {
        throw new Error("invalid container count");
      }
      for (let index = 0; index < count; index += 1) {
        const id = await bridge.getContainerId(index);
        const name = await bridge.getContainerName(index);
        const mounted = await bridge.getContainerMounted(index);
        const sizeMib = await bridge.getContainerSizeMib(index);
        const drive = await bridge.getContainerDriveLetter(index);
        const validId = Number.isInteger(id) && id > 0;
        const validName = typeof name === "string" && name.length !== 0;
        const validMounted = mounted === true || mounted === false;
        const validSize = Number.isInteger(sizeMib) && sizeMib >= 0 &&
          sizeMib <= 1048576;
        const validDrive = typeof drive === "string" &&
          ((mounted === true && /^[A-Z]:$/.test(drive) && nextActiveVolumes) ||
           (mounted !== true && drive === ""));
        if (!validId || !validName || !validMounted || !validSize ||
            !validDrive) {
          throw new Error("invalid container status");
        }
        nextContainers.push({ id, name, mounted, sizeMib, drive });
      }

      const operationCount = await bridge.getOperationCount();
      if (!Number.isInteger(operationCount) || operationCount < 0 ||
          operationCount > 128) {
        throw new Error("invalid operation count");
      }
      for (let index = 0; index < operationCount; index += 1) {
        const completedAtUnixMs =
          await bridge.getOperationCompletedAtUnixMs(index);
        const kind = await bridge.getOperationKind(index);
        const outcome = await bridge.getOperationOutcome(index);
        const name = await bridge.getOperationContainerName(index);
        const drive = await bridge.getOperationDriveLetter(index);
        const validTime = Number.isSafeInteger(completedAtUnixMs) &&
          completedAtUnixMs > 0;
        const validKind = Number.isInteger(kind) &&
          kind >= operationKind.sessionOpened &&
          kind <= operationKind.containerUnmounted;
        const validOutcome = Number.isInteger(outcome) &&
          outcome >= operationOutcome.succeeded &&
          outcome <= operationOutcome.failed;
        const validName = typeof name === "string" &&
          name.indexOf("/") === -1 && name.indexOf("\\") === -1;
        const validDriveText = typeof drive === "string";
        const hasName = validName && name.length !== 0;
        const hasDrive = validDriveText && /^[A-Z]:$/.test(drive);
        const noDrive = drive === "";
        const validShape =
          (kind === operationKind.sessionOpened &&
           outcome === operationOutcome.succeeded && !hasName && noDrive) ||
          (kind === operationKind.containerCreated && hasName && noDrive) ||
          (kind === operationKind.containerMounted && hasName &&
           ((outcome === operationOutcome.succeeded && hasDrive) ||
            (outcome === operationOutcome.failed && noDrive))) ||
          (kind === operationKind.containerUnmounted && hasName && hasDrive);
        if (!validTime || !validKind || !validOutcome || !validName ||
            !validDriveText || !validShape) {
          throw new Error("invalid operation event");
        }
        nextOperationEvents.push(
          { completedAtUnixMs, kind, outcome, name, drive });
      }
    }
    currentState = nextActiveVolumes &&
        nextState !== nativeState.authenticated &&
        nextState !== nativeState.unavailable
      ? nativeState.unavailable
      : nextState;
    activeVolumes = nextActiveVolumes;
    containers = nextContainers;
    operationEvents = nextOperationEvents;
  };

  const messageForCurrentState = () => {
    switch (currentState) {
      case nativeState.noCard:
        return "等待安全卡。";
      case nativeState.initializationRequired:
        return "请输入并确认日常密码，至少12字符。";
      case nativeState.loginRequired:
        return "请输入密码。";
      case nativeState.authenticated:
        return activeVolumes
          ? "安全会话已打开。"
          : "认证成功，当前没有活动卷。";
      case nativeState.unavailable:
      default:
        return activeVolumes
          ? "安全操作未完成，请尝试安全卸载。"
          : "请重新插入安全卡后重试。";
    }
  };

  const refreshAfterExternalChange = async () => {
    externalRefreshPending = true;
    clearDomInputs();
    if (busy || bridge === null) {
      return;
    }

    do {
      externalRefreshPending = false;
      busy = true;
      render("正在确认安全卡状态");
      try {
        await refreshNativeState();
        busy = false;
        render(messageForCurrentState());
        updateStrength();
        focusCurrentInput();
      } catch (_) {
        currentState = nativeState.unavailable;
        activeVolumes = true;
        busy = false;
        render("无法确认安全会话状态，请尝试安全卸载。");
      }
    } while (externalRefreshPending);
  };

  window.addEventListener("usb-vault-session-changed", () => {
    void refreshAfterExternalChange();
  });

  const submitLogin = async () => {
    let encodedPasswordBytes = null;
    let passwordBytes = null;
    busy = true;
    render("正在验证安全卡");
    try {
      try {
        encodedPasswordBytes = new TextEncoder().encode(passwordInput.value);
        passwordBytes = Array.from(encodedPasswordBytes);
      } finally {
        passwordInput.value = "";
        resetPasswordVisibility();
        wipe(encodedPasswordBytes);
        encodedPasswordBytes = null;
      }
      try {
        await bridge.submitLoginPasswordBytes(passwordBytes);
      } finally {
        wipe(passwordBytes);
      }
      await refreshNativeState();
      busy = false;
      render(messageForCurrentState());
    } catch (_) {
      try {
        await refreshNativeState();
      } catch (_) {
        currentState = nativeState.unavailable;
        activeVolumes = true;
      }
      busy = false;
      render(currentState === nativeState.loginRequired
        ? "无法打开安全会话，请检查密码后重试。"
        : messageForCurrentState());
    } finally {
      wipe(encodedPasswordBytes);
      wipe(passwordBytes);
      focusCurrentInput();
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    }
  };

  const submitInitialization = async () => {
    let encodedPasswordBytes = null;
    let passwordBytes = null;
    let encodedConfirmationBytes = null;
    let confirmationBytes = null;
    // Retained only so a failed attempt can be edited instead of retyped; both
    // copies are dropped as soon as the route stops accepting a retry.
    let retainedPassword = passwordInput.value;
    let retainedConfirmation = confirmationInput.value;
    busy = true;
    render("正在初始化安全卡");
    try {
      try {
        encodedPasswordBytes = new TextEncoder().encode(retainedPassword);
        passwordBytes = Array.from(encodedPasswordBytes);
        encodedConfirmationBytes =
          new TextEncoder().encode(retainedConfirmation);
        confirmationBytes = Array.from(encodedConfirmationBytes);
      } finally {
        passwordInput.value = "";
        confirmationInput.value = "";
        resetPasswordVisibility();
        wipe(encodedPasswordBytes);
        wipe(encodedConfirmationBytes);
        encodedPasswordBytes = null;
        encodedConfirmationBytes = null;
      }
      try {
        await bridge.submitInitializationPasswordBytes(
          passwordBytes, confirmationBytes);
      } finally {
        wipe(passwordBytes);
        wipe(confirmationBytes);
      }
      retainedPassword = "";
      retainedConfirmation = "";
      await refreshNativeState();
      busy = false;
      render(messageForCurrentState());
      updateStrength();
    } catch (_) {
      try {
        await refreshNativeState();
      } catch (_) {
        currentState = nativeState.unavailable;
        activeVolumes = true;
      }
      const retryable =
        currentState === nativeState.initializationRequired &&
        !externalRefreshPending;
      const mismatched =
        !initializationInputsMatch(retainedPassword, retainedConfirmation);
      if (retryable) {
        passwordInput.value = retainedPassword;
        confirmationInput.value = retainedConfirmation;
      }
      retainedPassword = "";
      retainedConfirmation = "";
      busy = false;
      render(messageForCurrentState(), retryable
        ? (mismatched ? "初始化失败，两次密码不一致。" : "初始化失败，请重试。")
        : "");
      updateStrength();
    } finally {
      retainedPassword = "";
      retainedConfirmation = "";
      wipe(encodedPasswordBytes);
      wipe(encodedConfirmationBytes);
      wipe(passwordBytes);
      wipe(confirmationBytes);
      focusCurrentInput();
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    }
  };

  const unmount = async () => {
    busy = true;
    render("正在安全卸载");
    try {
      await bridge.unmount();
      await refreshNativeState();
      busy = false;
      render(messageForCurrentState());
    } catch (_) {
      try {
        await refreshNativeState();
      } catch (_) {
        currentState = nativeState.unavailable;
        activeVolumes = true;
      }
      busy = false;
      render(activeVolumes
        ? "无法安全卸载，请关闭占用文件后重试。"
        : messageForCurrentState());
    } finally {
      focusCurrentInput();
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    }
  };

  // One shared shape for the dashboard-side container actions: serialize on
  // the busy flag, refresh from the native side, and report failure on the
  // dashboard instead of the hidden session surface.
  const runContainerAction = async (action, failure) => {
    busy = true;
    setDashboardStatus("");
    render(messageForCurrentState());
    try {
      const outcome = await action();
      await refreshNativeState();
      busy = false;
      render(messageForCurrentState());
      if (outcome === false) setDashboardStatus("已取消。");
    } catch (_) {
      try {
        await refreshNativeState();
      } catch (_) {
        currentState = nativeState.unavailable;
        activeVolumes = true;
      }
      busy = false;
      render(messageForCurrentState());
      setDashboardStatus(failure, "error");
    } finally {
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    }
  };

  const mountExistingContainer = () => runContainerAction(
    () => bridge.mountContainer(),
    "无法导入该容器，请确认文件与当前安全卡匹配。");

  const mountAdoptedContainer = (id) => runContainerAction(
    () => bridge.mountAdoptedContainer(id),
    "无法挂载该容器，请稍后重试。");

  const unmountAdoptedContainer = (id) => runContainerAction(
    () => bridge.unmountContainer(id),
    "无法卸载该容器，请关闭占用文件后重试。");

  const createContainerB = async () => {
    const sizes = containerBSizes();
    if (sizes === null || !containerBInputReady() ||
        creationKind !== "hidden" ||
        currentState !== nativeState.authenticated) {
      creationStatus.textContent = "请检查容量、诱饵密码与下方的确认项。";
      creationStatus.dataset.tone = "error";
      updateCreationAvailability();
      return;
    }

    let encodedSecond = null;
    let encodedConfirmation = null;
    let secondBytes = null;
    let confirmationBytes = null;
    busy = true;
    creationStatus.textContent = "正在写入安全卡并创建容器。";
    creationStatus.dataset.tone = "";
    render(messageForCurrentState());
    try {
      try {
        const encoder = new TextEncoder();
        encodedSecond = encoder.encode(containerBSecond.value);
        encodedConfirmation =
          encoder.encode(containerBSecondConfirmation.value);
        secondBytes = Array.from(encodedSecond);
        confirmationBytes = Array.from(encodedConfirmation);
      } finally {
        containerBSecond.value = "";
        containerBSecondConfirmation.value = "";
        wipe(encodedSecond);
        wipe(encodedConfirmation);
        encodedSecond = null;
        encodedConfirmation = null;
      }
      try {
        // The password this session was opened under stays native: the page
        // never received it back and never sends it anywhere.
        await bridge.createContainerBMib(sizes.outer, sizes.hidden,
                                         secondBytes, confirmationBytes);
      } finally {
        wipe(secondBytes);
        wipe(confirmationBytes);
      }
      await refreshNativeState();
      busy = false;
      creationCompleted = true;
      render(messageForCurrentState());
      creationDescription.textContent = "容器已经创建并挂载。";
      creationStatus.textContent = "创建成功，可以开始使用。";
      creationStatus.dataset.tone = "success";
      updateCreationAvailability();
    } catch (_) {
      try {
        await refreshNativeState();
      } catch (_) {
        currentState = nativeState.unavailable;
        activeVolumes = true;
      }
      busy = false;
      render(messageForCurrentState());
      if (!creationDialog.hidden &&
          currentState === nativeState.authenticated) {
        creationStatus.textContent =
          "创建失败。请稍后重试。";
        creationStatus.dataset.tone = "error";
        updateCreationAvailability();
      }
    } finally {
      wipe(secondBytes);
      wipe(confirmationBytes);
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    }
  };

  const createContainer = async () => {
    const sizeMib = containerCapacityMib();
    if (sizeMib === null || creationKind !== "encrypted" ||
        currentState !== nativeState.authenticated) {
      creationStatus.textContent = "请检查容量设置。";
      creationStatus.dataset.tone = "error";
      updateCreationAvailability();
      return;
    }

    busy = true;
    creationStatus.textContent = "请选择保存位置，随后确认管理员权限。";
    creationStatus.dataset.tone = "";
    render(messageForCurrentState());
    try {
      const created = await bridge.createContainerMib(sizeMib);
      await refreshNativeState();
      busy = false;
      render(messageForCurrentState());
      if (created === false) {
        creationStatus.textContent = "已取消创建。";
        creationStatus.dataset.tone = "";
        updateCreationAvailability();
        return;
      }
      creationCompleted = true;
      creationDescription.textContent = "容器已经创建并挂载。";
      creationStatus.textContent = "创建成功，可以开始使用。";
      creationStatus.dataset.tone = "success";
      updateCreationAvailability();
    } catch (_) {
      try {
        await refreshNativeState();
      } catch (_) {
        currentState = nativeState.unavailable;
        activeVolumes = true;
      }
      busy = false;
      render(messageForCurrentState());
      if (!creationDialog.hidden &&
          currentState === nativeState.authenticated) {
        creationStatus.textContent =
          "创建失败。若容器已完成但未挂载，请重新插卡后重试。";
        creationStatus.dataset.tone = "error";
        updateCreationAvailability();
      }
    } finally {
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    }
  };

  for (const item of dashboardNavigation) {
    item.addEventListener("click", () => {
      if (!busy && currentState === nativeState.authenticated) {
        showDashboardSection(item.dataset.section);
      }
    });
  }
  operationHistoryClear.addEventListener("click", async () => {
    if (!busy && currentState === nativeState.authenticated &&
        operationEvents.length !== 0) {
      await runContainerAction(
        () => bridge.clearOperationHistory(),
        "无法清空当前会话日志，请稍后重试。");
    }
  });
  // Settings switches are page-local for now: nothing here reaches the native
  // bridge and nothing is written to disk, so a reload restores the defaults.
  // Each switch gets its store when its own slice lands.
  for (const control of settingSwitches) {
    control.addEventListener("click", () => {
      const enabled = control.getAttribute("aria-checked") === "true";
      control.setAttribute("aria-checked", enabled ? "false" : "true");
    });
  }
  for (const button of creationButtons) {
    button.addEventListener("click", () => {
      openCreationFlow();
    });
  }
  for (const button of creationTypeButtons) {
    button.addEventListener("click", () => {
      if (busy || creationStep !== "type") return;
      creationKind =
        button.dataset.createKind === "hidden" ? "hidden" : "encrypted";
      updateCreationAvailability();
    });
  }
  creationCloseButton.addEventListener("click", () => closeCreationFlow());
  creationCancelButton.addEventListener("click", () => {
    if (busy) return;
    if (creationStep === "params" && !creationCompleted) {
      returnToCreationTypes();
      return;
    }
    closeCreationFlow();
  });
  creationContinueButton.addEventListener("click", async () => {
    if (busy) return;
    if (creationStep === "type") {
      enterCreationParameters();
      return;
    }
    if (creationKind === "encrypted") {
      await createContainer();
    } else if (creationKind === "hidden") {
      await createContainerB();
    }
  });
  for (const button of mountExistingButtons) {
    button.addEventListener("click", async () => {
      if (!busy && currentState === nativeState.authenticated) {
        await mountExistingContainer();
      }
    });
  }
  for (const list of containerLists) {
    list.addEventListener("click", async (event) => {
      const target = event.target instanceof Element
        ? event.target.closest("[data-container-action]")
        : null;
      if (target === null || busy ||
          currentState !== nativeState.authenticated) {
        return;
      }
      const id = Number(target.dataset.containerId);
      if (!Number.isInteger(id) || id <= 0) return;
      if (target.dataset.containerAction === "unmount") {
        await unmountAdoptedContainer(id);
      } else {
        await mountAdoptedContainer(id);
      }
    });
  }
  for (const input of [containerCapacity, ...containerBInputs]) {
    for (const name of ["input", "change"]) {
      input.addEventListener(name, () => {
        creationStatus.textContent = "";
        creationStatus.dataset.tone = "";
        updateCreationAvailability();
      });
    }
  }
  for (const picker of unitPickers) {
    picker.button.addEventListener("click", (event) => {
      event.stopPropagation();
      if (picker.button.disabled) return;
      const opening = picker.list.hidden;
      closeUnitLists();
      picker.list.hidden = !opening;
      picker.button.setAttribute("aria-expanded", opening ? "true" : "false");
    });
    for (const option of picker.options) {
      option.addEventListener("click", (event) => {
        event.stopPropagation();
        applyCapacityUnit(picker, option.dataset.unit);
        closeUnitLists();
        creationStatus.textContent = "";
        creationStatus.dataset.tone = "";
        updateCreationAvailability();
        picker.button.focus();
      });
    }
  }
  document.addEventListener("click", () => closeUnitLists());
  creationDialog.addEventListener("click", (event) => {
    if (event.target === creationDialog) {
      closeCreationFlow();
    }
  });
  document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape" || creationDialog.hidden) return;
    // An open unit list is the innermost thing Escape can close.
    if (unitPickers.some((picker) => !picker.list.hidden)) {
      closeUnitLists();
      return;
    }
    closeCreationFlow();
  });
  dashboardUnmountButton.addEventListener("click", async () => {
    if (!busy && currentState === nativeState.authenticated && activeVolumes) {
      await unmount();
    }
  });

  actionButton.addEventListener("click", async () => {
    if (busy) {
      return;
    }
    if (activeVolumes &&
        (currentState === nativeState.authenticated ||
         currentState === nativeState.unavailable)) {
      await unmount();
      return;
    }
    if (acceptsLogin()) {
      if (passwordInput.value.length === 0) {
        render("请输入密码。");
        passwordInput.focus();
        return;
      }
      await submitLogin();
      return;
    }
    if (acceptsInitialization()) {
      if (!initializationInputReady()) {
        render("请输入并确认日常密码，至少12字符。");
        updateStrength();
        passwordInput.focus();
        return;
      }
      await submitInitialization();
    }
  });

  passwordInput.addEventListener("input", () => {
    clearError();
    updateStrength();
    updateActionAvailability();
  });
  confirmationInput.addEventListener("input", () => {
    clearError();
    updateActionAvailability();
  });
  passwordVisibilityButton.addEventListener("click", () => {
    setPasswordVisibility(
      passwordInput, passwordVisibilityButton,
      passwordVisibilityButton.getAttribute("aria-pressed") !== "true");
  });
  confirmationVisibilityButton.addEventListener("click", () => {
    setPasswordVisibility(
      confirmationInput, confirmationVisibilityButton,
      confirmationVisibilityButton.getAttribute("aria-pressed") !== "true");
  });
  passwordInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && acceptsLogin() && !passwordInput.disabled) {
      event.preventDefault();
      actionButton.click();
    }
  });
  confirmationInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && acceptsInitialization() &&
        !confirmationInput.disabled) {
      event.preventDefault();
      actionButton.click();
    }
  });

  (async () => {
    try {
      bridge = chrome.webview.hostObjects.nativeBridge;
      await refreshNativeState();
      busy = false;
      render(messageForCurrentState());
      updateStrength();
      focusCurrentInput();
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    } catch (_) {
      currentState = nativeState.unavailable;
      activeVolumes = true;
      busy = false;
      render("安全组件不可用，请尝试安全卸载。");
    }
  })();
})();
