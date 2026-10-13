(() => {
  "use strict";
  // Fixed, non-secret readiness result polled by the native startup watchdog.
  window.usbVaultPageStartupStatus = 0;
  // Diagnostics never read input values or serialize rejected objects. The
  // native bridge enforces the same bounds again at the trust boundary.
  const pageEventQueue = [];
  let pageEventBridge = null;
  let pageEventRetryTimer = null;
  let pageEventsClosed = false;
  const retryPageEventBridge = () => {
    if (pageEventsClosed || pageEventRetryTimer !== null || !pageEventQueue.length) return;
    pageEventRetryTimer = setTimeout(() => {
      pageEventRetryTimer = null;
      try {
        const host = window.chrome?.webview?.hostObjects?.nativeBridge;
        if (host) connectPageEventBridge(host);
      } catch (_) { /* diagnostics only */ }
      if (pageEventBridge === null) retryPageEventBridge();
    }, 250);
  };
  const cleanPageEventText = (text, limit) => {
    if (typeof text !== "string") return "";
    return text.slice(0, limit).replace(/[\x00-\x1f\x7f-\x9f\u061c\u200e\u200f\u2028-\u202e\u2066-\u2069]/g, " ");
  };
  const sendPageEvent = (event) => {
    try {
      // Reporting failures must never report themselves or change UI state.
      void Promise.resolve(pageEventBridge.reportPageEvent(...event)).catch(() => {});
    } catch (_) { /* diagnostics only */ }
  };
  const logPageEvent = (category, source, content) => {
    try {
      if (pageEventsClosed || ![1, 2, 3, 4].includes(category) || typeof source !== "string" || typeof content !== "string") return;
      const event = [category, cleanPageEventText(source, 64), cleanPageEventText(content, 2000)];
      if (pageEventBridge === null) {
        if (pageEventQueue.length < 50) pageEventQueue.push(event);
        retryPageEventBridge();
      } else sendPageEvent(event);
    } catch (_) { /* diagnostics only */ }
  };
  const connectPageEventBridge = (host) => {
    if (!host || pageEventsClosed) return;
    pageEventBridge = host;
    if (pageEventRetryTimer !== null) clearTimeout(pageEventRetryTimer);
    pageEventRetryTimer = null;
    for (const event of pageEventQueue.splice(0)) sendPageEvent(event);
  };
  window.addEventListener("pagehide", () => {
    pageEventsClosed = true;
    if (pageEventRetryTimer !== null) clearTimeout(pageEventRetryTimer);
    pageEventQueue.length = 0;
    pageEventBridge = null;
  });
  // Only messages authored by this page are safe to persist verbatim. A
  // bridge rejection or arbitrary Error.message may contain a wiped secret.
  const safePageErrorMessages = new Set([
    "invalid BitLocker progress", "invalid BitLocker percentage", "consent rejected", "cancel rejected",
    "invalid creation progress", "invalid creation progress range", "unexpected pause result",
    "creation-cancelled", "creation-cleanup-failed", "native creation failed",
    "invalid directory listing", "invalid directory listing header", "invalid directory entry",
    "invalid protection status", "invalid progress", "invalid result", "survey failed", "invalid status",
    "invalid file progress", "invalid capacity facts", "invalid partition list", "partition list too large",
    "invalid partition row", "invalid partition facts", "scan rejected", "scan did not complete",
    "invalid partition display", "selection rejected", "invalid force preference", "invalid stored setting",
    "invalid native session state", "invalid reset availability", "invalid card reset availability",
    "invalid container count", "invalid container status", "invalid operation count", "invalid operation event",
    "session changed during refresh", "invalid preference", "session changed", "batch failed", "session ended",
    "invalid reset status", "card changed", "preparation failed", "invalid reset preparation", "reset incomplete",
    "creation rejected", "conversion rejected", "partition creation rejected", "invalid credential status",
    "invalid startup view", "invalid window preference", "invalid creation failure code", "invalid volume operation status",
  ]);
  const pageErrorProperty = (error, key) => {
    try {
      if (error === null || (typeof error !== "object" && typeof error !== "function")) return undefined;
      // Data properties only: do not execute message/stack/reason getters.
      return Object.getOwnPropertyDescriptor(error, key)?.value;
    } catch (_) { return undefined; /* diagnostics only */ }
  };
  const pageErrorMessage = (error) => {
    const message = typeof error === "string" ? error : pageErrorProperty(error, "message");
    return safePageErrorMessages.has(message) ? message : "异常原文未列入安全白名单，已省略";
  };
  const pageErrorDetail = (error) => {
    let type = "未知异常";
    try {
      const name = pageErrorProperty(error, "name") ??
        (error && typeof error === "object" ? pageErrorProperty(Object.getPrototypeOf(error), "name") : undefined);
      if (["Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError", "URIError", "EvalError"].includes(name)) type = name;
    } catch (_) { /* diagnostics only */ }
    let detail = `${type}；${pageErrorMessage(error)}`;
    const code = pageErrorProperty(error, "number") ?? pageErrorProperty(error, "code");
    if (Number.isInteger(code) && code >= -2147483648 && code <= 4294967295) detail += `；异常数值码 ${code}`;
    const stack = pageErrorProperty(error, "stack");
    if (typeof stack === "string") {
      // Keep only our script's frame coordinates, never the message header,
      // arbitrary function names, URLs, query strings or filesystem paths.
      const frames = stack.slice(0, 8000).match(/\bcard_session\.js:\d{1,8}:\d{1,8}\b/g);
      if (frames) detail += `；调用栈 ${frames.slice(0, 6).join(" ← ")}`;
    }
    return detail;
  };
  const reportedPageErrors = new WeakSet();
  const logCaughtPageError = (source, error, category = 2) => {
    try {
      const message = pageErrorMessage(error);
      if (message === "creation-cancelled") return; // Normal requested cancellation.
      if (error && typeof error === "object") {
        if (reportedPageErrors.has(error)) return;
        reportedPageErrors.add(error);
      }
      if (/^(session changed|session ended|card changed)/.test(message)) category = 4;
      else if (/^(invalid |unexpected |partition list too large)/.test(message)) category = 2;
      logPageEvent(category, source, pageErrorDetail(error));
    } catch (_) { /* diagnostics only */ }
  };
  const markStartupScriptFailure = () => {
    if (window.usbVaultPageStartupStatus === 0) window.usbVaultPageStartupStatus = 1408;
  };
  const reportUnhandledPageError = (source, error, file, line, column) => {
    markStartupScriptFailure();
    const script = typeof file === "string" && /(?:^|\/)card_session\.js(?:[?#]|$)/.test(file)
      ? "card_session.js" : "未知脚本";
    const position = (value) => Number.isInteger(value) && value >= 0 && value <= 100000000 ? value : 0;
    logPageEvent(3, source, `${pageErrorDetail(error)}；文件 ${script}；行 ${position(line)}；列 ${position(column)}`);
  };
  window.addEventListener("error", (event) => {
    reportUnhandledPageError("window.error", event.error ?? event.message, event.filename, event.lineno, event.colno);
  });
  window.addEventListener("unhandledrejection", (event) => {
    reportUnhandledPageError("window.unhandledrejection", event.reason);
  });

  // Register mappings without logging: calculating a message is not evidence
  // that it was displayed. The display observer joins it to the final text.
  const pageErrorMappings = new Map();
  const mappedPageError = (text, code, origin) => {
    if (pageErrorMappings.size >= 50) pageErrorMappings.delete(pageErrorMappings.keys().next().value);
    pageErrorMappings.set(text, `${origin} ${Number.isInteger(code) ? code : "无效"} → ${text}`);
    return text;
  };
  const logShownPageError = (source, text, mapping) => {
    if (mapping === undefined) mapping = [...pageErrorMappings].reverse().find(([message]) => text.startsWith(message))?.[1] || "";
    const code = text.match(/错误码[：:\s（(]*([0-9]+)/)?.[1];
    logPageEvent(1, source, `显示错误码 ${code || "无"}；${mapping ? mapping + "；" : ""}提示 ${text}`);
  };
  // Explicit text-only surfaces. Never observe a form, input, credential
  // preview, whole dialog or whole document for its textContent.
  const pageErrorSurfaces = [
    "dashboard-status", "card-session-status", "card-session-error", "setup-status", "password-strength-description",
    "create-volume-status", "partition-status", "forgot-password-status", "reset-card-status",
    "forget-container-status", "file-name-status", "file-list-status",
    "backup-export-status", "backup-migrate-status", "settings-window-size-status",
    "decoy-verify-status", "decoy-results-status", "decoy-maintenance-status", "decoy-drop-status",
    "decoy-protection-status", "batch-result-setting-status",
    "create-container-name-error", "create-container-directory-error", "create-container-error",
    "create-container-b-name-error", "create-container-b-directory-error", "create-container-b-error",
  ];
  const pageErrorAlwaysMarked = new Set([
    "card-session-error", "settings-window-size-status", "decoy-protection-status", "batch-result-setting-status",
    ...pageErrorSurfaces.filter((id) => id.startsWith("create-container") && id.endsWith("-error")),
  ]);
  const lastShownPageErrors = new Map();
  const pageErrorVisible = (element) => element && !element.closest("[hidden]") &&
    !element.closest(".dashboard-section:not(.active)");
  const capturePageErrors = () => {
    try {
      for (const id of pageErrorSurfaces) {
        const element = document.getElementById(id);
        const strengthError = id === "password-strength-description" &&
          document.getElementById("card-session-surface").dataset.sessionState === "initialization-required" &&
          element.closest("#password-strength").dataset.level === "invalid";
        const text = element && pageErrorVisible(element) &&
          (strengthError || element.dataset.tone === "error" || pageErrorAlwaysMarked.has(id)) ? element.textContent.trim() : "";
        if (text && lastShownPageErrors.get(id) !== text) {
          if (strengthError || (id.startsWith("create-container") && id.endsWith("-error"))) logPageEvent(2, id, `输入校验失败；${text}`);
          logShownPageError(id, text);
        }
        lastShownPageErrors.set(id, text);
      }
      const dialog = document.getElementById("volume-operation-dialog");
      const text = pageErrorVisible(dialog) && dialog.dataset.result === "error"
        ? document.getElementById("volume-operation-title").textContent + "；" +
          document.getElementById("volume-operation-note").textContent : "";
      if (text && lastShownPageErrors.get("volume-operation") !== text) logShownPageError("volume-operation", text);
      lastShownPageErrors.set("volume-operation", text);
    } catch (_) { /* diagnostics only */ }
  };
  const pageErrorObserver = new MutationObserver(capturePageErrors);
  pageErrorObserver.observe(document.body, {subtree: true, childList: true, characterData: true,
    attributes: true, attributeFilter: ["data-tone", "data-result", "data-level", "data-session-state", "hidden", "class"]});

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
  const configurationButton = document.getElementById("open-configuration");
  const backupButtons = [document.getElementById("card-backup-settings")];
  const sessionTools = document.getElementById("session-tools");
  const sessionToolsToggle = document.getElementById("session-tools-toggle");
  const sessionToolsMenu = document.getElementById("session-tools-menu");
  const closeSessionTools = () => {
    sessionToolsMenu.hidden = true;
    sessionToolsToggle.setAttribute("aria-expanded", "false");
  };
  sessionToolsToggle.addEventListener("click", () => {
    if (sessionToolsToggle.disabled) return;
    const open = sessionToolsMenu.hidden;
    sessionToolsMenu.hidden = !open;
    sessionToolsToggle.setAttribute("aria-expanded", String(open));
  });
  document.addEventListener("click", event => {
    if (!sessionTools.contains(event.target)) closeSessionTools();
  });
  sessionTools.addEventListener("keydown", event => {
    if (event.key === "Escape") {
      event.preventDefault(); closeSessionTools(); sessionToolsToggle.focus();
    }
  });
  const appExit = document.getElementById("app-exit");
  const appExitToggle = document.getElementById("app-exit-toggle");
  const appExitMenu = document.getElementById("app-exit-menu");
  const returnToLogin = document.getElementById("return-to-login");
  const exitApplication = document.getElementById("exit-application");
  const closeAppExit = () => {
    appExitMenu.hidden = true;
    appExitToggle.setAttribute("aria-expanded", "false");
  };
  appExitToggle.addEventListener("click", () => {
    if (busy || currentState !== nativeState.authenticated) return;
    const open = appExitMenu.hidden;
    appExitMenu.hidden = !open;
    appExitToggle.setAttribute("aria-expanded", String(open));
    if (open) returnToLogin.focus();
  });
  document.addEventListener("click", event => { if (!appExit.contains(event.target)) closeAppExit(); });
  appExit.addEventListener("keydown", event => {
    if (event.key === "Escape") { event.preventDefault(); closeAppExit(); appExitToggle.focus(); }
  });
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
  const dashboardVolumeState =
    document.getElementById("dashboard-volume-state");
  const dashboardContainerCount =
    document.getElementById("dashboard-container-count");
  const navContainerCount = document.getElementById("nav-container-count");
  const dashboardStatus = document.getElementById("dashboard-status");
  const windowSizeSelect = document.getElementById("settings-window-size");
  const windowSizeStatus = document.getElementById("settings-window-size-status");
  let appliedWindowSize = "1";
  if (windowSizeSelect !== null) windowSizeSelect.addEventListener("change", async () => {
    const value = windowSizeSelect.value;
    if (!["0", "1", "2", "3"].includes(value)) return;
    windowSizeSelect.disabled = true;
    windowSizeStatus.textContent = "";
    try {
      await bridge.setWindowSize(Number(value));
      appliedWindowSize = value;
    } catch (pageError) { logCaughtPageError("windowSizeSelect.change/1", pageError);
      windowSizeSelect.value = appliedWindowSize;
      windowSizeStatus.textContent = "无法调整窗口尺寸，请重试。";
    } finally {
      windowSizeSelect.disabled = false;
    }
  });
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
  const fileBreadcrumbHome = fileBreadcrumb.parentNode;
  const fileBreadcrumbNext = fileBreadcrumb.nextSibling;
  const managementFilesHome = managementFiles.parentNode;
  const managementFilesNext = managementFiles.nextSibling;
  const fileContextMenu = document.getElementById("file-context-menu");
  const fileContextRename = document.getElementById("file-context-rename");
  const fileListBody = document.getElementById("file-list-body");
  const fileListStatus = document.getElementById("file-list-status");
  const fileCreateDirectoryButton =
    document.getElementById("file-create-directory");
  const fileRefreshButton = document.getElementById("file-refresh");
  const fileMountToggle = document.getElementById("file-mount-toggle");
  const fileNameDialog = document.getElementById("file-name-dialog");
  const fileNameTitle = document.getElementById("file-name-title");
  const fileNameInput = document.getElementById("file-name-input");
  const fileNameStatus = document.getElementById("file-name-status");
  const fileNameCloseButton = document.getElementById("file-name-close");
  const fileNameCancelButton = document.getElementById("file-name-cancel");
  const fileNameConfirmButton = document.getElementById("file-name-confirm");
  const decoyVerify = document.getElementById("decoy-verify");
  const decoyPassword = document.getElementById("decoy-password");
  const decoyPasswordVisibility = document.getElementById("decoy-password-visibility");
  const decoyVerifySubmit = document.getElementById("decoy-verify-submit");
  const decoyVerifyStatus = document.getElementById("decoy-verify-status");
  const decoyProgress = document.getElementById("decoy-progress");
  const decoyProgressCopy = document.getElementById("decoy-progress-copy");
  const decoyProgressBar = document.getElementById("decoy-progress-bar");
  const decoyProgressCancel = document.getElementById("decoy-progress-cancel");
  const decoyResults = document.getElementById("decoy-results");
  const decoyExit = document.getElementById("decoy-exit");
  const decoyRefresh = document.getElementById("decoy-refresh");
  const decoyContainerList = document.getElementById("decoy-container-list");
  const decoyResultsStatus = document.getElementById("decoy-results-status");
  const decoyOverviewHeader = document.getElementById("decoy-overview-header");
  const decoyMaintenance = document.getElementById("decoy-maintenance");
  const decoyMaintenanceBreadcrumbSlot =
    document.getElementById("decoy-maintenance-breadcrumb-slot");
  const decoyMaintenanceFilesSlot =
    document.getElementById("decoy-maintenance-files-slot");
  const decoyMaintenanceContainer =
    document.getElementById("decoy-maintenance-container");
  const decoyMaintenanceDrive = document.getElementById("decoy-maintenance-drive");
  const decoyMaintenanceEnd = document.getElementById("decoy-maintenance-end");
  const decoyMaintenanceStatus =
    document.getElementById("decoy-maintenance-status");
  const decoyMaintenanceWritable =
    document.getElementById("decoy-maintenance-writable");
  const decoyMaintenanceSpaceDetail =
    document.getElementById("decoy-maintenance-space-detail");
  const decoyMaintenanceUsed = document.getElementById("decoy-maintenance-used");
  const decoyMaintenanceTotal = document.getElementById("decoy-maintenance-total");
  const decoyMaintenanceSpaceBar =
    document.getElementById("decoy-maintenance-space-bar");
  const decoyMaintenanceFreshness =
    document.getElementById("decoy-maintenance-freshness");
  const decoyDropZone = document.getElementById("decoy-drop-zone");
  const decoyDropRun = document.getElementById("decoy-drop-run");
  const decoyDropCopy = document.getElementById("decoy-drop-copy");
  const decoyDropBar = document.getElementById("decoy-drop-bar");
  const decoyDropCancel = document.getElementById("decoy-drop-cancel");
  const decoyDropStatus = document.getElementById("decoy-drop-status");
  const decoyEmptyDialog = document.getElementById("decoy-empty-dialog");
  const decoyEmptyClose = document.getElementById("decoy-empty-close");
  const decoyConfirmDialog = document.getElementById("decoy-confirm-dialog");
  const decoyConfirmClose = document.getElementById("decoy-confirm-close");
  const decoyConfirmCancel = document.getElementById("decoy-confirm-cancel");
  const decoyConfirmAccept = document.getElementById("decoy-confirm-accept");
  const exportContainerList = document.getElementById("export-container-list");
  const backupExportLocation =
    document.getElementById("backup-export-location");
  const backupExportBrowse = document.getElementById("backup-export-browse");
  const backupExportConfirm = document.getElementById("backup-export-confirm");
  const backupExportStatus = document.getElementById("backup-export-status");
  const migrateContainerList =
    document.getElementById("migrate-container-list");
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

  const forgotOpen = document.getElementById("forgot-password-open");
  const forgotDialog = document.getElementById("forgot-password-dialog");
  const forgotTitle = document.getElementById("forgot-password-title");
  const forgotIntro = document.getElementById("forgot-password-intro");
  const forgotMounts = document.getElementById("forgot-password-mounts");
  const forgotLead = document.getElementById("forgot-password-mount-lead");
  const forgotDrives = document.getElementById("forgot-password-drives");
  const forgotGuidance = document.getElementById("forgot-password-mount-guidance");
  const forgotAcknowledge = document.getElementById("forgot-password-acknowledge");
  const forgotStatus = document.getElementById("forgot-password-status");
  const forgotClose = document.getElementById("forgot-password-close");
  const forgotCancel = document.getElementById("forgot-password-cancel");
  const forgotConfirm = document.getElementById("forgot-password-confirm");
  const forgotForce = document.getElementById("forgot-password-force");
  const forgotSkip = document.getElementById("forgot-password-skip");
  let forgotAvailable = false, forgotRunning = false, forgotTicket = 0, forgotRevision = 0;
  const resetDialog = document.getElementById("reset-card-dialog");
  const resetCloseButton = document.getElementById("reset-card-close");
  const resetCancelButton = document.getElementById("reset-card-cancel");
  const resetConfirmButton = document.getElementById("reset-card-confirm");
  const resetAcknowledge = document.getElementById("reset-card-acknowledge");
  const resetStatus = document.getElementById("reset-card-status");
  const resetBlocked = document.getElementById("reset-card-blocked");
  const creationDialog = document.getElementById("create-volume-dialog");
  const creationStorageSwitch = document.getElementById("create-storage-switch");
  const creationStorageButtons = Array.from(document.querySelectorAll("[data-create-storage]"));
  const creationDiskStep = document.getElementById("create-disk-step");
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
  const creationProgress = document.getElementById("creation-progress");
  const creationProgressTime = document.getElementById("creation-progress-time");
  const creationProgressWork = document.getElementById("creation-progress-work");
  const creationProgressCheck = document.getElementById("creation-progress-check");
  const creationProgressAuthorize = document.getElementById("creation-progress-authorize");
  const creationProgressAllocate = document.getElementById("creation-progress-allocate");
  const creationCancelWork = document.getElementById("creation-cancel-work");
  // Carried as an error because that is how the creation flow already ends
  // early; the two callers tell it apart from a failure by this text alone.
  const kCreationCancelled = "creation-cancelled";
  const kCreationCleanupFailed = "creation-cleanup-failed";
  const kCreationCleanupMessage = "创建未完成，未能确认临时文件已清理。请检查保存位置中的残留文件后再重试。";
  const requestCreationCancellation = () => bridge.cancelContainerCreation();
  const bitlockerPage = document.getElementById("creation-bitlocker");
  const bitlockerDescription = document.getElementById("bitlocker-description");
  const bitlockerConsent = document.getElementById("bitlocker-consent");
  const bitlockerConsentLabel = document.getElementById("bitlocker-consent-label");
  const bitlockerHelp = document.getElementById("bitlocker-help");
  const bitlockerDecryption = document.getElementById("bitlocker-decryption");
  const bitlockerProgressText = document.getElementById("bitlocker-progress-text");
  const bitlockerProgressBar = document.getElementById("bitlocker-progress-bar");
  const bitlockerCancel = document.getElementById("bitlocker-cancel");
  const bitlockerContinue = document.getElementById("bitlocker-continue");
  let bitlockerPhase = 0;
  let bitlockerSubmitted = false;
  let bitlockerCancelled = false;
  const resetBitlocker = () => {
    bitlockerPhase = 0; bitlockerSubmitted = false; bitlockerCancelled = false;
    bitlockerConsent.checked = false; bitlockerPage.hidden = true;
    bitlockerCancel.hidden = true; bitlockerContinue.hidden = true;
    creationDialog.dataset.bitlocker = "false";
  };
  const showBitlocker = (packed) => {
    if (typeof packed !== "string" || !/^[0-2],[0-5],\d{1,3}$/.test(packed)) throw new Error("invalid BitLocker progress");
    const [phase, state, encrypted] = packed.split(",").map(Number);
    if (encrypted > 100 || (state === 0 && encrypted !== 0)) throw new Error("invalid BitLocker percentage");
    if (!phase) {
      if (bitlockerPhase) {
        resetBitlocker(); creationProgress.hidden = false;
        creationTitle.textContent = creationInplace ? "正在加密分区" : "正在创建安全盘";
        creationStatus.textContent = "";
      }
      return;
    }
    if (phase !== bitlockerPhase) creationStatus.textContent = "";
    bitlockerPhase = phase;
    setCreationDescription("");
    bitlockerPage.hidden = false; creationProgress.hidden = true;
    creationDialog.dataset.bitlocker = "true";
    creationTitle.textContent = phase === 1 ? "关闭 BitLocker" : "正在解密 BitLocker";
    const selected = partitionRows.find(row => row.id === partitionSelected);
    const name = selected?.display || (selected ? partitionLocation(selected) : "所选分区");
    const stateText = ["已完成解密", "完全加密", "正在加密", "正在解密", "加密已暂停", "解密已暂停"][state];
    bitlockerDescription.textContent = `${name} ${state === 1 ? "已被 Windows BitLocker 完全加密" : `的 Windows BitLocker 状态为：${stateText}`}，要继续使用这个分区，需要`;
    const emphasis = document.createElement("strong");
    emphasis.textContent = "关闭 BitLocker 并等待解密完成。";
    bitlockerDescription.append(emphasis);
    bitlockerConsentLabel.hidden = phase !== 1;
    bitlockerHelp.hidden = phase !== 1;
    bitlockerDecryption.hidden = phase !== 2;
    bitlockerCancel.hidden = false;
    bitlockerCancel.disabled = bitlockerCancelled;
    bitlockerCancel.textContent = bitlockerCancelled ? "正在取消…" : phase === 1 ? "取消" : "取消等待";
    bitlockerContinue.hidden = phase !== 1;
    bitlockerContinue.disabled = !bitlockerConsent.checked || bitlockerSubmitted || bitlockerCancelled;
    bitlockerConsent.disabled = bitlockerSubmitted || bitlockerCancelled;
    bitlockerProgressText.textContent = `${state === 5 ? "Windows 解密已暂停" : "正在关闭 BitLocker"}，已解密 ${100 - encrypted}%`;
    bitlockerProgressBar.setAttribute("aria-valuenow", String(100 - encrypted));
    bitlockerProgressBar.style.setProperty("--creation-percent", `${100 - encrypted}%`);
  };
  bitlockerConsent.addEventListener("change", () => {
    bitlockerContinue.disabled = !bitlockerConsent.checked || bitlockerPhase !== 1 || bitlockerSubmitted || bitlockerCancelled;
  });
  bitlockerHelp.addEventListener("click", () => { if (bitlockerPhase === 1) void bridge.openBitLockerHelp().catch((error) => { logCaughtPageError("bitlockerHelp.click/1", error); }); });
  bitlockerContinue.addEventListener("click", async () => {
    if (!creationRunning || bitlockerPhase !== 1 || !bitlockerConsent.checked || bitlockerSubmitted || bitlockerCancelled) return;
    bitlockerSubmitted = true; bitlockerContinue.disabled = true; bitlockerConsent.disabled = true;
    try { if (await bridge.confirmPartitionBitLocker() !== true) throw new Error("consent rejected"); }
    catch (pageError) { logCaughtPageError("bitlockerContinue.click/1", pageError); creationStatus.textContent = "未能确认关闭 BitLocker，请取消后重试。"; creationStatus.dataset.tone = "error"; }
  });
  bitlockerCancel.addEventListener("click", async () => {
    if (!creationRunning || !bitlockerPhase || bitlockerCancelled) return;
    bitlockerCancelled = true; bitlockerCancel.disabled = true; bitlockerContinue.disabled = true;
    try { if (await requestCreationCancellation() !== true) throw new Error("cancel rejected"); }
    catch (pageError) { logCaughtPageError("bitlockerCancel.click/1", pageError); bitlockerCancelled = false; bitlockerCancel.disabled = false; }
  });
  let creationInplace = false;
  let creationDrive = "";
  let creationRunning = false;
  let creationFinalizing = false;
  let creationResult = "";
  let creationProgressTimer = null;
  const stopCreationProgress = () => {
    if (creationProgressTimer !== null) clearInterval(creationProgressTimer);
    creationProgressTimer = null;
    creationRunning = false;
    creationResult = "";
    resetBitlocker();
    creationProgress.hidden = true;
    creationProgress.removeAttribute("data-result");
    creationProgress.querySelector("[role=progressbar]")?.removeAttribute("aria-valuenow");
    creationDialog.dataset.running = "false";
    creationProgressTime.textContent = "";
    creationDrive = "";
  };
  const startCreationProgress = () => {
    stopCreationProgress();
    creationRunning = true;
    creationFinalizing = false;
    creationProgress.hidden = false;
    creationDialog.dataset.running = "true";
    creationTitle.textContent = creationInplace ? "正在加密分区" : "正在创建安全盘";
    creationProgressAllocate.children[1].textContent = creationInplace ? "准备文件系统" : "分配安全盘文件";
    creationProgressWork.children[1].textContent = creationInplace ? "加密原有数据" : "格式化安全盘";
    creationProgressCheck.children[1].textContent = creationInplace ? "保存进度与完成处理" : "挂载并分配盘符";
    for (const step of [creationProgressAuthorize, creationProgressAllocate, creationProgressWork]) {
      step.className = "";
      step.removeAttribute("aria-current");
      step.querySelector(".step-tail").textContent = "等待中";
    }
    creationProgressAuthorize.className = "current";
    creationProgressAuthorize.querySelector(".step-tail").textContent = "准备中";
    creationProgressCheck.className = "";
    creationProgressCheck.querySelector(".step-tail").textContent = "等待中";
    creationProgressWork.removeAttribute("aria-current");
    creationProgressCheck.removeAttribute("aria-current");
    creationProgressAuthorize.setAttribute("aria-current", "step");
    creationCancelWork.disabled = false;
    delete creationCancelWork.dataset.finalizing;
    creationCancelWork.textContent = creationInplace ? "暂停加密" : "取消创建";
    const started = performance.now();
    const updateTime = () => {
      const seconds = Math.floor((performance.now() - started) / 1000);
      creationProgressTime.textContent = `已用时 ${String(Math.floor(seconds / 60)).padStart(2, "0")}:${String(seconds % 60).padStart(2, "0")}`;
    };
    updateTime();
    creationProgressTimer = setInterval(updateTime, 1000);
    updateCreationAvailability();
  };
  const confirmCreationResult = () => {
    creationProgressWork.className = "done";
    creationProgressWork.querySelector(".step-tail").textContent = "已完成";
    creationProgressWork.removeAttribute("aria-current");
    creationProgressCheck.className = "current";
    creationProgressCheck.setAttribute("aria-current", "step");
    creationProgressCheck.querySelector(".step-tail").textContent = "确认中";
  };
  const finishCreationProgress = () => {
    if (creationDialog.hidden || !creationRunning) return;
    if (creationProgressTimer !== null) clearInterval(creationProgressTimer);
    creationProgressTimer = null;
    creationRunning = false;
    if (creationStatus.dataset.tone === "paused") {
      creationResult = "paused"; creationProgress.dataset.result = "paused";
      creationDialog.dataset.running = "false"; creationTitle.textContent = "加密已暂停";
      creationProgressWork.className = ""; creationProgressWork.querySelector(".step-tail").textContent = "已暂停";
      creationProgressCheck.className = "done"; creationProgressCheck.querySelector(".step-tail").textContent = "进度已保存";
      for (const step of [creationProgressAuthorize, creationProgressAllocate, creationProgressWork, creationProgressCheck]) step.removeAttribute("aria-current");
      setCreationDescription("可以退出应用；下次用原安全卡登录，在安全盘列表选择继续加密。");
      return;
    }
    resetBitlocker();
    creationProgress.hidden = false;
    const succeeded = creationStatus.dataset.tone === "success";
    const cancelled = creationStatus.dataset.tone === "cancelled";
    creationResult = succeeded ? "success" : cancelled ? "cancelled" : "error";
    creationProgress.dataset.result = creationResult;
    creationDialog.dataset.running = "false";
    creationTitle.textContent = succeeded
      ? (creationInplace ? "分区加密已完成" : "安全盘已创建并挂载")
      : cancelled ? "已取消创建" : "创建未完成";
    setCreationDescription("");
    creationProgressWork.removeAttribute("aria-current");
    creationProgressCheck.removeAttribute("aria-current");
    const created = creationProgressWork.className === "done";
    creationProgressWork.className = succeeded || created ? "done" : "failed";
    creationProgressWork.querySelector(".step-tail").textContent =
      succeeded || created ? "已完成" : cancelled ? "已取消" : "未完成";
    creationProgressCheck.className = succeeded ? "done" : "failed";
    creationProgressCheck.querySelector(".step-tail").textContent =
      succeeded ? (creationDrive || (creationInplace ? "可以挂载使用" : "挂载成功")) : cancelled ? "已取消" : "未确认";
    for (const step of [creationProgressAuthorize, creationProgressAllocate]) {
      if (step.classList.contains("current")) {
        step.className = succeeded ? "done" : "failed";
        step.querySelector(".step-tail").textContent =
          succeeded ? "已完成" : cancelled ? "已取消" : "未完成";
      }
      step.removeAttribute("aria-current");
    }
    if (succeeded) creationProgress.querySelector("[role=progressbar]")?.setAttribute("aria-valuenow", "100");
  };
  // Let the renderer paint the execution view before entering the native call.
  const paintCreationProgress = () => new Promise((resolve) => {
    requestAnimationFrame(() => setTimeout(resolve, 0));
  });
  const waitForCreation = async () => {
    for (;;) {
      let packed;
      try { packed = await bridge.getCreationProgress(); }
      catch (error) { logCaughtPageError("waitForCreation/progress", error, 4); throw error; }
      if (typeof packed !== "string" || !/^\d+,\d+,\d+,\d+,\d+$/.test(packed)) throw new Error("invalid creation progress");
      const [status, stage, percent, drive, flags] = packed.split(",").map(Number);
      if (status < 1 || status > 6 || stage > 5 || percent > 100 || flags > 3 ||
          (drive !== 0 && (drive < 65 || drive > 90))) throw new Error("invalid creation progress range");
      if (status === 1) showBitlocker(await bridge.getPartitionBitLockerProgress());
      if (status === 6) { if (!creationInplace) throw new Error("unexpected pause result"); return 6; }
      if (stage >= 4) {
        creationFinalizing = true;
        creationCancelWork.disabled = true;
        creationCancelWork.textContent = "正在完成创建";
        creationCancelWork.dataset.finalizing = "true";
      }
      const setStep = (step, state, text) => {
        step.className = state;
        step.querySelector(".step-tail").textContent = text;
        step.removeAttribute("aria-current");
        if (state === "current") step.setAttribute("aria-current", "step");
      };
      setStep(creationProgressAuthorize, flags & 1 ? "done" : "current", flags & 1 ? "已授权" : "准备／等待授权");
      setStep(creationProgressAllocate, creationInplace && stage >= 3 ? "done" : flags & 2 ? "done" : stage >= 1 ? "current" : "",
        creationInplace ? (stage >= 3 ? "已准备" : "准备中") : flags & 2 ? "已分配" : stage >= 1 ? "分配中" : "等待中");
      setStep(creationProgressWork, stage >= 4 ? "done" : stage === 3 ? "current" : "", stage >= 4 ? "已完成" : stage === 3 ? `${percent}%` : "等待中");
      setStep(creationProgressCheck, stage === 5 ? "done" : stage === 4 ? "current" : "", stage === 5 ? (creationInplace && !drive ? "加密完成" : "挂载成功") : stage === 4 ? "挂载中" : "等待中");
      if (drive) creationDrive = `${String.fromCharCode(drive)}:`;
      const bar = creationProgress.querySelector("[role=progressbar]");
      if (stage >= 3) {
        const progress = stage >= 4 ? 100 : percent;
        bar?.setAttribute("aria-valuenow", String(progress));
        bar?.style.setProperty("--creation-percent", `${progress}%`);
        bar?.setAttribute("aria-label", creationInplace ? "加密进度" : "格式化进度");
      }
      if (status === 2) return 2;
      if (status === 4) throw new Error(kCreationCancelled);
      if (status === 5) throw new Error(kCreationCleanupFailed);
      if (status === 3) throw new Error("native creation failed");
      await new Promise((resolve) => setTimeout(resolve, 150));
    }
  };
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
  const hiddenReview = document.getElementById("hidden-create-review");
  const hiddenReviewPassword = document.getElementById("hidden-create-review-password");
  const hiddenReviewVisibility = document.getElementById("hidden-create-review-visibility");
  const hiddenReviewCancel = document.getElementById("hidden-create-review-cancel");
  const hiddenReviewConfirm = document.getElementById("hidden-create-review-confirm");
  const closeHiddenReview = () => {
    hiddenReview.hidden = true;
    hiddenReviewPassword.value = "";
    hiddenReviewPassword.type = "password";
    hiddenReviewVisibility.setAttribute("aria-pressed", "false");
    hiddenReviewVisibility.setAttribute("aria-label", "显示当前诱饵密码");
    containerBAcknowledge.checked = false;
    hiddenReviewConfirm.disabled = true;
    creationDialog.inert = false;
  };
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
  let batchSessionRevision = 0;
  const batchForceUnmount = document.getElementById("unmount-force");
  let savedForceUnmount = false;
  const batchControls = ["mount", "unmount"].map((kind) => ({
    kind, mounting: kind === "mount", selected: new Set(),
    button: document.getElementById(`dashboard-${kind}`),
    arrow: document.getElementById(`${kind}-select`),
    menu: document.getElementById(`${kind}-menu`),
    items: document.getElementById(`${kind}-items`),
    all: document.getElementById(`${kind}-select-all`),
    split: document.getElementById(`${kind}-split`),
  }));
  const closeBatchMenus = () => {
    for (const control of batchControls) {
      control.menu.hidden = true;
      control.arrow.setAttribute("aria-expanded", "false");
    }
  };
  const updateBatchSelection = (control) => {
    const eligible = containers.filter((item) => item.mounted !== control.mounting && !item.conversion && !isMaintenanceContainer(item.id));
    for (const id of control.selected) {
      if (!eligible.some((item) => item.id === id)) control.selected.delete(id);
    }
    control.button.disabled = busy || currentState !== nativeState.authenticated;
    control.all.disabled = busy || !eligible.length;
    control.all.checked = eligible.length > 0 && control.selected.size === eligible.length;
    control.all.indeterminate = control.selected.size > 0 && control.selected.size < eligible.length;
    for (const input of control.items.querySelectorAll("input")) {
      if (!input.disabled) input.checked = control.selected.has(Number(input.dataset.id));
    }
  };
  const renderBatchControls = () => {
    const authenticated = currentState === nativeState.authenticated;
    batchForceUnmount.disabled = busy || !authenticated;
    if (!authenticated) batchForceUnmount.checked = false;
    if (busy || !authenticated) closeBatchMenus();
    for (const control of batchControls) {
      if (!authenticated) control.selected.clear();
      control.arrow.disabled = busy || !authenticated;
      control.items.replaceChildren();
      // Clear names while refreshing or working; a card event can arrive
      // before the native session projection has been replaced.
      for (const item of authenticated && !busy ? containers : []) {
        const row = document.createElement("label");
        const input = document.createElement("input");
        input.type = "checkbox";
        input.dataset.id = String(item.id);
        const reached = item.mounted === control.mounting;
        input.checked = reached || control.selected.has(item.id);
        input.disabled = reached || busy || isMaintenanceContainer(item.id);
        const name = document.createElement("span");
        name.className = "batch-name";
        name.textContent = item.name;
        row.append(name);
        if (reached || isMaintenanceContainer(item.id)) {
          const state = document.createElement("span");
          state.className = "batch-state";
          state.textContent = isMaintenanceContainer(item.id) ? "正在进行高级更新" : item.mounted ? "已挂载" : "未挂载";
          row.append(state);
        }
        row.append(input);
        control.items.append(row);
      }
      if (!control.items.childElementCount && authenticated && !busy) {
        const empty = document.createElement("div");
        empty.className = "batch-empty";
        empty.textContent = "暂无安全盘";
        control.items.append(empty);
      }
      updateBatchSelection(control);
    }
  };
  // While the button is down this holds the control being painted and the
  // state the press decided on, so crossing a row applies that one state
  // instead of flipping each row it touches.
  let batchPaint = null;
  // Set while a press owns the box it just painted, so the click that press
  // produces can be cancelled. Keyboard activation files no pointerdown.
  let batchClickFromPointer = false;
  const paintBatchRow = (control, input) => {
    if (input.disabled || input.checked === batchPaint.checked) return;
    input.checked = batchPaint.checked;
    const id = Number(input.dataset.id);
    if (batchPaint.checked) control.selected.add(id);
    else control.selected.delete(id);
    updateBatchSelection(control);
  };
  const batchRowInput = (target) =>
    target?.closest?.("label")?.querySelector("input[data-id]") || null;
  document.addEventListener("pointerup", () => {
    batchPaint = null;
    // A release normally produces the click this press owns; the guard
    // below spends itself on that one. Drop it afterwards so a release that
    // produced no click cannot swallow someone else's.
    setTimeout(() => { batchClickFromPointer = false; }, 0);
  });
  document.addEventListener("pointercancel", () => {
    batchPaint = null;
    batchClickFromPointer = false;
  });
  // The press already set the box, so the click it produces must not reach
  // the row label and flip it straight back.
  document.addEventListener("click", (event) => {
    if (!batchClickFromPointer) return;
    batchClickFromPointer = false;
    if (event.target.closest?.(".batch-items")) event.preventDefault();
  }, true);
  const toggleBatchMenu = (control) => {
    if (busy || currentState !== nativeState.authenticated || !volumeOperationDialog.hidden || !batchResultDialog.hidden) return;
    const opening = control.menu.hidden;
    closeBatchMenus();
    control.menu.hidden = !opening;
    control.arrow.setAttribute("aria-expanded", String(opening));
    if (opening) control.menu.querySelector("input:not(:disabled)")?.focus();
  };
  for (const control of batchControls) {
    control.arrow.addEventListener("click", () => toggleBatchMenu(control));
    // With nothing picked the action has nothing to run, so it stands in
    // for the arrow and offers the selection instead of rejecting a click.
    control.button.addEventListener("click", async () => {
      if (!control.selected.size) toggleBatchMenu(control);
      else await runSelectedBatch(control);
    });
    control.all.addEventListener("change", () => {
      if (busy || externalRefreshPending || currentState !== nativeState.authenticated) return;
      control.selected.clear();
      if (control.all.checked) for (const item of containers) {
        if (item.mounted !== control.mounting && !item.conversion && !isMaintenanceContainer(item.id)) control.selected.add(item.id);
      }
      updateBatchSelection(control);
    });
    control.items.addEventListener("pointerdown", (event) => {
      if (event.button !== 0 || busy || externalRefreshPending ||
          currentState !== nativeState.authenticated) return;
      const input = batchRowInput(event.target);
      if (!input || input.disabled) return;
      // The box is ours for the duration of the press: stop the browser
      // from selecting the name the pointer starts on.
      event.preventDefault();
      batchClickFromPointer = true;
      batchPaint = { control, checked: !input.checked };
      paintBatchRow(control, input);
    });
    control.items.addEventListener("pointerover", (event) => {
      if (batchPaint?.control !== control || busy || externalRefreshPending ||
          currentState !== nativeState.authenticated) return;
      const input = batchRowInput(event.target);
      if (input) paintBatchRow(control, input);
    });
    control.items.addEventListener("change", (event) => {
      const input = event.target;
      if (busy || externalRefreshPending || currentState !== nativeState.authenticated ||
          input.disabled || !input.matches("input[data-id]")) return;
      const id = Number(input.dataset.id);
      if (input.checked) control.selected.add(id);
      else control.selected.delete(id);
      updateBatchSelection(control);
    });
    control.split.addEventListener("keydown", (event) => {
      if (event.key === "Escape" && !control.menu.hidden) {
        event.preventDefault();
        event.stopPropagation();
        closeBatchMenus();
        control.arrow.focus();
      }
    });
    control.split.addEventListener("focusout", (event) => {
      if (event.relatedTarget !== null && !control.split.contains(event.relatedTarget)) {
        control.menu.hidden = true;
        control.arrow.setAttribute("aria-expanded", "false");
      }
    });
  }
  document.addEventListener("click", (event) => {
    if (!batchControls.some((control) => control.split.contains(event.target))) closeBatchMenus();
  });
  let creationStorage = "file";
  let creationKind = null;
  // The dialog is one flow in two steps: "type" picks the container shape,
  // "params" collects what that shape needs.
  let creationStep = "params";
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
    } catch (pageError) { logCaughtPageError("normalizedPasswordLength/1", pageError);
      return null;
    }
  };

  // The native route compares both inputs only after NFC normalization, so a
  // failure is reported as a mismatch on exactly the same terms.
  const initializationInputsMatch = (password, confirmation) => {
    try {
      return password.normalize("NFC") === confirmation.normalize("NFC");
    } catch (pageError) { logCaughtPageError("initializationInputsMatch/1", pageError);
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
  const kContainerExtensions = [".hc", ".sys", ".dat", ".bin"];
  // Presentation only: identities, native aliases and exported paths stay native.
  const displayContainerName = (name, partition = false) => partition
    ? name : name.replace(/^(.+)\.(?:hc|sys|dat|bat|bin)$/i, "$1");

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
    const existingExtension = kContainerExtensions.find(suffix => name.length > suffix.length && name.toLowerCase().endsWith(suffix));
    const extension = 4 - (existingExtension?.length ?? 0);
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

  const containerBOuterMib = () => {
    if (creationStorage !== "disk") return capacityMib(containerBCapacity, containerBUnit);
    const selected = partitionRows.find(row => row.id === partitionSelected);
    return selected ? selected.bytes / 1048576 : null;
  };

  const containerBProblem = () => {
    const outer = containerBOuterMib();
    if (creationStorage === "disk" && outer === null) return "请先选择分区。";
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
      outer: containerBOuterMib(),
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
    } catch (pageError) { logCaughtPageError("normalizedLength/1", pageError);
      return null;
    }
  };

  const containerBInputReady = () => {
    const second = normalizedLength(containerBSecond.value);
    return containerBSizes() !== null &&
      (creationStorage === "disk" || containerLocationReady(containerBName, containerBDirectory)) &&
      second !== null &&
      second >= 12 && second <= 128 &&
      containerBSecond.value === containerBSecondConfirmation.value;
  };

  let partitionReviewOpen = false;
  const updateCreationAvailability = () => {
    const encrypted = creationStorage === "file" && creationKind === "encrypted";
    const hiddenVolume = creationKind === "hidden";
    const choosing = false;
    const authenticated = currentState === nativeState.authenticated;
    const ready = creationStorage === "disk" ? partitionSelected !== 0 && (!partitionReviewOpen || partitionErase.checked) && (!hiddenVolume || containerBInputReady()) : choosing
      ? encrypted || hiddenVolume
      : encrypted
        ? containerCapacityMib() !== null &&
          containerLocationReady(containerName, containerDirectory)
        : hiddenVolume && containerBInputReady();
    const reviewing = partitionReviewOpen && !creationRunning && creationResult === "";
    document.getElementById("partition-review").hidden = !reviewing;
    creationDialog.dataset.step = creationStep;
    creationDialog.dataset.storage = creationStorage;
    const diskCapacity = creationStorage === "disk" ? containerBOuterMib() : null;
    document.getElementById("create-partition-outer-capacity").value = diskCapacity === null ? "" : String(Number(diskCapacity.toFixed(2)));
    creationDiskStep.hidden = creationStorage !== "disk" || reviewing || creationRunning || creationResult !== "";
    creationStorageSwitch.hidden = reviewing || creationRunning || creationResult !== "";
    for (const button of creationStorageButtons) {
      button.setAttribute("aria-pressed", String(button.dataset.createStorage === creationStorage));
      button.disabled = busy || !authenticated || creationCompleted;
    }
    creationTypeStep.hidden = reviewing || creationRunning || creationResult !== "";
    containerForm.hidden = reviewing || creationRunning || creationResult !== "" || choosing || !encrypted;
    containerBForm.hidden = reviewing || creationRunning || creationResult !== "" || choosing || !hiddenVolume;
    // 8.7.2: both shapes are offered on every visit, in the same order and
    // with the same weight. Neither entry is ever withheld.
    for (const button of creationTypeButtons) {
      button.setAttribute(
          "aria-checked",
          button.dataset.createKind === creationKind ? "true" : "false");
      button.disabled = busy || !authenticated || creationCompleted;
    }
    creationTypeNote.hidden = true;
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
    const hiddenLocationQuiet = hiddenQuiet || creationStorage === "disk";
    partitionPreserve.closest("label").hidden = hiddenVolume;
    if (hiddenVolume) partitionPreserve.checked = false;
    updatePartitionMode();
    showFieldProblem(
      containerNameError,
      standardQuiet || !containerName.value.trim() ? "" : containerNameProblem(containerName));
    showFieldProblem(
      containerDirectoryError,
      standardQuiet || !containerDirectory.value.trim()
        ? ""
        : containerDirectoryProblem(containerDirectory, containerName));
    showFieldProblem(
      containerError, standardQuiet ? "" : containerCapacityProblem());
    showFieldProblem(
      containerBNameError,
      hiddenLocationQuiet || !containerBName.value.trim() ? "" : containerNameProblem(containerBName));
    showFieldProblem(
      containerBDirectoryError,
      hiddenLocationQuiet || !containerBDirectory.value.trim()
        ? ""
        : containerDirectoryProblem(containerBDirectory, containerBName));
    showFieldProblem(
      containerBError, hiddenQuiet || (creationStorage === "disk" && containerBOuterMib() === null) ? "" : containerBProblem());
    creationCloseButton.disabled = busy;
    creationCancelButton.disabled = busy;
    creationContinueButton.disabled = busy || !authenticated ||
      creationCompleted || !ready;
    creationContinueButton.textContent = creationStorage === "disk" ? (reviewing ? (partitionPreserve.checked ? "开始加密" : "清空并创建") : "下一步") : "下一步";
    creationContinueButton.hidden = creationResult !== "";
    creationCancelLabel.textContent = creationResult !== "" ? "关闭" : creationCompleted ? "完成" : "取消";
    creationBackIcon.hidden = true;
  };

  const setCreationDescription = (text) => {
    creationDescription.textContent = text;
    creationDescription.hidden = text === "";
  };
  const applyCreationStepCopy = () => {
    creationTitle.textContent = "新建安全盘";
    setCreationDescription(creationStorage === "disk"
      ? "安全盘为物理硬盘上已有的独立数据分区"
      : "安全盘为一个加密文件，挂载后显示为普通磁盘");
  };

  // Leaving a parameter step discards what was typed on it: a decoy password
  // must not outlive the step that asked for it.
  const resetCreationInputs = () => {
    document.getElementById("create-partition-outer-capacity").value = "";
    partitionReviewOpen = false;
    stopCreationProgress();
    closeHiddenReview();
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
    document.getElementById("forget-container-description").textContent =
      containers.find(container => container.id === id)?.partition
        ? "只移除加密身份记录，不会删除分区或解密数据。当前版本无法通过文件导入找回分区；移除后将失去应用中的挂载入口，请确认不再需要此记录。"
        : "只移除记录，不会删除安全盘文件。文件仍在原路径，可通过「导入」重新添加至列表。";
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
    clearCreationPartitions();
    creationDialog.hidden = true;
    creationKind = null;
    creationStep = "params";
    creationDialog.dataset.step = "type";
    creationCompleted = false;
    creationStatus.textContent = "";
    creationStatus.dataset.tone = "";
    creationCancelLabel.textContent = "取消";
    creationBackIcon.hidden = true;
    renderContainers();
    if (activeSection === "decoy") void refreshDecoyCandidates();
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
  let operationLoggingEnabled = true;
  let fileContainerId = 0;
  let filePath = [];
  let fileEntries = [];
  let fileListingRevision = 0;
  let fileRefreshPendingRevision = 0;

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
      if (container.id === fileContainerId) {
        if (inDecoyFileView() && !decoyMounted.has(container.id)) return { ...container, mounted: false, drive: "" };
        return container;
      }
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
    closeFileContextMenu();
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
      }
    }
    document.getElementById("management-list-actions").hidden = fileContainerId !== 0;
    managementContainers.hidden = fileContainerId !== 0;
    managementFiles.hidden = fileContainerId === 0;
    const container = selectedFileContainer();
    const mounted = container !== null && container.mounted;
    fileMountToggle.textContent = mounted ? "卸载" : "挂载";
    fileMountToggle.classList.toggle("danger", mounted);
    fileMountToggle.hidden = inDecoyFileView();
    fileMountToggle.disabled = busy || container === null || currentState !== nativeState.authenticated;
    fileCreateDirectoryButton.hidden = inDecoyFileView();
    fileCreateDirectoryButton.disabled = busy || !mounted || inDecoyFileView();
    renderFileBreadcrumb();
    updateFileRefreshAvailability();
  };

  const updateFileRefreshAvailability = () => {
    const container = selectedFileContainer();
    const loading = fileRefreshPendingRevision !== 0 &&
      fileRefreshPendingRevision === fileListingRevision;
    fileRefreshButton.disabled = busy || loading || container === null || !container.mounted;
    fileRefreshButton.setAttribute("aria-busy", String(loading));
  };

  const renderFileBreadcrumb = () => {
    while (fileBreadcrumb.firstChild !== null) {
      fileBreadcrumb.removeChild(fileBreadcrumb.firstChild);
    }
    const container = selectedFileContainer();
    const heading = document.createElement("h2");
    heading.id = "files-title";
    if (inDecoyFileView()) {
      const back = document.createElement("button");
      back.type = "button";
      back.className = "file-crumb";
      back.textContent = "高级更新";
      back.dataset.decoyBack = "";
      back.disabled = busy;
      heading.appendChild(back);
    } else {
      const back = document.createElement("button");
      back.type = "button";
      back.className = "file-crumb";
      back.textContent = "安全盘管理";
      back.dataset.fileBack = "";
      back.disabled = busy;
      heading.appendChild(back);
    }
    fileBreadcrumb.appendChild(heading);
    const crumbs = container === null ? [] : [container.name, ...filePath];
    for (let index = 0; index < crumbs.length; index += 1) {
      const separator = document.createElement("span");
      separator.className = "file-crumb-separator";
      separator.textContent = ">";
      fileBreadcrumb.appendChild(separator);
      const crumb = document.createElement("button");
      crumb.className = "file-crumb";
      crumb.type = "button";
      crumb.textContent = crumbs[index];
      if (index === crumbs.length - 1) {
        crumb.setAttribute("aria-current", "page");
      }
      crumb.dataset.fileDepth = String(index);
      crumb.disabled = busy;
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
      appendFileNotice("还没有安全盘。先新建或导入一个安全盘，再回到这里。");
      return;
    }
    if (!container.mounted) {
      appendFileNotice(inDecoyFileView() ? "点击右上角的「更新」挂载后，即可查看文件。" : "这个安全盘没有挂载。点击工具栏右侧的「挂载」，即可查看文件。");
      return;
    }
    if (fileEntries.length === 0) {
      appendFileNotice("这个文件夹是空的。");
      return;
    }
    for (const entry of fileEntries) {
      const row = document.createElement("tr");
      row.tabIndex = 0;
      row.draggable = false;
      row.dataset.fileEntry = entry.name;
      if (entry.isDirectory) row.dataset.fileEnter = entry.name;
      const name = document.createElement("td");
      const identity = document.createElement("div");
      identity.className = "file-identity";
      identity.appendChild(exportGlyph(fileTypePaths(entry),
        entry.isDirectory ? "file-glyph directory" : "file-glyph"));
      if (entry.isDirectory) {
        const open = document.createElement("button");
        open.className = "file-name-button";
        open.type = "button";
        open.textContent = entry.name;
        open.dataset.fileEnter = entry.name;
        open.disabled = busy;
        identity.appendChild(open);
      } else {
        const label = document.createElement("span");
        label.className = "file-name";
        label.textContent = entry.name;
        identity.appendChild(label);
      }
      if (entry.isLink) {
        // Shown rather than hidden: a manager that silently omits what the
        // system file manager shows is worse than one that marks it.
        const mark = document.createElement("span");
        mark.className = "file-entry-link";
        mark.textContent = "链接";
        identity.appendChild(mark);
      }
      name.appendChild(identity);
      const size = document.createElement("td");
      size.className = "file-size";
      size.textContent = formatEntrySize(entry);
      const modified = document.createElement("td");
      modified.className = "file-modified";
      modified.textContent = formatOperationTime(entry.modifiedUnixMs).slice(0, 16);
      const actions = document.createElement("td");
      const buttons = document.createElement("div");
      buttons.className = "file-actions";
      for (const [action, label, danger] of [
        ["export", "导出", false],
        ["delete", "删除", true],
      ]) {
        if (action === "export" && isMaintenanceContainer(container.id)) continue;
        const button = document.createElement("button");
        button.className = danger ? "file-icon-button danger" : "file-icon-button";
        button.type = "button";
        button.setAttribute("aria-label", `${label} ${entry.name}`);
        button.title = danger ? "永久删除，不进回收站" : label;
        button.appendChild(exportGlyph(danger
          ? ["M4 7h16", "M9 7V4h6v3", "M6 7l1 13h10l1-13", "M10 10v7M14 10v7"]
          : ["M13 5H5v14h14v-8", "M12 12 21 3", "M15 3h6v6"]));
        button.disabled = busy;
        button.dataset.fileAction = action;
        button.dataset.fileName = entry.name;
        buttons.appendChild(button);
      }
      actions.appendChild(buttons);
      row.append(name, size, modified, actions);
      fileListBody.appendChild(row);
    }
  };

  let fileContextTarget = null;
  const closeFileContextMenu = () => {
    fileContextTarget = null;
    fileContextMenu.hidden = true;
  };

  const openFileContextMenu = (row, x, y) => {
    const container = selectedFileContainer();
    if (busy || currentState !== nativeState.authenticated ||
        container === null || !container.mounted ||
        !fileEntries.some((entry) => entry.name === row.dataset.fileEntry)) return;
    fileContextTarget = { id: container.id, directory: filePath.join("/"), name: row.dataset.fileEntry };
    fileContextMenu.hidden = false;
    const bounds = fileContextMenu.getBoundingClientRect();
    fileContextMenu.style.left = `${Math.max(4, Math.min(x, window.innerWidth - bounds.width - 4))}px`;
    fileContextMenu.style.top = `${Math.max(4, Math.min(y, window.innerHeight - bounds.height - 4))}px`;
    fileContextRename.focus();
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
    } catch (pageError) { logCaughtPageError("runFileAction/1", pageError);
      failed = true;
      try {
        await refreshNativeState();
      } catch (pageError) { logCaughtPageError("runFileAction/2", pageError);
        currentState = nativeState.unavailable;
        activeVolumes = true;
      }
    }
    busy = false;
    render(messageForCurrentState());
    await refreshFileManager();
    if (inDecoyFileView()) await refreshDecoyMaintenanceFacts();
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
    "M12 3 3 7.5v9L12 21l9-4.5v-9L12 3z",
    "m3 7.5 9 4.5 9-4.5M12 12v9M7.5 5.25l9 4.5",
  ]);
  const kCheckPaths = Object.freeze(["M6 12.5l4 4 8-9"]);
  // Six dots, drawn as circles so the handle reads as a grip rather than a
  // glyph. They need their own builder because the icons elsewhere are paths.
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

  // The eleven file kinds the list can tell apart. The page and the fold are
  // taken from kFilePaths rather than redrawn, so the document family never
  // grows a second kind of paper; the rest carry their own geometry in the
  // same 24-unit grid every other glyph here uses. None of them sets a
  // colour: an icon says what a file is, and must not also read as a state.
  const kFileTypeIcons = Object.freeze([
    Object.freeze({
      suffixes: Object.freeze([".txt", ".md", ".rtf", ".doc", ".docx", ".log"]),
      paths: Object.freeze([
        ...kFilePaths,
        "M8.5 9.5h2.5M8.5 12.5h7M8.5 15.5h7",
      ]),
    }),
    Object.freeze({
      suffixes: Object.freeze([".xls", ".xlsx", ".csv"]),
      paths: Object.freeze([
        ...kFilePaths,
        "M8 12.5h8M8 16.5h8",
        "M12 12.5v6",
      ]),
    }),
    Object.freeze({
      suffixes: Object.freeze([".pdf"]),
      paths: Object.freeze([
        ...kFilePaths,
        "M8.5 14.5h7a1 1 0 011 1v2a1 1 0 01-1 1h-7a1 1 0 01-1-1v-2a1 1" +
          " 0 011-1z",
      ]),
    }),
    Object.freeze({
      suffixes: Object.freeze([
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".svg", ".ico",
      ]),
      paths: Object.freeze([
        "M4.5 6.5A1.5 1.5 0 016 5h12a1.5 1.5 0 011.5 1.5v11a1.5 1.5 0" +
          " 01-1.5 1.5H6a1.5 1.5 0 01-1.5-1.5z",
        "M9.25 11.25a1.25 1.25 0 100-2.5 1.25 1.25 0 000 2.5z",
        "M5.5 17.5l4-4 3 3 3.5-3.5 3 3",
      ]),
    }),
    Object.freeze({
      suffixes: Object.freeze([
        ".mp3", ".wav", ".flac", ".m4a", ".aac", ".ogg",
      ]),
      paths: Object.freeze([
        "M10 17.5V7.5l7-1.5v10",
        "M10 17.5a1.5 1.5 0 10-3 0 1.5 1.5 0 003 0z",
        "M17 16a1.5 1.5 0 10-3 0 1.5 1.5 0 003 0z",
      ]),
    }),
    Object.freeze({
      suffixes: Object.freeze([
        ".mp4", ".mkv", ".avi", ".mov", ".wmv", ".webm",
      ]),
      paths: Object.freeze([
        "M4.5 7.5A1.5 1.5 0 016 6h12a1.5 1.5 0 011.5 1.5v9a1.5 1.5 0" +
          " 01-1.5 1.5H6a1.5 1.5 0 01-1.5-1.5z",
        "M10.5 9.5l4.5 2.5-4.5 2.5z",
      ]),
    }),
    Object.freeze({
      suffixes: Object.freeze([".zip", ".rar", ".7z", ".tar", ".gz"]),
      paths: Object.freeze([
        "M5.5 6.5A1.5 1.5 0 017 5h10a1.5 1.5 0 011.5 1.5v11a1.5 1.5 0" +
          " 01-1.5 1.5H7a1.5 1.5 0 01-1.5-1.5z",
        "M12 5v2.5m0 2v2.5m0 2v2.5m0 2V19",
      ]),
    }),
    Object.freeze({
      suffixes: Object.freeze([".exe", ".msi", ".dll", ".bat", ".cmd", ".ps1"]),
      paths: Object.freeze([
        "M9.5 8.5h5a1 1 0 011 1v5a1 1 0 01-1 1h-5a1 1 0 01-1-1v-5a1 1 0 011-1z",
        "M12 5.5v3M12 15.5v3",
        "M5.5 12h3M15.5 12h3",
      ]),
    }),
    Object.freeze({
      suffixes: Object.freeze([".vc", ".hc"]),
      paths: Object.freeze([
        "M5.5 7c0-1.38 2.91-2.5 6.5-2.5S18.5 5.62 18.5 7s-2.91 2.5-6.5" +
          " 2.5S5.5 8.38 5.5 7z",
        "M5.5 7v10c0 1.38 2.91 2.5 6.5 2.5s6.5-1.12 6.5-2.5V7",
      ]),
    }),
  ]);

  // Which icon a row gets is decided here and nowhere else. The suffix is
  // already on screen in the name, so reading it costs the bridge nothing: no
  // listing field, no native call, no second walk of the volume. A name that
  // is nothing but a suffix is a name, not a kind, so it falls through to the
  // plain page along with everything unrecognised.
  const fileTypePaths = (entry) => {
    if (entry.isDirectory) return kFolderPaths;
    const name = entry.name.toLowerCase();
    for (const icon of kFileTypeIcons) {
      for (const suffix of icon.suffixes) {
        if (name.length > suffix.length && name.endsWith(suffix)) {
          return icon.paths;
        }
      }
    }
    return kFilePaths;
  };

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
  // Native snapshots restore the encrypted order after each refresh.
  let containerOrder = [];
  const pendingNewContainers = new Set();
  const markNewContainers = (previousIds) => {
    for (const container of containers) {
      if (!previousIds.has(container.id)) pendingNewContainers.add(container.id);
    }
  };

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

  // Move complete existing groups, including expanded details and selections.
  // Sorting changes order only; it does not invalidate any container facts.
  const moveContainerNodes = (ids) => {
    containers = orderedContainers();
    for (const control of batchControls) {
      const rows = new Map([...control.items.querySelectorAll("input[data-id]")]
        .map(input => [Number(input.dataset.id), input.parentElement]));
      for (const id of ids) {
        const row = rows.get(id);
        if (row) control.items.append(row);
      }
    }
    const scroll = new Map();
    for (const list of new Set([...containerLists, exportContainerList, migrateContainerList, decoyContainerList])) {
      for (let parent = list; parent; parent = parent.parentElement) {
        if (!scroll.has(parent)) scroll.set(parent, [parent.scrollLeft, parent.scrollTop]);
      }
      const groups = new Map(rowsAround(list).map(row => [Number(row.dataset.rowContainer),
        row.closest(".container-group") || (row.parentElement === list ? row : row.parentElement)]));
      let anchor = list.firstElementChild;
      for (const id of ids) {
        const group = groups.get(id);
        if (!group) continue;
        if (group !== anchor) list.insertBefore(group, anchor);
        anchor = group.nextElementSibling;
      }
    }
    if (decoyStage === "maintenance") renderDecoyMaintenanceChoices();
    for (const [element, [left, top]] of scroll) {
      element.scrollLeft = left; element.scrollTop = top;
    }
  };

  const rememberContainerOrder = async (ordered) => {
    if (busy || externalRefreshPending || bridge === null || currentState !== nativeState.authenticated) return;
    const ids = ordered.map((container) => container.id);
    const previous = orderedContainers().map(container => container.id);
    if (ids.join(",") === previous.join(",")) return;
    const revision = batchSessionRevision;
    const focused = document.activeElement;
    const wasInert = dashboard.hasAttribute("inert");
    busy = true;
    // Exclude concurrent actions without disabling/rebuilding every control.
    dashboard.setAttribute("inert", "");
    setDashboardStatus("");
    try {
      containerOrder = ids;
      moveContainerNodes(ids);
      await bridge.reorderContainers(ids.join(","));
      if (revision !== batchSessionRevision) { logPageEvent(4, "rememberContainerOrder", "会话或页面状态变化，已放弃等待结果。"); return; }
    } catch (pageError) { logCaughtPageError("rememberContainerOrder/1", pageError);
      if (revision === batchSessionRevision) {
        containerOrder = previous;
        moveContainerNodes(previous);
        setDashboardStatus("排序保存失败，已保留原顺序，请重试。", "error");
      }
    } finally {
      busy = false;
      if (!wasInert) dashboard.removeAttribute("inert");
      if (revision === batchSessionRevision && !externalRefreshPending && focused?.isConnected)
        focused.focus({ preventScroll: true });
      if (externalRefreshPending) void refreshAfterExternalChange();
    }
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
    mount.disabled = busy || container.conversion === 1;
    mount.dataset.rowMount = String(container.id);
    mount.textContent = container.conversion === 2 ? "继续加密" : container.conversion === 1 ? "转换未完成" : container.mounted ? "卸载" : "挂载";
    mount.addEventListener("click", async () => {
      if (busy) return;
      if (container.conversion === 2) { await runPartitionConversion(container.id, true); }
      else if (container.mounted) {
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
    [...list.querySelectorAll(":scope > .container-row, :scope > .xrow, :scope > div > .xrow, :scope > .container-group > .container-swipe > .container-row")];

  const beginContainerReorder = (event, row, id) => {
    if (busy || decoyRefreshing || reorderState !== null || event.button !== 0) return;
    closeContainerSwipe();
    const list = row.closest(".xlist, [data-container-list], #decoy-container-list");
    if (list === null) return;
    event.preventDefault();
    const marker = document.createElement("div");
    marker.className = "xins";
    const bounds = row.getBoundingClientRect();
    const preview = row.cloneNode(true);
    // Visual snapshot only: no duplicate IDs, action targets or focusable copy.
    for (const node of [preview, ...preview.querySelectorAll("*")]) {
      for (const attribute of [...node.attributes]) {
        if (attribute.name === "id" || attribute.name === "tabindex" || attribute.name.startsWith("data-")) node.removeAttribute(attribute.name);
      }
    }
    preview.classList.remove("container-new", "xrow-ghost");
    preview.classList.add("container-reorder-preview");
    preview.setAttribute("aria-hidden", "true");
    preview.setAttribute("inert", "");
    preview.style.width = `${bounds.width}px`;
    preview.style.height = `${bounds.height}px`;
    preview.style.transform = `translate3d(${bounds.left}px, ${bounds.top}px, 0)`;
    document.body.appendChild(preview);
    document.body.classList.add("container-reordering");
    reorderState = { list, row, id, marker, preview, offsetX: event.clientX - bounds.left,
      offsetY: event.clientY - bounds.top, index: null, pointerId: event.pointerId };
    row.classList.add("xrow-ghost");
    try {
      event.target.setPointerCapture(event.pointerId);
    } catch (pageError) { logCaughtPageError("beginContainerReorder/1", pageError);
      // A pointer that cannot be captured still moves; the listeners below are
      // on the document, so the drag keeps working without it.
    }
    document.addEventListener("pointermove", moveContainerReorder);
    document.addEventListener("pointerup", endContainerReorder);
    document.addEventListener("pointercancel", cancelContainerReorder);
    window.addEventListener("blur", cancelContainerReorderOnBlur);
    document.addEventListener("keydown", cancelContainerReorderOnKey);
  };

  const moveContainerReorder = (event) => {
    if (reorderState === null || event.pointerId !== reorderState.pointerId) return;
    reorderState.preview.style.transform = `translate3d(${event.clientX - reorderState.offsetX}px, ${event.clientY - reorderState.offsetY}px, 0)`;
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
      const holder = anchor.closest(".container-group") || (anchor.parentElement === reorderState.list
        ? anchor
        : anchor.parentElement);
      reorderState.list.insertBefore(reorderState.marker, holder);
    }
  };

  const finishContainerReorder = () => {
    document.removeEventListener("pointermove", moveContainerReorder);
    document.removeEventListener("pointerup", endContainerReorder);
    document.removeEventListener("pointercancel", cancelContainerReorder);
    window.removeEventListener("blur", cancelContainerReorderOnBlur);
    document.removeEventListener("keydown", cancelContainerReorderOnKey);
    document.body.classList.remove("container-reordering");
    if (reorderState !== null) {
      reorderState.row.classList.remove("xrow-ghost");
      reorderState.marker.remove();
      reorderState.preview.remove();
    }
  };

  const cancelContainerReorder = (event) => {
    if (event && reorderState !== null && event.pointerId !== reorderState.pointerId) return;
    finishContainerReorder();
    reorderState = null;
  };
  const cancelContainerReorderOnBlur = () => cancelContainerReorder();
  const cancelContainerReorderOnKey = event => {
    if (event.key === "Escape") { event.preventDefault(); cancelContainerReorder(); }
  };

  const endContainerReorder = async (event) => {
    if (reorderState === null || event.pointerId !== reorderState.pointerId) return;
    const { id, index, list } = reorderState;
    finishContainerReorder();
    reorderState = null;
    if (index !== null) {
      const ordered = orderedContainers();
      const visible = list === decoyContainerList ? ordered.filter(item => decoyFound.includes(item.id)) : ordered;
      const from = visible.findIndex((container) => container.id === id);
      if (from !== -1) {
        const [moved] = visible.splice(from, 1);
        const to = index > from ? index - 1 : index;
        visible.splice(Math.max(0, Math.min(to, visible.length)), 0, moved);
        let at = 0;
        await rememberContainerOrder(list === decoyContainerList
          ? ordered.map(item => decoyFound.includes(item.id) ? visible[at++] : item) : ordered);
      }
    }
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
    row.appendChild(exportGlyph(fileTypePaths(entry),
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
    for (const [key, entry] of exportSelection) {
      if (isMaintenanceContainer(entry.id)) exportSelection.delete(key);
    }
    while (exportContainerList.firstChild !== null) {
      exportContainerList.removeChild(exportContainerList.firstChild);
    }
    if (containers.length === 0) {
      exportNote(exportContainerList, "还没有安全盘。");
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
      const checked = every || (roots === null && picked.length !== 0);
      const tail = checkBox(checked);
      tail.dataset.exportSelectAll = String(container.id);
      tail.addEventListener("click", (event) => {
        event.stopPropagation();
        void toggleExportContainerAll(container.id, !checked);
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
        if (isMaintenanceContainer(container.id)) {
          exportNote(files, "该安全盘正在进行高级更新，暂不支持此操作。");
        } else if (!container.mounted) {
          exportNote(files, "这个安全盘没有挂载。");
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
            exportNote(files, "这个安全盘是空的。");
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
      exportNote(migrateContainerList, "还没有安全盘。");
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
        if (busy || container.mounted || container.partition) return;
        migrateContainerId =
          migrateContainerId === container.id ? 0 : container.id;
        setMigrateStatus("");
        renderBackupCentre();
      };
      const tail = checkBox(container.id === migrateContainerId);
      tail.classList.add(container.mounted || container.partition ? "off" : "selectable");
      if (container.partition) tail.title = "暂不支持分区密文导出";
      tail.dataset.migrateSelect = String(container.id);
      if (!container.mounted && !container.partition) {
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
    backupExportLocation.disabled = busy;
    backupExportConfirm.disabled =
      busy || exportSelection.size === 0 || exportLocation === "";
    backupExportConfirm.textContent = exportSelection.size === 0
      ? "开始导出"
      : `开始导出（${exportSelection.size}）`;

    renderMigrateContainers();
    let migrateReady = false;
    for (const container of containers) {
      if (container.id === migrateContainerId && !container.mounted && !container.partition) {
        migrateReady = true;
      }
    }
    backupMigrateLocation.textContent =
      migrateLocation === "" ? "选择一个位置…" : migrateLocation;
    backupMigrateLocation.classList.toggle("ph", migrateLocation === "");
    backupMigrateBrowse.disabled = busy;
    backupMigrateLocation.disabled = busy;
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
    } catch (pageError) { logCaughtPageError("readContainerRoot/1", pageError);
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
    if (container === null || !container.mounted || isMaintenanceContainer(id)) return;
    const listing = await readContainerRoot(id);
    if (openExportContainer !== id) return;
    if (listing === null) {
      openExportError = "无法读取这个安全盘的根目录。";
    } else {
      openExportListing = listing;
    }
    renderBackupCentre();
  };

  const toggleExportPick = (id, directory, entry) => {
    if (isMaintenanceContainer(id)) return;
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
    if (isMaintenanceContainer(id)) {
      if (openExportContainer !== id) void toggleExportContainer(id);
      return;
    }
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
    if (container === null || !container.mounted || isMaintenanceContainer(container.id)) return;
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

  const exportResultDialog = document.getElementById("export-result-dialog");
  const exportResultTitle = document.getElementById("export-result-title");
  const exportResultSummary = document.getElementById("export-result-summary");
  const exportResultList = document.getElementById("export-result-list");
  const exportResultConfirm = document.getElementById("export-result-confirm");
  let exportResultItems = [];
  let exportResultRunning = false;
  let exportResultOwnsInert = false;
  const clearExportResults = () => {
    exportResultItems = [];
    exportResultRunning = false;
    exportResultList.replaceChildren();
    exportResultTitle.textContent = "";
    exportResultSummary.textContent = "";
    exportResultDialog.hidden = true;
    if (exportResultOwnsInert) dashboard.removeAttribute("inert");
    exportResultOwnsInert = false;
  };
  const renderExportResults = () => {
    const success = exportResultItems.filter(item => item.state === "success").length;
    const failed = exportResultItems.filter(item => item.state === "failed").length;
    exportResultTitle.textContent = exportResultRunning ? "正在导出" : "导出结果";
    exportResultSummary.textContent = "已完成 " + (success + failed) + "/" + exportResultItems.length + " 项　成功：" + success + "　失败：" + failed;
    exportResultConfirm.disabled = exportResultRunning;
    exportResultList.replaceChildren();
    for (const item of exportResultItems) {
      const row = document.createElement("div");
      row.className = "batch-result-row";
      row.dataset.state = item.state;
      const name = document.createElement("span");
      name.className = "batch-result-name";
      name.textContent = item.name;
      name.title = item.container + " / " + item.name;
      const state = document.createElement("span");
      state.className = "export-result-state";
      state.dataset.state = item.state;
      state.textContent = { waiting: "等待导出", running: "导出中", success: "导出成功", failed: "导出失败" }[item.state];
      row.append(name);
      if (item.state === "success" || item.state === "failed") {
        const mark = document.createElement("span");
        mark.className = "batch-result-sign " + (item.state === "success" ? "batch-result-ok" : "batch-result-why");
        mark.setAttribute("aria-hidden", "true");
        row.append(mark);
      }
      row.append(state);
      exportResultList.append(row);
    }
  };
  const dismissExportResults = () => {
    if (exportResultRunning) return;
    clearExportResults();
    if (currentState === nativeState.authenticated) backupExportConfirm.focus();
  };
  exportResultConfirm.addEventListener("click", dismissExportResults);
  exportResultDialog.addEventListener("pointerdown", event => {
    if (event.button === 0 && event.target === exportResultDialog) dismissExportResults();
  });
  exportResultDialog.addEventListener("keydown", event => {
    if (event.key === "Escape" || event.key === "Tab") {
      event.preventDefault(); event.stopPropagation();
      if (event.key === "Escape") dismissExportResults();
      else (exportResultRunning ? exportResultDialog : exportResultConfirm).focus();
    }
  });

  // 🔴 Every ticked entry is exported by the one entry point that may leave a
  // volume, called once per entry. A directory goes across whole, because the
  // copy underneath it already walks one.
  //
  // ⚠️ The calls run in order on the thread that draws this page, so a large
  // selection freezes the window for as long as the whole of it takes. The
  // asynchronous path and its cancellation are still not built; this panel
  // makes that gap easier to reach, and nothing here hides it.
  const runExport = async () => {
    if (busy || currentState !== nativeState.authenticated || exportSelection.size === 0 || exportLocation === "" || !exportResultDialog.hidden) return;
    const requests = [...exportSelection.values()];
    const location = exportLocation;
    const revision = batchSessionRevision;
    const current = () => revision === batchSessionRevision && currentState === nativeState.authenticated;
    busy = true;
    setBackupStatus("");
    exportResultItems = requests.map(item => ({ name: item.name, container: containers.find(c => c.id === item.id)?.name || "", state: "waiting" }));
    exportResultRunning = true;
    exportResultOwnsInert = !dashboard.hasAttribute("inert");
    dashboard.setAttribute("inert", "");
    exportResultDialog.hidden = false;
    renderExportResults();
    exportResultDialog.focus();
    render(messageForCurrentState());
    const failed = [];
    try {
      for (const [index, request] of requests.entries()) {
        if (!current()) return;
        exportResultItems[index].state = "running";
        renderExportResults();
        await new Promise((resolve) => { setTimeout(resolve, 0); });
        if (!current()) return;
        try {
          await bridge.exportContainerEntry(request.id, request.directory, request.name, location);
          if (!current()) return;
          exportSelection.delete(
          exportKey(request.id, request.directory, request.name));
          exportResultItems[index].state = "success";
        } catch (pageError) {
          if (!current()) return;
          logCaughtPageError("runExport/1", pageError);
          failed.push(request);
          exportResultItems[index].state = "failed";
        }
        renderExportResults();
      }
      try { await refreshNativeState(); }
      catch (pageError) { logCaughtPageError("runExport/2", pageError); currentState = nativeState.unavailable; activeVolumes = true; }
      if (!current()) return;
      if (failed.length !== 0) {
        setBackupStatus("部分文件导出失败，请检查目标位置和可用空间后重试。", "error");
      } else {
        exportLocation = "";
        setBackupStatus("");
      }
    } finally {
      busy = false;
      exportResultRunning = false;
      if (current()) { renderExportResults(); exportResultConfirm.focus(); }
      else { clearExportResults(); exportSelection.clear(); exportLocation = ""; }
      render(messageForCurrentState());
      renderBackupCentre();
      if (externalRefreshPending) await refreshAfterExternalChange();
    }
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
    } catch (pageError) { logCaughtPageError("runMigration/1", pageError);
      failed = true;
    }
    try {
      await refreshNativeState();
    } catch (pageError) { logCaughtPageError("runMigration/2", pageError);
      currentState = nativeState.unavailable;
      activeVolumes = true;
    }
    busy = false;
    render(messageForCurrentState());
    if (failed) {
      setMigrateStatus(
        "迁移没有完成。安全盘可能仍挂载着，位置不可写、空间不足，或那里已经有同名文件了。",
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

  // One message for every failure told the user their files were gone whenever
  // anything went wrong. These are the cases a person can act on differently;
  // none of them claims the contents are missing unless the volume itself
  // answered and the folder was the part that was not there.
  const directoryFailureMessages = {
    1: "这个安全盘当前没有挂载，请先挂载再浏览。",
    2: "这个安全盘已不在当前会话，请刷新列表后重试。",
    3: "读不到这个安全盘的磁盘。它可能已被卸载或断开，请重新挂载后再试。",
    4: "这个文件夹已经不在了，可能已被删除或移动。已返回安全盘根目录。",
    5: "没有读取这个文件夹的权限。",
    6: "读取磁盘时出错，请检查存储设备的连接及可用状态。",
    7: "这个文件夹的内容无法安全显示，已拒绝读取。",
  };
  const kDirectoryFailureUnknown =
    "暂时读不到这个文件夹，原因无法确定。请确认安全盘仍处于挂载状态后重试。";
  const readDirectoryFailure = async () => {
    try {
      const reason = await bridge.getDirectoryError();
      if (Number.isInteger(reason) && directoryFailureMessages[reason]) {
        return directoryFailureMessages[reason];
      }
    } catch (pageError) { logCaughtPageError("readDirectoryFailure/1", pageError);}
    return kDirectoryFailureUnknown;
  };

  const refreshFileManager = async () => {
    const revision = ++fileListingRevision;
    fileRefreshPendingRevision = revision;
    renderManagementView();
    renderFileBreadcrumb();
    const container = selectedFileContainer();
    if (bridge === null || container === null || !container.mounted || (inDecoyFileView() && !decoyMounted.has(container.id))) {
      fileEntries = [];
      setFileStatus("");
      renderFileList();
      fileRefreshPendingRevision = 0;
      updateFileRefreshAvailability();
      return;
    }
    const requestedId = container.id;
    const requestedPath = filePath.join("/");
    try {
      const packed = await bridge.listContainerDirectory(
        container.id, filePath.join("/"));
      if (revision !== fileListingRevision || fileContainerId !== requestedId || filePath.join("/") !== requestedPath ||
          currentState !== nativeState.authenticated) { logPageEvent(4, "refreshFileManager", "会话或页面状态变化，已放弃等待结果。"); return; }
      const listing = parseDirectoryListing(packed);
      fileEntries = listing.entries;
      setFileStatus(listing.truncated
        ? "这个文件夹里的条目太多，只显示了其中一部分。"
        : "");
    } catch (pageError) { logCaughtPageError("refreshFileManager/1", pageError);
      if (revision !== fileListingRevision || fileContainerId !== requestedId || filePath.join("/") !== requestedPath) { logPageEvent(4, "refreshFileManager", "会话或页面状态变化，已放弃等待结果。"); return; }
      // The root is the one place that always has somewhere to land, so a
      // folder that cannot be read does not strand the page inside it.
      fileEntries = [];
      filePath = [];
      renderFileBreadcrumb();
      setFileStatus(await readDirectoryFailure(), "error");
    } finally {
      if (fileRefreshPendingRevision === revision) fileRefreshPendingRevision = 0;
      updateFileRefreshAvailability();
    }
    renderFileList();
    // The destination a drop would land in is the directory just listed, so
    // it is re-named after every move through the file manager.
    await armDecoyDrop();
  };

  const browseContainer = (id) => {
    if (busy || currentState !== nativeState.authenticated || isMaintenanceContainer(id)) return;
    fileContainerId = id;
    filePath = [];
    fileEntries = [];
    showDashboardSection("files");
  };

  // Which of the three panels the maintenance column is showing. It starts
  // over whenever the session does: the walk's findings are session facts.
  let decoyStage = "verify";
  let decoyRefreshing = false;
  let decoyRefreshRevision = 0;
  let decoyRefreshStatusTimer = null;
  let decoyListVisible = false;
  let decoyMaintenanceId = 0;
  let decoyMaintenancePath = [];
  const decoyMounted = new Map();
  const isMaintenanceContainer = (id) => decoyMounted.has(id);
  const inDecoyFileView = () => decoyStage === "maintenance" && !decoyListVisible && activeSection === "decoy";
  let decoyFound = [];
  let decoyUnreachable = 0;
  let decoyCancelRequested = false;
  let decoyMaintenanceRevision = 0;
  const decoyProtectionWarning = document.getElementById("decoy-protection-warning");
  const decoyProtectionUnmount = document.getElementById("decoy-protection-unmount");
  const decoyProtectionUnmountStatus = document.getElementById("decoy-protection-unmount-status");
  document.body.appendChild(decoyProtectionWarning);
  let decoyProtectionOwnsInert = false;
  let decoyProtectionTarget = 0;
  const showDecoyProtectionWarning = (id) => {
    if (!decoyProtectionWarning.hidden) return;
    decoyProtectionTarget = id;
    const item = containers.find(item => item.id === id);
    document.getElementById("decoy-protection-container").textContent = item ? `${item.name} · ${item.drive}` : "";
    decoyProtectionOwnsInert = !dashboard.hasAttribute("inert");
    dashboard.setAttribute("inert", "");
    decoyProtectionWarning.hidden = false;
    decoyProtectionUnmount.focus();
  };
  decoyProtectionWarning.addEventListener("keydown", (event) => {
    if (event.key === "Escape" || event.key === "Tab") {
      event.preventDefault(); event.stopPropagation();
      if (event.key === "Tab") (decoyProtectionUnmount.disabled ? decoyProtectionWarning : decoyProtectionUnmount).focus();
    }
  });
  const decoyProtectionStatus = document.getElementById("decoy-protection-status");
  let decoyProtectionTimer = null;
  let decoyProtectionPending = false;
  let decoyProtectionGeneration = 0;
  let decoyWriteBlocked = false;
  let decoyUploadRecovery = false;
  const decoySelectFiles = document.getElementById("decoy-select-files");
  let decoyProtectionDisposed = false;

  const scheduleDecoyProtection = () => {
    if (decoyProtectionDisposed || decoyProtectionTimer !== null || decoyMounted.size === 0) return;
    decoyProtectionTimer = setTimeout(() => {
      decoyProtectionTimer = null;
      void pollDecoyProtection();
    }, 1000);
  };
  const pollDecoyProtection = async () => {
    if (decoyMounted.size === 0 || currentState !== nativeState.authenticated) return;
    if (busy || decoyProtectionPending || bridge === null) {
      scheduleDecoyProtection();
      return;
    }
    const revision = batchSessionRevision;
    const generation = decoyProtectionGeneration;
    const current = () => !decoyProtectionDisposed && revision === batchSessionRevision &&
      generation === decoyProtectionGeneration &&
      decoyMounted.size > 0 && currentState === nativeState.authenticated;
    decoyProtectionPending = true;
    const uploading = async () => {
      const status = String(await bridge.getExternalDropStatus());
      return /^1,\d+,\d+,\d+$/.test(status);
    };
    try {
      // The worker owns the session before its window notification reaches JS.
      // Atomic upload progress is readable during that interval; protection
      // queries are intentionally rejected until the worker releases it.
      if (await uploading()) {
        if (current()) {
          decoyProtectionStatus.hidden = true;
          void watchDecoyDrop();
        }
        return;
      }
      if (!current() || busy) return;
      const ids = [...decoyMounted.keys()].sort((a, b) => Number(b === decoyMaintenanceId) - Number(a === decoyMaintenanceId));
      let queryError = null;
      for (const id of ids) {
        if (!current() || busy) return;
        const known = decoyMounted.get(id);
        if (known?.blocked) { showDecoyProtectionWarning(id); continue; }
        let blocked;
        try {
          blocked = await bridge.getDecoyMaintenanceWriteBlocked(id);
          if (typeof blocked !== "boolean") throw new Error("invalid protection status");
        } catch (error) { queryError ??= error; continue; }
        if (!current()) return;
        const facts = decoyMounted.get(id);
        if (!facts) continue;
        facts.blocked ||= blocked;
        if (facts.blocked) showDecoyProtectionWarning(id);
      }
      decoyWriteBlocked = decoyMounted.get(decoyMaintenanceId)?.blocked ?? false;
      decoyProtectionStatus.hidden = true;
      if (decoyWriteBlocked) {
        decoyMaintenanceWritable.textContent = "写入已阻止";
        decoyMaintenanceSpaceDetail.textContent = "请结束维护并检查未完成文件，再重新进入维护。";
        decoyMaintenanceSpaceBar.style.width = "0%";
        decoyMaintenanceUsed.textContent = "—";
        decoyMaintenanceTotal.textContent = "";
        disarmDecoyDrop();
      }
      if (queryError) throw queryError;
    } catch (pageError) {
      if (!current()) return;
      // A drop can start between the progress read and the protection query.
      try {
        if (await uploading()) {
          if (current()) {
            decoyProtectionStatus.hidden = true;
            void watchDecoyDrop();
          }
          return;
        }
      } catch (pageError) { logCaughtPageError("pollDecoyProtection/2", pageError);}
      if (!current() || busy) return;
      logCaughtPageError("pollDecoyProtection/1", pageError);
      decoyProtectionStatus.textContent = "暂时无法确认写入保护状态，请暂停修改并稍后重试；无法确认不代表可以继续写入。";
      decoyProtectionStatus.hidden = false;
    } finally {
      decoyProtectionPending = false;
      scheduleDecoyProtection();
    }
  };
  window.addEventListener("pagehide", () => {
    decoyProtectionDisposed = true;
    ++decoyProtectionGeneration;
    clearTimeout(decoyProtectionTimer);
    decoyProtectionTimer = null;
  });
  window.addEventListener("focus", () => { void pollDecoyProtection(); });
  window.addEventListener("usb-vault-window-activated", () => {
    if (decoyMounted.size === 0 || decoyProtectionDisposed) return;
    clearTimeout(decoyProtectionTimer);
    decoyProtectionTimer = null;
    void pollDecoyProtection();
  });
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) void pollDecoyProtection();
  });
  // What the native window was last told to accept a drop into, so the page
  // does not re-arm it on every render. It is the destination only: the page
  // never learns what was dropped or where it came from.
  let decoyDropTarget = "";
  let decoyDropWatching = false;
  // The last status the page acted on. A small drop can begin and end between
  // two of the window's notices, so what is read has to be enough on its own
  // to tell a run the page has already reported from one it has not.
  let decoyDropReported = "0,0,0,0";

  const disarmDecoyDrop = () => {
    if (bridge === null || decoyDropTarget === "") return;
    decoyDropTarget = "";
    void Promise.resolve(bridge.clearExternalDropTarget()).catch((error) => { logCaughtPageError("disarmDecoyDrop/1", error); });
  };

  // 🔴 The page names a container and a directory and nothing else. Arming
  // happens only from the maintenance screen, so a drag anywhere else in the
  // product is refused by the window before the page is involved at all.
  const syncDecoyDropBounds = async () => {
    if (!bridge || typeof bridge.setExternalDropBounds !== "function") return;
    const ready = inDecoyFileView() && !busy && !decoyUploadRecovery && !decoyWriteBlocked &&
      currentState === nativeState.authenticated && decoyDropTarget !== "";
    const rect = decoyDropZone.getBoundingClientRect();
    const scale = window.devicePixelRatio || 1;
    const left = Math.max(0, rect.left), top = Math.max(0, rect.top);
    const right = Math.min(window.innerWidth, rect.right), bottom = Math.min(window.innerHeight, rect.bottom);
    const bounds = ready && right > left && bottom > top
      ? [Math.ceil(left * scale), Math.ceil(top * scale), Math.max(0, Math.floor(right * scale) - Math.ceil(left * scale)), Math.max(0, Math.floor(bottom * scale) - Math.ceil(top * scale))]
      : [0, 0, 0, 0];
    try {
      await bridge.setExternalDropBounds(...bounds);
      if (typeof bridge.setExternalDropButtonBounds === "function") {
        const button = decoySelectFiles.getBoundingClientRect();
        await bridge.setExternalDropButtonBounds(
          Math.max(0, Math.round(button.left * scale)), Math.max(0, Math.round(button.top * scale)),
          Math.max(0, Math.round(button.width * scale)), Math.max(0, Math.round(button.height * scale)));
      }
    } catch (pageError) { logCaughtPageError("syncDecoyDropBounds/1", pageError);}
  };

  const armDecoyDrop = async () => {
    if (bridge === null) return;
    if (!inDecoyFileView() || !decoyMounted.has(decoyMaintenanceId) || fileContainerId !== decoyMaintenanceId || decoyMaintenanceId === 0 || decoyUploadRecovery || decoyWriteBlocked ||
        currentState !== nativeState.authenticated) {
      disarmDecoyDrop();
      return;
    }
    const wanted = `${fileContainerId}/${filePath.join("/")}`;
    if (wanted === decoyDropTarget) { void syncDecoyDropBounds(); return; }
    try {
      await bridge.setExternalDropTarget(fileContainerId, filePath.join("/"));
      if (!inDecoyFileView() || `${fileContainerId}/${filePath.join("/")}` !== wanted) {
        decoyDropTarget = "";
        await bridge.clearExternalDropTarget();
        return;
      }
      decoyDropTarget = wanted;
      void syncDecoyDropBounds();
      // Naming a destination retires the run before it on the window's side,
      // so the page stops treating that run's numbers as already reported.
      // Without this a second drop with the same outcome would read as the
      // first one and go unreported.
      decoyDropReported = "0,0,0,0";
    } catch (pageError) { logCaughtPageError("armDecoyDrop/1", pageError);
      decoyDropTarget = "";
    }
  };

  const restoreFileManagerPlacement = () => {
    if (fileBreadcrumb.parentNode !== fileBreadcrumbHome) {
      fileBreadcrumbHome.insertBefore(
        fileBreadcrumb,
        fileBreadcrumbNext?.parentNode === fileBreadcrumbHome ? fileBreadcrumbNext : null);
    }
    if (managementFiles.parentNode !== managementFilesHome) {
      managementFilesHome.insertBefore(
        managementFiles,
        managementFilesNext?.parentNode === managementFilesHome ? managementFilesNext : null);
    }
  };

  const placeFileManagerInDecoyMaintenance = () => {
    if (fileBreadcrumb.parentNode !== decoyMaintenanceBreadcrumbSlot) {
      decoyMaintenanceBreadcrumbSlot.appendChild(fileBreadcrumb);
    }
    if (managementFiles.parentNode !== decoyMaintenanceFilesSlot) {
      decoyMaintenanceFilesSlot.appendChild(managementFiles);
    }
  };

  const renderDecoyDropZone = () => {
    decoySelectFiles.disabled = !decoyMounted.has(decoyMaintenanceId) || busy || decoyUploadRecovery || decoyWriteBlocked || currentState !== nativeState.authenticated;
    decoyDropRun.hidden = !decoyDropWatching;
    decoyDropCancel.disabled = !decoyDropWatching;
    const disabled = !decoyMounted.has(decoyMaintenanceId) || busy || decoyUploadRecovery || decoyWriteBlocked;
    decoyDropZone.setAttribute("aria-disabled", String(disabled));
    decoyDropZone.tabIndex = disabled ? -1 : 0;
    if (disabled) {
      decoyDropZone.classList.remove("drop-hover");
      decoySelectFiles.classList.remove("drop-button-hover");
    }
    void syncDecoyDropBounds();
  };

  const clearDecoyMaintenanceFacts = () => {
    ++decoyProtectionGeneration;
    clearTimeout(decoyProtectionTimer);
    decoyProtectionTimer = null;
    decoyWriteBlocked = decoyMounted.get(decoyMaintenanceId)?.blocked ?? false;
    decoyUploadRecovery = decoyMounted.get(decoyMaintenanceId)?.recovery ?? false;
    decoyProtectionTarget = 0;
    decoyProtectionWarning.hidden = true;
    if (decoyProtectionOwnsInert) dashboard.removeAttribute("inert");
    decoyProtectionOwnsInert = false;
    decoyProtectionUnmountStatus.textContent = "";
    decoyProtectionStatus.hidden = true;
    decoyMaintenanceWritable.textContent = "—";
    decoyMaintenanceSpaceDetail.textContent = "";
    decoyMaintenanceSpaceBar.style.width = "0%";
    decoyMaintenanceUsed.textContent = "—";
    decoyMaintenanceTotal.textContent = "";
    decoyMaintenanceFreshness.textContent = "—";
    decoyMaintenanceFreshness.dataset.age = "";
    setDecoyStatus(decoyMaintenanceStatus, "");
    setDecoyStatus(decoyDropStatus, "");
    restoreDecoyFacts();
  };

  const maintenanceCapacityState = () => decoyWriteBlocked ? "写入已阻止"
    : decoyUploadRecovery ? "需重新进入维护" : "";
  const formatSafeBytes = (bytes) => {
    if (bytes < 1024) return `${bytes} 字节`;
    const unit = bytes < 1048576 ? 1024 : bytes < 1073741824 ? 1048576 : 1073741824;
    const label = unit === 1024 ? "KiB" : unit === 1048576 ? "MiB" : "GiB";
    return `${Math.floor(bytes / unit * 100) / 100} ${label}`;
  };

  const formatCapacityMib = (mib) => {
    if (mib < 1024) return `${Math.floor(mib)} MiB`;
    const gib = mib / 1024;
    return `${gib >= 10 ? gib.toFixed(0) : gib.toFixed(1)} GiB`;
  };

  // Display-only snapshots belong to one live maintenance mount, never to disk.
  const renderDecoyCapacity = ({ total, free, safeFree }, cached = false) => {
    const used = total - free;
    const capacityUnit = total >= 1073741824 ? 1073741824 : 1048576;
    const capacityLabel = capacityUnit === 1073741824 ? "GiB" : "MiB";
    decoyMaintenanceUsed.textContent = (used / capacityUnit).toFixed(1);
    decoyMaintenanceTotal.textContent = `/ ${(total / capacityUnit).toFixed(1)} ${capacityLabel}`;
    decoyMaintenanceWritable.textContent = maintenanceCapacityState() || formatSafeBytes(safeFree);
    decoyMaintenanceSpaceDetail.textContent =
      (maintenanceCapacityState() ? "请结束维护并检查未完成文件，再重新进入维护。" :
        `当前安全空闲 ${safeFree.toLocaleString("zh-CN")} 字节，已排除保护区域。目录及文件系统元数据也会占用空间，不保证同等大小文件均可放入。`);
    decoyMaintenanceSpaceBar.style.width = total === 0
      ? "0%" : `${Math.min(100, Math.round((used / total) * 100))}%`;
    if (cached && !maintenanceCapacityState()) {
      decoyMaintenanceSpaceDetail.textContent = "上次计算结果，正在更新。" + decoyMaintenanceSpaceDetail.textContent.replace("当前安全空闲", "上次安全空闲");
    }
  };

  const restoreDecoyFacts = () => {
    const facts = decoyMounted.get(decoyMaintenanceId);
    if (!facts) return;
    if (maintenanceCapacityState()) {
      decoyMaintenanceWritable.textContent = maintenanceCapacityState();
      decoyMaintenanceSpaceDetail.textContent = "请结束维护并检查未完成文件，再重新进入维护。";
    } else if (facts.capacity) {
      renderDecoyCapacity(facts.capacity, true);
    }
    if (facts.freshness) {
      decoyMaintenanceFreshness.textContent = facts.freshness.text;
      decoyMaintenanceFreshness.dataset.age = facts.freshness.age;
    }
  };

  const renderDecoyFreshness = (listing) => {
    const timestamps = listing.entries.map((entry) => entry.modifiedUnixMs).filter((value) => value > 0);
    decoyMaintenanceFreshness.dataset.age = "";
    if (!listing.entries.length) {
      decoyMaintenanceFreshness.textContent = "暂无内容";
    } else if (!timestamps.length) {
      decoyMaintenanceFreshness.textContent = "上次修改：未知";
    } else {
      const modified = Math.max(...timestamps);
      const days = Math.max(0, Math.floor((Date.now() - modified) / 86400000));
      const when = days === 0 ? "今天"
        : days < 30 ? `${days} 日前` : days < 365 ? `${Math.floor(days / 30)} 月前` : `${Math.floor(days / 365)} 年前`;
      decoyMaintenanceFreshness.textContent = `上次修改：${when}`;
      decoyMaintenanceFreshness.dataset.age = days < 30 ? "fresh" : days < 180 ? "aging" : "stale";
    }
  };

  const renderDecoyMaintenanceChoices = () => {
    decoyMaintenanceContainer.replaceChildren();
    for (const known of orderedContainers().filter(item => decoyFound.includes(item.id))) {
      const id = known.id;
      if (known === undefined) continue;
      const option = document.createElement("option");
      option.value = String(id);
      option.textContent = known.name;
      option.selected = id === decoyMaintenanceId;
      decoyMaintenanceContainer.appendChild(option);
    }
    decoyMaintenanceContainer.disabled = busy || decoyMaintenanceContainer.options.length < 2;
  };

  const setDecoyStatus = (element, message, tone = "") => {
    if (element === decoyResultsStatus && decoyRefreshStatusTimer !== null) {
      clearTimeout(decoyRefreshStatusTimer);
      decoyRefreshStatusTimer = null;
    }
    element.textContent = message;
    element.dataset.tone = tone;
    if (element === decoyResultsStatus || element === decoyMaintenanceStatus ||
        element === decoyDropStatus) {
      element.hidden = message === "";
    }
  };

  const renderDecoySection = () => {
    scheduleDecoyProtection();
    decoyExit.disabled = busy || decoyRefreshing || currentState !== nativeState.authenticated;
    decoyRefresh.dataset.refreshing = String(decoyRefreshing && !busy);
    decoyRefresh.disabled = busy || decoyRefreshing || currentState !== nativeState.authenticated;
    const mounted = containers.find(item => item.id === decoyMaintenanceId && item.mounted);
    const maintenanceMounted = decoyStage === "maintenance" && decoyMounted.has(decoyMaintenanceId) && mounted;
    decoyMaintenanceDrive.textContent = maintenanceMounted ? `盘符 ${mounted.drive}` : "该安全盘未挂载，点击右侧更新以查看安全盘详情";
    decoyMaintenanceDrive.classList.toggle("unmounted", !maintenanceMounted);
    decoyProtectionUnmount.disabled = busy || currentState !== nativeState.authenticated;
    decoyVerify.hidden = decoyStage !== "verify";
    decoyProgress.hidden = decoyStage !== "running";
    decoyResults.hidden = decoyStage !== "results" && !decoyListVisible;
    decoyMaintenance.hidden = decoyStage !== "maintenance" || decoyListVisible;
    decoyOverviewHeader.hidden = decoyStage === "maintenance" && !decoyListVisible;
    decoyVerifySubmit.disabled = busy || currentState !== nativeState.authenticated;
    if (decoyStage === "maintenance") {
      scheduleDecoyProtection();
      if (inDecoyFileView()) placeFileManagerInDecoyMaintenance();
      else restoreFileManagerPlacement();
      decoyMaintenanceEnd.disabled = busy || currentState !== nativeState.authenticated;
      decoyMaintenanceEnd.textContent = decoyMounted.has(decoyMaintenanceId) ? "卸载" : "更新";
      decoyMaintenanceEnd.classList.toggle("danger", decoyMounted.has(decoyMaintenanceId));
      renderDecoyMaintenanceChoices();
      renderDecoyDropZone();
      renderManagementView();
      if (decoyListVisible) renderDecoyList();
    } else {
      restoreFileManagerPlacement();
      if (decoyStage === "results") renderDecoyList();
    }
    void armDecoyDrop();
  };

  const resetDecoySection = () => {
    ++decoyRefreshRevision;
    decoyRefreshing = false;
    decoyEmptyDialog.hidden = true;
    decoyMounted.clear();
    decoyMaintenanceId = 0;
    decoyListVisible = false;
    decoyMaintenancePath = [];
    decoyUploadRecovery = false;
    ++decoyMaintenanceRevision;
    disarmDecoyDrop();
    decoyDropWatching = false;
    decoyDropReported = "0,0,0,0";
    decoyStage = "verify";
    decoyFound = [];
    decoyUnreachable = 0;
    decoyCancelRequested = false;
    decoyPassword.value = "";
    decoyPassword.type = "password";
    decoyPasswordVisibility.setAttribute("aria-pressed", "false");
    decoyPasswordVisibility.setAttribute("aria-label", "显示密码");
    setDecoyStatus(decoyVerifyStatus, "");
    setDecoyStatus(decoyResultsStatus, "");
    while (decoyContainerList.firstChild !== null) {
      decoyContainerList.removeChild(decoyContainerList.firstChild);
    }
    fileContainerId = 0;
    filePath = [];
    fileEntries = [];
    clearDecoyMaintenanceFacts();
    restoreFileManagerPlacement();
    renderDecoySection();
  };

  const refreshDecoyCandidates = async (manual = false) => {
    if (busy || reorderState !== null || decoyRefreshing || currentState !== nativeState.authenticated ||
        !["results", "maintenance"].includes(decoyStage)) return;
    const revision = ++decoyRefreshRevision;
    const session = batchSessionRevision;
    decoyRefreshing = true;
    renderDecoySection();
    try {
      const raw = String(await bridge.refreshDecoyCandidates());
      if (revision !== decoyRefreshRevision || session !== batchSessionRevision) return;
      if (raw !== "" && !/^[1-9]\d*(,[1-9]\d*)*$/.test(raw)) throw new Error("invalid candidates");
      const ids = raw === "" ? [] : raw.split(",").map(Number);
      if (ids.some(id => !Number.isSafeInteger(id))) throw new Error("invalid candidates");
      decoyFound = [...new Set(ids)].filter(id => containers.some(item => item.id === id));
      if (manual) {
        setDecoyStatus(decoyResultsStatus, "列表已刷新。", "");
        decoyRefreshStatusTimer = setTimeout(() => {
          setDecoyStatus(decoyResultsStatus, "");
        }, 3000);
      }
    } catch (error) {
      if (revision === decoyRefreshRevision && session === batchSessionRevision)
        setDecoyStatus(decoyResultsStatus, "无法刷新列表，请稍后重试；验证已失效时请重新验证。", "error");
    } finally {
      if (revision === decoyRefreshRevision && session === batchSessionRevision) {
        decoyRefreshing = false;
        renderDecoySection();
      }
    }
  };
  decoyRefresh.addEventListener("click", () => { void refreshDecoyCandidates(true); });

  const renderDecoyList = () => {
    if (reorderState?.list === decoyContainerList) cancelContainerReorder();
    while (decoyContainerList.firstChild !== null) {
      decoyContainerList.removeChild(decoyContainerList.firstChild);
    }
    for (const known of orderedContainers().filter(item => decoyFound.includes(item.id))) {
      const id = known.id;
      const row = document.createElement("div");
      row.className = "container-row";
      row.dataset.decoyContainer = String(id);
      row.dataset.rowContainer = String(id);
      const grab = document.createElement("button");
      grab.type = "button";
      grab.className = "grab container-grab";
      grab.disabled = busy;
      grab.setAttribute("aria-label", "调整 " + known.name + " 的顺序");
      grab.appendChild(gripGlyph());
      grab.addEventListener("pointerdown", event => beginContainerReorder(event, row, id));
      grab.addEventListener("keydown", async event => {
        if (busy || !event.altKey || !["ArrowUp", "ArrowDown"].includes(event.key)) return;
        event.preventDefault();
        const ordered = orderedContainers();
        const visible = ordered.filter(item => decoyFound.includes(item.id));
        const from = visible.findIndex(item => item.id === id);
        const to = from + (event.key === "ArrowUp" ? -1 : 1);
        if (to < 0 || to >= visible.length) return;
        [visible[from], visible[to]] = [visible[to], visible[from]];
        let at = 0;
        await rememberContainerOrder(ordered.map(item => decoyFound.includes(item.id) ? visible[at++] : item));
      });

      const mark = document.createElement("span");
      mark.className = "container-mark";
      mark.appendChild(lockIcon());

      const identity = document.createElement("div");
      identity.className = "container-identity";
      const name = document.createElement("div");
      name.className = "container-name";
      name.textContent = known ? known.name : "";
      name.title = name.textContent;
      const state = document.createElement("div");
      state.className = "container-state";
      state.textContent = known && known.mounted ? "已挂载" : "未挂载";
      identity.append(name, state);

      const facts = document.createElement("div");
      facts.className = "container-facts";
      facts.append(containerFact("盘符", known && known.mounted ? known.drive : "—"));

      const actions = document.createElement("div");
      actions.className = "container-actions";
      const open = document.createElement("button");
      open.className = known.mounted ? "shell-button danger" : "shell-button";
      open.type = "button";
      open.textContent = known.mounted ? "卸载" : "更新";
      open.disabled = busy;
      open.addEventListener("click", () => {
        if (decoyMounted.has(id)) void endDecoyMaintenance(id);
        else if (known.mounted) void unmountAdoptedContainer(id);
        else void openDecoyMaintenance(id);
      });
      name.tabIndex = 0;
      name.setAttribute("role", "button");
      name.setAttribute("aria-label", `查看 ${known.name}`);
      name.style.cursor = "pointer";
      name.addEventListener("click", () => { void openDecoyMaintenance(id, false); });
      name.addEventListener("keydown", event => {
        if (event.key === "Enter" || event.key === " ") { event.preventDefault(); void openDecoyMaintenance(id, false); }
      });
      actions.appendChild(open);

      row.append(grab, mark, identity, facts, actions);
      decoyContainerList.appendChild(row);
    }
  };

  const closeDecoyConfirm = () => {
    decoyConfirmDialog.hidden = true;
    decoyVerifySubmit.disabled = busy || currentState !== nativeState.authenticated;
    decoyVerifySubmit.focus();
  };

  const openDecoyConfirm = () => {
    if (busy || currentState !== nativeState.authenticated) return;
    if (decoyPassword.value === "") {
      setDecoyStatus(decoyVerifyStatus, "请输入密码。", "error");
      decoyPassword.focus();
      return;
    }
    setDecoyStatus(decoyVerifyStatus, "");
    decoyConfirmDialog.hidden = false;
    decoyConfirmAccept.focus();
  };

  const runDecoySurvey = async () => {
    let emptyResult = false;
    const revision = batchSessionRevision;
    let encoded = null;
    let passwordBytes = null;
    decoyConfirmDialog.hidden = true;
    decoyCancelRequested = false;
    decoyStage = "running";
    decoyProgressCopy.textContent = "正在验证...";
    decoyProgressBar.style.width = "0%";
    busy = true;
    renderDecoySection();
    try {
      try {
        encoded = new TextEncoder().encode(decoyPassword.value);
        passwordBytes = Array.from(encoded);
      } finally {
        decoyPassword.value = "";
        wipe(encoded);
        encoded = null;
      }
      try {
        await bridge.beginDecoySurvey(passwordBytes);
      } finally {
        wipe(passwordBytes);
        passwordBytes = null;
      }
      for (;;) {
        const raw = String(await bridge.getDecoySurveyProgress());
        if (revision !== batchSessionRevision) { logPageEvent(4, "runDecoySurvey", "会话或页面状态变化，已放弃等待结果。"); return; }
        if (!/^\d+,\d+,\d+,\d+,\d+,[01]$/.test(raw)) throw new Error("invalid progress");
        const [state, total, done, found, unreachable, cancelled] =
          raw.split(",").map(Number);
        if (done > total || found > total) throw new Error("invalid progress");
        if (state === 2) {
          decoyUnreachable = unreachable;
          decoyFound = [];
          for (let index = 0; index < found; ++index) {
            const id = await bridge.getDecoySurveyId(index);
            if (revision !== batchSessionRevision) { logPageEvent(4, "runDecoySurvey", "会话或页面状态变化，已放弃等待结果。"); return; }
            if (!Number.isInteger(id) || id <= 0) throw new Error("invalid result");
            decoyFound.push(id);
          }
          emptyResult = decoyFound.length === 0;
          decoyStage = emptyResult ? "verify" : "results";
          if (emptyResult) {
            setDecoyStatus(decoyVerifyStatus, "");
            setDecoyStatus(decoyResultsStatus, "");
          } else if (cancelled === 1) {
            setDecoyStatus(decoyResultsStatus,
              "验证已中止，下面只列出中止前确认可更新的安全盘。", "warn");
          } else if (unreachable > 0) {
            setDecoyStatus(decoyResultsStatus,
              `有 ${unreachable} 个安全盘未能验证，通常是其中还有文件正在使用。`, "warn");
          } else setDecoyStatus(decoyResultsStatus, "");
          break;
        }
        if (state !== 1) throw new Error("survey failed");
        decoyProgressBar.style.width = total === 0 ? "0%" : `${Math.round((done / total) * 100)}%`;
        decoyProgressCopy.textContent = "正在验证...";
        await new Promise((resolve) => setTimeout(resolve, 100));
      }
    } catch (pageError) { logCaughtPageError("runDecoySurvey/1", pageError);
      decoyStage = "verify";
      setDecoyStatus(decoyVerifyStatus, "无法完成验证，请稍后重试。", "error");
    } finally {
      wipe(encoded);
      wipe(passwordBytes);
      busy = false;
      renderDecoySection();
      await refreshAfterExternalChange();
      if (emptyResult && revision === batchSessionRevision &&
          currentState === nativeState.authenticated && decoyStage === "verify") {
        decoyEmptyDialog.hidden = false;
        decoyEmptyClose.focus();
      }
    }
  };

  // The window tells the page a drop started; everything after that is read
  // back as four numbers until the run ends. Nothing about the source files
  // crosses the boundary, so what arrived is learnt by listing the directory.
  const reportDecoyDrop = async ([state, , done, failed]) => {
    if (state === 3 || state === 4) {
      decoyUploadRecovery = true;
      if (decoyMounted.has(decoyMaintenanceId)) decoyMounted.get(decoyMaintenanceId).recovery = true;
      decoyMaintenanceWritable.textContent = maintenanceCapacityState();
      decoyMaintenanceSpaceDetail.textContent = "请结束维护并检查未完成文件，再重新进入维护。";
      disarmDecoyDrop();
    }
    setDecoyStatus(decoyDropStatus,
      state === 2
        ? ""
        : state === 4
          ? `已停止，放入了 ${done} 个文件。请结束维护并检查未完成文件，再重新进入维护。`
          : `已放入 ${done} 个文件，${failed} 个未完成，后续文件已停止。请结束维护并检查未完成文件，再重新进入维护。`,
      state === 2 ? "success" : state === 4 ? "warn" : "error");
    renderDecoySection();
    // Re-arming is what retires the finished run on the native side, so the
    // destination is named again from scratch rather than through the guard.
    decoyDropTarget = "";
    await refreshFileManager();
    await refreshDecoyMaintenanceFacts();
  };

  const watchDecoyDrop = async () => {
    if (decoyDropWatching || bridge === null) return;
    let packed = "";
    try {
      packed = String(await bridge.getExternalDropStatus());
    } catch (pageError) { logCaughtPageError("watchDecoyDrop/1", pageError);
      return;
    }
    if (!/^[0-4],\d+,\d+,\d+$/.test(packed) || packed === decoyDropReported) {
      return;
    }
    decoyDropReported = packed;
    const seen = packed.split(",").map(Number);
    if (seen[0] === 0) return;
    if (seen[0] !== 1) {
      await reportDecoyDrop(seen);
      return;
    }

    const revision = batchSessionRevision;
    decoyDropWatching = true;
    ++decoyProtectionGeneration;
    decoyProtectionStatus.hidden = true;
    setDecoyStatus(decoyDropStatus, "");
    busy = true;
    renderDecoySection();
    renderManagementView();
    let outcome = [3, 0, 0, 0];
    try {
      for (;;) {
        const raw = String(await bridge.getExternalDropStatus());
        if (revision !== batchSessionRevision) { logPageEvent(4, "watchDecoyDrop", "会话或页面状态变化，已放弃等待结果。"); return; }
        if (!/^[0-4],\d+,\d+,\d+$/.test(raw)) throw new Error("invalid status");
        const [state, total, done, failed] = raw.split(",").map(Number);
        if (done + failed > total) throw new Error("invalid status");
        if (state !== 1) {
          outcome = [state, total, done, failed];
          decoyDropReported = raw;
          break;
        }
        const percent = Number(await bridge.getExternalFileProgress());
        if (revision !== batchSessionRevision) { logPageEvent(4, "watchDecoyDrop", "会话或页面状态变化，已放弃等待结果。"); return; }
        if (!Number.isInteger(percent) || percent < 0 || percent > 99) throw new Error("invalid file progress");
        decoyDropBar.style.width = total === 0
          ? "0%" : `${Math.min(99, Math.round(((done + percent / 100) / total) * 100))}%`;
        decoyDropCopy.textContent = `正在放入（${done}/${total}），当前文件 ${percent}%…`;
        await new Promise((resolve) => setTimeout(resolve, 150));
      }
    } catch (pageError) { logCaughtPageError("watchDecoyDrop/2", pageError, 4);
      outcome = [3, 0, 0, 0];
    } finally {
      decoyDropWatching = false;
      decoyDropBar.style.width = "0%";
      decoyDropCopy.textContent = "正在放入…";
      busy = false;
    }
    if (revision !== batchSessionRevision) { logPageEvent(4, "watchDecoyDrop", "会话或页面状态变化，已放弃等待结果。"); return; }
    await reportDecoyDrop(outcome);
  };

  const refreshDecoyMaintenanceFacts = async () => {
    if (busy || bridge === null || decoyStage !== "maintenance" || !decoyMounted.has(decoyMaintenanceId) ||
        currentState !== nativeState.authenticated) return;
    const sessionRevision = batchSessionRevision;
    const maintenanceId = decoyMaintenanceId;
    await pollDecoyProtection();
    if (busy || decoyWriteBlocked || sessionRevision !== batchSessionRevision ||
        maintenanceId !== decoyMaintenanceId || decoyStage !== "maintenance") return;
    const id = decoyMaintenanceId;
    const revision = ++decoyMaintenanceRevision;
    let capacityFailed = false;
    let freshnessFailed = false;
    busy = true;
    setDecoyStatus(decoyMaintenanceStatus, "");
    renderDecoySection();
    try {
      try {
        await waitForVolumeOperation(() => bridge.beginMeasureDecoySpace(id));
        const packed = String(await bridge.getDecoyMaintenanceSpace());
        if (!/^\d+,\d+,\d+$/.test(packed)) throw new Error("invalid capacity facts");
        const [total, free, safeFree] = packed.split(",").map(Number);
        if (![total, free, safeFree].every((value) =>
              Number.isSafeInteger(value) && value >= 0) ||
            free > total || safeFree > free) {
          throw new Error("invalid capacity facts");
        }
        if (revision !== decoyMaintenanceRevision || id !== decoyMaintenanceId ||
            decoyStage !== "maintenance") { logPageEvent(4, "refreshDecoyMaintenanceFacts", "会话或页面状态变化，已放弃等待结果。"); return; }
        const facts = decoyMounted.get(id);
        if (facts) facts.capacity = { total, free, safeFree };
        renderDecoyCapacity({ total, free, safeFree });
      } catch (pageError) { logCaughtPageError("refreshDecoyMaintenanceFacts/1", pageError);
        if (revision !== decoyMaintenanceRevision || id !== decoyMaintenanceId ||
            decoyStage !== "maintenance") { logPageEvent(4, "refreshDecoyMaintenanceFacts", "会话或页面状态变化，已放弃等待结果。"); return; }
        capacityFailed = true;
        const facts = decoyMounted.get(id);
        if (facts) delete facts.capacity;
        decoyMaintenanceWritable.textContent = maintenanceCapacityState() || "无法读取";
        decoyMaintenanceSpaceDetail.textContent = "容量事实暂不可用，请稍后刷新。";
        decoyMaintenanceSpaceBar.style.width = "0%";
        decoyMaintenanceUsed.textContent = "—";
        decoyMaintenanceTotal.textContent = "";
      }

      if (revision !== decoyMaintenanceRevision || id !== decoyMaintenanceId ||
          decoyStage !== "maintenance") { logPageEvent(4, "refreshDecoyMaintenanceFacts", "会话或页面状态变化，已放弃等待结果。"); return; }
      const listing = await readContainerRoot(id);
      if (revision !== decoyMaintenanceRevision || id !== decoyMaintenanceId ||
          decoyStage !== "maintenance") { logPageEvent(4, "refreshDecoyMaintenanceFacts", "会话或页面状态变化，已放弃等待结果。"); return; }
      if (listing === null) {
        freshnessFailed = true;
        const facts = decoyMounted.get(id);
        if (facts) delete facts.freshness;
        decoyMaintenanceFreshness.textContent = "无法读取";
        decoyMaintenanceFreshness.dataset.age = "";
      } else {
        renderDecoyFreshness(listing);
        const facts = decoyMounted.get(id);
        if (facts) facts.freshness = { text: decoyMaintenanceFreshness.textContent, age: decoyMaintenanceFreshness.dataset.age };
      }
      if (capacityFailed || freshnessFailed) {
        setDecoyStatus(decoyMaintenanceStatus,
          "部分维护信息没有刷新，可稍后重试。", "error");
      }
    } finally {
      busy = false;
      if (revision === decoyMaintenanceRevision) {
        renderDecoySection();
        void pollDecoyProtection();
      } else if (externalRefreshPending) {
        await refreshAfterExternalChange();
        renderDecoySection();
      }
    }
  };


  // Explorer can write straight into the outer volume while it is open here,
  // and nothing on that path tells the page. Coming back to the window is the
  // moment a user expects to see what they did, so the listing and capacity
  // are re-read then. The native side repeats its activation notice every
  // second while the window stays in front; only a notice after a gap counts.
  const kDecoyActivationGapMs = 2500;
  const kDecoyActivationRetryMs = 300;
  const kDecoyActivationGiveUpMs = 5000;
  let decoyLastActivationNotice = 0;
  let decoyActivationTimer = null;
  let decoyActivationDeadline = 0;
  let decoyActivationRunning = false;
  // Anything the user is in the middle of keeps its rows: a refresh would
  // replace the very element a menu or dialog is pointing at.
  const decoyActivationBlocked = () =>
    busy || externalRefreshPending || reorderState !== null || decoyDropWatching ||
    !fileNameDialog.hidden || !fileContextMenu.hidden;
  const runDecoyActivationRefresh = async () => {
    decoyActivationTimer = null;
    if (decoyActivationRunning || !inDecoyFileView() || bridge === null ||
        currentState !== nativeState.authenticated || !decoyMounted.has(decoyMaintenanceId)) return;
    if (decoyActivationBlocked()) {
      if (Date.now() < decoyActivationDeadline) {
        decoyActivationTimer = setTimeout(() => { void runDecoyActivationRefresh(); }, kDecoyActivationRetryMs);
      }
      return;
    }
    const session = batchSessionRevision;
    const maintenanceId = decoyMaintenanceId;
    decoyActivationRunning = true;
    try {
      await refreshFileManager();
      if (session !== batchSessionRevision || maintenanceId !== decoyMaintenanceId || !inDecoyFileView()) return;
      await refreshDecoyMaintenanceFacts();
    } catch (pageError) { logCaughtPageError("runDecoyActivationRefresh", pageError);
    } finally {
      decoyActivationRunning = false;
    }
  };
  const scheduleDecoyActivationRefresh = () => {
    if (!inDecoyFileView()) return;
    decoyActivationDeadline = Date.now() + kDecoyActivationGiveUpMs;
    clearTimeout(decoyActivationTimer);
    decoyActivationTimer = setTimeout(() => { void runDecoyActivationRefresh(); }, 150);
  };
  window.addEventListener("usb-vault-window-activated", () => {
    const now = Date.now();
    const edge = now - decoyLastActivationNotice > kDecoyActivationGapMs;
    decoyLastActivationNotice = now;
    if (edge) scheduleDecoyActivationRefresh();
  });
  window.addEventListener("blur", () => { decoyLastActivationNotice = 0; });
  window.addEventListener("focus", scheduleDecoyActivationRefresh);
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) decoyLastActivationNotice = 0;
    else scheduleDecoyActivationRefresh();
  });
  window.addEventListener("pagehide", () => {
    clearTimeout(decoyActivationTimer);
    decoyActivationTimer = null;
  });

  const openDecoyMaintenance = async (id, mount = true) => {
    if (busy || currentState !== nativeState.authenticated) return;
    const revision = batchSessionRevision;
    const wasMaintenance = decoyStage === "maintenance";
    let opened = false;
    busy = true;
    if (wasMaintenance) {
      ++decoyMaintenanceRevision;
      clearDecoyMaintenanceFacts();
    }
    renderDecoySection();
    try {
      if (mount && !decoyMounted.has(id)) {
        await waitForVolumeOperation(() => bridge.beginOpenDecoyMaintenance(id));
        if (revision === batchSessionRevision) decoyMounted.set(id, { blocked: false, recovery: false });
      }
      if (revision !== batchSessionRevision) { logPageEvent(4, "openDecoyMaintenance", "会话或页面状态变化，已放弃等待结果。"); return; }
      opened = true;

      decoyStage = "maintenance";
      decoyListVisible = false;
      decoyMaintenanceId = id;
      decoyMaintenancePath = [];
      fileContainerId = id;
      filePath = [];
      fileEntries = [];
      clearDecoyMaintenanceFacts();
    } catch (pageError) { logCaughtPageError("openDecoyMaintenance/1", pageError);
      setDecoyStatus(wasMaintenance ? decoyMaintenanceStatus : decoyResultsStatus,
        "无法打开该安全盘的外层卷。", "error");
    } finally {
      busy = false;
      renderDecoySection();
      await refreshAfterExternalChange();
      if (opened && revision === batchSessionRevision && decoyStage === "maintenance") {
        await refreshFileManager();
        await refreshDecoyMaintenanceFacts();
      } else if (wasMaintenance && revision === batchSessionRevision &&
                 decoyStage === "maintenance") {
        await refreshFileManager();
      }
    }
  };

  const endDecoyMaintenance = async (id = decoyMaintenanceId) => {
    if (busy || currentState !== nativeState.authenticated ||
        !decoyMounted.has(id)) return;
    const revision = batchSessionRevision;
    busy = true;
    setDecoyStatus(decoyMaintenanceStatus, "");
    decoyProtectionUnmountStatus.textContent = "";
    renderDecoySection();
    try {
      await waitForVolumeOperation(() => bridge.beginEndDecoyMaintenance(id));
      if (revision !== batchSessionRevision) { logPageEvent(4, "endDecoyMaintenance", "会话或页面状态变化，已放弃等待结果。"); return; }
      decoyMounted.delete(id);
      ++decoyMaintenanceRevision;
      decoyStage = "results";
      decoyListVisible = false;
      decoyMaintenanceId = 0;
      decoyMaintenancePath = [];
      fileContainerId = 0;
      filePath = [];
      fileEntries = [];
      clearDecoyMaintenanceFacts();
      showDashboardSection("decoy");
    } catch (pageError) { logCaughtPageError("endDecoyMaintenance/1", pageError);
      if (revision !== batchSessionRevision) return;
      decoyProtectionUnmountStatus.textContent = "无法卸载，请关闭占用该卷的程序后重试。";
      setDecoyStatus(decoyListVisible || decoyStage === "results" ? decoyResultsStatus : decoyMaintenanceStatus,
        "无法结束维护，请关闭可能占用文件的程序后重试。", "error");
    } finally {
      busy = false;
      renderDecoySection();
      await refreshAfterExternalChange();
    }
  };

  decoyExit.addEventListener("click", async () => {
    if (busy || decoyRefreshing || currentState !== nativeState.authenticated) return;
    const revision = batchSessionRevision;
    for (const id of [...decoyMounted.keys()]) {
      await endDecoyMaintenance(id);
      if (revision !== batchSessionRevision || currentState !== nativeState.authenticated) return;
      if (decoyMounted.has(id)) {
        decoyListVisible = true;
        renderDecoySection();
        setDecoyStatus(decoyResultsStatus, "无法退出高级更新，请关闭占用该卷的程序后重试。", "error");
        return;
      }
    }
    resetDecoySection();
    decoyPassword.focus();
  });

  decoyVerifySubmit.addEventListener("click", openDecoyConfirm);
  decoyPassword.addEventListener("keydown", (event) => {
    if (event.key === "Enter") openDecoyConfirm();
  });
  decoyPasswordVisibility.addEventListener("click", () => {
    const visible = decoyPassword.type === "text";
    decoyPassword.type = visible ? "password" : "text";
    decoyPasswordVisibility.setAttribute("aria-pressed", String(!visible));
    decoyPasswordVisibility.setAttribute("aria-label", visible ? "显示密码" : "隐藏密码");
    decoyPassword.focus();
  });
  decoyConfirmAccept.addEventListener("click", () => { void runDecoySurvey(); });
  const closeDecoyEmpty = () => {
    decoyEmptyDialog.hidden = true;
    decoyPassword.focus();
  };
  decoyEmptyClose.addEventListener("click", closeDecoyEmpty);
  decoyEmptyDialog.addEventListener("pointerdown", (event) => {
    if (event.button === 0 && event.target === decoyEmptyDialog) closeDecoyEmpty();
  });
  decoyEmptyDialog.addEventListener("keydown", (event) => {
    if (event.key === "Escape" || event.key === "Tab") {
      event.preventDefault();
      event.stopPropagation();
      if (event.key === "Escape") closeDecoyEmpty();
      else decoyEmptyClose.focus();
    }
  });
  decoyConfirmCancel.addEventListener("click", closeDecoyConfirm);
  decoyConfirmClose.addEventListener("click", closeDecoyConfirm);
  decoyProgressCancel.addEventListener("click", async () => {
    if (decoyCancelRequested) return;
    decoyCancelRequested = true;
    decoyProgressCancel.disabled = true;
    try { await bridge.cancelDecoySurvey(); } catch (pageError) { logCaughtPageError("decoyProgressCancel.click/1", pageError);}
    decoyProgressCancel.disabled = false;
  });
  decoyMaintenanceContainer.addEventListener("change", () => {
    const id = Number(decoyMaintenanceContainer.value);
    if (!Number.isInteger(id) || id <= 0 || id === decoyMaintenanceId) return;
    void openDecoyMaintenance(id, false);
  });
  decoyMaintenanceEnd.addEventListener("click", () => {
    if (decoyMounted.has(decoyMaintenanceId)) void endDecoyMaintenance();
    else void openDecoyMaintenance(decoyMaintenanceId);
  });
  decoyProtectionUnmount.addEventListener("click", () => { void endDecoyMaintenance(decoyProtectionTarget); });
  decoySelectFiles.addEventListener("click", async () => {
    if (busy || !decoyMounted.has(decoyMaintenanceId) || decoyUploadRecovery || decoyWriteBlocked || decoyStage !== "maintenance") return;
    await armDecoyDrop();
    try { if (await bridge.selectExternalFiles()) await watchDecoyDrop(); }
    catch (pageError) { logCaughtPageError("decoySelectFiles.click/1", pageError); setDecoyStatus(decoyDropStatus, "无法选择文件，请稍后重试。", "error"); }
  });
  decoyDropZone.addEventListener("dblclick", (event) => {
    if (!(event.target instanceof Element) || event.target.closest("button")) return;
    if (!decoySelectFiles.disabled) decoySelectFiles.click();
  });
  decoyDropZone.addEventListener("keydown", (event) => {
    if (event.target !== decoyDropZone || !["Enter", " "].includes(event.key)) return;
    event.preventDefault();
    if (!decoySelectFiles.disabled) decoySelectFiles.click();
  });
  window.addEventListener("usb-vault-drop-hover", (event) => {
    const enabled = inDecoyFileView() && decoyMounted.has(decoyMaintenanceId) && !busy && !decoyUploadRecovery && !decoyWriteBlocked;
    decoyDropZone.classList.toggle("drop-hover", enabled && (event.detail === 1 || event.detail === 3));
    decoySelectFiles.classList.toggle("drop-button-hover", enabled && event.detail === 2);
  });
  window.addEventListener("resize", () => { void syncDecoyDropBounds(); });
  document.addEventListener("scroll", () => { void syncDecoyDropBounds(); }, true);
  if (typeof ResizeObserver === "function") new ResizeObserver(() => { void syncDecoyDropBounds(); }).observe(decoyDropZone);
  decoyDropCancel.addEventListener("click", async () => {
    decoyDropCancel.disabled = true;
    try { await bridge.cancelExternalDrop(); } catch (pageError) { logCaughtPageError("decoyDropCancel.click/1", pageError);}
  });

  const partitionPickerVisible = () => !creationDialog.hidden && creationStorage === "disk";
  const clearCreationPartitions = () => {
    discardPartitions();
    if (bridge !== null) void bridge.clearPartitions().catch((error) => { logCaughtPageError("clearCreationPartitions/1", error); });
  };
  const partitionBody = document.getElementById("partition-list-body");
  const partitionStatus = document.getElementById("partition-status");
  const partitionPreserve = document.getElementById("partition-preserve-data");
  const updatePartitionMode = () => {
    const eraseButton = document.getElementById("partition-method-erase");
    eraseButton.setAttribute("aria-pressed", String(!partitionPreserve.checked));
    eraseButton.disabled = busy || currentState !== nativeState.authenticated;
    document.getElementById("partition-conversion-note").hidden = !partitionPreserve.checked;
    document.getElementById("partition-consent-text").textContent = partitionPreserve.checked
      ? "我确认已备份重要文件，允许对所选 NTFS 分区原地加密并移除原盘符。"
      : "我确认已备份数据，允许清空所选分区的全部内容并移除原盘符。";
  };
  const partitionErase = document.getElementById("partition-erase-consent");
  partitionPreserve.addEventListener("change", () => { partitionErase.checked = false; updatePartitionMode(); updateCreationAvailability(); });
  partitionErase.addEventListener("change", () => updateCreationAvailability());
  document.getElementById("partition-method-erase").addEventListener("click", () => {
    if (busy || currentState !== nativeState.authenticated) return;
    partitionPreserve.checked = false;
    partitionPreserve.dispatchEvent(new Event("change"));
  });
  document.getElementById("partition-review-back").addEventListener("click", () => {
    if (busy) return;
    partitionReviewOpen = false;
    partitionErase.checked = false;
    updateCreationAvailability();
    creationContinueButton.focus();
  });
  const partitionRefresh = document.getElementById("partition-refresh");
  const partitionManagement = document.getElementById("partition-open-management");
  let partitionRows = [], partitionSelected = 0, partitionRevision = 0;
  const partitionReasons = ["可选择", "Windows 系统卷", "无法确认分区信息", "暂不支持的分区表格式(非GPT/MBR)",
    "受保护的分区", "分区信息不一致", "暂不支持的扇区大小", "暂不支持的文件系统", "无法确认磁盘身份"];
  const partitionLocation = row => row.part ? `磁盘 ${row.disk} · 分区 ${row.part}` :
    row.state === 1 ? "Windows 系统卷" : "分区信息不可用";
  const partitionName = row => `${row.display || "盘符未知 · 卷标不可读"}（${partitionLocation(row)}）`;
  let renderedPartitionRows = null;
  const renderPartitions = () => {
    const disabled = !partitionPickerVisible() || busy || currentState !== nativeState.authenticated;
    partitionPreserve.disabled = disabled;
    partitionErase.disabled = disabled || !partitionSelected;
    partitionRefresh.disabled = disabled;
    partitionManagement.disabled = disabled;
    if (renderedPartitionRows !== partitionRows) {
      renderedPartitionRows = partitionRows;
      partitionBody.replaceChildren();
      const unavailable = document.getElementById("partition-unavailable-list");
      unavailable.replaceChildren();
      const rejected = partitionRows.filter(row => row.state !== 0);
      document.getElementById("partition-unavailable").hidden = !rejected.length;
      document.getElementById("partition-unavailable-summary").textContent = `${rejected.length} 个不可用分区`;
      for (const item of rejected) {
        const note = document.createElement("div"); note.className = "partition-row";
        const drive = document.createElement("span"); drive.className = "partition-drive";
        drive.textContent = item.display?.match(/^[A-Za-z]:/)?.[0] || "—";
        const identity = document.createElement("span"); identity.className = "partition-identity";
        const name = document.createElement("span"), reason = document.createElement("span");
        name.textContent = item.display || partitionLocation(item);
        reason.className = "partition-location"; reason.textContent = `${partitionLocation(item)} · ${partitionReasons[item.state]}`;
        identity.append(name, reason);
        const size = document.createElement("span"); size.className = "partition-size"; size.textContent = item.bytes ? partitionSize(item) : "—";
        note.append(drive, identity, size); unavailable.append(note);
      }
      const available = partitionRows.filter(row => row.state === 0);
      if (!available.length) {
        const note = document.createElement("p"); note.className = "partition-empty";
        note.textContent = busy && partitionPickerVisible() ? "正在读取分区…" : "暂无可用分区";
        partitionBody.append(note);
      }
      for (const item of available) {
        const button = document.createElement("button");
        button.type = "button"; button.className = "partition-row"; button.dataset.partitionId = String(item.id);
        button.setAttribute("aria-pressed", String(partitionSelected === item.id));
        button.setAttribute("aria-label", `选择${partitionName(item)}`);
        button.disabled = disabled;
        const drive = document.createElement("span"); drive.className = "partition-drive";
        drive.textContent = item.display?.match(/^[A-Za-z]:/)?.[0] || "—";
        const identity = document.createElement("span"); identity.className = "partition-identity";
        const volume = document.createElement("span"), location = document.createElement("span");
        volume.textContent = item.display?.replace(/^[A-Za-z]:\s*[·•]?\s*/, "") || "卷标不可读";
        location.textContent = partitionLocation(item); location.className = "partition-location";
        identity.append(volume, location);
        const size = document.createElement("span"); size.className = "partition-size"; size.textContent = partitionSize(item);
        const radio = document.createElement("span"); radio.className = "partition-radio"; radio.setAttribute("aria-hidden", "true");
        button.append(drive, identity, size, radio);
        button.addEventListener("click", () => { void selectPartitionRow(item.id); });
        partitionBody.append(button);
      }
    }
    const empty = partitionBody.querySelector(".partition-empty");
    if (empty) empty.textContent = busy && partitionPickerVisible() ? "正在读取分区…" : "暂无可用分区";
    for (const button of partitionBody.querySelectorAll("button[data-partition-id]")) {
      button.disabled = disabled;
      button.setAttribute("aria-pressed", String(Number(button.dataset.partitionId) === partitionSelected));
    }

  };
  const partitionSize = row => row.bytes < 1024 ** 3 ? `${(row.bytes / 1048576).toFixed(0)} MiB` : `${(row.bytes / (1024 ** 3)).toFixed(2)} GiB`;
  const discardPartitions = () => {
    document.getElementById("create-partition-outer-capacity").value = "";
    partitionReviewOpen = false;
    document.getElementById("partition-review-facts").replaceChildren();
    document.getElementById("partition-unavailable").open = false;
    ++partitionRevision; partitionRows = []; partitionSelected = 0; partitionErase.checked = false;
    partitionStatus.textContent = ""; partitionStatus.dataset.tone = "";
    renderPartitions();
  };
  const parsePartitionRows = packed => {
    if (typeof packed !== "string" || packed.length > 300000) throw new Error("invalid partition list");
    const rows = packed ? packed.split("\n") : [];
    if (rows.length > 4096) throw new Error("partition list too large");
    const ids = new Set();
    return rows.map(line => {
      if (!/^[1-9]\d*,\d+,\d+,\d+,[0-8]$/.test(line)) throw new Error("invalid partition row");
      const [id, disk, part, bytes, state] = line.split(",").map(Number);
      if (![id, disk, part].every(n => Number.isInteger(n) && n >= 0 && n <= 0xffffffff) ||
          !Number.isSafeInteger(bytes) || bytes < 0 || ids.has(id) || (state === 0 && (!part || !bytes)))
        throw new Error("invalid partition facts");
      ids.add(id); return {id, disk, part, bytes, state};
    });
  };
  const refreshPartitions = async (retainRows = false) => {
    if (!partitionPickerVisible() || busy || bridge === null || currentState !== nativeState.authenticated) return;
    if (retainRows) {
      ++partitionRevision; partitionSelected = 0; partitionErase.checked = false; partitionReviewOpen = false;
      document.getElementById("partition-review-facts").replaceChildren();
    } else discardPartitions();
    const revision = partitionRevision;
    busy = true; renderPartitions(); creationContinueButton.disabled = true; partitionStatus.textContent = "正在读取分区…"; partitionStatus.dataset.tone = "";
    try {
      if (await bridge.beginPartitionRefresh() !== true) throw new Error("scan rejected");
      let status;
      do {
        status = await bridge.getPartitionRefreshStatus();
        if (status === 1) await new Promise(resolve => setTimeout(resolve, 100));
      } while (status === 1);
      if (status !== 2) throw new Error("scan did not complete");
      const rows = parsePartitionRows(await bridge.getPartitionList());
      for (const row of rows) {
        const display = await bridge.getPartitionDisplay(row.id);
        if (typeof display !== "string" || display.length > 512) throw new Error("invalid partition display");
        row.display = display;
      }
      if (revision !== partitionRevision || !partitionPickerVisible()) { logPageEvent(4, "refreshPartitions", "会话或页面状态变化，已放弃等待结果。"); return; }
      partitionRows = rows;
      partitionStatus.textContent = ""; partitionStatus.dataset.tone = "";
    } catch (pageError) { logCaughtPageError("refreshPartitions/1", pageError);
      if (revision === partitionRevision) {
        discardPartitions();
        partitionStatus.textContent = "无法读取分区，请稍后刷新重试。"; partitionStatus.dataset.tone = "error";
      }
    } finally {
      busy = false; renderPartitions(); updateCreationAvailability();
      if (externalRefreshPending) void refreshAfterExternalChange();
    }
  };
  const selectPartitionRow = async id => {
    if (!partitionPickerVisible() || busy || bridge === null || currentState !== nativeState.authenticated) return;
    const revision = partitionRevision;
    partitionErase.checked = false;
    busy = true; renderPartitions(); creationContinueButton.disabled = true;
    try {
      if (await bridge.selectPartition(id) !== true) throw new Error("selection rejected");
      if (revision === partitionRevision) {
        partitionSelected = id;
        partitionStatus.textContent = ""; partitionStatus.dataset.tone = "";
      }
    } catch (pageError) { logCaughtPageError("selectPartitionRow/1", pageError);
      if (revision === partitionRevision) {
        discardPartitions();
        partitionStatus.textContent = "分区选择已失效，请刷新后重新选择。"; partitionStatus.dataset.tone = "error";
      }
    } finally {
      busy = false; renderPartitions(); updateCreationAvailability();
      if (externalRefreshPending) void refreshAfterExternalChange();
    }
  };
  partitionRefresh.addEventListener("click", () => { void refreshPartitions(); });
  partitionManagement.addEventListener("click", async () => {
    if (!partitionPickerVisible() || busy || bridge === null || currentState !== nativeState.authenticated) return;
    const revision = partitionRevision;
    partitionSelected = 0; partitionErase.checked = false;
    busy = true; renderPartitions(); creationContinueButton.disabled = true;
    let opened = false;
    try { opened = await bridge.openDiskManagement() === true; }
    catch (pageError) { logCaughtPageError("partitionManagement.click/1", pageError); /* The native call may already have invalidated its snapshot. */ }
    finally {
      busy = false;
      if (revision === partitionRevision && partitionPickerVisible() && currentState === nativeState.authenticated && !externalRefreshPending) {
        // Launching Disk Management invalidates the native snapshot. Re-enumerate
        // before enabling selection, retaining disabled display rows while waiting.
        await refreshPartitions(true);
        if (!opened) partitionStatus.textContent = "无法打开磁盘管理，可从 Windows 开始菜单打开。";
      }
      renderPartitions(); updateCreationAvailability();
      if (externalRefreshPending) void refreshAfterExternalChange();
    }
  });

  const showDashboardSection = (sectionName) => {
    if (sectionName === "history" && !operationLoggingEnabled) return;
    closeContainerSwipe();
    if (inDecoyFileView() && sectionName !== "decoy") {
      decoyMaintenancePath = [...filePath];
      fileListingRevision += 1;
      fileContainerId = 0;
      filePath = [];
      fileEntries = [];
      restoreFileManagerPlacement();
      disarmDecoyDrop();
    }
    const returningToMaintenance = sectionName === "decoy" &&
      activeSection !== "decoy" && decoyStage === "maintenance" && !decoyListVisible;
    if (returningToMaintenance) {
      fileListingRevision += 1;
      fileContainerId = decoyMaintenanceId;
      filePath = [...decoyMaintenancePath];
      fileEntries = [];
    }
    closeFileContextMenu();
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
    if (sectionName === "decoy") {
      renderDecoySection();
      if (returningToMaintenance) void refreshFileManager().then(() => refreshDecoyCandidates());
      else void refreshDecoyCandidates();
    }
    renderContainers();
  };

  const openCreationFlow = () => {
    if (busy || currentState !== nativeState.authenticated) {
      return;
    }
    // The flow always opens on the same step with the same preselection, so
    // opening it says nothing about which password is logged in.
    creationStorage = "file";
    creationInplace = false; partitionPreserve.checked = false; updatePartitionMode();
    discardPartitions();
    creationKind = "encrypted";
    creationStep = "params";
    creationCompleted = false;
    creationStatus.textContent = "";
    creationStatus.dataset.tone = "";
    resetCreationInputs();
    applyCreationStepCopy();
    creationDialog.hidden = false;
    updateCreationAvailability();
    renderPartitions();
    creationStorageButtons[0].focus();
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
    creationStep = "params";
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
    capturePageErrors();
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
  let containerRename = null;
  let containerListPersistence = null;

  const cancelContainerRename = () => {
    if (!containerRename) return;
    const editor = containerRename;
    containerRename = null;
    editor.input.value = "";
    editor.host.replaceChildren();
    editor.host.textContent = containers.find(item => item.id === editor.id)?.name ?? editor.original;
    editor.host.removeAttribute("data-editing");
    editor.host.setAttribute("role", "button");
    editor.host.tabIndex = 0;
    editor.status.remove();
  };

  const validContainerAlias = (value) => {
    if (value.length > 100 || (value.length && !value.trim().length) || /[\u0000-\u001f\u007f-\u009f\u2028\u2029/\\]/u.test(value)) return false;
    for (let i = 0; i < value.length; i++) {
      const code = value.charCodeAt(i);
      if (code >= 0xd800 && code <= 0xdbff) {
        const low = value.charCodeAt(++i);
        if (!(low >= 0xdc00 && low <= 0xdfff)) return false;
      } else if (code >= 0xdc00 && code <= 0xdfff) return false;
    }
    return true;
  };

  const saveContainerRename = async () => {
    const editor = containerRename;
    if (!editor || editor.saving || editor.composing || busy) return;
    if (editor.revision !== batchSessionRevision || externalRefreshPending || currentState !== nativeState.authenticated) {
      cancelContainerRename(); return;
    }
    const value = editor.input.value;
    if (value === editor.original) { cancelContainerRename(); renderContainers(); return; }
    if (!validContainerAlias(value)) {
      editor.status.textContent = "名称最多100个字符，不能仅含空白，或包含控制字符、斜杠及反斜杠。";
      editor.input.setAttribute("aria-invalid", "true");
      editor.input.focus(); return;
    }
    if (containerListPersistence !== true || isMaintenanceContainer(editor.id)) {
      cancelContainerRename(); renderContainers(); return;
    }
    editor.saving = true;
    editor.input.readOnly = true;
    editor.status.textContent = "";
    busy = true;
    const wasInert = dashboard.hasAttribute("inert");
    dashboard.setAttribute("inert", "");
    let written = false;
    try {
      await bridge.renameContainer(editor.id, value);
      written = true;
      if (editor.revision !== batchSessionRevision || externalRefreshPending) return;
      await refreshNativeState();
      if (editor.revision !== batchSessionRevision || externalRefreshPending) return;
      cancelContainerRename();
      setDashboardStatus("名称已保存。");
    } catch (pageError) {
      logCaughtPageError("saveContainerRename", pageError);
      if (editor.revision === batchSessionRevision && !externalRefreshPending && containerRename === editor) {
        if (written) {
          cancelContainerRename();
          setDashboardStatus("名称已保存，但列表刷新失败，请重新登录确认。", "error");
        } else {
          editor.status.textContent = "名称保存失败，原名称未更改。请重试或按 Esc 取消。";
          editor.input.setAttribute("aria-invalid", "true");
        }
      }
    } finally {
      busy = false;
      if (!wasInert) dashboard.removeAttribute("inert");
      if (containerRename === editor) {
        editor.saving = false;
        editor.input.readOnly = false;
        editor.input.focus({ preventScroll: true });
      }
      if (externalRefreshPending) void refreshAfterExternalChange();
      else if (editor.revision === batchSessionRevision) updateProductSurface();
    }
  };

  const beginContainerRename = (container, host) => {
    if (busy || externalRefreshPending || bridge === null || reorderState !== null ||
        currentState !== nativeState.authenticated || isMaintenanceContainer(container.id)) return;
    if (containerListPersistence !== true) {
      setDashboardStatus(containerListPersistence === false
        ? "请先在系统设置开启“持久化安全盘列表”，再修改名称。"
        : "无法读取“持久化安全盘列表”设置，请稍后重试。", "error");
      return;
    }
    if (containerRename) { containerRename.input.focus(); return; }
    const input = document.createElement("input");
    input.className = "container-name-input";
    input.type = "text";
    input.maxLength = 100;
    input.autocomplete = "off";
    input.spellcheck = false;
    input.value = container.name;
    input.placeholder = "清空恢复默认名称";
    input.setAttribute("aria-label", "安全盘显示名称");
    const status = document.createElement("span");
    status.className = "container-name-status";
    status.setAttribute("role", "status");
    status.id = `container-name-status-${container.id}`;
    input.setAttribute("aria-describedby", status.id);
    const editor = { id: container.id, original: container.name, host, input, status,
      revision: batchSessionRevision, section: activeSection, saving: false, composing: false };
    containerRename = editor;
    host.setAttribute("data-editing", "true");
    host.removeAttribute("role");
    host.tabIndex = -1;
    host.replaceChildren(input);
    host.after(status);
    input.addEventListener("compositionstart", () => { editor.composing = true; });
    input.addEventListener("compositionend", () => {
      editor.composing = false;
      if (containerRename === editor && document.activeElement !== input) void saveContainerRename();
    });
    input.addEventListener("input", () => {
      status.textContent = "";
      input.removeAttribute("aria-invalid");
    });
    input.addEventListener("keydown", event => {
      event.stopPropagation();
      if (event.isComposing || editor.composing || event.keyCode === 229 || editor.saving) return;
      if (event.key === "Enter") { event.preventDefault(); void saveContainerRename(); }
      if (event.key === "Escape") { event.preventDefault(); cancelContainerRename(); renderContainers(); }
    });
    input.addEventListener("blur", () => {
      if (containerRename === editor && !editor.saving) void saveContainerRename();
    });
    input.focus();
    input.select();
  };

  const containerActionButton = (container, action, label, disabled = false) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = action === "unmount" ? "shell-button danger" : "shell-button";
    button.textContent = label;
    button.disabled = busy || disabled || isMaintenanceContainer(container.id) || (Boolean(container.conversion) && ["mount", "browse", "export"].includes(action));
    button.dataset.containerId = String(container.id);
    button.dataset.containerName = container.name;
    button.dataset.containerAction = action;
    return button;
  };

  let openContainerSwipe = null;
  let cancelContainerSwipe = null;
  const closeContainerSwipe = () => {
    cancelContainerSwipe?.();
    if (openContainerSwipe) {
      openContainerSwipe.row.style.transform = "";
      openContainerSwipe.remove.hidden = true;
      openContainerSwipe = null;
    }
  };
  document.addEventListener("pointerdown", event => {
    if (openContainerSwipe && !openContainerSwipe.rail.contains(event.target)) closeContainerSwipe();
  });
  window.addEventListener("blur", closeContainerSwipe);
  const wireContainerSwipe = (rail, row, remove) => {
    const swipeWidth = 108;
    const setDistance = distance => {
      remove.hidden = distance === 0;
      remove.style.clipPath = `inset(0 0 0 ${swipeWidth - distance}px)`;
      row.style.transform = `translateX(-${distance}px)`;
    };
    let suppressClick = false;
    const reveal = () => {
      closeContainerSwipe();
      setDistance(swipeWidth);
      openContainerSwipe = { rail, row, remove };
    };
    row.addEventListener("click", event => {
      if (suppressClick) {
        event.preventDefault(); event.stopImmediatePropagation();
        suppressClick = false;
        return;
      }
      if (openContainerSwipe?.row === row) {
        event.preventDefault(); event.stopImmediatePropagation();
        closeContainerSwipe();
      }
    }, true);
    row.addEventListener("keydown", event => {
      if (event.target.closest("input, button") || busy || row.dataset.busy === "true") return;
      if (event.key === "ArrowLeft") { event.preventDefault(); reveal(); if (!remove.disabled) remove.focus(); }
      if (["ArrowRight", "Escape"].includes(event.key)) { event.preventDefault(); closeContainerSwipe(); }
    });
    remove.addEventListener("keydown", event => {
      if (["Escape", "ArrowRight"].includes(event.key)) {
        event.preventDefault(); closeContainerSwipe(); row.querySelector(".container-name").focus();
      }
    });
    row.addEventListener("pointerdown", event => {
      if (event.button !== 0 || busy || reorderState || containerRename || row.dataset.busy === "true" ||
          event.target.closest("button, input, a")) return;
      suppressClick = false;
      const wasOpen = openContainerSwipe?.row === row;
      closeContainerSwipe();
      if (wasOpen) setDistance(swipeWidth);
      const startX = event.clientX, startY = event.clientY, pointer = event.pointerId;
      let distance = wasOpen ? swipeWidth : 0, dragging = false;
      const cleanup = () => {
        document.removeEventListener("pointermove", move);
        document.removeEventListener("pointerup", end);
        document.removeEventListener("pointercancel", cancel);
        rail.removeAttribute("data-dragging");
        cancelContainerSwipe = null;
      };
      const cancel = () => { cleanup(); suppressClick = false; row.style.transform = ""; remove.hidden = true; };
      const move = e => {
        if (e.pointerId !== pointer) return;
        const dx = e.clientX - startX, dy = e.clientY - startY;
        if (!dragging) {
          if (Math.abs(dy) > 10 && Math.abs(dy) >= Math.abs(dx)) { cancel(); return; }
          if (Math.abs(dx) < 10 || Math.abs(dx) <= Math.abs(dy)) return;
          dragging = true;
          suppressClick = true;
          rail.dataset.dragging = "true";
        }
        e.preventDefault();
        distance = Math.max(0, Math.min(swipeWidth, (wasOpen ? swipeWidth : 0) - dx));
        setDistance(distance);
      };
      const end = e => {
        if (e.pointerId !== pointer) return;
        cleanup();
        if (distance >= swipeWidth / 3 && dragging) reveal();
        else { row.style.transform = ""; remove.hidden = true; }
      };
      cancelContainerSwipe = cancel;
      document.addEventListener("pointermove", move, { passive: false });
      document.addEventListener("pointerup", end);
      document.addEventListener("pointercancel", cancel);
    });
  };

  const containerRow = (container, management) => {
    const group = document.createElement("div");
    group.className = "container-group";
    const row = document.createElement("div");
    row.dataset.rowContainer = String(container.id);
    row.className = container.mounted
      ? "container-row mounted"
      : "container-row";
    const maintaining = isMaintenanceContainer(container.id);
    row.dataset.busy = String(busy || maintaining);
    row.setAttribute("aria-disabled", String(maintaining));
    if (management) {
      row.classList.add("management-row");
      row.tabIndex = maintaining ? -1 : 0;
      row.setAttribute("aria-label", `打开安全盘 ${container.name}`);
      row.addEventListener("keydown", (event) => {
        if (event.target !== row || !["Enter", " "].includes(event.key)) return;
        event.preventDefault();
        browseContainer(container.id);
      });
    }
    row.addEventListener("click", (event) => {
      if (busy || !(event.target instanceof Element) ||
          event.target.closest("button") !== null) return;
      // The arrow is also the keyboard-accessible disclosure control. Row
      // clicks use that same action; buttons and the drag grip keep theirs.
      if (management) browseContainer(container.id);
      else row.querySelector("[data-container-action=details]").click();
    });

    const grab = document.createElement("button");
    grab.type = "button";
    grab.className = "grab container-grab";
    grab.disabled = busy || maintaining;
    grab.setAttribute("aria-label", `调整 ${container.name} 的顺序`);
    grab.title = "拖动排序；也可按 Alt + 上下方向键";
    grab.appendChild(gripGlyph());
    grab.addEventListener("pointerdown", (event) => {
      beginContainerReorder(event, row, container.id);
    });
    grab.addEventListener("keydown", async (event) => {
      if (busy || !event.altKey || !["ArrowUp", "ArrowDown"].includes(event.key)) return;
      event.preventDefault();
      const ordered = orderedContainers();
      const from = ordered.findIndex((item) => item.id === container.id);
      const to = from + (event.key === "ArrowUp" ? -1 : 1);
      if (from < 0 || to < 0 || to >= ordered.length) return;
      const list = row.closest("[data-container-list]");
      [ordered[from], ordered[to]] = [ordered[to], ordered[from]];
      await rememberContainerOrder(ordered);
      list.querySelector(`[data-row-container="${container.id}"] .container-grab`)?.focus({ preventScroll: true });
    });

    const mark = document.createElement("span");
    mark.className = "container-mark";
    mark.appendChild(lockIcon());

    const identity = document.createElement("div");
    identity.className = "container-identity";
    const name = document.createElement("div");
    name.className = "container-name";
    name.textContent = container.name;
    name.tabIndex = busy || maintaining ? -1 : 0;
    name.setAttribute("role", "button");
    name.setAttribute("aria-label", `${container.name}，双击或按 F2 修改名称`);
    name.addEventListener("click", event => event.stopPropagation());
    name.addEventListener("dblclick", event => {
      event.stopPropagation();
      if (event.target === name) beginContainerRename(container, name);
    });
    name.addEventListener("keydown", event => {
      if (event.target !== name || !["F2", "Enter"].includes(event.key)) return;
      event.preventDefault(); event.stopPropagation();
      beginContainerRename(container, name);
    });
    const state = document.createElement("div");
    state.className = "container-state";
    state.textContent = container.conversion === 2 ? "加密已暂停" : container.conversion === 1 ? "转换未完成，请检查磁盘" : maintaining ? "正在进行高级更新，请卸载后重新挂载" : container.unavailable ? "无法读取卷状态" : container.mounted ? "已挂载" : "未挂载";
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
    if (!management) {
      const reserve = document.createElement("span");
      reserve.className = "shell-button container-action-reserve";
      reserve.textContent = "从列表移除";
      reserve.setAttribute("aria-hidden", "true");
      actions.appendChild(reserve);
    }
    actions.appendChild(containerActionButton(container,
      container.conversion === 2 ? "resume" : container.mounted ? "unmount" : "mount", container.conversion === 2 ? "继续加密" : container.mounted ? "卸载" : "挂载"));
    const expanded = !management && expandedContainerId === container.id;
    const details = containerActionButton(container, "details", "");
    details.className = "container-details-toggle";
    details.setAttribute("aria-label", `${expanded ? "收起" : "展开"} ${container.name} 的详情`);
    details.setAttribute("aria-expanded", String(expanded));
    details.appendChild(exportGlyph(["M6 9l6 6 6-6"]));
    row.append(grab, mark, identity, facts, actions);
    if (!management) row.appendChild(details);
    const rail = document.createElement("div");
    rail.className = "container-swipe";
    const remove = containerActionButton(container, "forget", "从列表移除", container.mounted);
    remove.className = "container-swipe-remove";
    remove.hidden = true;
    if (container.mounted) remove.title = "请先卸载安全盘";
    rail.append(remove, row);
    wireContainerSwipe(rail, row, remove);
    group.appendChild(rail);

    if (expanded) {
      const panel = document.createElement("div");
      panel.className = "container-details";
      const grid = document.createElement("div");
      grid.className = "container-details-grid";
      grid.append(
        containerFact("名称", container.name),
        containerFact("状态", container.conversion === 2 ? "加密已暂停" : container.conversion === 1 ? "转换未完成" : container.unavailable ? "无法读取卷状态" : container.mounted ? "已挂载" : "未挂载"),
        containerFact("盘符", container.mounted ? container.drive : "—"),
        containerFact("容量", container.mounted && container.sizeMib > 0 ? containerSizeText(container) : "—"),
        containerFact("加密", "AES-256-XTS"),
        containerFact("派生", "Argon2id 512M·2·1"));
      const operations = document.createElement("div");
      operations.className = "container-detail-actions";
      operations.appendChild(containerActionButton(container, "browse", "浏览文件"));
      operations.appendChild(containerActionButton(container, "export", "导出与迁移"));
      panel.append(grid, operations);
      group.appendChild(panel);
    }
    return group;
  };

  const renderContainers = () => {
    closeContainerSwipe();
    if (containerRename) {
      const editor = containerRename;
      const exists = containers.some(item => item.id === editor.id);
      if (currentState === nativeState.authenticated && editor.revision === batchSessionRevision &&
          !externalRefreshPending && exists && !isMaintenanceContainer(editor.id) &&
          containerListPersistence === true && activeSection === editor.section &&
          (!busy || editor.saving) && editor.host.isConnected) return;
      cancelContainerRename();
    }
    if (reorderState !== null) {
      finishContainerReorder();
      reorderState = null;
    }
    if (currentState !== nativeState.authenticated) {
      fileListingRevision += 1;
      expandedContainerId = 0;
      containerOrder = [];
      pendingNewContainers.clear();
      fileContainerId = 0;
      filePath = [];
      fileEntries = [];
      renderFileBreadcrumb();
      renderFileList();
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
          "还没有安全盘。安全盘是一个加密文件，挂载后在系统里显示为一个普通磁盘。" +
          "新建一个，或导入已有的安全盘文件。";
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
    if (!busy && creationDialog.hidden && volumeOperationDialog.hidden && currentState === nativeState.authenticated) {
      for (const id of pendingNewContainers) {
        for (const list of containerLists) {
          const row = list.querySelector(`[data-row-container="${id}"]`);
          if (!row) continue;
          row.classList.add("container-new");
          setTimeout(() => row.classList.remove("container-new"), 2200);
        }
      }
      pendingNewContainers.clear();
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
    "打开会话", "创建安全盘", "挂载安全盘", "卸载安全盘",
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
    renderBatchControls();
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
    if (currentState !== nativeState.authenticated && partitionRows.length) discardPartitions();
    renderPartitions();
    appExitToggle.disabled = busy || currentState !== nativeState.authenticated;
    returnToLogin.disabled = appExitToggle.disabled;
    exitApplication.disabled = appExitToggle.disabled;
    if (appExitToggle.disabled) closeAppExit();
    sessionTools.hidden = bridge === null || currentState === nativeState.authenticated;
    sessionToolsToggle.disabled = busy || currentState === null || bridge === null;
    if (busy || sessionTools.hidden) closeSessionTools();
    forgotOpen.hidden = false;
    forgotOpen.disabled = busy || !forgotAvailable;
    backupButtons.forEach(button => { button.disabled = busy || bridge === null || currentState === null; });
    configurationButton.hidden = bridge === null || currentState === nativeState.authenticated;
    configurationButton.disabled = busy || activeVolumes || currentState === null || bridge === null;
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
        actionLabel.textContent = "一键卸载";
        break;
      case nativeState.unavailable:
        stateTitle.textContent = "安全组件当前不可用";
        actionLabel.textContent = "尝试一键卸载";
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
          : "正在一键卸载";
    }
    statusOutput.textContent = message;
    statusOutput.dataset.tone = /^(错误码：\d{4}$|无法|安全操作未完成|需要管理员身份)/.test(message) ? "error" : "";
    errorOutput.textContent = error;
    errorOutput.hidden = error.length === 0;
    updateProductSurface();
    capturePageErrors();
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
  // settings page is page-local and flips itself; these are views of what
  // the native side holds, and are never set from what the user clicked, so a
  // refused or failed write cannot leave the page showing a setting that was
  // not saved.
  const storedSettings = [
    {
      setting: "close-to-tray",
      read: () => bridge.getCloseToTrayEnabled(),
      write: (value) => bridge.setCloseToTrayEnabled(value),
    },
    {
      setting: "auto-mount",
      read: () => bridge.getAutoMountEnabled(),
      write: (value) => bridge.setAutoMountEnabled(value),
    },
    {
      setting: "operation-logging",
      read: () => bridge.getOperationLoggingEnabled(),
      write: (value) => bridge.setOperationLoggingEnabled(value),
    },
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
    await refreshExitHotkey();
    const force = await bridge.getForceUnmountEnabled();
    if (typeof force !== "boolean") throw new Error("invalid force preference");
    savedForceUnmount = force;
    batchForceUnmount.checked = force;
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
        if (stored.setting === "remember-containers") containerListPersistence = value;
        if (stored.setting === "operation-logging") {
          operationLoggingEnabled = value;
          for (const item of dashboardNavigation) {
            if (item.dataset.section === "history") item.hidden = !value;
          }
          if (!value && activeSection === "history") showDashboardSection("settings");
        }
      } catch (pageError) { logCaughtPageError("refreshStoredSwitches/1", pageError);
        if (stored.setting === "remember-containers") containerListPersistence = null;
        // An unreadable setting is not a licence to invent one. The switch
        // keeps the position it already had.
      }
    }
  };

  const exitHotkeyEnabled = document.getElementById("exit-hotkey-enabled");
  const exitHotkeyOptions = document.getElementById("exit-hotkey-options");
  const exitHotkeyKey = document.getElementById("exit-hotkey-key");
  const exitHotkeyCount = document.getElementById("exit-hotkey-count");
  const exitHotkeySave = document.getElementById("exit-hotkey-save");
  const exitHotkeyStatus = document.getElementById("exit-hotkey-status");
  const exitHotkeySaved = document.getElementById("exit-hotkey-saved");
  let savedExitHotkey = { enabled: false, key: 123, count: 3 };
  let exitHotkeySaving = false;
  const exitHotkeyKeys = new Map([[123, "F12"]]);
  for (let key = 112; key < 123; key++) exitHotkeyKeys.set(key, `F${key - 111}`);
  exitHotkeyKeys.set(27, "Esc"); exitHotkeyKeys.set(32, "空格");
  for (let key = 65; key <= 90; key++) exitHotkeyKeys.set(key, String.fromCharCode(key));
  for (let key = 48; key <= 57; key++) exitHotkeyKeys.set(key, String.fromCharCode(key));
  const exitHotkeyCodes = new Map([
    ["Backspace",8],["Tab",9],["Clear",12],["Enter",13],["NumpadEnter",13],["Pause",19],["CapsLock",20],
    ["PageUp",33],["PageDown",34],["End",35],["Home",36],["ArrowLeft",37],["ArrowUp",38],["ArrowRight",39],["ArrowDown",40],
    ["PrintScreen",44],["Insert",45],["Delete",46],["ContextMenu",93],
    ["NumpadMultiply",106],["NumpadAdd",107],["NumpadComma",108],["NumpadSubtract",109],["NumpadDecimal",110],["NumpadDivide",111],
    ["NumLock",144],["ScrollLock",145],["Semicolon",186],["Equal",187],["Comma",188],["Minus",189],["Period",190],["Slash",191],["Backquote",192],
    ["BracketLeft",219],["Backslash",220],["BracketRight",221],["Quote",222],["IntlBackslash",226]
  ]);
  for (let n=0;n<10;n++) exitHotkeyCodes.set('Numpad'+n,96+n);
  for (let n=1;n<=24;n++) exitHotkeyCodes.set('F'+n,111+n);
  for (const [label,key] of exitHotkeyCodes) if (!exitHotkeyKeys.has(key)) exitHotkeyKeys.set(key,label);
  for (let key=166;key<=183;key++) exitHotkeyKeys.set(key, '媒体/浏览器键 '+key);
  exitHotkeyKeys.set(223, 'OEM 8');
  const renderExitHotkey = () => {
    if (!exitHotkeyEnabled) return;
    exitHotkeyEnabled.setAttribute("aria-checked", String(savedExitHotkey.enabled));
    exitHotkeyEnabled.disabled = exitHotkeySaving;
    exitHotkeyOptions.hidden = !savedExitHotkey.enabled;
    exitHotkeyKey.value = exitHotkeyKeys.get(savedExitHotkey.key);
    exitHotkeyKey.dataset.key = String(savedExitHotkey.key);
    exitHotkeyCount.value = String(savedExitHotkey.count);
    exitHotkeyKey.disabled = exitHotkeyCount.disabled = exitHotkeySave.disabled = exitHotkeySaving;
  };
  const refreshExitHotkey = async (afterSave = false) => {
    if (!exitHotkeyEnabled || (exitHotkeySaving && !afterSave)) return;
    try {
      const packed = await bridge.getExitHotkey();
      if (!Number.isInteger(packed) || packed < 0 || packed > 0x1ffff) throw new Error("invalid exit shortcut");
      const value = { enabled: Boolean(packed & 65536), key: packed & 255, count: (packed >> 8) & 255 };
      if (!exitHotkeyKeys.has(value.key) || value.count < 1 || value.count > 10) throw new Error("invalid exit shortcut");
      savedExitHotkey = value;
      renderExitHotkey();
    } catch (error) { logCaughtPageError("refreshExitHotkey", error); }
  };
  const saveExitHotkey = async (enabled, key, count, showSaved = false) => {
    if (busy || exitHotkeySaving || currentState !== nativeState.authenticated) return;
    if (!exitHotkeyKeys.has(key) || !Number.isInteger(count) || count < 1 || count > 10) return;
    exitHotkeySaving = true; renderExitHotkey(); exitHotkeyStatus.textContent = ""; exitHotkeySaved.textContent = "";
    try {
      await bridge.setExitHotkey(enabled, key, count);
      savedExitHotkey = { enabled, key, count };
      exitHotkeySaved.textContent = showSaved ? "已保存" : "";
    } catch (error) {
      logCaughtPageError("saveExitHotkey", error);
      exitHotkeyStatus.textContent = "无法保存或启用退出快捷键，请重试。";
    } finally {
      await refreshExitHotkey(true); exitHotkeySaving = false; renderExitHotkey();
    }
  };
  if (exitHotkeyEnabled) {
    let captureReady = false;
    const capture = async active => {
      captureReady = false;
      try {
        await bridge.setExitHotkeyCapture(active);
        captureReady = active && document.activeElement === exitHotkeyKey && document.hasFocus();
      } catch { exitHotkeyStatus.textContent = active ? "暂时无法录入按键，请重新点击输入框。" : ""; }
    };
    exitHotkeyKey.addEventListener("focus", () => void capture(true));
    exitHotkeyKey.addEventListener("blur", () => void capture(false));
    window.addEventListener("blur", () => void capture(false));
    window.addEventListener("focus", () => { if (document.activeElement === exitHotkeyKey) void capture(true); });
    exitHotkeyKey.addEventListener("keydown", event => {
      if (event.key === "Tab" && event.shiftKey) return;
      event.preventDefault(); event.stopPropagation();
      if (!captureReady || event.repeat || event.isComposing || event.ctrlKey || event.altKey || event.shiftKey || event.metaKey) return;
      const key = exitHotkeyCodes.get(event.code) || (/^F([1-9]|1[0-9]|2[0-4])$/.test(event.key) ? 111 + Number(event.key.slice(1))
        : event.key === "Escape" ? 27 : event.key === " " ? 32
        : /^[a-z0-9]$/i.test(event.key) ? event.key.toUpperCase().charCodeAt(0) : exitHotkeyKeys.has(event.keyCode) ? event.keyCode : 0);
      if (!exitHotkeyKeys.has(key)) { exitHotkeyStatus.textContent = "请按单个普通键；Ctrl、Alt、Shift、Win 及组合键不支持。"; return; }
      exitHotkeyKey.dataset.key = String(key); exitHotkeyKey.value = exitHotkeyKeys.get(key);
      exitHotkeyStatus.textContent = ""; exitHotkeySaved.textContent = "";
    });
    for (let count = 1; count <= 10; count++) {
      const option = document.createElement("option"); option.value = String(count); option.textContent = `${count} 次`; exitHotkeyCount.append(option);
    }
    renderExitHotkey();
    for (const control of [exitHotkeyKey, exitHotkeyCount]) {
      control.addEventListener("change", () => { exitHotkeySaved.textContent = ""; });
    }
    exitHotkeyEnabled.addEventListener("click", () => void saveExitHotkey(!savedExitHotkey.enabled, savedExitHotkey.key, savedExitHotkey.count));
    exitHotkeySave.addEventListener("click", () => void saveExitHotkey(true, Number(exitHotkeyKey.dataset.key), Number(exitHotkeyCount.value), true));
  }

  const readContainerSnapshot = async (nextActiveVolumes) => {
    const nextContainers = [];
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
        const partition = await bridge.getContainerPartition(index);
        const conversion = await bridge.getContainerConversion(index);
        const unavailable = await bridge.getContainerVolumeUnavailable(index);
        const validId = Number.isInteger(id) && id > 0;
        const validName = typeof name === "string" && name.length !== 0;
        const validMounted = mounted === true || mounted === false;
        const validSize = Number.isInteger(sizeMib) && sizeMib >= 0 &&
          sizeMib <= 1048576;
        const validDrive = typeof drive === "string" &&
          ((mounted === true && /^[A-Z]:$/.test(drive) && nextActiveVolumes) ||
           (mounted !== true && drive === ""));
        if (!validId || !validName || !validMounted || !validSize ||
            !validDrive || typeof partition !== "boolean" || !Number.isInteger(conversion) || conversion < 0 || conversion > 2 || (conversion && (!partition || mounted)) ||
            typeof unavailable !== "boolean" || (unavailable && (!mounted || sizeMib !== 0))) {
          throw new Error("invalid container status");
        }
        nextContainers.push({ id, name: displayContainerName(name, partition), sourceName: name,
          mounted, sizeMib, drive, partition, conversion, unavailable });
      }

    return nextContainers;
  };

  const refreshNativeState = async () => {
    const revision = batchSessionRevision;
    const nextState = await bridge.getSessionState();
    const nextActiveVolumes = await bridge.hasActiveSession();
    if (!Number.isInteger(nextState) || nextState < nativeState.noCard ||
        nextState > nativeState.unavailable ||
        (nextActiveVolumes !== true && nextActiveVolumes !== false)) {
      throw Object.assign(new Error("invalid native session state"), { startupCode: 1404 });
    }
    const nextContainers = [];
    const nextOperationEvents = [];
    let nextResetAvailable = false;
    let nextForgotAvailable = false;
    if (nextState !== nativeState.authenticated) {
      nextForgotAvailable = await bridge.canLoginReset();
      if (typeof nextForgotAvailable !== "boolean") throw new Error("invalid reset availability");
    }
    if (nextState === nativeState.authenticated) {
      const resetAvailable = await bridge.canResetCardRecord();
      if (resetAvailable !== true && resetAvailable !== false) {
        throw new Error("invalid card reset availability");
      }
      nextResetAvailable = resetAvailable;
      await refreshStoredSwitches();
      nextContainers.push(...await readContainerSnapshot(nextActiveVolumes));

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
          { completedAtUnixMs, kind, outcome,
            name: displayContainerName(name, nextContainers.find(item => item.sourceName === name)?.partition), drive });
      }
    }
    if (revision !== batchSessionRevision) throw new Error("session changed during refresh");
    currentState = nextActiveVolumes &&
        nextState !== nativeState.authenticated &&
        nextState !== nativeState.unavailable
      ? nativeState.unavailable
      : nextState;
    activeVolumes = nextActiveVolumes;
    cardResetAvailable = nextResetAvailable;
    forgotAvailable = nextForgotAvailable;
    containers = nextContainers;
    for (const id of decoyMounted.keys()) {
      if (!containers.some(item => item.id === id && item.mounted)) decoyMounted.delete(id);
    }
    if (decoyProtectionTarget && !decoyMounted.has(decoyProtectionTarget)) clearDecoyMaintenanceFacts();
    containerOrder = nextContainers.map((container) => container.id);
    operationEvents = nextOperationEvents;
  };

  const startupFailureMessage = () => {
    const code = window.usbVaultStartupFailure?.code;
    const native = Number.isInteger(code) && code >= 1000 && code <= 9999;
    return mappedPageError(`错误码：${native ? code : 9001}`, native ? code : 9001, native ? "原生启动码" : "页面兜底码");
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
          ? "安全操作未完成，请尝试一键卸载。"
          : startupFailureMessage();
    }
  };

  let mountRefreshPending = false;
  let mountRefreshRunning = false;
  let mountRefreshTimer = null;
  let mountRefreshDisposed = false;
  const scheduleMountRefresh = () => {
    if (mountRefreshTimer !== null || mountRefreshDisposed || !mountRefreshPending) return;
    mountRefreshTimer = setTimeout(() => { mountRefreshTimer = null; void refreshMountInventory(); }, 200);
  };
  const refreshMountInventory = async () => {
    if (mountRefreshDisposed || !mountRefreshPending || mountRefreshRunning) return;
    if (busy || containerRename || reorderState !== null || exitHotkeySaving || externalRefreshPending || !bridge) { scheduleMountRefresh(); return; }
    if (currentState !== nativeState.authenticated) { mountRefreshPending = false; return; }
    const revision = batchSessionRevision;
    mountRefreshPending = false; mountRefreshRunning = true; busy = true;
    try {
      // A mount notice never changes authentication, settings drafts or result dialogs.
      if (await bridge.getSessionState() !== nativeState.authenticated) return;
      const next = await readContainerSnapshot(true);
      const active = await bridge.hasActiveSession();
      if (typeof active !== "boolean" || (next.some(item => item.mounted) && !active)) throw new Error("invalid container status");
      if (revision !== batchSessionRevision) return;
      const removed = new Set(containers.filter(item => item.mounted && !next.some(row => row.id === item.id && row.mounted)).map(item => item.id));
      containers = next; activeVolumes = active;
      containerOrder = next.map(item => item.id);
      for (const id of removed) decoyMounted.delete(id);
      for (const [key, entry] of exportSelection) if (removed.has(entry.id)) exportSelection.delete(key);
      if (removed.has(openExportContainer)) { openExportListing = null; openExportError = ""; }
      if (removed.has(fileContainerId)) {
        ++fileListingRevision; fileRefreshPendingRevision = 0;
        fileEntries = []; filePath = []; setFileStatus("");
        renderFileBreadcrumb(); renderFileList();
      }
      if (removed.has(decoyMaintenanceId) || removed.has(decoyProtectionTarget)) {
        clearDecoyMaintenanceFacts();
        if (removed.has(decoyMaintenanceId)) disarmDecoyDrop();
      }
    } catch {
      // Native workers can start between async bridge calls. Retry without
      // turning contention or a transient driver failure into a login error.
      if (revision === batchSessionRevision) mountRefreshPending = true;
    } finally {
      busy = false; mountRefreshRunning = false;
      if (revision === batchSessionRevision && currentState === nativeState.authenticated) {
        render(messageForCurrentState()); renderDecoySection();
      }
      if (externalRefreshPending) void refreshAfterExternalChange();
      scheduleMountRefresh();
    }
  };
  window.addEventListener("usb-vault-mounts-changed", () => {
    mountRefreshPending = true; void refreshMountInventory();
  });
  window.addEventListener("pagehide", () => {
    mountRefreshDisposed = true; clearTimeout(mountRefreshTimer); mountRefreshTimer = null;
  });

  const refreshAfterExternalChange = async () => {
    discardPartitions();
    closeSessionTools();
    ++forgotRevision;
    if (!forgotDialog.hidden && !forgotRunning) await closeForgotPassword();
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
        renderDecoySection();
        updateStrength();
        focusCurrentInput();
      } catch (pageError) { logCaughtPageError("refreshAfterExternalChange/1", pageError);
        currentState = nativeState.unavailable;
        activeVolumes = true;
        busy = false;
        render("无法确认安全会话状态，请尝试一键卸载。");
      }
    } while (externalRefreshPending);
    if (!volumeOperationDialog.hidden) {
      volumeForceContext = null;
      volumeOperationForce.hidden = true;
      if (volumeOperationDialog.dataset.result === "running") volumeOperationDialog.focus();
      else volumeOperationConfirm.focus();
    }
  };

  // The window raises this when a drag it accepted starts or finishes. The
  // page owns none of that work, so all it does is start reading the numbers.
  window.addEventListener("usb-vault-external-drop-changed", () => {
    void watchDecoyDrop();
  });

  window.addEventListener("usb-vault-session-changed", () => {
    mountRefreshPending = false; clearTimeout(mountRefreshTimer); mountRefreshTimer = null;
    ++batchSessionRevision;
    clearExportResults();
    exportSelection.clear();
    exportLocation = "";
    cancelContainerRename();
    // A walk's findings belong to the session that made them.
    resetDecoySection();
    pendingNewContainers.clear();
    batchForceUnmount.checked = false;
    batchForceUnmount.disabled = true;
    clearBatchResults();
    closeBatchMenus();
    for (const control of batchControls) {
      control.selected.clear();
      control.items.replaceChildren();
      control.button.disabled = true;
      control.arrow.disabled = true;
    }
    if (!volumeOperationDialog.hidden) {
      volumeOperationContext = null;
      volumeForceContext = null;
      volumeRetryContext = null;
      volumeOperationForce.hidden = true;
      volumeOperationRetry.hidden = true;
      volumeOperationTarget = "";
      if (volumeOperationDialog.dataset.result === "running") volumeOperationTitle.textContent = "正在处理磁盘操作…";
      else finishVolumeOperation(false, "安全卡状态已变化，请检查当前磁盘状态。");
    }
    void refreshAfterExternalChange();
  });

  const submitLogin = async () => {
    const loginRevision = batchSessionRevision;
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
      let autoMount = false;
      if (currentState === nativeState.authenticated && loginRevision === batchSessionRevision && !externalRefreshPending) {
        try { autoMount = (await bridge.getAutoMountEnabled()) === true; }
        catch (pageError) { logCaughtPageError("submitLogin/1", pageError); setDashboardStatus("无法读取自动挂载设置，本次请手动挂载。", "error"); }
      }
      busy = false;
      render(messageForCurrentState());
      if (autoMount && loginRevision === batchSessionRevision && !externalRefreshPending) {
        const targets = orderedContainers().filter((container) => !container.mounted && !container.conversion).map((container) => container.id);
        await runSelectedBatch(batchControls[0], null, targets);
      }
    } catch (pageError) { logCaughtPageError("submitLogin/2", pageError);
      try {
        await refreshNativeState();
      } catch (pageError) { logCaughtPageError("submitLogin/3", pageError);
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
      if (volumeOperationDialog.hidden && batchResultDialog.hidden) focusCurrentInput();
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
    } catch (pageError) { logCaughtPageError("submitInitialization/1", pageError);
      try {
        await refreshNativeState();
      } catch (pageError) { logCaughtPageError("submitInitialization/2", pageError);
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

  const volumeOperationDialog = document.getElementById("volume-operation-dialog");
  const volumeOperationTitle = document.getElementById("volume-operation-title");
  const volumeOperationNote = document.getElementById("volume-operation-note");
  const volumeOperationMark = document.getElementById("volume-operation-mark");
  const volumeOperationConfirm = document.getElementById("volume-operation-confirm");
  const volumeOperationForce = document.getElementById("volume-operation-force");
  const volumeOperationRetry = document.getElementById("volume-operation-retry");
  let volumeForceContext = null;
  let volumeRetryContext = null;
  let volumeOperationContext = null;
  let volumeOperationFocus = null;
  let volumeOperationTimer = null;
  let volumeOperationVerb = "";
  let volumeOperationTarget = "";
  const clearVolumeOperationTimer = () => {
    if (volumeOperationTimer !== null) clearTimeout(volumeOperationTimer);
    volumeOperationTimer = null;
  };
  const showVolumeOperation = (context) => {
    const { verb, target } = context;
    volumeOperationContext = context;
    volumeForceContext = null;
    volumeRetryContext = null;
    volumeOperationForce.hidden = true;
    volumeOperationRetry.hidden = true;
    volumeOperationForce.textContent = "强制卸载";
    volumeOperationConfirm.textContent = "确定";
    clearVolumeOperationTimer();
    volumeOperationFocus = document.activeElement;
    volumeOperationVerb = verb;
    volumeOperationTarget = target;
    // The volume name is set off by spaces so it does not run into the
    // surrounding Chinese, which matters most when the name is Latin or mixed.
    volumeOperationTitle.textContent = `正在${verb} ${target} …`;
    volumeOperationNote.textContent = "请稍候，完成后将自动更新磁盘状态。";
    volumeOperationDialog.dataset.result = "running";
    volumeOperationMark.textContent = "";
    volumeOperationMark.setAttribute("role", "progressbar");
    volumeOperationMark.removeAttribute("aria-hidden");
    volumeOperationConfirm.hidden = true;
    volumeOperationDialog.hidden = false;
    volumeOperationDialog.focus();
  };
  const hideVolumeOperation = () => {
    volumeForceContext = null;
    volumeRetryContext = null;
    volumeOperationContext = null;
    volumeOperationForce.hidden = true;
    volumeOperationRetry.hidden = true;
    clearVolumeOperationTimer();
    volumeOperationDialog.hidden = true;
    volumeOperationTitle.textContent = "";
    volumeOperationNote.textContent = "";
    volumeOperationTarget = "";
    if (volumeOperationFocus?.isConnected && !volumeOperationFocus.disabled) volumeOperationFocus.focus();
    else if (currentState === nativeState.authenticated) dashboardNavigation[0]?.focus();
    else focusCurrentInput();
    volumeOperationFocus = null;
  };
  const finishVolumeOperation = (succeeded, reason = "", failureCode = 0) => {
    clearVolumeOperationTimer();
    if (succeeded && externalRefreshPending) {
      succeeded = false;
      reason = "安全卡状态已变化，无法确认操作成功。请检查当前磁盘状态。";
    }
    // Names are existing session display facts; never retain them after logout.
    if (currentState !== nativeState.authenticated || externalRefreshPending) volumeOperationTarget = "";
    volumeOperationDialog.dataset.result = succeeded ? "success" : "error";
    volumeOperationTitle.textContent = `${volumeOperationVerb}${succeeded ? "成功" : "失败"}`;
    volumeOperationNote.textContent = succeeded
      ? (volumeOperationTarget ? ` ${volumeOperationTarget} 已${volumeOperationVerb}。` : "磁盘状态已更新。")
      : reason;
    if (!succeeded && Number.isInteger(failureCode) && failureCode > 0)
      mappedPageError(`${volumeOperationTitle.textContent}；${volumeOperationNote.textContent}`, failureCode, "原生状态码");
    volumeOperationMark.removeAttribute("role");
    volumeOperationMark.setAttribute("aria-hidden", "true");
    // The tick and the exclamation are drawn by the stylesheet from
    // data-result, so the mark carries no text of its own in any state.
    volumeOperationConfirm.hidden = false;
    volumeOperationConfirm.textContent = "确定";
    volumeOperationForce.textContent = "强制卸载";
    volumeForceContext = !succeeded && failureCode === 4 && !externalRefreshPending &&
      volumeOperationContext?.verb === "卸载" &&
      (volumeOperationContext.all ? activeVolumes : currentState === nativeState.authenticated &&
        containers.some((container) => container.id === volumeOperationContext.id && container.mounted))
      ? { ...volumeOperationContext } : null;
    volumeOperationForce.hidden = volumeForceContext === null;
    // Retrying a mount is not destructive, so any failure code may offer it;
    // the container only has to still be known to this session and unmounted.
    volumeRetryContext = !succeeded && !externalRefreshPending &&
      volumeOperationContext?.verb === "挂载" && currentState === nativeState.authenticated &&
      containers.some((container) => container.id === volumeOperationContext.id && !container.mounted)
      ? { ...volumeOperationContext } : null;
    volumeOperationRetry.hidden = volumeRetryContext === null;
    volumeOperationConfirm.focus();
    if (succeeded) volumeOperationTimer = setTimeout(hideVolumeOperation, 3000);
  };
  volumeOperationConfirm.addEventListener("click", () => {
    if (volumeOperationDialog.dataset.result !== "running") hideVolumeOperation();
  });
  volumeOperationForce.addEventListener("click", async () => {
    if (busy || externalRefreshPending || volumeForceContext === null) return;
    if (volumeOperationDialog.dataset.result !== "confirm-force") {
      clearVolumeOperationTimer();
      volumeOperationDialog.dataset.result = "confirm-force";
      volumeOperationTitle.textContent = `强制卸载 ${volumeForceContext.target} ？`;
      volumeOperationNote.textContent = "未保存或尚未写入磁盘的数据可能丢失，文件或文件系统可能损坏，仍要强制卸载吗？";
      volumeOperationConfirm.textContent = "取消";
      volumeOperationForce.textContent = "确认强制卸载";
      volumeOperationConfirm.focus();
      return;
    }
    const context = { ...volumeForceContext, verb: "强制卸载" };
    hideVolumeOperation();
    await runContainerAction(() => bridge.beginForceUnmountLast(),
      "强制卸载未完成，请检查磁盘状态。", context);
  });
  volumeOperationRetry.addEventListener("click", async () => {
    if (busy || externalRefreshPending || volumeRetryContext === null) return;
    // hideVolumeOperation first: runContainerAction refuses to start while the
    // dialog is still up, and it opens its own fresh progress state.
    const context = { ...volumeRetryContext };
    hideVolumeOperation();
    await runContainerAction(() => bridge.beginMountAdoptedContainer(context.id),
      "无法挂载该安全盘，请稍后重试。", context);
  });
  // A finished result is safe to dismiss by clicking away; a running operation is not.
  volumeOperationDialog.addEventListener("pointerdown", (event) => {
    if (event.button === 0 && event.target === volumeOperationDialog && volumeOperationDialog.dataset.result !== "running") hideVolumeOperation();
  });
  volumeOperationDialog.addEventListener("keydown", (event) => {
    if (event.key === "Tab" || event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      if (volumeOperationDialog.dataset.result === "running") volumeOperationDialog.focus();
      else if (event.key === "Escape") hideVolumeOperation();
      else if (document.activeElement === volumeOperationConfirm && !volumeOperationForce.hidden) volumeOperationForce.focus();
      else if (document.activeElement === volumeOperationConfirm && !volumeOperationRetry.hidden) volumeOperationRetry.focus();
      else volumeOperationConfirm.focus();
    }
  });
  class VolumeOperationFailure extends Error {
    constructor(message, code = 0) { super(message); this.code = code; }
  }
  const volumeFailureMessages = {
    3: "操作未完成，暂时无法确定具体原因。请检查磁盘状态后重试。",
    4: "卷正在被其他程序占用或锁定。请关闭使用该卷的文件、窗口或程序后重试。",
    5: "找不到安全盘文件或所在位置。请确认文件未被移动，且存储设备已连接。",
    6: "访问被拒绝。请检查安全盘文件和所在位置的访问权限。",
    7: "无法打开安全盘：当前安全卡或登录信息不匹配，或安全盘数据无法识别。",
    8: "读取或写入磁盘失败。请检查存储设备的连接及可用状态。",
    9: "加密驱动或目标磁盘不可用，或驱动版本不兼容。请检查安全组件和磁盘状态。",
    10: "操作期间安全卡已拔出，无法确认操作成功。请重新插卡并检查磁盘状态。",
  };
  const batchResultDialog = document.getElementById("batch-result-dialog");
  const batchResultTitle = document.getElementById("batch-result-title");
  const batchResultList = document.getElementById("batch-result-list");
  const batchResultAll = document.getElementById("batch-result-all");
  const batchResultRetry = document.getElementById("batch-result-retry");
  const batchResultConfirm = document.getElementById("batch-result-confirm");
  const batchResultForce = document.getElementById("batch-result-force");
  const batchResultSettingStatus = document.getElementById("batch-result-setting-status");
  let batchResultContext = null;
  const batchItemFailed = (item) => item.outcome !== 2 && item.outcome !== 11;
  const retryableBatchItems = (context) => context.items.filter((item) =>
    batchItemFailed(item) && containers.some((container) =>
      container.id === item.id && container.mounted !== context.mounting && !container.conversion && !isMaintenanceContainer(container.id)));
  const clearBatchResults = () => {
    if (batchResultContext) {
      batchResultContext.items.length = 0;
      batchResultContext.selected.clear();
    }
    batchResultContext = null;
    batchResultSettingStatus.textContent = "";
    batchResultSettingStatus.hidden = true;
    batchResultDialog.hidden = true;
    batchResultTitle.textContent = "";
    batchResultList.replaceChildren();
    batchResultAll.checked = false;
    batchResultAll.indeterminate = false;
    batchResultAll.disabled = true;
    batchResultRetry.disabled = true;
    batchResultForce.checked = false;
    batchResultForce.disabled = true;
    batchResultForce.closest("label").hidden = true;
  };
  const dismissBatchResults = () => {
    if (busy || batchResultDialog.hidden) return;
    const focus = batchResultContext?.focus;
    clearBatchResults();
    if (focus?.isConnected && !focus.disabled) focus.focus();
    else dashboardNavigation[0]?.focus();
  };
  const updateBatchResultSelection = () => {
    const context = batchResultContext;
    if (!context) return;
    const eligible = retryableBatchItems(context);
    for (const id of context.selected) {
      if (!eligible.some((item) => item.id === id)) context.selected.delete(id);
    }
    batchResultAll.checked = eligible.length > 0 && context.selected.size === eligible.length;
    batchResultAll.indeterminate = context.selected.size > 0 && context.selected.size < eligible.length;
    batchResultAll.disabled = busy || !eligible.length;
    batchResultAll.closest("label").hidden = !eligible.length;
    batchResultRetry.hidden = !eligible.length;
    batchResultRetry.disabled = busy || !context.selected.size;
    batchResultForce.closest("label").hidden = context.mounting || !eligible.length;
    batchResultForce.disabled = busy || context.mounting || !eligible.length;
    for (const input of batchResultList.querySelectorAll("input:not(:disabled)")) {
      input.checked = context.selected.has(Number(input.dataset.id));
    }
  };
  // Every row gets the same slot so the retry boxes stay in one column:
  // a tick when it worked, an exclamation that owns the reason bubble when
  // it did not, and nothing at all for a container already at its target.
  const batchResultMark = (item, verb, reason) => {
    const slot = document.createElement("span");
    slot.className = "batch-result-mark";
    const note = reason || (item.outcome === 2 ? `${verb}成功` : "");
    if (!note) return slot;
    const mark = document.createElement("button");
    mark.type = "button";
    mark.className = `batch-result-sign ${reason ? "batch-result-why" : "batch-result-ok"}`;
    mark.setAttribute("aria-expanded", "false");
    mark.setAttribute("aria-label", note);
    const bubble = document.createElement("span");
    bubble.className = "batch-result-bubble";
    bubble.setAttribute("role", "tooltip");
    bubble.textContent = note;
    mark.addEventListener("pointerenter", () => openBatchReason(mark));
    mark.addEventListener("focus", () => openBatchReason(mark));
    mark.addEventListener("pointerleave", () => closeBatchReason(mark));
    mark.addEventListener("blur", () => closeBatchReason(mark));
    slot.append(mark, bubble);
    return slot;
  };
  // Fixed placement needs real coordinates: sit above the mark when there is
  // room, otherwise below it, and never past either edge of the window.
  const openBatchReason = (mark) => {
    const bubble = mark.nextElementSibling;
    bubble.dataset.open = "true";
    const anchor = mark.getBoundingClientRect();
    const box = bubble.getBoundingClientRect();
    const margin = 12;
    const limit = Math.max(margin, window.innerWidth - box.width - margin);
    const above = anchor.top - box.height - 6;
    bubble.style.left = `${Math.max(margin, Math.min(anchor.right - box.width, limit))}px`;
    bubble.style.top = `${above < margin ? anchor.bottom + 6 : above}px`;
  };
  const closeBatchReason = (mark) => {
    if (mark.getAttribute("aria-expanded") !== "true") delete mark.nextElementSibling.dataset.open;
  };
  const unpinBatchReasons = (except = null) => {
    for (const mark of batchResultList.querySelectorAll(".batch-result-sign")) {
      if (mark === except) continue;
      mark.setAttribute("aria-expanded", "false");
      if (!mark.matches(":hover")) delete mark.nextElementSibling.dataset.open;
    }
  };
  // A pinned bubble would float away from its row once the list moves.
  batchResultList.addEventListener("scroll", () => unpinBatchReasons());
  const showBatchResults = (context) => {
    batchResultContext = context;
    context.selected.clear();
    batchResultForce.checked = Boolean(context.force);
    const verb = context.mounting ? "挂载" : context.force ? "强制卸载" : "卸载";
    const heading = document.createElement("span");
    heading.textContent = `${verb}结果`;
    const counts = document.createElement("span");
    counts.className = "batch-result-counts";
    counts.textContent = `成功：${context.items.filter((item) => item.outcome === 2).length}` +
      `　失败：${context.items.filter(batchItemFailed).length}`;
    batchResultTitle.replaceChildren(heading, counts);
    batchResultRetry.textContent = context.mounting ? "重试挂载选中项" : "重试卸载选中项";
    batchResultList.replaceChildren();
    const eligible = new Set(retryableBatchItems(context).map((item) => item.id));
    // Stable sort preserves the original batch order within each outcome group.
    for (const item of [...context.items].sort((a, b) => Number(batchItemFailed(b)) - Number(batchItemFailed(a)))) {
      const row = document.createElement(eligible.has(item.id) ? "label" : "div");
      row.className = "batch-result-row";
      row.dataset.id = String(item.id);
      row.dataset.failed = String(batchItemFailed(item));
      const input = document.createElement("input");
      input.type = "checkbox";
      input.dataset.id = String(item.id);
      input.id = `batch-retry-${item.id}`;
      if (eligible.has(item.id)) row.htmlFor = input.id;
      input.disabled = !eligible.has(item.id);
      input.checked = !batchItemFailed(item);
      const name = document.createElement("span");
      name.className = "batch-result-name";
      name.textContent = item.name;
      name.title = item.name;
      const failure = batchItemFailed(item)
        ? `${verb}失败：${volumeFailureMessages[item.outcome] || volumeFailureMessages[3]}` +
          (eligible.has(item.id) ? "" : "当前状态无需重试，或安全盘已不在本会话中。")
        : "";
      row.append(name, batchResultMark(item, verb, failure));
      if (failure) logShownPageError(`batch-result/${item.id}`, `${item.name}；${failure}`,
        `原生状态码 ${item.outcome} → ${failure}`);
      if (eligible.has(item.id)) row.append(input);
      batchResultList.append(row);
    }
    updateBatchResultSelection();
    batchResultDialog.hidden = false;
    batchResultConfirm.focus();
  };
  batchResultAll.addEventListener("change", () => {
    if (busy || !batchResultContext || externalRefreshPending) return;
    batchResultContext.selected.clear();
    if (batchResultAll.checked) for (const item of retryableBatchItems(batchResultContext)) {
      batchResultContext.selected.add(item.id);
    }
    updateBatchResultSelection();
  });
  const saveForcePreference = async (source) => {
    if (busy || externalRefreshPending || currentState !== nativeState.authenticated) return;
    const revision = batchSessionRevision;
    const context = batchResultContext;
    const previous = savedForceUnmount;
    const requested = source.checked;
    batchResultSettingStatus.textContent = "";
    batchResultSettingStatus.hidden = true;
    busy = true;
    batchForceUnmount.disabled = true;
    batchResultForce.disabled = true;
    if (context) updateBatchResultSelection();
    try {
      await bridge.setForceUnmountEnabled(requested);
      const value = await bridge.getForceUnmountEnabled();
      if (typeof value !== "boolean") throw new Error("invalid preference");
      savedForceUnmount = value;
      if (revision === batchSessionRevision) {
        batchForceUnmount.checked = value;
        if (context && batchResultContext === context) {
          context.force = value;
          batchResultForce.checked = value;
        }
      }
    } catch (pageError) { logCaughtPageError("saveForcePreference/1", pageError);
      if (revision === batchSessionRevision) {
        source.checked = previous;
        setDashboardStatus("无法保存强制卸载设置，请重试。", "error");
        if (context && batchResultContext === context && !batchResultDialog.hidden) {
          batchResultSettingStatus.textContent = "无法保存强制卸载设置，请重试。";
          batchResultSettingStatus.hidden = false;
        }
      }
    } finally {
      busy = false;
      renderBatchControls();
      if (batchResultContext) updateBatchResultSelection();
      if (externalRefreshPending) void refreshAfterExternalChange();
    }
  };
  batchForceUnmount.addEventListener("change", () => { void saveForcePreference(batchForceUnmount); });
  batchResultForce.addEventListener("change", () => { void saveForcePreference(batchResultForce); });
  batchResultList.addEventListener("change", (event) => {
    const input = event.target;
    if (busy || !batchResultContext || externalRefreshPending || input.disabled || !input.matches("input[data-id]")) return;
    const id = Number(input.dataset.id);
    if (input.checked) batchResultContext.selected.add(id);
    else batchResultContext.selected.delete(id);
    updateBatchResultSelection();
  });
  batchResultRetry.addEventListener("click", () => {
    if (busy || !batchResultContext || batchResultDialog.hidden || externalRefreshPending) return;
    void runSelectedBatch(batchControls[batchResultContext.mounting ? 0 : 1], batchResultContext);
  });
  batchResultConfirm.addEventListener("click", dismissBatchResults);
  batchResultDialog.addEventListener("click", (event) => {
    const mark = event.target.closest?.(".batch-result-sign");
    if (mark) {
      // A button inside a label already suppresses the label's activation;
      // cancel it outright so reading a reason can never flip its retry box.
      event.preventDefault();
      const pinned = mark.getAttribute("aria-expanded") === "true";
      unpinBatchReasons(mark);
      mark.setAttribute("aria-expanded", pinned ? "false" : "true");
      if (pinned) closeBatchReason(mark);
      else openBatchReason(mark);
      return;
    }
    unpinBatchReasons();

  });
  batchResultDialog.addEventListener("pointerdown", (event) => {
    if (event.button === 0 && event.target === batchResultDialog) dismissBatchResults();
  });
  batchResultDialog.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.preventDefault(); event.stopPropagation();
      if (batchResultList.querySelector('.batch-result-sign[aria-expanded="true"]')) unpinBatchReasons();
      else dismissBatchResults();
    } else if (event.key === "Tab") {
      const controls = [...batchResultDialog.querySelectorAll("input:not(:disabled), button:not(:disabled)")]
        .filter((control) => !control.closest("[hidden]"));
      const index = controls.indexOf(document.activeElement);
      event.preventDefault(); event.stopPropagation();
      controls[(index + (event.shiftKey ? -1 : 1) + controls.length) % controls.length]?.focus();
    }
  });
  const waitForVolumeOperation = async (start) => {
    // Give the overlay a paint before dispatching native work.
    await new Promise((resolve) => requestAnimationFrame(() => setTimeout(resolve, 0)));
    try { await start(); }
    catch (pageError) { logCaughtPageError("waitForVolumeOperation/1", pageError); throw new VolumeOperationFailure("操作未能开始。请检查安全卡及磁盘状态后重试。"); }
    for (;;) {
      let status;
      try { status = await bridge.getVolumeOperationStatus(); }
      catch (pageError) { logCaughtPageError("waitForVolumeOperation/2", pageError, 4); throw new VolumeOperationFailure("无法获取操作结果，请检查当前磁盘状态后重试。"); }
      if (status === 2) return;
      if (!Number.isInteger(status) || status < 1 || status > 10)
        logCaughtPageError("waitForVolumeOperation/status", new Error("invalid volume operation status"));
      if (status !== 1) throw new VolumeOperationFailure(
        Number.isInteger(status) && status >= 3 && status <= 10 ? volumeFailureMessages[status] : volumeFailureMessages[3], status);
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  };

  const leaveApplication = async (toLogin) => {
    if (busy || bridge === null || currentState !== nativeState.authenticated) return;
    closeAppExit();
    if (!toLogin) {
      try { await bridge.exitApplication(); }
      catch (error) { logCaughtPageError("leaveApplication/exit", error); setDashboardStatus("未能退出，请重试。"); }
      return;
    }
    busy = true;
    disarmDecoyDrop();
    render(toLogin ? "正在返回登录页" : "正在退出应用");
    setDashboardStatus("正在安全卸载当前会话的磁盘…");
    try {
      await waitForVolumeOperation(() => bridge.beginUnmount());
      if (toLogin) await bridge.returnToLogin();
      else await bridge.exitApplication();
      // A login transition replaces this document and retires the old bridge.
      if (toLogin) return;
      busy = false;
      render(messageForCurrentState());
      setDashboardStatus("");
    } catch (pageError) { logCaughtPageError("leaveApplication/1", pageError);
      try { await refreshNativeState(); } catch (pageError) { logCaughtPageError("leaveApplication/2", pageError);}
      busy = false;
      render(messageForCurrentState());
      setDashboardStatus("未能离开，请等待当前操作结束，并关闭占用磁盘的文件后重试。", "error");
      if (externalRefreshPending) void refreshAfterExternalChange();
    }
  };
  returnToLogin.addEventListener("click", () => { void leaveApplication(true); });
  exitApplication.addEventListener("click", () => { void leaveApplication(false); });

  const unmount = async () => {
    if (busy || bridge === null || !volumeOperationDialog.hidden || !batchResultDialog.hidden) return;
    busy = true;
    render("正在一键卸载");
    setDashboardStatus("");
    const mountedNames = containers.filter((container) => container.mounted).map((container) => container.name);
    showVolumeOperation({ verb: "卸载", all: true, target: mountedNames.length === 1 ? mountedNames[0] : "全部已挂载卷" });
    try {
      await waitForVolumeOperation(() => bridge.beginUnmount());
      await refreshNativeState();
      busy = false;
      render(messageForCurrentState());
      setDashboardStatus("一键卸载已完成。");
      finishVolumeOperation(true);
    } catch (error) { logCaughtPageError("unmount/1", error);
      try {
        await refreshNativeState();
      } catch (pageError) { logCaughtPageError("unmount/2", pageError);
        currentState = nativeState.unavailable;
        activeVolumes = true;
      }
      busy = false;
      render(activeVolumes
        ? "无法一键卸载，请检查磁盘状态，并关闭可能占用文件的程序后重试。"
        : messageForCurrentState());
      if (activeVolumes) setDashboardStatus("无法一键卸载，请检查磁盘状态，并关闭可能占用文件的程序后重试。", "error");
      finishVolumeOperation(false, error instanceof VolumeOperationFailure ? error.message : "无法确认最新磁盘状态，请刷新或重新插卡后检查。", error instanceof VolumeOperationFailure ? error.code : 0);
    } finally {
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    }
  };

  // One shared shape for the dashboard-side container actions: serialize on
  // the busy flag, refresh from the native side, and report failure on the
  // dashboard instead of the hidden session surface.
  const runContainerAction = async (action, failure, progress = null, newArrival = false) => {
    if (busy || bridge === null || !volumeOperationDialog.hidden || !batchResultDialog.hidden) return;
    busy = true;
    const previousIds = new Set(containers.map((container) => container.id));
    const revision = batchSessionRevision;
    setDashboardStatus("");
    render(messageForCurrentState());
    if (progress) showVolumeOperation(progress);
    try {
      const outcome = progress ? await waitForVolumeOperation(action) : await action();
      await refreshNativeState();
      if (newArrival && revision === batchSessionRevision && !externalRefreshPending) markNewContainers(previousIds);
      busy = false;
      render(messageForCurrentState());
      if (activeSection === "files") void refreshFileManager();
      if (outcome === false) setDashboardStatus("已取消。");
      else if (progress) {
        setDashboardStatus(`${progress.verb}成功。`);
        finishVolumeOperation(true);
      }
    } catch (error) { logCaughtPageError("runContainerAction/1", error);
      try {
        await refreshNativeState();
      } catch (pageError) { logCaughtPageError("runContainerAction/2", pageError);
        currentState = nativeState.unavailable;
        activeVolumes = true;
      }
      busy = false;
      render(messageForCurrentState());
      if (activeSection === "files") void refreshFileManager();
      setDashboardStatus(failure, "error");
      if (progress) finishVolumeOperation(false, error instanceof VolumeOperationFailure ? error.message : "无法确认最新磁盘状态，请刷新或重新插卡后检查。", error instanceof VolumeOperationFailure ? error.code : 0);
    } finally {
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    }
  };

  const mountExistingContainer = () => runContainerAction(
    () => bridge.mountContainer(),
    "无法导入该安全盘，请确认文件仍在原位置且与当前安全卡匹配。", null, true);

  const mountAdoptedContainer = (id) => runContainerAction(
    () => bridge.beginMountAdoptedContainer(id),
    "无法挂载该安全盘，请稍后重试。", { verb: "挂载", id, target: containers.find((container) => container.id === id)?.name || "当前卷" });

  const unmountAdoptedContainer = (id) => runContainerAction(
    () => bridge.beginUnmountContainer(id),
    "无法卸载该安全盘，请检查磁盘状态，并关闭可能占用文件的程序后重试。", { verb: "卸载", id, target: containers.find((container) => container.id === id)?.name || "当前卷" });

  const forgetAdoptedContainer = (id) => runContainerAction(
    () => bridge.forgetContainer(id),
    "无法从列表移除该安全盘，请稍后重试。");

  const runSelectedBatch = async (control, retryContext = null, automaticIds = null) => {
    if (busy || bridge === null || currentState !== nativeState.authenticated ||
        externalRefreshPending || !volumeOperationDialog.hidden) return;
    if (retryContext ? retryContext !== batchResultContext : !batchResultDialog.hidden) return;
    if (retryContext) updateBatchResultSelection();
    else updateBatchSelection(control);
    const ids = (automaticIds || [...(retryContext ? retryContext.selected : control.selected)])
      .filter((id) => !isMaintenanceContainer(id));
    if (!ids.length) return;
    const revision = batchSessionRevision;
    const checkSession = () => {
      if (externalRefreshPending || revision !== batchSessionRevision) throw new Error("session changed");
    };
    const originFocus = retryContext?.focus || document.activeElement;
    const force = !control.mounting && (retryContext ? retryContext.force : batchForceUnmount.checked);
    const verb = control.mounting ? "挂载" : force ? "强制卸载" : "卸载";
    busy = true;
    batchResultDialog.hidden = true;
    batchResultList.replaceChildren();
    batchResultRetry.disabled = true;
    closeBatchMenus();
    render(messageForCurrentState());
    setDashboardStatus("");
    showVolumeOperation({ verb, target: automaticIds ? "全部可用安全盘" : "所选安全盘", batch: true });
    try {
      await new Promise((resolve) => requestAnimationFrame(() => setTimeout(resolve, 0)));
      checkSession();
      if (control.mounting) await bridge.beginMountBatch(ids.join(","));
      else if (force) await bridge.beginForceUnmountBatch(ids.join(","));
      else await bridge.beginUnmountBatch(ids.join(","));
      for (;;) {
        const raw = String(await bridge.getBatchProgress());
        checkSession();
        if (!/^\d+,\d+,\d+,\d+,\d+,\d+$/.test(raw)) throw new Error("invalid progress");
        const [state, kind, total, completed, current, id] = raw.split(",").map(Number);
        if (kind !== (control.mounting ? 0 : 1) || total !== ids.length ||
            completed > total || current > total || (current && ids[current - 1] !== id)) throw new Error("invalid progress");
        if (state === 2) break;
        if (state !== 1) throw new Error("batch failed");
        if (current) {
          const name = await bridge.getBatchItemName(current - 1);
          checkSession();
          volumeOperationTitle.textContent = `正在${verb} ${name} (${current}/${total})`;
        } else volumeOperationTitle.textContent = `正在${verb}所选安全盘 (${completed}/${total})`;
        await new Promise((resolve) => setTimeout(resolve, 100));
      }
      const results = [];
      for (let index = 0; index < ids.length; ++index) {
        const outcome = await bridge.getBatchItemOutcome(index);
        checkSession();
        const id = await bridge.getBatchItemId(index);
        checkSession();
        if (id !== ids[index] || !Number.isInteger(outcome) || outcome < 2 || outcome > 11) throw new Error("invalid result");
        const name = await bridge.getBatchItemName(index);
        checkSession();
        results.push({ id, name, outcome });
      }
      await refreshNativeState();
      checkSession();
      if (currentState !== nativeState.authenticated) throw new Error("session ended");
      control.selected.clear();
      busy = false;
      render(messageForCurrentState());
      hideVolumeOperation();
      // Retry replaces only the attempted entries, retaining the whole batch.
      const items = retryContext ? retryContext.items.map((item) =>
        results.find((result) => result.id === item.id) || item) : results;
      showBatchResults({ mounting: control.mounting, force, items, selected: new Set(), focus: originFocus });
      if (activeSection === "files") void refreshFileManager();
    } catch (pageError) { logCaughtPageError("runSelectedBatch/1", pageError);
      clearBatchResults();
      try { await refreshNativeState(); }
      catch (pageError) { logCaughtPageError("runSelectedBatch/2", pageError); currentState = nativeState.unavailable; activeVolumes = true; }
      busy = false;
      render(messageForCurrentState());
      finishVolumeOperation(false, externalRefreshPending || revision !== batchSessionRevision
        ? "安全卡状态已变化，请检查当前磁盘状态。"
        : "无法确认批量操作结果，请检查当前磁盘状态后重试。");
    } finally {
      if (externalRefreshPending) void refreshAfterExternalChange();
    }
  };

  const updateForgotControls = () => {
    for (const control of [forgotClose, forgotCancel, forgotAcknowledge, forgotForce, forgotSkip]) control.disabled = forgotRunning;
    forgotConfirm.disabled = forgotRunning || !forgotAcknowledge.checked;
  };
  const closeForgotPassword = async () => {
    if (forgotRunning) return;
    const ticket = forgotTicket;
    forgotTicket = 0; ++forgotRevision;
    forgotDialog.hidden = true; forgotAcknowledge.checked = false;
    if (ticket) { try { await bridge.cancelLoginReset(ticket); } catch (pageError) { logCaughtPageError("closeForgotPassword/1", pageError);} }
    busy = false; render(messageForCurrentState());
    if (!sessionTools.hidden) sessionToolsToggle.focus();
  };
  const pollForgotPassword = async revision => {
    for (;;) {
      const status = await bridge.getLoginResetStatus();
      if (!Number.isInteger(status) || status < 0 || status > 6) throw new Error("invalid reset status");
      if (status !== 1) {
        if (revision !== forgotRevision) throw new Error("card changed");
        return status;
      }
      await new Promise(resolve => setTimeout(resolve, 100));
    }
  };
  const runForgotPassword = async (prepare, force = false) => {
    if (forgotRunning || forgotDialog.hidden || (prepare && !forgotAcknowledge.checked) || (!prepare && !forgotTicket)) return;
    const revision = forgotRevision;
    forgotRunning = true; updateForgotControls();
    forgotStatus.dataset.tone = "";
    forgotStatus.textContent = prepare ? "正在核验安全卡，请勿拔卡。" : force ? "正在卸载安全盘并重置安全卡，请勿拔卡。" : "正在重置安全卡，请勿拔卡。";
    let writing = !prepare;
    try {
      if (prepare) {
        await bridge.beginLoginResetPreparation();
        if (await pollForgotPassword(revision) !== 2) throw new Error("preparation failed");
        forgotTicket = await bridge.getLoginResetTicket();
        const known = await bridge.getLoginResetMountsKnown();
        const drives = await bridge.getLoginResetDrives();
        if (!Number.isInteger(forgotTicket) || forgotTicket <= 0 || typeof known !== "boolean" ||
            typeof drives !== "string" || (drives !== "" && !/^[A-Z]:(, [A-Z]:)*$/.test(drives))) throw new Error("invalid reset preparation");
        if (revision !== forgotRevision) throw new Error("card changed");
        if (!known || drives !== "") {
          forgotIntro.hidden = true; forgotMounts.hidden = false; forgotConfirm.hidden = true;
          forgotForce.hidden = !known || drives === ""; forgotSkip.hidden = false;
          forgotTitle.textContent = known ? "仍有安全盘处于挂载状态" : "无法确认挂载状态";
          forgotLead.textContent = known ? "检测到当前仍有处于挂载状态的安全盘，请确认是否需要在重置安全卡前卸载。" : "暂时无法确认安全盘的挂载状态。你可以自行检查并卸载安全盘，或点击“跳过”继续重置。";
          forgotDrives.textContent = drives; forgotDrives.hidden = !known;
          forgotGuidance.hidden = !known;
          forgotStatus.textContent = ""; forgotRunning = false; updateForgotControls(); forgotSkip.focus();
          return;
        }
      }
      writing = true;
      await bridge.beginConfirmLoginReset(forgotTicket, force);
      const outcome = await pollForgotPassword(revision);
      if (outcome === 4) {
        forgotStatus.textContent = "强制卸载未完成。请检查安全盘状态，或点击“跳过”继续重置。";
        forgotStatus.dataset.tone = "error";
        forgotRunning = false; updateForgotControls(); forgotSkip.focus(); return;
      }
      if (outcome !== 5) throw new Error("reset incomplete");
      forgotTicket = 0;
      forgotStatus.textContent = "已重新初始化，正在关闭应用。";
      forgotStatus.dataset.tone = "success";
    } catch (pageError) { logCaughtPageError("runForgotPassword/1", pageError);
      const ticket = forgotTicket; forgotTicket = 0;
      if (ticket) { try { await bridge.cancelLoginReset(ticket); } catch (pageError) { logCaughtPageError("runForgotPassword/2", pageError);} }
      forgotRunning = false; forgotAcknowledge.checked = false;
      forgotConfirm.hidden = true; forgotForce.hidden = true; forgotSkip.hidden = true;
      forgotStatus.textContent = writing ? "无法确认重置结果。请拔出安全卡重新插入，按实际状态继续；请勿重复执行旧确认。" : "无法准备重置。请重新插入安全卡，确认凭据配置后再试。";
      forgotStatus.dataset.tone = "error"; updateForgotControls(); forgotClose.focus();
      if (externalRefreshPending) { await closeForgotPassword(); void refreshAfterExternalChange(); }
    }
  };
  forgotOpen.addEventListener("click", () => {
    if (busy || !forgotAvailable) return;
    clearDomInputs(); busy = true; ++forgotRevision; forgotTicket = 0;
    forgotAcknowledge.checked = false; forgotRunning = false;
    forgotTitle.textContent = "重新初始化安全卡";
    forgotIntro.hidden = false; forgotMounts.hidden = true;
    forgotConfirm.hidden = false; forgotForce.hidden = true; forgotSkip.hidden = true;
    forgotStatus.textContent = ""; forgotStatus.dataset.tone = "";
    forgotDialog.hidden = false; updateForgotControls(); render(messageForCurrentState()); forgotAcknowledge.focus();
  });
  forgotAcknowledge.addEventListener("change", updateForgotControls);
  forgotClose.addEventListener("click", () => { void closeForgotPassword(); });
  forgotCancel.addEventListener("click", () => { void closeForgotPassword(); });
  forgotConfirm.addEventListener("click", () => { void runForgotPassword(true); });
  forgotForce.addEventListener("click", () => { void runForgotPassword(false, true); });
  forgotSkip.addEventListener("click", () => { void runForgotPassword(false); });
  forgotDialog.addEventListener("keydown", event => {
    if (event.key === "Escape") { event.preventDefault(); event.stopPropagation(); void closeForgotPassword(); }
    if (event.key === "Tab") {
      const items = [...forgotDialog.querySelectorAll("button,input")].filter(item => !item.disabled && !item.closest("[hidden]"));
      const index = items.indexOf(document.activeElement);
      if (items.length && ((event.shiftKey && index <= 0) || (!event.shiftKey && index === items.length - 1))) {
        event.preventDefault(); items[event.shiftKey ? items.length - 1 : 0].focus();
      }
    }
  });

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
    } catch (pageError) { logCaughtPageError("resetCardRecord/1", pageError);
      try {
        await refreshNativeState();
      } catch (pageError) { logCaughtPageError("resetCardRecord/2", pageError);
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
    const previousIds = new Set(containers.map((container) => container.id));
    const arrivalRevision = batchSessionRevision;
    if (busy || hiddenReview.hidden || !containerBAcknowledge.checked) return;
    const sizes = containerBSizes();
    if (sizes === null || !containerBInputReady() ||
        creationKind !== "hidden" ||
        (creationStorage === "disk" && (!partitionSelected || !partitionErase.checked)) ||
        currentState !== nativeState.authenticated) {
      creationStatus.textContent = "请检查容量、诱饵密码与下方的确认项。";
      creationStatus.dataset.tone = "error";
      updateCreationAvailability();
      return;
    }

    const disk = creationStorage === "disk", partitionId = partitionSelected;
    if (disk) partitionErase.checked = false;
    let creationAccepted = false;
    closeHiddenReview();
    let encodedSecond = null;
    let encodedConfirmation = null;
    let secondBytes = null;
    let confirmationBytes = null;
    busy = true;
    creationStatus.textContent =
      "正在创建，随后写入安全卡并检查管理员会话。";
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
        startCreationProgress();
        await paintCreationProgress();
        const accepted = disk ? await bridge.beginPartitionPair(partitionId, sizes.hidden, secondBytes, confirmationBytes)
          : await bridge.beginCreateContainerBMib(name, directory, sizes.outer, sizes.hidden, secondBytes, confirmationBytes);
        if (disk && accepted !== true) throw new Error("creation rejected");
        creationAccepted = true;
      } finally {
        wipe(secondBytes);
        wipe(confirmationBytes);
      }
      await waitForCreation();
      confirmCreationResult();
      await refreshNativeState();
      if (arrivalRevision === batchSessionRevision && !externalRefreshPending) markNewContainers(previousIds);
      busy = false;
      render(messageForCurrentState());
      creationCompleted = true;
      setCreationDescription("安全盘已经创建并挂载。");
      creationStatus.textContent = "创建成功，可以开始使用。";
      creationStatus.dataset.tone = "success";
      updateCreationAvailability();
    } catch (error) { logCaughtPageError("createContainerB/1", error);
      await showFileCreationFailure(error, creationAccepted,
        "名称、路径与诱饵密码", disk, previousIds);
    } finally {
      wipe(secondBytes);
      wipe(confirmationBytes);
      if (disk) discardPartitions();
      finishCreationProgress();
      updateCreationAvailability();
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    }
  };

  // A bare number told the user nothing, and the file-container path did not
  // even show that much. These say what happened and what can be done about
  // it; the code stays on the end so a report still carries it.
  const creationFailureMessages = {
    1: "创建参数不被接受，请检查容量、名称与保存位置。",
    2: "与安全组件的通信异常，请重新打开应用后重试。",
    3: "未获得管理员授权，创建已取消。",
    4: "无法启动安全组件，请确认应用安装完整后重试。",
    5: "无法连接安全组件，请重新打开应用后重试。",
    6: "安全组件未取得所需权限，请重新打开应用并允许管理员授权。",
    7: "加密组件不可用，请确认应用安装完整。",
    8: "加密组件初始化失败，请重新打开应用后重试。",
    9: "目标被加密组件拒绝，请检查容量设置与保存位置。",
    10: "该位置已存在同名文件，请换一个名称或保存位置。",
    11: "无法在该位置创建文件。请检查写入权限，或换一个不受安全软件保护的位置。",
    12: "无法挂载卷。可能没有空闲盘符，或目标暂时被占用。",
    13: "无法读取外层卷的空间分布，请重试。",
    14: "隐藏盘容量超出外层卷能安全容纳的范围，请调小隐藏容量。",
    15: kCreationCleanupMessage,
    16: "已取消创建。",
    17: "读写失败。请检查保存位置的剩余空间、文件系统限制（FAT32 单个文件不能超过 4 GiB）以及设备连接。",
    18: "安全组件意外结束，请重新打开应用后重试。",
    19: "保存位置的剩余空间不足，装不下这个容量的安全盘。请换个位置，或调小容量。",
  };
  let creationFailureOrigin = "来源未知";
  const describeCreationFailure = (code) => {
    const known = creationFailureMessages[code];
    const text = known ? `${known}（错误码${code}）` : `创建失败，原因无法确定。（错误码${code}）`;
    return mappedPageError(text, code, creationFailureOrigin);
  };
  const readCreationFailureCode = async (accepted) => {
    creationFailureOrigin = "页面兜底码";
    try {
      const code = accepted ? await bridge.getCreationError() : 0;
      if (Number.isInteger(code) && code > 0 && code <= 4294967295) {
        creationFailureOrigin = "原生错误码";
        return code;
      }
      if (accepted) logCaughtPageError("readCreationFailureCode/value", new Error("invalid creation failure code"));
    } catch (pageError) { logCaughtPageError("readCreationFailureCode/1", pageError);}
    return 2147500037; // E_FAIL when no native result is available.
  };

  const showFileCreationFailure = async (error, accepted, fields, disk = false, previousIds = null) => {
    const cancelled = error instanceof Error && error.message === kCreationCancelled;
    const cleanupFailed = error instanceof Error && error.message === kCreationCleanupFailed;
    // Read before releasing busy state, including branches with custom copy.
    const failureCode = cancelled ? null : await readCreationFailureCode(accepted);
    try {
      await refreshNativeState();
    } catch (pageError) { logCaughtPageError("showFileCreationFailure/1", pageError);
      currentState = nativeState.unavailable;
      activeVolumes = true;
    }
    busy = false;
    render(messageForCurrentState());
    if (creationDialog.hidden || currentState !== nativeState.authenticated) return;
    const retry = `重试前需要重新填写${fields}。`;
    const publishedUnmounted = !disk && previousIds instanceof Set &&
      containers.some((container) => !previousIds.has(container.id) && !container.mounted);
    let failure;
    if (cleanupFailed) failure = kCreationCleanupMessage;
    else if (cancelled) failure = "已取消创建，没有留下安全盘文件。" + retry;
    else if (publishedUnmounted) {
      failure = "安全盘已经创建，但挂载没有成功。它已在安全盘列表中，可以稍后挂载。";
    } else {
      failure = describeCreationFailure(failureCode);
      if (!disk) failure += retry;
    }
    creationStatus.textContent = cancelled ? failure : mappedPageError(failure, failureCode, creationFailureOrigin);
    creationStatus.dataset.tone = cancelled ? "cancelled" : "error";
    updateCreationAvailability();
  };

  const runPartitionConversion = async (id, resume) => {
    if (busy || currentState !== nativeState.authenticated) return;
    if (!resume && await bridge.getRememberContainerPaths() !== true) {
      creationStatus.textContent = "请先在系统设置开启“持久化安全盘列表”，再开始分区加密。";
      creationStatus.dataset.tone = "error"; return;
    }
    if (resume) {
      openCreationFlow();
      creationStorage = "disk"; // Existing conversions retain their resume path.
    }
    creationInplace = true; partitionErase.checked = false; busy = true;
    creationStatus.textContent = resume ? "正在认证并继续加密，正在使用本次管理员授权。" : "正在准备原地加密，正在使用本次管理员授权。";
    creationStatus.dataset.tone = "";
    let creationAccepted = false;
    try {
      startCreationProgress(); await paintCreationProgress();
      const accepted = resume ? await bridge.beginPartitionResume(id) : await bridge.beginPartitionConversion(id);
      if (accepted !== true) throw new Error("conversion rejected");
      creationAccepted = true;
      const result = await waitForCreation();
      if (result !== 6) confirmCreationResult();
      await refreshNativeState(); busy = false; render(messageForCurrentState());
      creationCompleted = true;
      creationStatus.textContent = result === 6 ? "进度已保存，可以安全退出应用。下次用原安全卡登录后继续加密。" : "分区加密已完成，可以挂载使用。";
      creationStatus.dataset.tone = result === 6 ? "paused" : "success";
    } catch (pageError) { logCaughtPageError("runPartitionConversion/1", pageError);
      const failureCode = await readCreationFailureCode(creationAccepted);
      try { await refreshNativeState(); } catch (pageError) { logCaughtPageError("runPartitionConversion/2", pageError); currentState = nativeState.unavailable; }
      busy = false; render(messageForCurrentState());
      creationStatus.textContent = describeCreationFailure(failureCode);
      creationStatus.dataset.tone = "error";
    } finally {
      busy = false; if (!resume) discardPartitions(); finishCreationProgress(); updateCreationAvailability();
      if (externalRefreshPending) void refreshAfterExternalChange();
    }
  };

  const createPartition = async () => {
    if (!partitionSelected || !partitionErase.checked || busy || currentState !== nativeState.authenticated) return;
    const id = partitionSelected;
    if (partitionPreserve.checked) { await runPartitionConversion(id, false); return; }
    const previousIds = new Set(containers.map(container => container.id));
    const revision = batchSessionRevision;
    partitionErase.checked = false;
    busy = true;
    creationStatus.textContent = "正在创建加密盘，正在使用本次管理员授权。";
    creationStatus.dataset.tone = "";
    render(messageForCurrentState());
    let creationAccepted = false;
    try {
      startCreationProgress();
      await paintCreationProgress();
      if (await bridge.beginPartitionCreate(id) !== true) throw new Error("partition creation rejected");
      creationAccepted = true;
      await waitForCreation();
      confirmCreationResult();
      await refreshNativeState();
      if (revision === batchSessionRevision && !externalRefreshPending) markNewContainers(previousIds);
      busy = false;
      render(messageForCurrentState());
      creationCompleted = true;
      setCreationDescription("加密盘已经创建并挂载。");
      creationStatus.textContent = "创建成功，可以开始使用。";
      creationStatus.dataset.tone = "success";
    } catch (pageError) { logCaughtPageError("createPartition/1", pageError);
      const failureCode = await readCreationFailureCode(creationAccepted);
      try { await refreshNativeState(); } catch (pageError) { logCaughtPageError("createPartition/2", pageError); currentState = nativeState.unavailable; activeVolumes = true; }
      busy = false;
      render(messageForCurrentState());
      if (!creationDialog.hidden && currentState === nativeState.authenticated) {
        const createdUnmounted = failureCode === 12 &&
          containers.some((container) => !previousIds.has(container.id) && !container.mounted);
        creationStatus.textContent = createdUnmounted
          ? mappedPageError("分区已经创建，但没有挂载。它已在安全盘列表中，可以稍后挂载。", failureCode, creationFailureOrigin)
          : describeCreationFailure(failureCode);
        creationStatus.dataset.tone = "error";
      }
    } finally {
      discardPartitions();
      finishCreationProgress();
      updateCreationAvailability();
      if (externalRefreshPending) void refreshAfterExternalChange();
    }
  };

  const createContainer = async () => {
    const previousIds = new Set(containers.map((container) => container.id));
    const arrivalRevision = batchSessionRevision;
    const sizeMib = containerCapacityMib();
    if (sizeMib === null || creationStorage !== "file" || creationKind !== "encrypted" ||
        currentState !== nativeState.authenticated) {
      creationStatus.textContent = "请检查容量设置。";
      creationStatus.dataset.tone = "error";
      updateCreationAvailability();
      return;
    }

    busy = true;
    creationStatus.textContent = "正在创建，正在使用本次管理员授权。";
    creationStatus.dataset.tone = "";
    render(messageForCurrentState());
    let creationAccepted = false;
    try {
      // The name and the location are ordinary strings, but a container path
      // is what section 8.4 keeps off this machine, so the fields are cleared
      // as soon as the values are handed over rather than left in the DOM.
      const name = containerName.value;
      const directory = containerDirectory.value;
      containerName.value = "";
      containerDirectory.value = "";
      startCreationProgress();
      await paintCreationProgress();
      await bridge.beginCreateContainerMib(name, directory, sizeMib);
      creationAccepted = true;
      await waitForCreation();
      confirmCreationResult();
      await refreshNativeState();
      if (arrivalRevision === batchSessionRevision && !externalRefreshPending) markNewContainers(previousIds);
      busy = false;
      render(messageForCurrentState());
      creationCompleted = true;
      setCreationDescription("安全盘已经创建并挂载。");
      creationStatus.textContent = "创建成功，可以开始使用。";
      creationStatus.dataset.tone = "success";
      updateCreationAvailability();
    } catch (error) { logCaughtPageError("createContainer/1", error);
      await showFileCreationFailure(error, creationAccepted, "名称与路径", false, previousIds);
    } finally {
      finishCreationProgress();
      updateCreationAvailability();
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    }
  };

  for (const item of dashboardNavigation) {
    item.addEventListener("click", () => {
      if (!busy && currentState === nativeState.authenticated) {
        if (item.dataset.section === "files" && !inDecoyFileView()) {
          fileContainerId = 0;
          filePath = [];
          fileEntries = [];
        }
        showDashboardSection(item.dataset.section);
      }
    });
  }
  fileBreadcrumb.addEventListener("click", (event) => {
    if (event.target instanceof Element && event.target.closest("[data-decoy-back]")) {
      if (busy || !inDecoyFileView()) return;
      decoyMaintenancePath = [...filePath];
      decoyListVisible = true;
      fileListingRevision += 1;
      fileContainerId = 0;
      filePath = [];
      fileEntries = [];
      disarmDecoyDrop();
      renderDecoySection();
      decoyContainerList.querySelector(".container-actions button")?.focus();
      return;
    }
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
    const open = element.closest("[data-file-action]") === null
      ? element.closest("[data-file-enter]") : null;
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
  fileListBody.addEventListener("contextmenu", (event) => {
    const row = event.target instanceof Element ? event.target.closest("[data-file-entry]") : null;
    if (row === null) return;
    event.preventDefault();
    openFileContextMenu(row, event.clientX, event.clientY);
  });
  // Block browser-native text/image drags as well as entry dragging.
  fileListBody.addEventListener("dragstart", (event) => event.preventDefault());
  fileListBody.addEventListener("keydown", (event) => {
    if ((event.key === "Enter" || event.key === " ") && event.target instanceof Element &&
        event.target.matches("tr[data-file-enter]") && !busy && currentState === nativeState.authenticated) {
      event.preventDefault();
      event.target.click();
      return;
    }
    if (event.key !== "ContextMenu" && !(event.shiftKey && event.key === "F10")) return;
    const row = event.target instanceof Element ? event.target.closest("[data-file-entry]") : null;
    if (row === null) return;
    event.preventDefault();
    const bounds = row.getBoundingClientRect();
    openFileContextMenu(row, bounds.left + 24, bounds.top + 24);
  });
  fileContextRename.addEventListener("click", () => {
    const target = fileContextTarget;
    closeFileContextMenu();
    if (target === null || target.id !== fileContainerId || target.directory !== filePath.join("/") ||
        !fileEntries.some((entry) => entry.name === target.name)) return;
    openFileNameFlow("rename", target.name);
  });
  document.addEventListener("pointerdown", (event) => {
    if (!fileContextMenu.contains(event.target)) closeFileContextMenu();
  });
  window.addEventListener("resize", closeFileContextMenu);
  document.addEventListener("scroll", closeFileContextMenu, true);
  backupExportLocation.addEventListener("click", () => { backupExportBrowse.click(); });
  backupExportBrowse.addEventListener("click", async () => {
    if (busy || bridge === null) return;
    const chosen = await bridge.chooseContainerDirectory();
    if (typeof chosen !== "string" || chosen.length === 0) return;
    exportLocation = chosen;
    setBackupStatus("");
    renderBackupCentre();
  });
  backupExportConfirm.addEventListener("click", async () => { await runExport(); });
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
  backupMigrateLocation.addEventListener("click", () => { backupMigrateBrowse.click(); });
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
  fileCreateDirectoryButton.addEventListener(
    "click", () => openFileNameFlow("create", ""));
  fileRefreshButton.addEventListener("click", async () => {
    if (fileRefreshButton.disabled || currentState !== nativeState.authenticated) return;
    await refreshFileManager();
    if (inDecoyFileView()) await refreshDecoyMaintenanceFacts();
  });
  fileMountToggle.addEventListener("click", async () => {
    const container = selectedFileContainer();
    if (busy || currentState !== nativeState.authenticated || container === null) return;
    // A pending listing belongs to the mount state before this action.
    fileListingRevision += 1;
    if (container.mounted) await unmountAdoptedContainer(container.id);
    else await mountAdoptedContainer(container.id);
  });
  fileNameCloseButton.addEventListener("click", () => closeFileNameFlow());
  fileNameCancelButton.addEventListener("click", () => closeFileNameFlow());
  fileNameDialog.addEventListener("pointerdown", (event) => {
    if (event.button === 0 && event.target === fileNameDialog) closeFileNameFlow();
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
      control.setAttribute("aria-disabled", "true");
      try {
        await stored.write(!enabled);
      } catch (pageError) { logCaughtPageError("control.click/1", pageError);
        setDashboardStatus(
          "无法更改该设置，请稍后重试。", "error");
      }
      await refreshStoredSwitches();
      control.setAttribute("aria-disabled", "false");
    });
  }
  for (const button of creationButtons) {
    button.addEventListener("click", () => {
      openCreationFlow();
    });
  }
  for (const button of creationStorageButtons) {
    button.addEventListener("click", () => {
      if (busy || creationCompleted || creationDialog.hidden || currentState !== nativeState.authenticated) return;
      if (button.dataset.createStorage === creationStorage) return;
      // Refresh itself invalidates the native snapshot before scanning.
      if (button.dataset.createStorage === "disk") discardPartitions();
      else clearCreationPartitions();
      creationStorage = button.dataset.createStorage === "file" ? "file" : "disk";
      returnToCreationTypes();
      renderPartitions();
      button.focus();
      if (creationStorage === "disk") void refreshPartitions();
    });
  }
  for (const button of creationTypeButtons) {
    button.addEventListener("click", () => {
      if (busy || creationCompleted || currentState !== nativeState.authenticated) return;
      containerBSecond.value = ""; containerBSecondConfirmation.value = "";
      partitionErase.checked = false;
      creationKind = button.dataset.createKind === "hidden" ? "hidden" : "encrypted";
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
  resetDialog.addEventListener("pointerdown", (event) => {
    if (event.button === 0 && event.target === resetDialog) closeResetFlow();
  });
  forgetCloseButton.addEventListener("click", () => closeForgetFlow());
  forgetCancelButton.addEventListener("click", () => closeForgetFlow());
  forgetDialog.addEventListener("pointerdown", (event) => {
    if (event.button === 0 && event.target === forgetDialog) closeForgetFlow();
  });
  forgetConfirmButton.addEventListener("click", async () => {
    const id = pendingForgetId;
    if (busy || id === 0) return;
    closeForgetFlow();
    await forgetAdoptedContainer(id);
  });
  creationCancelWork.addEventListener("click", async () => {
    if (creationCancelWork.disabled) return;
    creationCancelWork.disabled = true;
    creationCancelWork.textContent = creationInplace ? "正在保存进度并暂停…" : "正在取消…";
    // The reply only acknowledges the request. The actual outcome still
    // arrives through progress polling, including cleanup failures.
    try {
      const accepted = await requestCreationCancellation();
      if (creationRunning && accepted === false) {
        creationFinalizing = true;
        creationCancelWork.textContent = "正在完成创建";
        creationCancelWork.dataset.finalizing = "true";
      }
    } catch (pageError) { logCaughtPageError("creationCancelWork.click/1", pageError);
      if (creationRunning && !creationFinalizing) {
        creationCancelWork.disabled = false;
        creationCancelWork.textContent = creationInplace ? "暂停请求未确认，点击重试" : "取消请求未确认，点击重试";
        logShownPageError("creation-cancel", creationCancelWork.textContent);
      }
    }
  });
  creationCloseButton.addEventListener("click", () => closeCreationFlow());
  creationCancelButton.addEventListener("click", () => {
    if (busy) return;
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
    } catch (pageError) { logCaughtPageError("browseForDirectory/1", pageError);
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
    if (busy || creationDialog.hidden || currentState !== nativeState.authenticated) return;
    if (creationStorage === "disk" && !partitionReviewOpen) {
      const selected = partitionRows.find(row => row.id === partitionSelected && row.state === 0);
      if (!selected || (creationKind === "hidden" && !containerBInputReady())) return;
      const facts = document.getElementById("partition-review-facts");
      facts.replaceChildren();
      for (const [label, value] of [["分区", partitionName(selected)], ["容量", partitionSize(selected)],
        ["创建方式", partitionPreserve.checked ? "保留文件并加密" : "清空并创建"]]) {
        const row = document.createElement("div"), term = document.createElement("dt"), detail = document.createElement("dd");
        term.textContent = label; detail.textContent = value; row.append(term, detail); facts.append(row);
      }
      partitionErase.checked = false;
      partitionReviewOpen = true;
      updateCreationAvailability();
      document.getElementById("partition-review-back").focus();
      return;
    }
    if (creationStorage === "disk" && creationKind === "encrypted") { await createPartition(); return; }
    if (creationStorage === "disk" && (!partitionSelected || !partitionErase.checked)) return;
    if (creationKind === "encrypted") {
      await createContainer();
    } else if (creationKind === "hidden") {
      if (!containerBInputReady() || currentState !== nativeState.authenticated) return;
      closeHiddenReview();
      hiddenReviewPassword.value = containerBSecond.value;
      hiddenReview.hidden = false;
      creationDialog.inert = true;
      hiddenReviewCancel.focus();
    }
  });
  hiddenReview.addEventListener("pointerdown", (event) => {
    if (event.button === 0 && event.target === hiddenReview && !busy) hiddenReviewCancel.click();
  });
  forgotDialog.addEventListener("pointerdown", (event) => {
    if (event.button === 0 && event.target === forgotDialog) void closeForgotPassword();
  });
  decoyConfirmDialog.addEventListener("pointerdown", (event) => {
    if (event.button === 0 && event.target === decoyConfirmDialog && !busy) closeDecoyConfirm();
  });
  hiddenReviewCancel.addEventListener("click", () => {
    closeHiddenReview();
    creationContinueButton.focus();
  });
  containerBAcknowledge.addEventListener("change", () => {
    hiddenReviewConfirm.disabled = busy || !containerBAcknowledge.checked;
  });
  hiddenReviewVisibility.addEventListener("click", () => {
    const visible = hiddenReviewPassword.type === "password";
    hiddenReviewPassword.type = visible ? "text" : "password";
    hiddenReviewVisibility.setAttribute("aria-pressed", String(visible));
    hiddenReviewVisibility.setAttribute("aria-label", visible ? "隐藏当前诱饵密码" : "显示当前诱饵密码");
  });
  hiddenReviewConfirm.addEventListener("click", async () => { await createContainerB(); });
  hiddenReview.addEventListener("keydown", (event) => {
    if (event.key === "Tab") {
      const controls = Array.from(hiddenReview.querySelectorAll("input, button"))
        .filter((control) => !control.disabled);
      const first = controls[0];
      const last = controls[controls.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault(); last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault(); first.focus();
      }
    }
    if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      hiddenReviewCancel.click();
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
      if (!Number.isInteger(id) || id <= 0 || isMaintenanceContainer(id)) return;
      if (target.dataset.containerAction === "forget") {
        openForgetFlow(id, target.dataset.containerName ?? "");
      } else if (target.dataset.containerAction === "unmount") {
        await unmountAdoptedContainer(id);
      } else if (target.dataset.containerAction === "resume") {
        await runPartitionConversion(id, true);
      } else if (target.dataset.containerAction === "mount") {
        await mountAdoptedContainer(id);
      } else if (target.dataset.containerAction === "browse") {
        browseContainer(id);
      } else if (target.dataset.containerAction === "details") {
        expandedContainerId = expandedContainerId === id ? 0 : id;
        renderContainers();
        list.querySelector(`[data-row-container="${id}"] [data-container-action="details"]`)?.focus();
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
  creationDialog.addEventListener("pointerdown", (event) => {
    if (event.button === 0 && event.target === creationDialog) {
      closeCreationFlow();
    }
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !fileContextMenu.hidden) {
      const target = fileContextTarget;
      closeFileContextMenu();
      for (const row of fileListBody.querySelectorAll("[data-file-entry]")) {
        if (target !== null && row.dataset.fileEntry === target.name) row.focus();
      }
      event.preventDefault();
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
  window.addEventListener("usb-vault-card-backup-closed", (event) => {
    busy = false;
    if (event.detail?.sessionChanged === true) {
      window.dispatchEvent(new Event("usb-vault-session-changed"));
      return;
    }
    render(messageForCurrentState());
  });
  backupButtons.forEach(button => button.addEventListener("click", async () => {
    if (busy || !bridge || currentState === null) return;
    busy = true;
    passwordInput.value = "";
    confirmationInput.value = "";
    render("正在打开备卡向导…");
    try { await bridge.openCardBackup(); }
    catch (pageError) {
      logCaughtPageError("cardBackup.open", pageError);
      busy = false;
      render(messageForCurrentState(), "无法开始备卡，请确认原卡已连接且没有其他任务。");
    }
  }));
  configurationButton.addEventListener("click", async () => {
    if (busy || activeVolumes || !bridge || currentState === null || currentState === nativeState.authenticated) return;
    busy = true;
    passwordInput.value = "";
    confirmationInput.value = "";
    render("正在打开凭据配置…");
    try {
      await bridge.openConfiguration();
      // The native host retires this page and builds a new session afterwards.
    } catch (pageError) { logCaughtPageError("configurationButton.click/1", pageError);
      busy = false;
      render(messageForCurrentState(), "无法打开配置，请稍后重试。");
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

  // ================= 首次配置 =================
  // Setup runs on a bridge of its own: it can store the credential set and
  // hand the process to the product session, and it can reach nothing else.
  // The values leave the page the way the login password does, as a byte
  // array that is wiped on every return path.
  const setupSurface = document.getElementById("configuration-surface");
  const setupLead = document.getElementById("setup-lead");
  const setupSave = document.getElementById("setup-save");
  const setupBack = document.getElementById("setup-back");
  const setupStatus = document.getElementById("setup-status");
  // The card ships with this user password. It is a starting value, not a
  // secret: it is prefilled so a first run is three fields of typing rather
  // than four, and the user can replace it here like any other value.
  const setupDefaultUserPin = "11111111";
  // Setup itself never touches the card, so going back is the first moment
  // the credentials are used for real and the first place a wrong user
  // password can cost an attempt. The user is told before they choose it.
  const kSetupReturnNotice =
    "返回后会用这三项访问安全卡，用户密码有误会消耗一次尝试次数。";
  const setupCredentialStatus = Object.freeze({ ready: 0, missing: 1, unreadable: 2 });
  const setupOutcome = Object.freeze({ saved: 0, invalidFormat: 1, storageFailure: 2 });
  // Shown as typed rather than masked. These are supplier-issued values the
  // user proof-reads against a document, and a masked 128-character key cannot
  // be proof-read at all; the session password, which is the user's own
  // secret, keeps its mask.
  const setupFields = [
    { id: "setup-user-sn", accepts: (value) => /^[0-9a-fA-F]{32}$/.test(value) },
    { id: "setup-api-key", accepts: (value) => /^[0-9a-fA-F]{128}$/.test(value) },
    { id: "setup-user-pin", accepts: (value) => /^[0-9A-Za-z]{8}$/.test(value) },
  ].map((field) => ({ ...field, input: document.getElementById(field.id) }));
  let setupBusy = false;

  const setSetupStatus = (text, tone) => {
    setupStatus.textContent = text;
    if (tone === undefined) delete setupStatus.dataset.tone;
    else setupStatus.dataset.tone = tone;
  };

  const setupSetComplete = () => setupFields.every((field) => field.accepts(field.input.value));

  const updateSetupAvailability = () => {
    for (const field of setupFields) {
      field.input.dataset.valid = field.accepts(field.input.value) ? "true" : "false";
    }
    setupSave.disabled = setupBusy || !setupSetComplete();
  };

  const setSetupEnabled = (enabled) => {
    setupSurface.dataset.busy = enabled ? "false" : "true";
    for (const field of setupFields) field.input.disabled = !enabled;
    // The way back is available on every route into setup and at every moment
    // except while a save is in flight, so an unconfigured first run is never
    // a page the user cannot leave.
    setupBack.disabled = !enabled;
    updateSetupAvailability();
  };

  const clearSetupFields = () => {
    for (const field of setupFields) field.input.value = "";
    updateSetupAvailability();
  };

  const submitSetup = async () => {
    if (setupBusy || !setupSetComplete()) return;
    setupBusy = true;
    setSetupEnabled(false);
    setSetupStatus("正在保存并复读验证…");
    let encoded = [null, null, null];
    let payload = [null, null, null];
    try {
      try {
        const encoder = new TextEncoder();
        for (let index = 0; index < setupFields.length; ++index) {
          encoded[index] = encoder.encode(setupFields[index].input.value);
          payload[index] = Array.from(encoded[index]);
        }
      } finally {
        for (const value of encoded) wipe(value);
        encoded = [null, null, null];
      }
      let outcome = setupOutcome.storageFailure;
      try {
        outcome = Number(
          await bridge.saveCredentialBytes(payload[0], payload[1], payload[2]));
      } finally {
        for (const value of payload) wipe(value);
        payload = [null, null, null];
      }
      setupBusy = false;
      if (outcome === setupOutcome.saved) {
        clearSetupFields();
        setSetupEnabled(true);
        setSetupStatus(`凭据已保存并复读验证。${kSetupReturnNotice}`, "ok");
        setupBack.focus();
        return;
      }
      setSetupEnabled(true);
      setSetupStatus(outcome === setupOutcome.invalidFormat
        ? "三项凭据的格式不符合要求，请核对后重新保存全部三项。"
        // A read-back failure happens after the write, so the previously
        // stored set may already be gone. Saying it did not take effect would
        // send the user away believing the old configuration still works.
        : "保存后无法复读确认，原有配置可能已被覆盖。请重新保存全部三项。", "error");
    } catch (pageError) { logCaughtPageError("submitSetup/1", pageError);
      setupBusy = false;
      setSetupEnabled(true);
      setSetupStatus("保存失败，请稍后重新保存全部三项。", "error");
    } finally {
      for (const value of encoded) wipe(value);
      for (const value of payload) wipe(value);
    }
  };

  for (const field of setupFields) {
    field.input.addEventListener("input", () => {
      updateSetupAvailability();
    });
    field.input.addEventListener("keydown", (event) => {
      if (event.key !== "Enter" || setupSave.disabled) return;
      event.preventDefault();
      void submitSetup();
    });
  }
  setupSave.addEventListener("click", () => { void submitSetup(); });
  setupBack.addEventListener("click", async () => {
    if (setupBusy) return;
    setupBusy = true;
    setupBack.disabled = true;
    setSetupStatus("正在返回…");
    try {
      await bridge.continueToSession();
      // The native host retires this page and builds the product session.
    } catch (pageError) { logCaughtPageError("setupBack.click/1", pageError);
      setupBusy = false;
      setupBack.disabled = false;
      setSetupStatus("无法返回，请稍后重试。", "error");
    }
  });

  const startConfiguration = async () => {
    document.body.dataset.view = "configuration";
    surface.hidden = true;
    dashboard.hidden = true;
    setupSurface.hidden = false;
    let status = setupCredentialStatus.missing;
    let queryFailed = false;
    try {
      status = Number(await bridge.getCredentialStatus());
      if (!Object.values(setupCredentialStatus).includes(status)) throw new Error("invalid credential status");
    } catch (pageError) { logCaughtPageError("startConfiguration/1", pageError);
      status = setupCredentialStatus.unreadable;
      queryFailed = true;
    }
    // Always the shipped value, on every route into setup. A stored password
    // that is not the factory one is therefore overwritten by a save that
    // leaves this field untouched, which is the accepted behaviour here.
    setupFields[2].input.value = setupDefaultUserPin;
    setupBusy = false;
    setSetupEnabled(true);
    if (status === setupCredentialStatus.ready) {
      setupLead.textContent =
        "当前 Windows 用户已保存凭据。重新填写三项可以覆盖保存，或直接返回。";
      setSetupStatus(kSetupReturnNotice);
    } else if (status === setupCredentialStatus.unreadable) {
      setSetupStatus(queryFailed
        ? "错误码：1407"
        : "错误码：1108", "error");
    } else {
      setSetupStatus("");
    }
    setupFields[0].input.focus();
    if (window.usbVaultPageStartupStatus === 0) {
      window.usbVaultPageStartupStatus = queryFailed ? 1407
        : status === setupCredentialStatus.unreadable ? 1108 : 1;
    }
  };

  const showPermissionRequired = () => {
    bridge = null;
    currentState = nativeState.unavailable; activeVolumes = false; busy = false;
    render("需要管理员身份"); statusOutput.dataset.tone = "error";
    window.usbVaultPageStartupStatus = 1;
  };
  (async () => {
    // This native-injected flag only removes capabilities. Denied startup must
    // not depend on a host-object handshake to display its inert error page.
    if (window.usbVaultPermissionRequired === true) {
      showPermissionRequired();
      return;
    }
    let failureCode = 1401;
    try {
      bridge = chrome.webview.hostObjects.nativeBridge;
      connectPageEventBridge(bridge);
      // The host decides which surface this page opened for. Setup has no
      // session state to read, so it must be answered before anything asks
      // the bridge for one.
      failureCode = 1402;
      const startupView = String(await bridge.getStartupView());
      if (startupView === "permission-required") {
        showPermissionRequired();
        return;
      }
      if (!["configuration", "session"].includes(startupView)) throw new Error("invalid startup view");
      if (startupView === "configuration") {
        await startConfiguration();
        if (window.usbVaultPageStartupStatus === 0) window.usbVaultPageStartupStatus = 1;
        return;
      }
      failureCode = 1403;
      const preset = await bridge.getWindowSize();
      if (![0, 1, 2, 3].includes(preset)) throw new Error("invalid window preference");
      appliedWindowSize = String(preset);
      windowSizeSelect.value = appliedWindowSize;
      failureCode = 1405;
      await refreshNativeState();
      busy = false;
      render(messageForCurrentState());
      updateStrength();
      focusCurrentInput();
      if (window.usbVaultPageStartupStatus === 0) window.usbVaultPageStartupStatus = 1;
      if (externalRefreshPending) {
        void refreshAfterExternalChange();
      }
    } catch (error) { logCaughtPageError("startup/1", error);
      if (error && error.startupCode === 1404) failureCode = 1404;
      window.usbVaultPageStartupStatus = failureCode;
      bridge = null;
      currentState = nativeState.unavailable;
      activeVolumes = false;
      busy = false;
      render(mappedPageError(`错误码：${failureCode}`, failureCode, "页面启动阶段码"));
    }
  })();
})();
