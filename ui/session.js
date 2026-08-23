(() => {
  "use strict";

  const surface = document.getElementById("session-surface");
  const passwordInput = document.getElementById("session-password");
  const actionButton = document.getElementById("session-action");
  const statusOutput = document.getElementById("session-status");

  let bridge = null;
  let active = false;
  let busy = true;

  const render = (state, message) => {
    surface.dataset.sessionState = state;
    passwordInput.disabled = active || busy;
    actionButton.disabled = busy;
    actionButton.textContent = active ? "安全卸载" : "打开安全会话";
    statusOutput.textContent = message;
  };

  const refreshActiveState = async () => {
    const value = await bridge.hasActiveSession();
    if (value !== true && value !== false) {
      throw new Error("invalid active-session state");
    }
    active = value;
  };

  const submitPassword = async () => {
    let passwordBytes = null;
    let message = "无法打开安全会话，请检查密码后重试。";
    busy = true;
    render("opening", "正在打开安全会话");

    try {
      try {
        passwordBytes = new TextEncoder().encode(passwordInput.value);
      } finally {
        passwordInput.value = "";
      }

      try {
        await bridge.submitPasswordBytes(passwordBytes);
      } finally {
        passwordBytes.fill(0);
      }
      active = true;
      message = "安全会话已打开。";
    } catch (_) {
      try {
        await refreshActiveState();
      } catch (_) {
        active = true;
      }
      if (active) {
        message = "安全操作未完成，请尝试安全卸载。";
      }
    } finally {
      if (passwordBytes !== null) {
        passwordBytes.fill(0);
      }
      busy = false;
      render(active ? "active" : "ready", message);
      if (!active) {
        passwordInput.focus();
      }
    }
  };

  const unmount = async () => {
    let message = "无法安全卸载，请关闭占用文件后重试。";
    busy = true;
    render("closing", "正在安全卸载");

    try {
      await bridge.unmount();
      await refreshActiveState();
      if (!active) {
        message = "安全会话已关闭。";
      }
    } catch (_) {
      try {
        await refreshActiveState();
      } catch (_) {
        active = true;
      }
    } finally {
      busy = false;
      render(active ? "active" : "ready", message);
      if (!active) {
        passwordInput.focus();
      }
    }
  };

  actionButton.addEventListener("click", async () => {
    if (busy) {
      return;
    }
    if (!active && passwordInput.value.length === 0) {
      render("ready", "请输入密码。");
      passwordInput.focus();
      return;
    }
    if (active) {
      await unmount();
    } else {
      await submitPassword();
    }
  });

  passwordInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && !passwordInput.disabled) {
      event.preventDefault();
      actionButton.click();
    }
  });

  (async () => {
    try {
      chrome.webview.hostObjects.options.shouldPassTypedArraysAsArrays = true;
      bridge = chrome.webview.hostObjects.nativeBridge;
      await refreshActiveState();
      busy = false;
      render(active ? "active" : "ready",
             active ? "安全会话已打开。" : "安全组件已就绪。");
      if (!active) {
        passwordInput.focus();
      }
    } catch (_) {
      active = false;
      busy = true;
      render("unavailable", "安全组件不可用。");
    }
  })();
})();
