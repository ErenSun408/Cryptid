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
   bridge.beginForceUnmountForExit = () => { throw new Error("normal exit requested force"); };
   w.eval(read("ui/card_session.js")); await settle();
   const el = id => w.document.getElementById(id);
   assert.equal(el("force-exit-dialog"), null);
   assert.equal(w.usbVaultConfirmForceExit, undefined);
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
 console.log("PASS: six application exit DOM scenarios; force-exit UI absent");
})().catch(error=>{console.error(error);process.exitCode=1;});
