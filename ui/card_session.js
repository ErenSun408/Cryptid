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
  const containerAState = Object.freeze({
    absent: 0,
    present: 1,
    mounted: 2,
  });

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
  const statusOutput = document.getElementById("card-session-status");
  const errorOutput = document.getElementById("card-session-error");
  const dashboard = document.getElementById("authenticated-dashboard");
  const dashboardUnmountButton = document.getElementById("dashboard-unmount");
  const dashboardVolumeState =
    document.getElementById("dashboard-volume-state");
  const dashboardContainerAState =
    document.getElementById("dashboard-container-a-state");
  const containerATitles =
    Array.from(document.querySelectorAll("[data-container-a-title]"));
  const containerASummaries =
    Array.from(document.querySelectorAll("[data-container-a-summary]"));
  const containerAFacts =
    Array.from(document.querySelectorAll("[data-container-a-facts]"));
  const containerASizes =
    Array.from(document.querySelectorAll("[data-container-a-size]"));
  const containerADrives =
    Array.from(document.querySelectorAll("[data-container-a-drive]"));
  const dashboardNavigation =
    Array.from(document.querySelectorAll(".dashboard-nav"));
  const dashboardSections =
    Array.from(document.querySelectorAll(".dashboard-section"));
  const creationButtons =
    Array.from(document.querySelectorAll("[data-create-kind]"));
  const creationDialog = document.getElementById("create-volume-dialog");
  const creationTitle = document.getElementById("create-volume-title");
  const creationDescription =
    document.getElementById("create-volume-description");
  const creationCloseButton = document.getElementById("create-volume-close");
  const creationCancelButton = document.getElementById("create-volume-cancel");
  const creationContinueButton =
    document.getElementById("create-volume-continue");
  const containerAForm = document.getElementById("create-container-a-form");
  const containerACapacity =
    document.getElementById("create-container-a-capacity");
  const creationStatus = document.getElementById("create-volume-status");

  let bridge = null;
  let currentState = null;
  let activeVolumes = false;
  let currentContainerAState = containerAState.absent;
  let currentContainerASizeMib = 0;
  let currentContainerADrive = "";
  let busy = true;
  let externalRefreshPending = false;
  let creationKind = null;
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

  const containerACapacityMib = () => {
    const raw = containerACapacity.value.trim();
    if (!/^[0-9]+$/.test(raw)) return null;
    const value = Number(raw);
    return Number.isSafeInteger(value) && value >= 20 && value <= 1048576
      ? value
      : null;
  };

  const updateCreationAvailability = () => {
    const encrypted = creationKind === "encrypted";
    const authenticated = currentState === nativeState.authenticated;
    containerAForm.hidden = !encrypted;
    containerACapacity.disabled = busy || !authenticated || !encrypted ||
      creationCompleted;
    creationCloseButton.disabled = busy;
    creationCancelButton.disabled = busy;
    creationContinueButton.disabled = busy || !authenticated || !encrypted ||
      creationCompleted || containerACapacityMib() === null;
    creationContinueButton.title = encrypted ? "" : "正在开发中";
    creationContinueButton.textContent = encrypted ? "创建加密盘" : "继续";
    creationCancelButton.textContent = creationCompleted ? "完成" : "返回";
  };

  const closeCreationFlow = (force = false) => {
    if (busy && !force) return;
    creationDialog.hidden = true;
    creationKind = null;
    creationCompleted = false;
    creationStatus.textContent = "";
    creationStatus.dataset.tone = "";
    creationCancelButton.textContent = "返回";
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

  const openCreationFlow = (kind) => {
    if (busy || currentState !== nativeState.authenticated) {
      return;
    }
    const hiddenVolume = kind === "hidden";
    if (!hiddenVolume && currentContainerAState !== containerAState.absent) {
      return;
    }
    creationKind = hiddenVolume ? "hidden" : "encrypted";
    creationCompleted = false;
    creationStatus.textContent = "";
    creationStatus.dataset.tone = "";
    containerACapacity.value = "64";
    creationTitle.textContent = hiddenVolume ? "新建隐藏盘" : "新建加密盘";
    creationDescription.textContent = hiddenVolume
      ? "隐藏盘参数与卡记录事务将在后续切片接入。"
      : "请选择加密盘容量。创建过程将请求管理员权限。";
    creationDialog.hidden = false;
    updateCreationAvailability();
    if (hiddenVolume) {
      creationCloseButton.focus();
    } else {
      containerACapacity.focus();
      containerACapacity.select();
    }
  };

  const updateProductSurface = () => {
    const showDashboard = currentState === nativeState.authenticated;
    surface.hidden = showDashboard;
    dashboard.hidden = !showDashboard;
    document.body.dataset.view = showDashboard ? "dashboard" : "session";
    dashboardVolumeState.textContent = activeVolumes ? "已挂载" : "未挂载";
    const containerAExists =
      currentContainerAState !== containerAState.absent;
    const containerAMounted =
      currentContainerAState === containerAState.mounted;
    dashboardContainerAState.textContent = containerAMounted
      ? "已挂载"
      : containerAExists ? "未挂载" : "未创建";
    for (const title of containerATitles) {
      title.textContent = containerAMounted
        ? "加密盘已挂载"
        : containerAExists ? "加密盘已创建" : "尚未创建加密盘";
    }
    for (const summary of containerASummaries) {
      summary.textContent = containerAMounted
        ? "加密盘已安全挂载，可以从对应盘符访问。"
        : containerAExists
          ? "加密盘当前未挂载，请重新插卡并登录后打开。"
          : "点击“新建”创建并挂载加密盘。";
    }
    for (const facts of containerAFacts) facts.hidden = !containerAExists;
    const capacity = currentContainerASizeMib > 0
      ? `${currentContainerASizeMib.toLocaleString("zh-CN")} MiB`
      : "不可用";
    for (const size of containerASizes) size.textContent = capacity;
    for (const drive of containerADrives) {
      drive.textContent = containerAMounted ? currentContainerADrive : "未挂载";
    }
    dashboardUnmountButton.hidden = !showDashboard || !activeVolumes;
    dashboardUnmountButton.disabled = busy || !showDashboard || !activeVolumes;
    for (const button of creationButtons) {
      const existingContainerA = button.dataset.createKind === "encrypted" &&
        containerAExists;
      button.hidden = existingContainerA;
      button.disabled = busy || !showDashboard || existingContainerA;
    }
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
        actionButton.textContent = "初始化安全卡";
        break;
      case nativeState.loginRequired:
        stateTitle.textContent = "打开安全会话";
        passwordLabel.textContent = "密码";
        actionButton.textContent = "打开安全会话";
        break;
      case nativeState.authenticated:
        stateTitle.textContent = activeVolumes
          ? "安全会话已打开"
          : "安全卡已认证";
        actionButton.textContent = "安全卸载";
        break;
      case nativeState.unavailable:
        stateTitle.textContent = "安全组件当前不可用";
        actionButton.textContent = "尝试安全卸载";
        break;
      default:
        stateTitle.textContent = "正在连接安全组件";
        break;
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
    let nextContainerAState = containerAState.absent;
    let nextContainerASizeMib = 0;
    let nextContainerADrive = "";
    if (nextState === nativeState.authenticated) {
      nextContainerAState = await bridge.getContainerAState();
      nextContainerASizeMib = await bridge.getContainerASizeMib();
      nextContainerADrive = await bridge.getContainerADriveLetter();
      const validState = Number.isInteger(nextContainerAState) &&
        nextContainerAState >= containerAState.absent &&
        nextContainerAState <= containerAState.mounted;
      const validSize = Number.isInteger(nextContainerASizeMib) &&
        ((nextContainerAState === containerAState.absent &&
          nextContainerASizeMib === 0) ||
         (nextContainerAState !== containerAState.absent &&
          nextContainerASizeMib >= 20 &&
          nextContainerASizeMib <= 1048576));
      const validDrive = typeof nextContainerADrive === "string" &&
        ((nextContainerAState === containerAState.mounted &&
          /^[A-Z]:$/.test(nextContainerADrive) && nextActiveVolumes) ||
         (nextContainerAState !== containerAState.mounted &&
          nextContainerADrive === ""));
      if (!validState || !validSize || !validDrive) {
        throw new Error("invalid container A status");
      }
    }
    currentState = nextActiveVolumes &&
        nextState !== nativeState.authenticated &&
        nextState !== nativeState.unavailable
      ? nativeState.unavailable
      : nextState;
    activeVolumes = nextActiveVolumes;
    currentContainerAState = nextContainerAState;
    currentContainerASizeMib = nextContainerASizeMib;
    currentContainerADrive = nextContainerADrive;
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

  const createContainerA = async () => {
    const sizeMib = containerACapacityMib();
    if (sizeMib === null || creationKind !== "encrypted" ||
        currentState !== nativeState.authenticated) {
      creationStatus.textContent = "请输入有效的加密盘容量。";
      creationStatus.dataset.tone = "error";
      updateCreationAvailability();
      return;
    }

    busy = true;
    creationStatus.textContent = "正在请求管理员权限并创建加密盘。";
    creationStatus.dataset.tone = "";
    render(messageForCurrentState());
    try {
      await bridge.createContainerAMib(sizeMib);
      await refreshNativeState();
      busy = false;
      creationCompleted = true;
      render(messageForCurrentState());
      creationDescription.textContent = "加密盘已经创建并挂载。";
      creationStatus.textContent = "创建成功，可以开始使用加密盘。";
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
  for (const button of creationButtons) {
    button.addEventListener("click", () => {
      openCreationFlow(button.dataset.createKind);
    });
  }
  creationCloseButton.addEventListener("click", () => closeCreationFlow());
  creationCancelButton.addEventListener("click", () => closeCreationFlow());
  creationContinueButton.addEventListener("click", async () => {
    if (!busy && creationKind === "encrypted") {
      await createContainerA();
    }
  });
  containerACapacity.addEventListener("input", () => {
    creationStatus.textContent = "";
    creationStatus.dataset.tone = "";
    updateCreationAvailability();
  });
  creationDialog.addEventListener("click", (event) => {
    if (event.target === creationDialog) {
      closeCreationFlow();
    }
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !creationDialog.hidden) {
      closeCreationFlow();
    }
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
