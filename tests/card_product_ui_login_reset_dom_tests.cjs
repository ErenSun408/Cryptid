// Offline DOM scenarios; no SDK, card, driver or credential access.
const fs = require("node:fs"), path = require("node:path"), assert = require("node:assert/strict");
const { JSDOM } = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, "..");
const read = p => fs.readFileSync(path.join(root, p), "utf8");
const settle = () => new Promise(r => setTimeout(r, 30));
(async () => {
 for (const scenario of ["skip", "force", "force-failure", "unknown", "none", "cancel", "change", "failure"]) {
  const dom = new JSDOM(read("ui/card_session.html"), {runScripts:"outside-only", pretendToBeVisual:true});
  const w = dom.window; w.TextEncoder = TextEncoder;
  try {
   w.eval(read("tests/card_product_ui_navigation_mock.js"));
   w.navigationFixture.state = 2;
   const bridge = w.chrome.webview.hostObjects.nativeBridge;
   let prepared = 0, status = 0, cancelled = 0;
   const confirms = [];
   bridge.hasActiveSession = () => false;
   bridge.beginLoginResetPreparation = () => { ++prepared; status = 2; return true; };
   bridge.getLoginResetStatus = () => status;
   bridge.getLoginResetTicket = () => 7;
   bridge.getLoginResetMountsKnown = () => scenario !== "unknown";
   bridge.getLoginResetDrives = () => scenario === "none" || scenario === "unknown" ? "" : "X:, Y:";
   bridge.cancelLoginReset = ticket => { assert.equal(ticket, 7); ++cancelled; };
   bridge.beginConfirmLoginReset = (ticket, force) => { assert.equal(ticket,7); confirms.push(force); status = scenario === "force-failure" && force ? 4 : scenario === "failure" ? 3 : 5; return true; };
   bridge.resetCardRecord = () => { throw new Error("settings reset used before login"); };
   w.eval(read("ui/card_session.js")); await settle();
   const el = name => w.document.getElementById("forgot-password-" + name);
   assert.equal(el("open").hidden, false);
   const gear = w.document.getElementById("session-tools-toggle");
   const menu = w.document.getElementById("session-tools-menu");
   assert.equal(menu.hidden, true); gear.click();
   assert.equal(menu.hidden, false); assert.equal(gear.getAttribute("aria-expanded"), "true");
   assert.equal(menu.contains(el("open")), true);
   assert.equal(el("open").classList.contains("danger"), true);
   el("open").click();
   assert.equal(menu.hidden, true);
   assert.equal(el("dialog").hidden, false); assert.equal(el("acknowledge").checked, false);
   el("confirm").click(); assert.equal(prepared, 0);
   el("acknowledge").checked = true; el("acknowledge").dispatchEvent(new w.Event("change"));
   el("confirm").click(); el("confirm").click(); await settle(); assert.equal(prepared, 1);
   if (scenario === "none") { assert.deepEqual(confirms, [false]); continue; }
   assert.equal(confirms.length, 0);
   assert.equal(el("force").hidden, scenario === "unknown");
   assert.equal(el("skip").hidden, false);
   assert.match(el("mounts").textContent, /强制卸载可能导致未保存的数据丢失。重置后，旧安全盘将无法凭重新初始化后的安全卡打开。/);
   if (scenario === "cancel" || scenario === "change") {
    if (scenario === "cancel") el("close").click();
    else { w.navigationFixture.state = 0; w.dispatchEvent(new w.Event("usb-vault-session-changed")); }
    await settle(); assert.equal(el("dialog").hidden,true); assert.equal(el("acknowledge").checked,false);
    assert.equal(confirms.length,0); assert.equal(cancelled,1); continue;
   }
   if (scenario === "force" || scenario === "force-failure") el("force").click();
   else el("skip").click();
   await settle();
   if (scenario === "force-failure") {
    assert.equal(el("skip").disabled,false); assert.match(el("status").textContent,/强制卸载未完成/);
    el("skip").click(); await settle(); assert.deepEqual(confirms,[true,false]);
   } else assert.deepEqual(confirms,[scenario === "force"]);
   if (scenario === "failure") {
    assert.match(el("status").textContent,/无法确认重置结果/); assert.equal(el("confirm").hidden,true); assert.equal(el("skip").hidden,true);
   } else assert.match(el("status").textContent,/正在关闭应用/);
  } finally { w.close(); }
 }
 console.log("PASS: eight login reset DOM scenarios");
})().catch(error => { console.error(error); process.exitCode = 1; });
