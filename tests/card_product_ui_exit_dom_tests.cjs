// Offline exit scenarios: no card, driver, or actual application close.
const fs = require("node:fs"), path = require("node:path"), assert = require("node:assert/strict");
const { JSDOM } = require(path.resolve(process.argv[2]));
const read = name => fs.readFileSync(path.resolve(__dirname, "..", name), "utf8");
const settle = () => new Promise(resolve => setTimeout(resolve, 80));
(async () => {
 for (const scenario of ["login", "exit", "unmount-failure", "request-failure", "pending", "unauthenticated"]) {
  const dom = new JSDOM(read("ui/card_session.html"), {runScripts:"outside-only", pretendToBeVisual:true});
  const w = dom.window; w.TextEncoder = TextEncoder;
  try {
   w.eval(read("tests/card_product_ui_navigation_mock.js"));
   w.navigationFixture.state = scenario === "unauthenticated" ? 2 : 3;
   const bridge = w.chrome.webview.hostObjects.nativeBridge;
   const calls = [];
   let status = scenario === "pending" ? 1 : scenario === "unmount-failure" ? 4 : 2;
   bridge.beginUnmount = () => { calls.push("unmount"); };
   bridge.getVolumeOperationStatus = () => status;
   bridge.returnToLogin = () => { calls.push("login"); if (scenario === "request-failure") throw new Error("denied"); };
   bridge.exitApplication = () => { calls.push("exit"); };
   w.eval(read("ui/card_session.js")); await settle();
   const el = id => w.document.getElementById(id);
   const toggle = el("app-exit-toggle"), menu = el("app-exit-menu");
   assert.equal(menu.hidden, true);
   if (scenario === "unauthenticated") {
    assert.equal(toggle.disabled,true); el("return-to-login").click(); assert.deepEqual(calls,[]); continue;
   }
   toggle.click(); assert.equal(menu.hidden,false);
   toggle.dispatchEvent(new w.KeyboardEvent("keydown",{key:"Escape",bubbles:true}));
   assert.equal(menu.hidden,true);
   toggle.click(); w.document.body.click(); assert.equal(menu.hidden,true);
   toggle.click(); el(scenario === "exit" ? "exit-application" : "return-to-login").click();
   await settle();
   assert.equal(menu.hidden,true);
   if (scenario === "unmount-failure") {
    assert.deepEqual(calls,["unmount"]); assert.match(el("dashboard-status").textContent,/未能离开/);
    assert.equal(toggle.disabled,false);
   } else if (scenario === "pending") {
    assert.deepEqual(calls,["unmount"]); el("exit-application").click(); assert.deepEqual(calls,["unmount"]);
    status = 2; await new Promise(resolve=>setTimeout(resolve,160)); assert.deepEqual(calls,["unmount","login"]);
   } else {
    assert.deepEqual(calls,scenario === "exit" ? ["exit"] : ["unmount","login"]);
    if (scenario === "request-failure") assert.match(el("dashboard-status").textContent,/未能离开/);
   }
  } finally { w.close(); }
 }
 for (const scenario of ["cancel", "click", "mousedown", "pointerdown", "escape", "inside", "confirm", "failure", "pending"]) {
  const dom = new JSDOM(read("ui/card_session.html"), {runScripts:"outside-only", pretendToBeVisual:true});
  const w = dom.window; w.TextEncoder = TextEncoder;
  try {
   w.eval(read("tests/card_product_ui_navigation_mock.js")); w.navigationFixture.state = 3;
   const bridge = w.chrome.webview.hostObjects.nativeBridge, calls = [];
   let status = scenario === "failure" ? 8 : scenario === "pending" ? 1 : 2;
   bridge.beginForceUnmountForExit = () => { calls.push("force"); };
   bridge.dismissCloseConfirmation = () => { calls.push("cancel"); };
   bridge.exitApplication = () => { calls.push("exit"); };
   bridge.getVolumeOperationStatus = () => status;
   w.eval(read("ui/card_session.js")); await settle();
   const el = id => w.document.getElementById(id), dialog = el("force-exit-dialog");
   assert.equal(w.usbVaultConfirmForceExit(), true); assert.equal(w.usbVaultConfirmForceExit(), true);
   assert.match(dialog.textContent, /当前仍有挂载中的容器，确定要强制卸载并退出吗？/);
   assert.equal(w.document.activeElement, el("force-exit-cancel"));
   if (scenario === "cancel") el("force-exit-cancel").click();
   else if (["click", "mousedown", "pointerdown"].includes(scenario))
    dialog.dispatchEvent(new w.MouseEvent(scenario, {bubbles:true}));
   else if (scenario === "escape") dialog.dispatchEvent(new w.KeyboardEvent("keydown", {key:"Escape",bubbles:true}));
   else if (scenario === "inside") dialog.querySelector(".modal-body").dispatchEvent(new w.MouseEvent("mousedown",{bubbles:true}));
   else { el("force-exit-confirm").click(); el("force-exit-confirm").click(); }
   await settle();
   if (scenario === "inside") { assert.equal(dialog.hidden,false); assert.deepEqual(calls,[]); }
   else if (scenario === "confirm") { assert.equal(dialog.hidden,true); assert.deepEqual(calls,["force","exit"]); }
   else if (scenario === "failure") {
    assert.equal(dialog.hidden,false); assert.deepEqual(calls,["force"]);
    assert.match(el("force-exit-status").textContent,/未完成/); assert.equal(el("force-exit-confirm").disabled,true);
    el("force-exit-cancel").click(); await settle(); assert.equal(dialog.hidden,true);
   } else if (scenario === "pending") {
    dialog.dispatchEvent(new w.MouseEvent("mousedown",{bubbles:true}));
    assert.equal(dialog.hidden,false); assert.deepEqual(calls,["force"]);
    status=2; await new Promise(resolve=>setTimeout(resolve,180)); assert.deepEqual(calls,["force","exit"]);
   } else { assert.equal(dialog.hidden,true); assert.deepEqual(calls,["cancel"]); }
  } finally { w.close(); }
 }
 console.log("PASS: six application exit and nine force-exit DOM scenarios");
})().catch(error=>{console.error(error);process.exitCode=1;});
