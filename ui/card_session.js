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
  const fileContainerSelect = document.getElementById("file-container-select");
  const fileBreadcrumb = document.getElementById("file-breadcrumb");
  const fileListBody = document.getElementById("file-list-body");
  const fileListStatus = document.getElementById("file-list-status");
  const fileCreateDirectoryButton =
    document.getElementById("file-create-directory");
  const fileNameDialog = document.getElementById("file-name-dialog");
  const fileNameTitle = document.getElementById("file-name-title");
  const fileNameInput = document.getElementById("file-name-input");
  const fileNameStatus = document.getElementById("file-name-status");
  const fileNameCloseButton = document.getElementById("file-name-close");
  const fileNameCancelButton = document.getElementById("file-name-cancel");
  const fileNameConfirmButton = document.getElementById("file-name-confirm");
  const creationButtons =
    Array.from(document.querySelectorAll("[data-create-container]"));
  const creationTypeButtons =
    Array.from(document.querySelectorAll("[data-create-kind]"));
  const resetOpenButton = document.getElementById("reset-card-open");
  const forgetDialog = document.getElementById("forget-container-dialog");
  const forgetName = document.getElementById("forget-container-name");
  const forgetStatus = document.getElementById("forget-container-status");
  const forgetCloseButton = document.getElementById("forget-container-close");
  const forgetCancelButton = document.getElementById("forget-container-cancel");
  const forgetConfirmButton =
    document.getElementById("forget-container-confirm");
  // The container the open dialog is asking about. Zero means no question is
  // pending, so a stale confirmation can never act on the wrong row.
  let pendingForgetId = 0;

  const resetDialog = document.getElementById("reset-card-dialog");
  const resetCloseButton = document.getElementById("reset-card-close");
  const resetCancelButton = document.getElementById("reset-card-cancel");
  const resetConfirmButton = document.getElementById("reset-card-confirm");
  const resetAcknowledge = document.getElementById("reset-card-acknowledge");
  const resetStatus = document.getElementById("reset-card-status");
  const resetBlocked = document.getElementById("reset-card-blocked");
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
  const containerName = document.getElementById("create-container-name");
  const containerNameError =
    document.getElementById("create-container-name-error");
  const containerDirectory =
    document.getElementById("create-container-directory");
  const containerDirectoryError =
    document.getElementById("create-container-directory-error");
  const containerBrowse = document.getElementById("create-container-browse");
  const containerCapacity =
    document.getElementById("create-container-capacity");
  const containerError = document.getElementById("create-container-error");
  const creationStatus = document.getElementById("create-volume-status");
  const creationTypeNote = document.getElementById("create-type-note");
  const containerBForm = document.getElementById("create-container-b-form");
  const containerBName = document.getElementById("create-container-b-name");
  const containerBNameError =
    document.getElementById("create-container-b-name-error");
  const containerBDirectory =
    document.getElementById("create-container-b-directory");
  const containerBDirectoryError =
    document.getElementById("create-container-b-directory-error");
  const containerBBrowse =
    document.getElementById("create-container-b-browse");
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
    containerBName, containerBDirectory, containerBCapacity,
    containerBHiddenCapacity, containerBSecond,
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
  // Native capability gate for the destructive reset action. The entry layer
  // never keeps the one-shot login/initialization context for this; it performs
  // a new physical-card inspection only after the user confirms the action.
  let cardResetAvailable = false;

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

  // The page restates the rules `resolve_container_target_path` enforces, so a
  // mistyped field is named under the field instead of returning as an opaque
  // creation failure. The native check is still the only one that decides: it
  // runs again on whatever is submitted, including a directory this page got
  // back from the browse button.
  const kForbiddenNameCharacters = /[<>:"/\\|?*\u0000-\u001f\u007f]/;
  const kForbiddenPathCharacters = /[<>"|?*\u0000-\u001f\u007f]/;
  const kReservedDeviceNames = /^(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$/i;
  const kMaximumNameLength = 96;
  const kMaximumPathLength = 259;
  const kContainerExtension = ".hc";

  const containerNameProblem = (input) => {
    const value = input.value;
    if (value === "") return "请填写名称。";
    if (value.length > kMaximumNameLength) return "名称过长。";
    if (value.startsWith(" ")) return "名称不能以空格开头。";
    if (value.endsWith(" ") || value.endsWith(".")) {
      return "名称不能以空格或点号结尾。";
    }
    if (kForbiddenNameCharacters.test(value)) {
      return "名称不能包含 \\ / : * ? \" < > | 或控制字符。";
    }
    if (kReservedDeviceNames.test(value)) return "名称是系统保留名，请换一个。";
    return "";
  };

  const containerDirectoryProblem = (directoryInput, nameInput) => {
    const value = directoryInput.value;
    if (value === "") return "请填写路径。";
    if (!/^[A-Za-z]:[\\/]/.test(value)) {
      return "需要本机盘符路径，例如 D:\\Vault。";
    }
    if (kForbiddenPathCharacters.test(value)) {
      return "路径不能包含 * ? \" < > | 或控制字符。";
    }
    if (value.slice(2).includes(":")) return "路径只能有一个冒号。";
    const segments = value.slice(3).split(/[\\/]/);
    for (let index = 0; index < segments.length; index += 1) {
      const segment = segments[index];
      if (segment === "") {
        // Only one trailing separator is allowed to produce an empty segment.
        if (index !== segments.length - 1) return "路径有空的目录层级。";
        continue;
      }
      if (segment === "." || segment === "..") {
        return "路径不能包含 . 或 .. 层级。";
      }
      if (segment.startsWith(" ")) return "路径中的目录名不能以空格开头。";
      if (segment.endsWith(" ") || segment.endsWith(".")) {
        return "路径中的目录名不能以空格或点号结尾。";
      }
      if (kReservedDeviceNames.test(segment)) {
        return "路径含系统保留名。";
      }
    }
    // The joint limit lands on this field because the directory is the half a
    // user can shorten without renaming the container.
    const name = nameInput.value;
    const extension =
      name.toLowerCase().endsWith(kContainerExtension) ? 0
        : kContainerExtension.length;
    const separator = /[\\/]$/.test(value) ? 0 : 1;
    if (value.length + separator + name.length + extension >
        kMaximumPathLength) {
      return "路径与名称合计过长。";
    }
    return "";
  };

  const containerLocationReady = (nameInput, directoryInput) =>
    containerNameProblem(nameInput) === "" &&
    containerDirectoryProblem(directoryInput, nameInput) === "";

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
    return containerBSizes() !== null &&
      containerLocationReady(containerBName, containerBDirectory) &&
      second !== null &&
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
        ? containerCapacityMib() !== null &&
          containerLocationReady(containerName, containerDirectory)
        : hiddenVolume && containerBInputReady();
    creationDialog.dataset.step = creationStep;
    creationTypeStep.hidden = !choosing;
    containerForm.hidden = choosing || !encrypted;
    containerBForm.hidden = choosing || !hiddenVolume;
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
    containerName.disabled = containerCapacity.disabled;
    containerDirectory.disabled = containerCapacity.disabled;
    containerBrowse.disabled = containerCapacity.disabled;
    containerUnit.button.disabled = containerCapacity.disabled;
    for (const input of containerBInputs) {
      input.disabled = busy || !authenticated || choosing || !hiddenVolume ||
        creationCompleted;
    }
    containerBBrowse.disabled = containerBCapacity.disabled;
    containerBUnit.button.disabled = containerBCapacity.disabled;
    if (containerUnit.button.disabled && containerBUnit.button.disabled) {
      closeUnitLists();
    }
    const standardQuiet = choosing || !encrypted || creationCompleted;
    const hiddenQuiet = choosing || !hiddenVolume || creationCompleted;
    showFieldProblem(
      containerNameError,
      standardQuiet ? "" : containerNameProblem(containerName));
    showFieldProblem(
      containerDirectoryError,
      standardQuiet
        ? ""
        : containerDirectoryProblem(containerDirectory, containerName));
    showFieldProblem(
      containerError, standardQuiet ? "" : containerCapacityProblem());
    showFieldProblem(
      containerBNameError,
      hiddenQuiet ? "" : containerNameProblem(containerBName));
    showFieldProblem(
      containerBDirectoryError,
      hiddenQuiet
        ? ""
        : containerDirectoryProblem(containerBDirectory, containerBName));
    showFieldProblem(
      containerBError, hiddenQuiet ? "" : containerBProblem());
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

  // An empty description is hidden rather than left as a blank paragraph, so
  // the fields sit directly under the title on the parameter step.
  const setCreationDescription = (text) => {
    creationDescription.textContent = text;
    creationDescription.hidden = text === "";
  };

  const applyCreationStepCopy = () => {
    if (creationStep === "type") {
      creationTitle.textContent = "新建容器";
      setCreationDescription(kCreationTypeLead);
      return;
    }
    creationTitle.textContent = creationKind === "hidden"
      ? "新建含隐藏卷的容器"
      : "新建标准容器";
    setCreationDescription("");
  };

  // Leaving a parameter step discards what was typed on it: a decoy password
  // must not outlive the step that asked for it.
  const resetCreationInputs = () => {
    applyCapacityUnit(containerUnit, "mib");
    applyCapacityUnit(containerBUnit, "mib");
    closeUnitLists();
    containerName.value = "";
    containerDirectory.value = "";
    containerBName.value = "";
    containerBDirectory.value = "";
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

  const updateResetAvailability = () => {
    const ready = currentState === nativeState.authenticated &&
      cardResetAvailable;
    resetOpenButton.disabled = busy || !ready;
    // A greyed button with only a hover tooltip reads as a dead control, so
    // the reason it cannot run is written where the button is.
    const blocked = currentState === nativeState.authenticated &&
      !cardResetAvailable;
    resetBlocked.hidden = !blocked;
    resetBlocked.textContent = blocked
      ? "当前会话无法重新初始化安全卡。请拔出后重新插卡并登录。"
      : "";
    resetAcknowledge.disabled = busy || !ready;
    resetCloseButton.disabled = busy;
    resetCancelButton.disabled = busy;
    resetConfirmButton.disabled =
      busy || !ready || !resetAcknowledge.checked;
  };

  const openForgetFlow = (id, name) => {
    if (busy || currentState !== nativeState.authenticated) return;
    pendingForgetId = id;
    forgetName.textContent = name;
    forgetStatus.textContent = "";
    forgetStatus.dataset.tone = "";
    forgetDialog.hidden = false;
  };

  const closeForgetFlow = (force = false) => {
    if (busy && !force) return;
    pendingForgetId = 0;
    forgetDialog.hidden = true;
    forgetName.textContent = "";
    forgetStatus.textContent = "";
    forgetStatus.dataset.tone = "";
  };

  const closeResetFlow = (force = false) => {
    if (busy && !force) return;
    resetDialog.hidden = true;
    resetAcknowledge.checked = false;
    resetStatus.textContent = "";
    resetStatus.dataset.tone = "";
    updateResetAvailability();
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

  // ---- File manager ------------------------------------------------------
  // 🔴 The page never names a drive letter and never builds an absolute path.
  // It says which container and which directory inside it; the native side
  // turns that into a path that provably cannot leave the volume. A letter
  // here would let the page name any volume on the machine.
  //
  // The container being browsed, and where inside it. Zero means none is
  // chosen, which is what an empty container list leaves behind.
  let activeSection = "dashboard";
  let fileContainerId = 0;
  let filePath = [];
  let fileEntries = [];

  // One call brings a whole directory back: records separated by U+001E,
  // fields by U+001F. Neither can occur in a Windows file name, so there is
  // nothing to unescape. The first record is a header carrying the truncation
  // flag, so an empty directory arrives as "0" and never as "".
  const listingRecordSeparator = "\u001e";
  const listingFieldSeparator = "\u001f";

  const parseDirectoryListing = (packed) => {
    if (typeof packed !== "string" || packed.length === 0) {
      throw new Error("invalid directory listing");
    }
    const records = packed.split(listingRecordSeparator);
    if (records[0] !== "0" && records[0] !== "1") {
      throw new Error("invalid directory listing header");
    }
    const entries = [];
    for (let index = 1; index < records.length; index += 1) {
      const fields = records[index].split(listingFieldSeparator);
      if (fields.length !== 5) throw new Error("invalid directory entry");
      const [name, isDirectory, isLink, sizeBytes, modifiedUnixMs] = fields;
      const size = Number(sizeBytes);
      const modified = Number(modifiedUnixMs);
      if (name.length === 0 ||
          (isDirectory !== "0" && isDirectory !== "1") ||
          (isLink !== "0" && isLink !== "1") ||
          !Number.isSafeInteger(size) || size < 0 ||
          !Number.isSafeInteger(modified)) {
        throw new Error("invalid directory entry");
      }
      entries.push({
        name,
        isDirectory: isDirectory === "1",
        isLink: isLink === "1",
        sizeBytes: size,
        modifiedUnixMs: modified,
      });
    }
    return { truncated: records[0] === "1", entries };
  };

  const selectedFileContainer = () => {
    for (const container of containers) {
      if (container.id === fileContainerId) return container;
    }
    return null;
  };

  const formatEntrySize = (entry) => {
    if (entry.isDirectory) return "—";
    const units = ["B", "KB", "MB", "GB", "TB"];
    let value = entry.sizeBytes;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit += 1;
    }
    return unit === 0
      ? `${entry.sizeBytes} B`
      : `${value.toFixed(1)} ${units[unit]}`;
  };

  const setFileStatus = (message, tone = "") => {
    fileListStatus.textContent = message;
    fileListStatus.dataset.tone = tone;
    fileListStatus.hidden = message === "";
  };

  const renderFileContainerOptions = () => {
    // Browsing stays where it was as long as that container is still listed.
    let known = false;
    for (const container of containers) {
      if (container.id === fileContainerId) known = true;
    }
    if (!known) {
      fileContainerId = containers.length === 0 ? 0 : containers[0].id;
      filePath = [];
    }
    while (fileContainerSelect.firstChild !== null) {
      fileContainerSelect.removeChild(fileContainerSelect.firstChild);
    }
    for (const container of containers) {
      const option = document.createElement("option");
      option.value = String(container.id);
      option.textContent = container.mounted
        ? `${container.name} · ${container.drive}`
        : `${container.name}（未挂载）`;
      option.selected = container.id === fileContainerId;
      fileContainerSelect.appendChild(option);
    }
    fileContainerSelect.disabled = busy || containers.length === 0;
  };

  const renderFileBreadcrumb = () => {
    while (fileBreadcrumb.firstChild !== null) {
      fileBreadcrumb.removeChild(fileBreadcrumb.firstChild);
    }
    const container = selectedFileContainer();
    const crumbs = [container === null ? "容器" : container.name, ...filePath];
    for (let index = 0; index < crumbs.length; index += 1) {
      if (index !== 0) {
        const separator = document.createElement("span");
        separator.className = "file-crumb-separator";
        separator.textContent = "/";
        fileBreadcrumb.appendChild(separator);
      }
      const crumb = document.createElement("button");
      crumb.className = "file-crumb";
      crumb.type = "button";
      crumb.textContent = crumbs[index];
      if (index === crumbs.length - 1) {
        crumb.setAttribute("aria-current", "page");
        crumb.disabled = true;
      } else {
        crumb.dataset.fileDepth = String(index);
        crumb.disabled = busy;
      }
      fileBreadcrumb.appendChild(crumb);
    }
  };

  const appendFileNotice = (text) => {
    const row = document.createElement("tr");
    const cell = document.createElement("td");
    cell.colSpan = 4;
    cell.className = "file-empty";
    cell.textContent = text;
    row.appendChild(cell);
    fileListBody.appendChild(row);
  };

  const renderFileList = () => {
    while (fileListBody.firstChild !== null) {
      fileListBody.removeChild(fileListBody.firstChild);
    }
    const container = selectedFileContainer();
    if (container === null) {
      appendFileNotice("还没有容器。先新建或导入一个容器，再回到这里。");
      return;
    }
    if (!container.mounted) {
      appendFileNotice("这个容器没有挂载。到「容器」里挂载它，才能看到里面的文件。");
      return;
    }
    if (fileEntries.length === 0) {
      appendFileNotice("这个文件夹是空的。");
      return;
    }
    for (const entry of fileEntries) {
      const row = document.createElement("tr");
      const name = document.createElement("td");
      if (entry.isDirectory) {
        const open = document.createElement("button");
        open.className = "file-name-button";
        open.type = "button";
        open.textContent = entry.name;
        open.dataset.fileEnter = entry.name;
        open.disabled = busy;
        name.appendChild(open);
      } else {
        name.textContent = entry.name;
      }
      if (entry.isLink) {
        // Shown rather than hidden: a manager that silently omits what the
        // system file manager shows is worse than one that marks it.
        const mark = document.createElement("span");
        mark.className = "file-entry-link";
        mark.textContent = "链接";
        name.appendChild(mark);
      }
      const size = document.createElement("td");
      size.textContent = formatEntrySize(entry);
      const modified = document.createElement("td");
      modified.textContent = formatOperationTime(entry.modifiedUnixMs);
      const actions = document.createElement("td");
      actions.className = "file-actions";
      for (const [action, label, danger] of [
        ["rename", "重命名", false], ["delete", "删除", true],
      ]) {
        const button = document.createElement("button");
        button.className = danger ? "shell-button danger" : "shell-button";
        button.type = "button";
        button.textContent = label;
        button.disabled = busy;
        button.dataset.fileAction = action;
        button.dataset.fileName = entry.name;
        actions.appendChild(button);
      }
      row.append(name, size, modified, actions);
      fileListBody.appendChild(row);
    }
  };

  // What the open name dialog is for, and about which entry. An empty mode
  // means no question is pending, so a stale confirmation cannot act on a row
  // the user has since navigated away from.
  let fileNameMode = "";
  let fileNameTarget = "";

  const closeFileNameFlow = (force = false) => {
    if (busy && !force) return;
    fileNameMode = "";
    fileNameTarget = "";
    fileNameDialog.hidden = true;
    fileNameInput.value = "";
    fileNameStatus.textContent = "";
    fileNameStatus.dataset.tone = "";
  };

  const openFileNameFlow = (mode, name) => {
    const container = selectedFileContainer();
    if (busy || currentState !== nativeState.authenticated ||
        container === null || !container.mounted) {
      return;
    }
    fileNameMode = mode;
    fileNameTarget = name;
    fileNameTitle.textContent = mode === "rename" ? "重命名" : "新建文件夹";
    fileNameInput.value = mode === "rename" ? name : "";
    fileNameStatus.textContent = "";
    fileNameStatus.dataset.tone = "";
    fileNameDialog.hidden = false;
    fileNameInput.focus();
    fileNameInput.select();
  };

  // A file-manager action reports where the file manager is looking, not on
  // the dashboard: the two views are never open at once and the message
  // belongs next to the list it is about.
  const runFileAction = async (action, failure) => {
    busy = true;
    setFileStatus("");
    render(messageForCurrentState());
    let failed = false;
    try {
      await action();
      await refreshNativeState();
    } catch (_) {
      failed = true;
      try {
        await refreshNativeState();
      } catch (_) {
        currentState = nativeState.unavailable;
        activeVolumes = true;
      }
    }
    busy = false;
    render(messageForCurrentState());
    await refreshFileManager();
    if (failed) setFileStatus(failure, "error");
  };

  const deleteFileEntry = async (name) => {
    const container = selectedFileContainer();
    if (container === null || !container.mounted) return;
    // 🔴 Permanent, and asked once. The product decided this on 2026-09-08:
    // the same standard as the system file manager, with no recycle bin behind
    // it. The page says so in a standing notice rather than in a dialog,
    // because a notice is not a second question.
    await runFileAction(
      () => bridge.deleteContainerEntry(
        container.id, filePath.join("/"), name),
      "无法删除这个条目，它可能正在被使用或已经不在了。");
  };

  const submitFileName = async () => {
    const container = selectedFileContainer();
    const mode = fileNameMode;
    const target = fileNameTarget;
    const value = fileNameInput.value;
    if (busy || mode === "" || container === null || !container.mounted) return;
    if (value.length === 0 || (mode === "rename" && value === target)) {
      closeFileNameFlow();
      return;
    }
    closeFileNameFlow();
    if (mode === "rename") {
      await runFileAction(
        () => bridge.renameContainerEntry(
          container.id, filePath.join("/"), target, value),
        "无法改名，这个名字可能不合规或已经被占用。");
      return;
    }
    await runFileAction(
      () => bridge.createContainerDirectory(
        container.id, filePath.join("/"), value),
      "无法新建文件夹，这个名字可能不合规或已经存在。");
  };

  const refreshFileManager = async () => {
    renderFileContainerOptions();
    renderFileBreadcrumb();
    const container = selectedFileContainer();
    if (bridge === null || container === null || !container.mounted) {
      fileEntries = [];
      setFileStatus("");
      renderFileList();
      return;
    }
    try {
      const packed = await bridge.listContainerDirectory(
        container.id, filePath.join("/"));
      const listing = parseDirectoryListing(packed);
      fileEntries = listing.entries;
      setFileStatus(listing.truncated
        ? "这个文件夹里的条目太多，只显示了其中一部分。"
        : "");
    } catch (_) {
      // The root is the one place that always has somewhere to land, so a
      // folder that cannot be read does not strand the page inside it.
      fileEntries = [];
      filePath = [];
      renderFileBreadcrumb();
      setFileStatus("无法读取这个文件夹，它可能已经不在这个容器里了。", "error");
    }
    renderFileList();
  };

  const showDashboardSection = (sectionName) => {
    activeSection = sectionName;
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
    // Opening the view is what asks for a listing. Nothing polls: the page
    // reads a directory when the user arrives at it or moves inside it.
    if (sectionName === "files") void refreshFileManager();
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
      containerBName.focus();
    } else {
      containerName.focus();
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
      // Capacity is the attached volume's, not the container file's: a
      // container holding a hidden volume is larger than the volume the user
      // actually gets. An unmounted container has no volume, so it reads the
      // same as its drive letter does.
      containerFact(
        "容量",
        container.mounted && container.sizeMib > 0
          ? `${container.sizeMib.toLocaleString("zh-CN")} MiB`
          : "—"),
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

    // Offered only while the container is unmounted. The session refuses to
    // drop one that still holds a drive letter, so the button is not there to
    // be refused.
    if (!container.mounted) {
      const forget = document.createElement("button");
      forget.type = "button";
      forget.className = "shell-button ghost";
      forget.textContent = "从列表移除";
      forget.disabled = busy;
      forget.dataset.containerId = String(container.id);
      forget.dataset.containerName = container.name;
      forget.dataset.containerAction = "forget";
      actions.appendChild(forget);
    }

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
    // The picker follows the container list at no cost. The listing is not
    // re-read here: exactly one place owns that, so a message set after an
    // action cannot race a refresh a re-render fired off.
    renderFileContainerOptions();
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
    updateResetAvailability();
    if (!showDashboard) {
      closeResetFlow(true);
      closeForgetFlow(true);
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

  // The switches that have a store behind them. Every other switch on the
  // settings page is page-local and flips itself; these two are views of what
  // the native side holds, and are never set from what the user clicked, so a
  // refused or failed write cannot leave the page showing a setting that was
  // not saved.
  const storedSettings = [
    {
      setting: "remember-containers",
      read: () => bridge.getRememberContainerPaths(),
      write: (value) => bridge.setRememberContainerPaths(value),
    },
    {
      setting: "mount-imported",
      read: () => bridge.getMountImportedContainers(),
      write: (value) => bridge.setMountImportedContainers(value),
    },
  ];
  const storedSettingFor = (control) => storedSettings.find(
    (candidate) => candidate.setting === control.dataset.setting);

  const refreshStoredSwitches = async () => {
    for (const stored of storedSettings) {
      const control = settingSwitches.find(
        (candidate) => candidate.dataset.setting === stored.setting);
      if (control === undefined) continue;
      try {
        const value = await stored.read();
        if (value !== true && value !== false) {
          throw new Error("invalid stored setting");
        }
        control.setAttribute("aria-checked", value ? "true" : "false");
      } catch (_) {
        // An unreadable setting is not a licence to invent one. The switch
        // keeps the position it already had.
      }
    }
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
    let nextResetAvailable = false;
    if (nextState === nativeState.authenticated) {
      const resetAvailable = await bridge.canResetCardRecord();
      if (resetAvailable !== true && resetAvailable !== false) {
        throw new Error("invalid card reset availability");
      }
      nextResetAvailable = resetAvailable;
      await refreshStoredSwitches();
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
    cardResetAvailable = nextResetAvailable;
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
      if (activeSection === "files") void refreshFileManager();
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
      if (activeSection === "files") void refreshFileManager();
      setDashboardStatus(failure, "error");
    } finally {
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    }
  };

  const mountExistingContainer = () => runContainerAction(
    () => bridge.mountContainer(),
    "无法导入该容器，请确认文件仍在原位置且与当前安全卡匹配。");

  const mountAdoptedContainer = (id) => runContainerAction(
    () => bridge.mountAdoptedContainer(id),
    "无法挂载该容器，请稍后重试。");

  const unmountAdoptedContainer = (id) => runContainerAction(
    () => bridge.unmountContainer(id),
    "无法卸载该容器，请关闭占用文件后重试。");

  const forgetAdoptedContainer = (id) => runContainerAction(
    () => bridge.forgetContainer(id),
    "无法从列表移除该容器，请稍后重试。");

  const resetCardRecord = async () => {
    if (busy || currentState !== nativeState.authenticated ||
        !resetAcknowledge.checked) {
      return;
    }
    busy = true;
    resetStatus.textContent = "正在卸载全部卷并重写安全卡。";
    resetStatus.dataset.tone = "";
    render(messageForCurrentState());
    updateResetAvailability();
    try {
      await bridge.resetCardRecord();
      // The native side closes the window once the record is committed, so
      // the page stays on this message instead of routing to a state that
      // describes a card this session can no longer act on.
      resetStatus.textContent = "已重新初始化，正在关闭应用。";
      resetStatus.dataset.tone = "success";
    } catch (_) {
      try {
        await refreshNativeState();
      } catch (_) {
        currentState = nativeState.unavailable;
        activeVolumes = true;
      }
      busy = false;
      render(messageForCurrentState());
      updateResetAvailability();
      if (!resetDialog.hidden) {
        // Only the mounted case is knowable from here. Every other failure
        // gets a message that does not invent a cause, and none of them claim
        // the card is untouched: a failure inside the write cannot promise it.
        resetStatus.textContent = activeVolumes
          ? "重新初始化未开始：仍有卷处于挂载状态，请先全部卸载后重试。"
          : "重新初始化未能完成。请拔出安全卡重新插入后重试。";
        resetStatus.dataset.tone = "error";
      }
    }
  };

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
    creationStatus.textContent =
      "正在创建，随后写入安全卡并确认管理员权限。";
    creationStatus.dataset.tone = "";
    render(messageForCurrentState());
    // Cleared with the passwords below for the same reason: nothing about a
    // container location outlives the call that carried it.
    const name = containerBName.value;
    const directory = containerBDirectory.value;
    containerBName.value = "";
    containerBDirectory.value = "";
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
        // The password this session was opened under still does not reach the
        // page: the session holds it. The location does, and it is passed in
        // exactly the same shape as a standard container's, so the two calls
        // stay word-for-word parallel (8.7.2).
        await bridge.createContainerBMib(name, directory, sizes.outer,
                                         sizes.hidden, secondBytes,
                                         confirmationBytes);
      } finally {
        wipe(secondBytes);
        wipe(confirmationBytes);
      }
      await refreshNativeState();
      busy = false;
      render(messageForCurrentState());
      creationCompleted = true;
      setCreationDescription("容器已经创建并挂载。");
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
          "创建失败。重试前需要重新填写名称、路径与诱饵密码。";
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
    creationStatus.textContent = "正在创建，请确认管理员权限。";
    creationStatus.dataset.tone = "";
    render(messageForCurrentState());
    try {
      // The name and the location are ordinary strings, but a container path
      // is what section 8.4 keeps off this machine, so the fields are cleared
      // as soon as the values are handed over rather than left in the DOM.
      const name = containerName.value;
      const directory = containerDirectory.value;
      containerName.value = "";
      containerDirectory.value = "";
      await bridge.createContainerMib(name, directory, sizeMib);
      await refreshNativeState();
      busy = false;
      render(messageForCurrentState());
      creationCompleted = true;
      setCreationDescription("容器已经创建并挂载。");
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
          "创建失败。重试前需要重新填写名称与路径；" +
          "若容器已完成但未挂载，请重新插卡后重试。";
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
  fileContainerSelect.addEventListener("change", () => {
    const chosen = Number(fileContainerSelect.value);
    if (!Number.isInteger(chosen) || chosen <= 0) return;
    fileContainerId = chosen;
    filePath = [];
    void refreshFileManager();
  });
  fileBreadcrumb.addEventListener("click", (event) => {
    const crumb = event.target instanceof Element
      ? event.target.closest("[data-file-depth]")
      : null;
    if (crumb === null || busy) return;
    filePath = filePath.slice(0, Number(crumb.dataset.fileDepth));
    void refreshFileManager();
  });
  fileListBody.addEventListener("click", async (event) => {
    const element = event.target instanceof Element ? event.target : null;
    if (element === null || busy ||
        currentState !== nativeState.authenticated) {
      return;
    }
    const open = element.closest("[data-file-enter]");
    if (open !== null) {
      filePath = [...filePath, open.dataset.fileEnter];
      void refreshFileManager();
      return;
    }
    const act = element.closest("[data-file-action]");
    if (act === null) return;
    if (act.dataset.fileAction === "delete") {
      await deleteFileEntry(act.dataset.fileName);
      return;
    }
    openFileNameFlow("rename", act.dataset.fileName);
  });
  fileCreateDirectoryButton.addEventListener(
    "click", () => openFileNameFlow("create", ""));
  fileNameCloseButton.addEventListener("click", () => closeFileNameFlow());
  fileNameCancelButton.addEventListener("click", () => closeFileNameFlow());
  fileNameDialog.addEventListener("click", (event) => {
    if (event.target === fileNameDialog) closeFileNameFlow();
  });
  fileNameInput.addEventListener("keydown", async (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      await submitFileName();
    }
  });
  fileNameConfirmButton.addEventListener("click", async () => {
    await submitFileName();
  });
  operationHistoryClear.addEventListener("click", async () => {
    if (!busy && currentState === nativeState.authenticated &&
        operationEvents.length !== 0) {
      await runContainerAction(
        () => bridge.clearOperationHistory(),
        "无法清空当前会话日志，请稍后重试。");
    }
  });
  // The switches without a store stay page-local: nothing reaches the bridge
  // and a reload restores their defaults, and each gets its store when its own
  // slice lands.
  for (const control of settingSwitches) {
    control.addEventListener("click", async () => {
      if (control.getAttribute("aria-disabled") === "true") return;
      const enabled = control.getAttribute("aria-checked") === "true";
      const stored = storedSettingFor(control);
      if (stored === undefined) {
        control.setAttribute("aria-checked", enabled ? "false" : "true");
        return;
      }
      if (busy || currentState !== nativeState.authenticated) return;
      // The switch shows what the native side holds, never what was clicked.
      // A refused or failed write must leave it where it really is rather
      // than where the click implied.
      try {
        await stored.write(!enabled);
      } catch (_) {
        setDashboardStatus(
          "无法更改该设置，请稍后重试。", "error");
      }
      await refreshStoredSwitches();
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
  resetOpenButton.addEventListener("click", () => {
    if (busy || currentState !== nativeState.authenticated) return;
    resetAcknowledge.checked = false;
    resetStatus.textContent = "";
    resetStatus.dataset.tone = "";
    resetDialog.hidden = false;
    updateResetAvailability();
  });
  resetCloseButton.addEventListener("click", () => closeResetFlow());
  resetCancelButton.addEventListener("click", () => closeResetFlow());
  resetAcknowledge.addEventListener("change", () => updateResetAvailability());
  resetConfirmButton.addEventListener("click", async () => {
    await resetCardRecord();
  });
  resetDialog.addEventListener("click", (event) => {
    if (event.target === resetDialog) closeResetFlow();
  });
  forgetCloseButton.addEventListener("click", () => closeForgetFlow());
  forgetCancelButton.addEventListener("click", () => closeForgetFlow());
  forgetDialog.addEventListener("click", (event) => {
    if (event.target === forgetDialog) closeForgetFlow();
  });
  forgetConfirmButton.addEventListener("click", async () => {
    const id = pendingForgetId;
    if (busy || id === 0) return;
    closeForgetFlow();
    await forgetAdoptedContainer(id);
  });
  creationCloseButton.addEventListener("click", () => closeCreationFlow());
  creationCancelButton.addEventListener("click", () => {
    if (busy) return;
    if (creationStep === "params" && !creationCompleted) {
      returnToCreationTypes();
      return;
    }
    closeCreationFlow();
  });
  const browseForDirectory = async (input) => {
    if (busy || currentState !== nativeState.authenticated) return;
    busy = true;
    creationStatus.textContent = "";
    creationStatus.dataset.tone = "";
    updateCreationAvailability();
    try {
      const directory = await bridge.chooseContainerDirectory();
      // An empty string is a cancelled picker, which must leave whatever the
      // user had already typed alone.
      if (typeof directory === "string" && directory !== "") {
        input.value = directory;
      }
    } catch (_) {
      creationStatus.textContent = "无法打开选择框，请直接填写路径。";
      creationStatus.dataset.tone = "error";
    } finally {
      busy = false;
      render(messageForCurrentState());
      updateCreationAvailability();
      input.focus();
    }
  };

  containerBrowse.addEventListener(
    "click", () => void browseForDirectory(containerDirectory));
  containerBBrowse.addEventListener(
    "click", () => void browseForDirectory(containerBDirectory));
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
      if (target.dataset.containerAction === "forget") {
        openForgetFlow(id, target.dataset.containerName ?? "");
      } else if (target.dataset.containerAction === "unmount") {
        await unmountAdoptedContainer(id);
      } else {
        await mountAdoptedContainer(id);
      }
    });
  }
  for (const input of [containerName, containerDirectory, containerCapacity,
                       ...containerBInputs]) {
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
    if (event.key === "Escape" && !forgetDialog.hidden) {
      closeForgetFlow();
      return;
    }
    if (event.key === "Escape" && !resetDialog.hidden) {
      closeResetFlow();
      return;
    }
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
