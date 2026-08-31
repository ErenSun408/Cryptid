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
      !action.hidden) {
    request("fail-initialization-submit");
    return;
  }

  request("request-login");
  if (!await waitForState("login-required") ||
      !confirmationField.hidden || password.disabled) {
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
      action.hidden || action.textContent !== "安全卸载") {
    request("fail-login-success");
    return;
  }

  request("request-residual-mount");
  if (!await waitForState("unavailable") || action.hidden ||
      action.textContent !== "尝试安全卸载") {
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
})();
