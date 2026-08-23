"use strict";

chrome.webview.postMessage("driver-loaded");

(async () => {
  const surface = document.getElementById("session-surface");
  const password = document.getElementById("session-password");
  const action = document.getElementById("session-action");
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
  action.click();
  if (!await waitForState("ready")) {
    chrome.webview.postMessage("fail-unmount");
    return;
  }
  chrome.webview.postMessage("pass");
})();
