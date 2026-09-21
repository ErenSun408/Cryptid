// No native host, card, credentials, resource loads, or real browser process.
const fs = require("node:fs");
const path = require("node:path");
const assert = require("node:assert/strict");
const { JSDOM } = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, "..");
const read = file => fs.readFileSync(path.join(root, file), "utf8");
const settle = () => new Promise(resolve => setTimeout(resolve, 20));
async function run(configure, verify) {
  const dom = new JSDOM(read("ui/card_session.html"), { runScripts: "outside-only" });
  const w = dom.window;
  w.TextEncoder = TextEncoder;
  try {
    w.eval(read("tests/card_product_ui_navigation_mock.js"));
    w.navigationFixture.state = 0;
    const bridge = w.chrome.webview.hostObjects.nativeBridge;
    bridge.hasActiveSession = () => false;
    let authenticationCalls = 0;
    bridge.submitLoginPasswordBytes = () => { ++authenticationCalls; };
    configure(w, bridge);
    w.eval(read("ui/card_session.js"));
    await settle();
    await verify(w);
    assert.equal(authenticationCalls, 0, "diagnostics triggered authentication");
  } finally { w.close(); }
}
(async () => {
  await run(() => {}, async w => {
    assert.equal(w.usbVaultPageStartupStatus, 1);
    assert.match(w.document.getElementById("card-session-status").textContent, /等待安全卡/);
  });
  await run((w, b) => { b.getStartupView = () => { throw new Error("private exception"); }; }, async w => {
    assert.equal(w.usbVaultPageStartupStatus, 1402);
    assert.match(w.document.body.textContent, /错误码：1402/);
    assert.doesNotMatch(w.document.body.textContent, /private exception/);
  });
  await run((w, b) => { b.getStartupView = () => "unknown"; }, async w => {
    assert.equal(w.usbVaultPageStartupStatus, 1402);
  });
  await run((w, b) => { b.getWindowSize = () => 99; }, async w => {
    assert.equal(w.usbVaultPageStartupStatus, 1403);
  });
  await run((w, b) => { b.getSessionState = () => "4"; }, async w => {
    assert.equal(w.usbVaultPageStartupStatus, 1404);
  });
  await run((w, b) => { b.getSessionState = () => { throw new Error("private"); }; }, async w => {
    assert.equal(w.usbVaultPageStartupStatus, 1405);
  });
  await run(w => {
    w.navigationFixture.state = 4;
    w.usbVaultStartupFailure = { code: 2019, reason: 18, nativeKind: 0,
      nativeCode: 12345, cleanupCode: 2015, privateText: "secret-sentinel" };
  }, async w => {
    assert.equal(w.usbVaultPageStartupStatus, 1, "card error is not a WebView failure");
    let text = w.document.getElementById("card-session-status").textContent;
    assert.equal(text, "错误码：2019");
    assert.equal(w.document.getElementById("card-session-status").dataset.tone, "error");
    assert.doesNotMatch(text, /12345|secret-sentinel|重新插入/);
    w.navigationFixture.state = 0;
    w.usbVaultStartupFailure = { code: 0 };
    w.dispatchEvent(new w.Event("usb-vault-session-changed"));
    await settle();
    text = w.document.getElementById("card-session-status").textContent;
    assert.match(text, /等待安全卡/); assert.doesNotMatch(text, /2019|2015/);
    assert.equal(w.document.getElementById("card-session-status").dataset.tone, "");
    w.navigationFixture.state = 4;
    w.usbVaultStartupFailure = { code: 2025, reason: 5, nativeKind: 0, cleanupCode: 0 };
    w.dispatchEvent(new w.Event("usb-vault-session-changed"));
    await settle();
    text = w.document.getElementById("card-session-status").textContent;
    assert.equal(text, "错误码：2025");
  });
  await run((w, b) => {
    b.getStartupView = () => new Promise(() => {});
  }, async w => {
    w.dispatchEvent(new w.ErrorEvent("error", { message: "secret-path-and-credential" }));
    assert.equal(w.usbVaultPageStartupStatus, 1408);
    assert.doesNotMatch(w.document.body.textContent, /secret-path-and-credential/);
  });
  await run((w, b) => {
    b.getStartupView = () => new Promise(() => {});
  }, async w => {
    w.dispatchEvent(new w.Event("unhandledrejection"));
    assert.equal(w.usbVaultPageStartupStatus, 1408);
  });
  await run((w, b) => {
    b.getStartupView = () => "configuration";
    b.getCredentialStatus = () => { throw new Error("secret-credential"); };
  }, async w => {
    assert.equal(w.usbVaultPageStartupStatus, 1407);
    assert.match(w.document.body.textContent, /1407/);
    assert.doesNotMatch(w.document.body.textContent, /secret-credential/);
  });
  await run((w, b) => {
    b.getStartupView = () => "configuration";
    b.getCredentialStatus = () => 99;
  }, async w => {
    assert.equal(w.usbVaultPageStartupStatus, 1407);
    assert.match(w.document.body.textContent, /1407/);
  });
  await run(w => { w.chrome = undefined; }, async w => {
    assert.equal(w.usbVaultPageStartupStatus, 1401);
    assert.match(w.document.body.textContent, /1401/);
  });
  console.log("PASS 12 startup DOM scenarios, zero authentication retries");
})().catch(error => { console.error(error); process.exitCode = 1; });
