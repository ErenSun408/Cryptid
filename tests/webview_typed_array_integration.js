"use strict";

chrome.webview.postMessage("driver-loaded");

(async () => {
  const surface = document.getElementById("session-surface");
  const password = document.getElementById("session-password");
  const action = document.getElementById("session-action");
  const status = document.getElementById("session-status");
  const waitForState = (expected) => {
    if (surface.dataset.sessionState === expected) {
      return Promise.resolve(true);
    }
    return new Promise((resolve) => {
      let settled = false;
      const observer = new MutationObserver(() => {
        if (surface.dataset.sessionState === expected) {
          finish(true);
        }
      });
      const timeout = setTimeout(() => finish(false), 3000);
      const finish = (matched) => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timeout);
        observer.disconnect();
        resolve(matched);
      };
      observer.observe(surface, {
        attributes: true,
        attributeFilter: ["data-session-state"],
      });
      if (surface.dataset.sessionState === expected) {
        finish(true);
      }
    });
  };
  const waitForStatus = (expected) => {
    if (status.textContent === expected) {
      return Promise.resolve(true);
    }
    return new Promise((resolve) => {
      let settled = false;
      const observer = new MutationObserver(() => {
        if (status.textContent === expected) {
          finish(true);
        }
      });
      const timeout = setTimeout(() => finish(false), 3000);
      const finish = (matched) => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timeout);
        observer.disconnect();
        resolve(matched);
      };
      observer.observe(status, { childList: true, characterData: true,
                                 subtree: true });
      if (status.textContent === expected) {
        finish(true);
      }
    });
  };

  if (!await waitForState("ready")) {
    chrome.webview.postMessage("fail-initialize");
    return;
  }
  password.value = "p\u0000ä";
  action.click();
  if (!await waitForState("active") || password.value !== "") {
    chrome.webview.postMessage("fail-submit");
    return;
  }
  chrome.webview.postMessage("request-external-refresh");
  if (!await waitForStatus(
      "安全卡状态已变化，但安全会话仍在。请尝试安全卸载。")) {
    chrome.webview.postMessage("fail-external-remains");
    return;
  }
  action.click();
  if (!await waitForState("ready")) {
    chrome.webview.postMessage("fail-unmount");
    return;
  }

  password.value = "p\u0000ä";
  action.click();
  if (!await waitForState("active") || password.value !== "") {
    chrome.webview.postMessage("fail-submit");
    return;
  }
  chrome.webview.postMessage("request-external-unmount");
  if (!await waitForState("ready") ||
      !await waitForStatus("安全卡状态已变化，安全会话已关闭。")) {
    chrome.webview.postMessage("fail-external-unmounted");
    return;
  }
  chrome.webview.postMessage("pass");
})();
