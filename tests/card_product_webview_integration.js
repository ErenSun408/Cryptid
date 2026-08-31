"use strict";

chrome.webview.postMessage("driver-loaded");

(async () => {
  const surface = document.getElementById("card-session-surface");
  const password = document.getElementById("card-password");
  const confirmation = document.getElementById("card-password-confirmation");
  const confirmationField = document.getElementById("confirmation-field");
  const strength = document.getElementById("password-strength");
  const action = document.getElementById("card-session-action");

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
      confirmationField.hidden || confirmation.disabled) {
    request("fail-initialization-route");
    return;
  }
  password.value = "abcdefghijke\u0301";
  password.dispatchEvent(new Event("input", { bubbles: true }));
  confirmation.value = "abcdefghijk\u00e9";
  if (!await waitFor(() => strength.textContent === "强度：可接受")) {
    request("fail-strength");
    return;
  }
  action.click();
  if (!await waitForState("authenticated") ||
      password.value !== "" || confirmation.value !== "" ||
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
  if (!await waitFor(() => password.value === "")) {
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
