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
  const managementContainers = document.getElementById("management-containers");
  const managementFiles = document.getElementById("management-files");
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
  const fileDropTargets = document.getElementById("file-drop-targets");
  const exportContainerList = document.getElementById("export-container-list");
  const backupExportLocation =
    document.getElementById("backup-export-location");
  const backupExportBrowse = document.getElementById("backup-export-browse");
  const backupExportConfirm = document.getElementById("backup-export-confirm");
  const backupExportStatus = document.getElementById("backup-export-status");
  const migrateContainerList =
    document.getElementById("migrate-container-list");
  const exportConfirmDialog = document.getElementById("export-confirm-dialog");
  const exportConfirmSummary =
    document.getElementById("export-confirm-summary");
  const exportConfirmCount = document.getElementById("export-confirm-count");
  const exportConfirmPhrase = document.getElementById("export-confirm-phrase");
  const exportConfirmStatus = document.getElementById("export-confirm-status");
  const exportConfirmClose = document.getElementById("export-confirm-close");
  const exportConfirmCancel = document.getElementById("export-confirm-cancel");
  const exportConfirmRun = document.getElementById("export-confirm-run");
  const exportModePlain = document.getElementById("export-mode-plain");
  const exportModeMigrate = document.getElementById("export-mode-migrate");
  const exportPlainPane = document.getElementById("export-plain-pane");
  const exportMigratePane = document.getElementById("export-migrate-pane");
  const backupMigrateLocation =
    document.getElementById("backup-migrate-location");
  const backupMigrateBrowse = document.getElementById("backup-migrate-browse");
  const backupMigrateConfirm =
    document.getElementById("backup-migrate-confirm");
  const backupMigrateStatus =
    document.getElementById("backup-migrate-status");
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
  let fileListingRevision = 0;

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

  const renderManagementView = () => {
    // Browsing stays where it was as long as that container is still listed.
    let known = false;
    for (const container of containers) {
      if (container.id === fileContainerId) known = true;
    }
    if (!known) {
      const departed = fileContainerId !== 0;
      fileContainerId = 0;
      filePath = [];
      fileEntries = [];
      if (departed) {
        fileListingRevision += 1;
        renderFileBreadcrumb();
        renderFileList();
        renderFileDropTargets();
      }
    }
    managementContainers.hidden = fileContainerId !== 0;
    managementFiles.hidden = fileContainerId === 0;
  };

  const renderFileBreadcrumb = () => {
    while (fileBreadcrumb.firstChild !== null) {
      fileBreadcrumb.removeChild(fileBreadcrumb.firstChild);
    }
    const container = selectedFileContainer();
    const back = document.createElement("button");
    back.type = "button";
    back.className = "file-crumb";
    back.textContent = "容器管理";
    back.dataset.fileBack = "";
    back.disabled = busy;
    fileBreadcrumb.appendChild(back);
    const crumbs = [container === null ? "容器" : container.name, ...filePath];
    for (let index = 0; index < crumbs.length; index += 1) {
      const separator = document.createElement("span");
      separator.className = "file-crumb-separator";
      separator.textContent = "/";
      fileBreadcrumb.appendChild(separator);
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
      appendFileNotice("这个容器没有挂载。返回「容器管理」列表挂载它，才能看到里面的文件。");
      return;
    }
    if (fileEntries.length === 0) {
      appendFileNotice("这个文件夹是空的。");
      return;
    }
    for (const entry of fileEntries) {
      const row = document.createElement("tr");
      // Dragging carries nothing but a marker: the page never hands the shell
      // a file, so a drag out of the window cannot become an export by
      // accident. Where it may land is decided below, in this page only.
      row.draggable = !busy;
      row.dataset.fileDrag = entry.name;
      if (entry.isDirectory && !entry.isLink) {
        row.dataset.fileDropDirectory = entry.name;
      }
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
        ["export", "导出", false], ["rename", "重命名", false],
        ["delete", "删除", true],
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
    // A transfer between two containers reads and writes every byte on the
    // thread that draws this page, so the disabled state has to reach the
    // screen before the call starts. One task boundary is what buys that.
    await new Promise((resolve) => { setTimeout(resolve, 0); });
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

  // ---- Dragging ----------------------------------------------------------
  // The entry being dragged, and whether this page took the drop. Empty means
  // no drag is in flight, so a stale drop cannot move an entry the user has
  // since navigated away from.
  let fileDragName = "";
  let fileDropHandled = false;
  let fileDropHighlight = null;

  const clearFileDropHighlight = () => {
    if (fileDropHighlight === null) return;
    fileDropHighlight.classList.remove("file-drop-over");
    fileDropHighlight = null;
  };

  const highlightFileDrop = (element) => {
    if (fileDropHighlight === element) return;
    clearFileDropHighlight();
    element.classList.add("file-drop-over");
    fileDropHighlight = element;
  };

  const transferFileEntry = async (name, destinationId, destinationPath,
                                   copy) => {
    const container = selectedFileContainer();
    if (container === null || !container.mounted || name === "") return;
    const here = filePath.join("/");
    if (!copy && destinationId === container.id && destinationPath === here) {
      return;
    }
    await runFileAction(
      () => (copy
        ? bridge.copyContainerEntry(
          container.id, here, name, destinationId, destinationPath)
        : bridge.moveContainerEntry(
          container.id, here, name, destinationId, destinationPath)),
      copy
        ? "无法复制到那里，目标可能已经有同名条目了。"
        : "无法移动到那里，目标可能已经有同名条目了。");
  };

  const renderFileDropTargets = () => {
    while (fileDropTargets.firstChild !== null) {
      fileDropTargets.removeChild(fileDropTargets.firstChild);
    }
    const container = selectedFileContainer();
    if (container === null || !container.mounted) return;
    const others = [];
    for (const other of containers) {
      if (other.mounted && other.id !== container.id) others.push(other);
    }
    if (others.length === 0) return;
    const hint = document.createElement("span");
    hint.className = "file-drop-hint";
    hint.textContent = "拖到这里放进另一个容器（按住 Ctrl 拖为复制）：";
    fileDropTargets.appendChild(hint);
    for (const other of others) {
      const chip = document.createElement("span");
      chip.className = "file-drop-chip";
      chip.textContent = `${other.name} · ${other.drive}`;
      chip.dataset.fileDropContainer = String(other.id);
      fileDropTargets.appendChild(chip);
    }
  };

  // 🔴 A drag that landed nowhere this page owns is the one gesture the file
  // manager has to answer for. Taking a file out of the volume is a plaintext
  // export, which lives behind the backup centre's confirmations, so the page
  // says where that is instead of doing it here.
  const reportFileDropRefused = () => {
    setFileStatus(
      "没有移动。文件只能放进本容器的文件夹，或另一个已挂载的容器。" +
      "要把文件拿出加密卷，那是明文导出，请到「导出与迁移」做。", "error");
  };

  // ---- Backup centre -----------------------------------------------------
  // 🔴 The plaintext export lives here and nowhere else. The file manager only
  // ever hands an entry over; what leaves the volume leaves from this section,
  // under the notice above it.
  //
  // ⚠️ There is no confirmation dialog. Q-04's acceptance items 1 and 2 ask for
  // two, the first of them explicitly non-skippable; the product decided on
  // 2026-09-09 not to have them. That decision, and the fact that it conflicts
  // with a client-signed requirement, is recorded in MILESTONES.md. The notice
  // stays because Q-04's wording can still be shown even when the blocking
  // screen is gone -- a notice is not a confirmation, and it stops nothing.
  //
  // What has been ticked, and where it goes. An empty selection means there is
  // nothing to export, which is what the page opens on.
  //
  // The key carries the directory as well as the name because the file manager
  // can hand over an entry from anywhere inside a container, while a card only
  // ever lists that container's root. Two entries of the same name in two
  // folders are two selections, and neither hides the other.
  const exportSelection = new Map();
  const exportKey = (id, directory, name) => `${id}\u0000${directory}\u0000${name}`;
  // One card is open at a time. Opening the next closes the last, so the panel
  // stays a list of containers rather than growing into a second file manager.
  let openExportContainer = 0;
  let openExportListing = null;
  let openExportError = "";
  // Which of the two exports the pane is showing. Plain is what the section
  // opens on: it is the one the file manager hands entries to.
  let exportMode = "plain";
  let exportLocation = "";
  // The migration export names a container of its own: the one being taken to
  // another machine is rarely the one being browsed.
  let migrateContainerId = 0;
  let migrateLocation = "";

  const exportGlyph = (paths, cls) => {
    const svg = document.createElementNS(kSvgNamespace, "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("aria-hidden", "true");
    if (cls !== undefined) svg.setAttribute("class", cls);
    for (const definition of paths) {
      const path = document.createElementNS(kSvgNamespace, "path");
      path.setAttribute("d", definition);
      svg.appendChild(path);
    }
    return svg;
  };

  const kBoxPaths = Object.freeze([
    "M5.5 6.5A1.5 1.5 0 017 5h10a1.5 1.5 0 011.5 1.5v11a1.5 1.5 0" +
      " 01-1.5 1.5H7a1.5 1.5 0 01-1.5-1.5z",
  ]);
  const kCheckPaths = Object.freeze(["M6 12.5l4 4 8-9"]);
  // Six dots, drawn as circles so the handle reads as a grip rather than a
  // glyph. They need their own builder because the icons elsewhere are paths.
  const kGripPaths = Object.freeze([]);
  const gripGlyph = () => {
    const svg = document.createElementNS(kSvgNamespace, "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("aria-hidden", "true");
    for (const [cx, cy] of [[9, 6], [15, 6], [9, 12], [15, 12], [9, 18], [15, 18]]) {
      const dot = document.createElementNS(kSvgNamespace, "circle");
      dot.setAttribute("cx", String(cx));
      dot.setAttribute("cy", String(cy));
      dot.setAttribute("r", "1.35");
      svg.appendChild(dot);
    }
    return svg;
  };
  const kFolderPaths = Object.freeze([
    "M4.5 7.5A1.5 1.5 0 016 6h3.6a1.5 1.5 0 011.2.6l1.05 1.4H18a1.5 1.5 0" +
      " 011.5 1.5v9a1.5 1.5 0 01-1.5 1.5H6a1.5 1.5 0 01-1.5-1.5z",
  ]);
  const kFilePaths = Object.freeze([
    "M13.5 3.5H7A1.5 1.5 0 005.5 5v14A1.5 1.5 0 007 20.5h10a1.5 1.5 0" +
      " 001.5-1.5V8.5z",
    "M13.5 3.5V8.5h5",
  ]);

  const checkBox = (on) => {
    const box = document.createElement("span");
    box.className = on ? "cbx on" : "cbx";
    if (on) box.appendChild(exportGlyph(kCheckPaths));
    return box;
  };

  const factCell = (key, value) => {
    const wrap = document.createElement("span");
    wrap.className = "fact";
    const k = document.createElement("span");
    k.className = "fk";
    k.textContent = key;
    const v = document.createElement("span");
    v.className = "fv";
    v.textContent = value;
    wrap.append(k, v);
    return wrap;
  };

  const containerSizeText = (container) => {
    const mib = container.sizeMib;
    return mib >= 1024
      ? `${(mib / 1024).toFixed(mib % 1024 === 0 ? 0 : 1)} GiB`
      : `${mib} MiB`;
  };

  // One container row, in the shape the design gives every container row: the
  // icon tile says mounted, the facts line up in monospace, and what the row
  // can be told to do sits at its end.
  // The order the user dragged the containers into. It holds identifiers
  // rather than containers because the container list is rebuilt from the
  // native side on every refresh, and an order that survives a refresh has to
  // be keyed by something that survives one.
  //
  // ⚠️ It lasts as long as the page does. Keeping it across sessions means a
  // field in the container index slot, which the format reserved room for but
  // which is not built; nothing here writes anything down.
  let containerOrder = [];

  const orderedContainers = () => {
    const byId = new Map();
    for (const container of containers) byId.set(container.id, container);
    const ordered = [];
    for (const id of containerOrder) {
      const container = byId.get(id);
      if (container !== undefined) {
        ordered.push(container);
        byId.delete(id);
      }
    }
    // Anything adopted since the last drag keeps its adoption order, at the end.
    for (const container of containers) {
      if (byId.has(container.id)) ordered.push(container);
    }
    return ordered;
  };

  const rememberContainerOrder = (ordered) => {
    containerOrder = ordered.map((container) => container.id);
  };

  const exportRow = (container, tail, onActivate) => {
    const row = document.createElement("div");
    row.className = "xrow";
    row.dataset.rowContainer = String(container.id);

    const grab = document.createElement("span");
    grab.className = "grab";
    grab.dataset.reorderGrip = String(container.id);
    grab.setAttribute("aria-hidden", "true");
    grab.appendChild(gripGlyph());
    grab.addEventListener("pointerdown", (event) => {
      beginContainerReorder(event, row, container.id);
    });
    row.appendChild(grab);

    const open = document.createElement("button");
    open.className = "xrow-open";
    open.type = "button";
    open.disabled = busy;
    open.addEventListener("click", onActivate);
    const icon = document.createElement("span");
    icon.className = container.mounted ? "ic on" : "ic";
    icon.appendChild(exportGlyph(kBoxPaths));
    open.appendChild(icon);
    const name = document.createElement("span");
    name.className = "nm";
    const title = document.createElement("span");
    title.className = "t";
    title.textContent = container.name;
    name.appendChild(title);
    open.appendChild(name);
    const facts = document.createElement("span");
    facts.className = "facts";
    facts.appendChild(factCell("盘符", container.mounted ? container.drive : "—"));
    open.appendChild(facts);
    row.appendChild(open);

    const ops = document.createElement("span");
    ops.className = "ops";
    const mount = document.createElement("button");
    mount.className = container.mounted ? "shell-button danger" : "shell-button";
    mount.type = "button";
    mount.disabled = busy;
    mount.dataset.rowMount = String(container.id);
    mount.textContent = container.mounted ? "卸载" : "挂载";
    mount.addEventListener("click", async () => {
      if (busy) return;
      if (container.mounted) {
        await unmountAdoptedContainer(container.id);
      } else {
        await mountAdoptedContainer(container.id);
      }
    });
    ops.appendChild(mount);
    ops.appendChild(tail);
    row.appendChild(ops);
    return row;
  };

  // Dragging starts from the handle and nowhere else, so the row body keeps
  // the meaning it already has. The drop lands between rows rather than on
  // one: a highlighted row would read as "put it inside this", which is what
  // a drag means everywhere else in this product.
  let reorderState = null;

  const rowsAround = (list) =>
    [...list.querySelectorAll(":scope > .xrow, :scope > div > .xrow, :scope > .container-group > .container-row")];

  const beginContainerReorder = (event, row, id) => {
    if (busy || reorderState !== null || event.button !== 0) return;
    const list = row.closest(".xlist, [data-container-list]");
    if (list === null) return;
    event.preventDefault();
    const marker = document.createElement("div");
    marker.className = "xins";
    reorderState = { list, row, id, marker, index: null, pointerId: event.pointerId };
    row.classList.add("xrow-ghost");
    try {
      event.target.setPointerCapture(event.pointerId);
    } catch (_) {
      // A pointer that cannot be captured still moves; the listeners below are
      // on the document, so the drag keeps working without it.
    }
    document.addEventListener("pointermove", moveContainerReorder);
    document.addEventListener("pointerup", endContainerReorder);
    document.addEventListener("pointercancel", cancelContainerReorder);
  };

  const moveContainerReorder = (event) => {
    if (reorderState === null || event.pointerId !== reorderState.pointerId) return;
    const rows = rowsAround(reorderState.list);
    let index = rows.length;
    for (let at = 0; at < rows.length; at += 1) {
      const box = rows[at].getBoundingClientRect();
      if (event.clientY < box.top + box.height / 2) {
        index = at;
        break;
      }
    }
    reorderState.index = index;
    const anchor = rows[index];
    if (anchor === undefined) {
      reorderState.list.appendChild(reorderState.marker);
    } else {
      const holder = anchor.parentElement === reorderState.list
        ? anchor
        : anchor.parentElement;
      reorderState.list.insertBefore(reorderState.marker, holder);
    }
  };

  const finishContainerReorder = () => {
    document.removeEventListener("pointermove", moveContainerReorder);
    document.removeEventListener("pointerup", endContainerReorder);
    document.removeEventListener("pointercancel", cancelContainerReorder);
    if (reorderState !== null) {
      reorderState.row.classList.remove("xrow-ghost");
      reorderState.marker.remove();
    }
  };

  const cancelContainerReorder = (event) => {
    if (event && reorderState !== null && event.pointerId !== reorderState.pointerId) return;
    finishContainerReorder();
    reorderState = null;
    renderContainers();
    renderBackupCentre();
  };

  const endContainerReorder = (event) => {
    if (reorderState === null || event.pointerId !== reorderState.pointerId) return;
    const { id, index } = reorderState;
    finishContainerReorder();
    reorderState = null;
    if (index !== null) {
      const ordered = orderedContainers();
      const from = ordered.findIndex((container) => container.id === id);
      if (from !== -1) {
        const [moved] = ordered.splice(from, 1);
        const to = index > from ? index - 1 : index;
        ordered.splice(Math.max(0, Math.min(to, ordered.length)), 0, moved);
        rememberContainerOrder(ordered);
      }
    }
    renderContainers();
    renderBackupCentre();
  };


  const selectionForContainer = (id) => {
    const picked = [];
    for (const entry of exportSelection.values()) {
      if (entry.id === id) picked.push(entry);
    }
    return picked;
  };

  const exportFileRow = (entry, checked, subtitle, onToggle) => {
    const row = document.createElement("button");
    row.className = "xfile";
    row.type = "button";
    row.disabled = busy;
    row.dataset.exportPick = entry.name;
    row.addEventListener("click", onToggle);
    row.appendChild(exportGlyph(
      entry.isDirectory ? kFolderPaths : kFilePaths,
      entry.isDirectory ? "g dir" : "g"));
    const name = document.createElement("span");
    name.className = "n";
    name.textContent = subtitle === "" || subtitle === undefined
      ? entry.name
      : `${subtitle} / ${entry.name}`;
    row.appendChild(name);
    const size = document.createElement("span");
    size.className = "s";
    size.textContent = entry.isDirectory ? "文件夹" : formatEntrySize(entry);
    row.appendChild(size);
    row.appendChild(checkBox(checked));
    return row;
  };

  const exportNote = (parent, text) => {
    const note = document.createElement("p");
    note.className = "xnote";
    note.textContent = text;
    parent.appendChild(note);
  };

  // 🔴 A card lists one container's root and nothing deeper. Ticking is what
  // this pane is for; browsing is the file manager's job, and building a
  // second one here would mean two places that can walk a volume.
  const renderExportContainers = () => {
    while (exportContainerList.firstChild !== null) {
      exportContainerList.removeChild(exportContainerList.firstChild);
    }
    if (containers.length === 0) {
      exportNote(exportContainerList, "还没有容器。");
      return;
    }
    for (const container of orderedContainers()) {
      const group = document.createElement("div");
      const open = container.id === openExportContainer;
      const picked = selectionForContainer(container.id);
      // The box at the end of a container row is that container's select-all.
      // It reads as full only when every root entry is ticked, so a container
      // holding one ticked file does not claim to be entirely chosen.
      const roots = open && openExportError === "" && openExportListing !== null
        ? openExportListing.entries
        : null;
      const every = roots !== null && roots.length !== 0 && roots.every(
        (entry) => exportSelection.has(exportKey(container.id, "", entry.name)));
      const tail = checkBox(every || (roots === null && picked.length !== 0));
      tail.dataset.exportSelectAll = String(container.id);
      tail.addEventListener("click", (event) => {
        event.stopPropagation();
        void toggleExportContainerAll(container.id, !every);
      });
      const head = exportRow(container, tail, () => {
        void toggleExportContainer(container.id);
      });
      head.dataset.exportToggle = String(container.id);
      head.setAttribute("aria-expanded", open ? "true" : "false");
      group.appendChild(head);

      if (open) {
        const files = document.createElement("div");
        files.className = "xfiles";
        if (!container.mounted) {
          exportNote(files, "这个容器没有挂载。");
        } else if (openExportError !== "") {
          exportNote(files, openExportError);
        } else if (openExportListing === null) {
          exportNote(files, "正在读取…");
        } else {
          const roots = openExportListing.entries;
          for (const entry of roots) {
            const key = exportKey(container.id, "", entry.name);
            files.appendChild(exportFileRow(
              entry, exportSelection.has(key), "",
              () => { toggleExportPick(container.id, "", entry); }));
          }
          const rootNames = new Set(roots.map((entry) => entry.name));
          for (const entry of picked) {
            if (entry.directory === "" && rootNames.has(entry.name)) continue;
            files.appendChild(exportFileRow(
              entry, true, entry.directory,
              () => { toggleExportPick(container.id, entry.directory, entry); }));
          }
          if (roots.length === 0 && picked.length === 0) {
            exportNote(files, "这个容器是空的。");
          }
          if (openExportListing.truncated) {
            exportNote(files, "条目太多，只列出了其中一部分。");
          }
        }
        group.appendChild(files);
      }
      exportContainerList.appendChild(group);
    }
  };

  // Every adopted container is offered, mounted or not, and a mounted one says
  // so in its state rather than quietly not being there.
  const renderMigrateContainers = () => {
    while (migrateContainerList.firstChild !== null) {
      migrateContainerList.removeChild(migrateContainerList.firstChild);
    }
    if (containers.length === 0) {
      exportNote(migrateContainerList, "还没有容器。");
      return;
    }
    let known = false;
    for (const container of containers) {
      if (container.id === migrateContainerId) known = true;
    }
    if (!known) migrateContainerId = 0;
    for (const container of orderedContainers()) {
      // 🔴 A mounted container cannot be migrated: the driver is writing to it,
      // so a copy taken now is a copy of something in flight. The box says so
      // by being unusable rather than by accepting a tick the export refuses.
      const pick = () => {
        if (busy || container.mounted) return;
        migrateContainerId =
          migrateContainerId === container.id ? 0 : container.id;
        setMigrateStatus("");
        renderBackupCentre();
      };
      const tail = checkBox(container.id === migrateContainerId);
      tail.classList.add(container.mounted ? "off" : "selectable");
      tail.dataset.migrateSelect = String(container.id);
      if (!container.mounted) {
        tail.addEventListener("click", (event) => {
          event.stopPropagation();
          pick();
        });
      }
      const row = exportRow(container, tail, pick);
      row.dataset.migratePick = String(container.id);
      migrateContainerList.appendChild(row);
    }
  };

  const renderBackupCentre = () => {
    const plain = exportMode === "plain";
    exportModePlain.setAttribute("aria-pressed", plain ? "true" : "false");
    exportModeMigrate.setAttribute("aria-pressed", plain ? "false" : "true");
    exportPlainPane.hidden = !plain;
    exportMigratePane.hidden = plain;
    exportModePlain.disabled = busy;
    exportModeMigrate.disabled = busy;

    renderExportContainers();
    backupExportLocation.textContent =
      exportLocation === "" ? "选择一个位置…" : exportLocation;
    backupExportLocation.classList.toggle("ph", exportLocation === "");
    backupExportBrowse.disabled = busy;
    backupExportConfirm.disabled =
      busy || exportSelection.size === 0 || exportLocation === "";
    backupExportConfirm.textContent = exportSelection.size === 0
      ? "开始导出"
      : `开始导出（${exportSelection.size}）`;

    renderMigrateContainers();
    let migrateReady = false;
    for (const container of containers) {
      if (container.id === migrateContainerId && !container.mounted) {
        migrateReady = true;
      }
    }
    backupMigrateLocation.textContent =
      migrateLocation === "" ? "选择一个位置…" : migrateLocation;
    backupMigrateLocation.classList.toggle("ph", migrateLocation === "");
    backupMigrateBrowse.disabled = busy;
    backupMigrateConfirm.disabled =
      busy || !migrateReady || migrateLocation === "";
  };


  const setBackupStatus = (message, tone = "") => {
    backupExportStatus.textContent = message;
    backupExportStatus.dataset.tone = tone;
  };

  const setMigrateStatus = (message, tone = "") => {
    backupMigrateStatus.textContent = message;
    backupMigrateStatus.dataset.tone = tone;
  };

  // 🔴 The one place this section reads a container. It reads the root and
  // only the root, so nothing here can become a second way to walk a volume,
  // and both the card and its select-all go through it.
  const readContainerRoot = async (id) => {
    try {
      return parseDirectoryListing(
        await bridge.listContainerDirectory(id, ""));
    } catch (_) {
      return null;
    }
  };

  // Opening a card reads that container's root.
  const toggleExportContainer = async (id) => {
    if (busy) return;
    if (openExportContainer === id) {
      openExportContainer = 0;
      openExportListing = null;
      openExportError = "";
      renderBackupCentre();
      return;
    }
    openExportContainer = id;
    openExportListing = null;
    openExportError = "";
    renderBackupCentre();
    let container = null;
    for (const candidate of containers) {
      if (candidate.id === id) container = candidate;
    }
    if (container === null || !container.mounted) return;
    const listing = await readContainerRoot(id);
    if (openExportContainer !== id) return;
    if (listing === null) {
      openExportError = "无法读取这个容器的根目录。";
    } else {
      openExportListing = listing;
    }
    renderBackupCentre();
  };

  const toggleExportPick = (id, directory, entry) => {
    const key = exportKey(id, directory, entry.name);
    if (exportSelection.has(key)) {
      exportSelection.delete(key);
    } else {
      exportSelection.set(key, {
        id,
        directory,
        name: entry.name,
        isDirectory: entry.isDirectory === true,
        sizeBytes: entry.sizeBytes,
        modifiedUnixMs: entry.modifiedUnixMs,
      });
    }
    setBackupStatus("");
    renderBackupCentre();
  };

  // Ticking a whole container from its own row. The root listing is what
  // "everything" means here, so a container that has not been read yet is read
  // first rather than ticking nothing and looking like it worked.
  const toggleExportContainerAll = async (id, checked) => {
    if (busy) return;
    if (!checked) {
      for (const entry of selectionForContainer(id)) {
        exportSelection.delete(exportKey(id, entry.directory, entry.name));
      }
      setBackupStatus("");
      renderBackupCentre();
      return;
    }
    let container = null;
    for (const candidate of containers) {
      if (candidate.id === id) container = candidate;
    }
    if (container === null || !container.mounted) {
      void toggleExportContainer(id);
      return;
    }
    let listing = openExportContainer === id && openExportError === ""
      ? openExportListing
      : null;
    if (listing === null) {
      listing = await readContainerRoot(id);
      if (listing === null) {
        void toggleExportContainer(id);
        return;
      }
      if (openExportContainer === id) openExportListing = listing;
    }
    for (const entry of listing.entries) {
      exportSelection.set(exportKey(id, "", entry.name), {
        id,
        directory: "",
        name: entry.name,
        isDirectory: entry.isDirectory === true,
        sizeBytes: entry.sizeBytes,
        modifiedUnixMs: entry.modifiedUnixMs,
      });
    }
    setBackupStatus("");
    renderBackupCentre();
  };

  // The file manager hands an entry over and steps aside. It does not export:
  // it ticks the entry here and opens the card it landed in.
  const offerEntryForExport = (name) => {
    const container = selectedFileContainer();
    if (container === null || !container.mounted) return;
    const directory = filePath.join("/");
    const entry = fileEntries.find((candidate) => candidate.name === name);
    exportSelection.set(exportKey(container.id, directory, name), {
      id: container.id,
      directory,
      name,
      isDirectory: entry !== undefined && entry.isDirectory === true,
      sizeBytes: entry === undefined ? 0 : entry.sizeBytes,
      modifiedUnixMs: entry === undefined ? 0 : entry.modifiedUnixMs,
    });
    setBackupStatus("");
    showDashboardSection("backup");
    void toggleExportContainer(container.id);
  };

  // 🔴 The plaintext export asks before it runs, and asks for something that
  // cannot be clicked through: the user has to type the words. Q-04 calls for
  // a non-skippable confirmation of exactly this kind, and with nothing about
  // an export written down anywhere, this screen is the only place the user is
  // ever told what is about to happen.
  const kExportConfirmPhrase = "明文导出";

  const updateExportConfirmAvailability = () => {
    exportConfirmRun.disabled =
      busy || exportConfirmPhrase.value.trim() !== kExportConfirmPhrase;
  };

  const closeExportConfirm = () => {
    exportConfirmDialog.hidden = true;
    exportConfirmPhrase.value = "";
    exportConfirmStatus.textContent = "";
    exportConfirmStatus.dataset.tone = "";
    updateExportConfirmAvailability();
  };

  const openExportConfirm = () => {
    if (busy || exportSelection.size === 0 || exportLocation === "") return;
    exportConfirmCount.textContent = `${exportSelection.size} 项`;
    exportConfirmSummary.textContent = exportLocation;
    exportConfirmPhrase.value = "";
    exportConfirmStatus.textContent = "";
    exportConfirmStatus.dataset.tone = "";
    exportConfirmDialog.hidden = false;
    updateExportConfirmAvailability();
    exportConfirmPhrase.focus();
  };

  // 🔴 Every ticked entry is exported by the one entry point that may leave a
  // volume, called once per entry. A directory goes across whole, because the
  // copy underneath it already walks one.
  //
  // ⚠️ The calls run in order on the thread that draws this page, so a large
  // selection freezes the window for as long as the whole of it takes. The
  // asynchronous path and its cancellation are still not built; this panel
  // makes that gap easier to reach, and nothing here hides it.
  const runExport = async () => {
    if (busy || exportSelection.size === 0 || exportLocation === "") return;
    closeExportConfirm();
    const requests = [...exportSelection.values()];
    const location = exportLocation;
    busy = true;
    setBackupStatus("");
    render(messageForCurrentState());
    // The same reason as a transfer: the copy runs on the thread that draws
    // this page, so the disabled state has to reach the screen first.
    await new Promise((resolve) => { setTimeout(resolve, 0); });
    const failed = [];
    for (const request of requests) {
      try {
        await bridge.exportContainerEntry(
          request.id, request.directory, request.name, location);
        exportSelection.delete(
          exportKey(request.id, request.directory, request.name));
      } catch (_) {
        failed.push(request);
      }
    }
    try {
      await refreshNativeState();
    } catch (_) {
      currentState = nativeState.unavailable;
      activeVolumes = true;
    }
    busy = false;
    render(messageForCurrentState());
    if (failed.length !== 0) {
      // What succeeded is already gone from the selection, so what stays
      // ticked is exactly what still has to be dealt with.
      const done = requests.length - failed.length;
      setBackupStatus(done === 0
        ? "导出没有完成。位置可能不可写、空间不足，或那里已经有同名文件了。"
        : `已导出 ${done} 项，${failed.length} 项没有完成，仍勾选着。` +
          "位置可能不可写、空间不足，或那里已经有同名文件了。", "error");
      renderBackupCentre();
      return;
    }
    // 🔴 Nothing about a finished export is kept: not the location, not the
    // entries, not that it happened. The page clears itself for the same
    // reason the native side writes nothing.
    exportLocation = "";
    setBackupStatus(
      `已导出 ${requests.length} 项。这些副本不再受本软件保护。`, "success");
    renderBackupCentre();
  };

  const runMigration = async () => {
    if (busy || migrateContainerId === 0 || migrateLocation === "") return;
    const id = migrateContainerId;
    const location = migrateLocation;
    busy = true;
    setMigrateStatus("");
    render(messageForCurrentState());
    // A whole container is copied on the thread that draws this page, and a
    // container is the largest thing this product moves.
    await new Promise((resolve) => { setTimeout(resolve, 0); });
    let failed = false;
    try {
      await bridge.exportContainerFile(id, location);
    } catch (_) {
      failed = true;
    }
    try {
      await refreshNativeState();
    } catch (_) {
      currentState = nativeState.unavailable;
      activeVolumes = true;
    }
    busy = false;
    render(messageForCurrentState());
    if (failed) {
      setMigrateStatus(
        "迁移没有完成。容器可能仍挂载着，位置不可写、空间不足，或那里已经有同名文件了。",
        "error");
      renderBackupCentre();
      return;
    }
    // The location goes the way the plaintext export's does, and for the same
    // reason: an export leaves nothing behind, on either side.
    migrateLocation = "";
    setMigrateStatus("已导出。换机后仍须原安全卡与原口令。", "success");
    renderBackupCentre();
  };

  const refreshFileManager = async () => {
    const revision = ++fileListingRevision;
    renderManagementView();
    renderFileBreadcrumb();
    renderFileDropTargets();
    const container = selectedFileContainer();
    if (bridge === null || container === null || !container.mounted) {
      fileEntries = [];
      setFileStatus("");
      renderFileList();
      return;
    }
    const requestedId = container.id;
    const requestedPath = filePath.join("/");
    try {
      const packed = await bridge.listContainerDirectory(
        container.id, filePath.join("/"));
      if (revision !== fileListingRevision || fileContainerId !== requestedId || filePath.join("/") !== requestedPath ||
          currentState !== nativeState.authenticated) return;
      const listing = parseDirectoryListing(packed);
      fileEntries = listing.entries;
      setFileStatus(listing.truncated
        ? "这个文件夹里的条目太多，只显示了其中一部分。"
        : "");
    } catch (_) {
      if (revision !== fileListingRevision || fileContainerId !== requestedId || filePath.join("/") !== requestedPath) return;
      // The root is the one place that always has somewhere to land, so a
      // folder that cannot be read does not strand the page inside it.
      fileEntries = [];
      filePath = [];
      renderFileBreadcrumb();
      setFileStatus("无法读取这个文件夹，它可能已经不在这个容器里了。", "error");
    }
    renderFileList();
  };

  const browseContainer = (id) => {
    if (busy || currentState !== nativeState.authenticated) return;
    fileContainerId = id;
    filePath = [];
    fileEntries = [];
    showDashboardSection("files");
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
    if (sectionName === "backup") renderBackupCentre();
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
  let expandedContainerId = 0;

  const containerActionButton = (container, action, label, disabled = false) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = action === "unmount" ? "shell-button danger" : "shell-button";
    button.textContent = label;
    button.disabled = busy || disabled;
    button.dataset.containerId = String(container.id);
    button.dataset.containerName = container.name;
    button.dataset.containerAction = action;
    return button;
  };

  const containerRow = (container, showOpenButton) => {
    const group = document.createElement("div");
    group.className = "container-group";
    const row = document.createElement("div");
    row.dataset.rowContainer = String(container.id);
    row.className = container.mounted
      ? "container-row mounted"
      : "container-row";
    row.dataset.busy = String(busy);
    row.addEventListener("click", (event) => {
      if (busy || !(event.target instanceof Element) ||
          event.target.closest("button") !== null) return;
      // The arrow is also the keyboard-accessible disclosure control. Row
      // clicks use that same action; buttons and the drag grip keep theirs.
      row.querySelector("[data-container-action=details]").click();
    });

    const grab = document.createElement("button");
    grab.type = "button";
    grab.className = "grab container-grab";
    grab.disabled = busy;
    grab.setAttribute("aria-label", `调整 ${container.name} 的顺序`);
    grab.title = "拖动排序；也可按 Alt + 上下方向键";
    grab.appendChild(gripGlyph());
    grab.addEventListener("pointerdown", (event) => {
      beginContainerReorder(event, row, container.id);
    });
    grab.addEventListener("keydown", (event) => {
      if (busy || !event.altKey || !["ArrowUp", "ArrowDown"].includes(event.key)) return;
      event.preventDefault();
      const ordered = orderedContainers();
      const from = ordered.findIndex((item) => item.id === container.id);
      const to = from + (event.key === "ArrowUp" ? -1 : 1);
      if (from < 0 || to < 0 || to >= ordered.length) return;
      const list = row.closest("[data-container-list]");
      [ordered[from], ordered[to]] = [ordered[to], ordered[from]];
      rememberContainerOrder(ordered);
      renderContainers();
      renderBackupCentre();
      list.querySelector(`[data-row-container="${container.id}"] .container-grab`).focus();
    });

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
    if (container.mounted && showOpenButton) {
      actions.appendChild(containerActionButton(container, "browse", "打开"));
    }
    actions.appendChild(containerActionButton(container,
      container.mounted ? "unmount" : "mount", container.mounted ? "卸载" : "挂载"));
    const remove = containerActionButton(container, "forget", "从列表移除", container.mounted);
    if (container.mounted) remove.title = "请先卸载容器";
    actions.appendChild(remove);
    const expanded = expandedContainerId === container.id;
    const details = containerActionButton(container, "details", "");
    details.className = "container-details-toggle";
    details.setAttribute("aria-label", `${expanded ? "收起" : "展开"} ${container.name} 的详情`);
    details.setAttribute("aria-expanded", String(expanded));
    details.appendChild(exportGlyph(["M6 9l6 6 6-6"]));
    row.append(grab, mark, identity, facts, actions, details);
    group.appendChild(row);

    if (expanded) {
      const panel = document.createElement("div");
      panel.className = "container-details";
      const grid = document.createElement("div");
      grid.className = "container-details-grid";
      grid.append(
        containerFact("名称", container.name),
        containerFact("状态", container.mounted ? "已挂载" : "未挂载"),
        containerFact("盘符", container.mounted ? container.drive : "—"),
        containerFact("容量", container.mounted && container.sizeMib > 0 ? containerSizeText(container) : "—"),
        containerFact("加密", "AES-256-XTS"),
        containerFact("派生", "Argon2id 512M·2·1"));
      const operations = document.createElement("div");
      operations.className = "container-detail-actions";
      operations.appendChild(containerActionButton(container, "browse", "浏览文件"));
      operations.appendChild(containerActionButton(container, "export", "导出与迁移"));
      const forget = containerActionButton(container, "forget", "从列表移除", container.mounted);
      forget.classList.add("container-forget");
      if (container.mounted) forget.title = "请先卸载容器";
      operations.appendChild(forget);
      panel.append(grid, operations);
      group.appendChild(panel);
    }
    return group;
  };

  const renderContainers = () => {
    if (reorderState !== null) {
      finishContainerReorder();
      reorderState = null;
    }
    if (currentState !== nativeState.authenticated) {
      fileListingRevision += 1;
      expandedContainerId = 0;
      containerOrder = [];
      fileContainerId = 0;
      filePath = [];
      fileEntries = [];
      renderFileBreadcrumb();
      renderFileList();
      renderFileDropTargets();
    }
    if (!containers.some((container) => container.id === expandedContainerId)) {
      expandedContainerId = 0;
    }
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
      for (const container of orderedContainers()) {
        list.appendChild(containerRow(container, list === managementContainers));
      }
    }
    // The management view follows the container list at no cost. The listing is not
    // re-read here: exactly one place owns that, so a message set after an
    // action cannot race a refresh a re-render fired off.
    renderManagementView();
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
    // This section is a view of the same native state the container list is,
    // so it has to be redrawn with it. Without this a mount made from one of
    // its own rows left the row saying what was true before the mount.
    renderBackupCentre();
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

    actionButton.classList.toggle("danger",
      currentState === nativeState.authenticated || currentState === nativeState.unavailable);
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
        if (item.dataset.section === "files") {
          fileContainerId = 0;
          filePath = [];
          fileEntries = [];
        }
        showDashboardSection(item.dataset.section);
      }
    });
  }
  fileBreadcrumb.addEventListener("click", (event) => {
    if (event.target instanceof Element && event.target.closest("[data-file-back]")) {
      if (busy) return;
      fileContainerId = 0;
      filePath = [];
      fileEntries = [];
      void refreshFileManager();
      return;
    }
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
    if (act.dataset.fileAction === "export") {
      offerEntryForExport(act.dataset.fileName);
      return;
    }
    openFileNameFlow("rename", act.dataset.fileName);
  });
  backupExportBrowse.addEventListener("click", async () => {
    if (busy || bridge === null) return;
    const chosen = await bridge.chooseContainerDirectory();
    if (typeof chosen !== "string" || chosen.length === 0) return;
    exportLocation = chosen;
    setBackupStatus("");
    renderBackupCentre();
  });
  // The section button opens the confirmation; only the dialog runs the export.
  backupExportConfirm.addEventListener("click", () => { openExportConfirm(); });
  exportConfirmPhrase.addEventListener("input", updateExportConfirmAvailability);
  exportConfirmPhrase.addEventListener("keydown", async (event) => {
    if (event.key !== "Enter" || exportConfirmRun.disabled) return;
    event.preventDefault();
    await runExport();
  });
  exportConfirmRun.addEventListener("click", async () => { await runExport(); });
  exportConfirmClose.addEventListener("click", closeExportConfirm);
  exportConfirmCancel.addEventListener("click", closeExportConfirm);
  exportConfirmDialog.addEventListener("click", (event) => {
    if (event.target === exportConfirmDialog) closeExportConfirm();
  });
  const chooseExportMode = (mode) => {
    if (busy || exportMode === mode) return;
    exportMode = mode;
    setBackupStatus("");
    setMigrateStatus("");
    renderBackupCentre();
  };
  exportModePlain.addEventListener("click", () => { chooseExportMode("plain"); });
  exportModeMigrate.addEventListener("click", () => {
    chooseExportMode("migrate");
  });
  backupMigrateBrowse.addEventListener("click", async () => {
    if (busy || bridge === null) return;
    const chosen = await bridge.chooseContainerDirectory();
    if (typeof chosen !== "string" || chosen.length === 0) return;
    migrateLocation = chosen;
    setMigrateStatus("");
    renderBackupCentre();
  });
  backupMigrateConfirm.addEventListener("click", async () => {
    await runMigration();
  });
  fileListBody.addEventListener("dragstart", (event) => {
    const row = event.target instanceof Element
      ? event.target.closest("[data-file-drag]")
      : null;
    if (row === null || busy || currentState !== nativeState.authenticated) {
      event.preventDefault();
      return;
    }
    fileDragName = row.dataset.fileDrag;
    fileDropHandled = false;
    row.classList.add("file-row-dragging");
    // Only a marker. No file, no path, and no name goes onto the drag, so
    // nothing outside this page can be handed the volume's contents.
    event.dataTransfer.effectAllowed = "copyMove";
    event.dataTransfer.setData("text/plain", "");
  });
  fileListBody.addEventListener("dragend", (event) => {
    const row = event.target instanceof Element
      ? event.target.closest("[data-file-drag]")
      : null;
    if (row !== null) row.classList.remove("file-row-dragging");
    clearFileDropHighlight();
    const dropped = fileDropHandled;
    fileDragName = "";
    fileDropHandled = false;
    if (!dropped) reportFileDropRefused();
  });

  const allowFileDrop = (event, element) => {
    event.preventDefault();
    event.dataTransfer.dropEffect = event.ctrlKey ? "copy" : "move";
    highlightFileDrop(element);
  };

  fileListBody.addEventListener("dragover", (event) => {
    const target = event.target instanceof Element
      ? event.target.closest("[data-file-drop-directory]")
      : null;
    if (target === null || fileDragName === "" || busy ||
        target.dataset.fileDropDirectory === fileDragName) {
      return;
    }
    allowFileDrop(event, target);
  });
  fileListBody.addEventListener("drop", async (event) => {
    const target = event.target instanceof Element
      ? event.target.closest("[data-file-drop-directory]")
      : null;
    if (target === null || fileDragName === "" || busy) return;
    event.preventDefault();
    clearFileDropHighlight();
    fileDropHandled = true;
    const name = fileDragName;
    fileDragName = "";
    const container = selectedFileContainer();
    if (container === null) return;
    await transferFileEntry(
      name, container.id,
      [...filePath, target.dataset.fileDropDirectory].join("/"),
      event.ctrlKey);
  });

  fileBreadcrumb.addEventListener("dragover", (event) => {
    const crumb = event.target instanceof Element
      ? event.target.closest("[data-file-depth]")
      : null;
    if (crumb === null || fileDragName === "" || busy) return;
    allowFileDrop(event, crumb);
  });
  fileBreadcrumb.addEventListener("drop", async (event) => {
    const crumb = event.target instanceof Element
      ? event.target.closest("[data-file-depth]")
      : null;
    if (crumb === null || fileDragName === "" || busy) return;
    event.preventDefault();
    clearFileDropHighlight();
    fileDropHandled = true;
    const name = fileDragName;
    fileDragName = "";
    const container = selectedFileContainer();
    if (container === null) return;
    await transferFileEntry(
      name, container.id,
      filePath.slice(0, Number(crumb.dataset.fileDepth)).join("/"),
      event.ctrlKey);
  });

  fileDropTargets.addEventListener("dragover", (event) => {
    const chip = event.target instanceof Element
      ? event.target.closest("[data-file-drop-container]")
      : null;
    if (chip === null || fileDragName === "" || busy) return;
    allowFileDrop(event, chip);
  });
  fileDropTargets.addEventListener("drop", async (event) => {
    const chip = event.target instanceof Element
      ? event.target.closest("[data-file-drop-container]")
      : null;
    if (chip === null || fileDragName === "" || busy) return;
    event.preventDefault();
    clearFileDropHighlight();
    fileDropHandled = true;
    const name = fileDragName;
    fileDragName = "";
    // The root of the other container: a drop onto its name says which
    // container, and says nothing about where inside it.
    await transferFileEntry(
      name, Number(chip.dataset.fileDropContainer), "", event.ctrlKey);
  });
  fileDropTargets.addEventListener("dragleave", () => clearFileDropHighlight());

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
      if (target === null || target.disabled || busy ||
          currentState !== nativeState.authenticated) {
        return;
      }
      const id = Number(target.dataset.containerId);
      if (!Number.isInteger(id) || id <= 0) return;
      if (target.dataset.containerAction === "forget") {
        openForgetFlow(id, target.dataset.containerName ?? "");
      } else if (target.dataset.containerAction === "unmount") {
        await unmountAdoptedContainer(id);
      } else if (target.dataset.containerAction === "mount") {
        await mountAdoptedContainer(id);
      } else if (target.dataset.containerAction === "browse") {
        browseContainer(id);
      } else if (target.dataset.containerAction === "details") {
        expandedContainerId = expandedContainerId === id ? 0 : id;
        renderContainers();
        list.querySelector(`[data-row-container="${id}"] [data-container-action="details"]`).focus();
      } else if (target.dataset.containerAction === "export") {
        showDashboardSection("backup");
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
    if (event.key === "Escape" && !exportConfirmDialog.hidden) {
      closeExportConfirm();
      return;
    }
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
