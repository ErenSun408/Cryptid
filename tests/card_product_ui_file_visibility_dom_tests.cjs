const fs = require("node:fs"), path = require("node:path"), assert = require("node:assert/strict");
const { JSDOM } = require(path.resolve(process.argv[2]));
const read = file => fs.readFileSync(path.resolve(__dirname, "..", file), "utf8");
const settle = () => new Promise(resolve => setTimeout(resolve, 100));
(async () => {
  for (const scenario of ["setting", "setting-failure", "reload", "hide", "unhide", "failure", "pending", "mounted", "partition", "session-change", "read-failure", "mismatch", "readback-failure"]) {
    const dom = new JSDOM(read("ui/card_session.html"), {runScripts:"outside-only", pretendToBeVisual:true});
    const w = dom.window; w.TextEncoder = TextEncoder;
    try {
      w.eval(read("tests/card_product_ui_navigation_mock.js"));
      const fixture = w.navigationFixture, bridge = w.chrome.webview.hostObjects.nativeBridge;
      fixture.hideFileContainers = scenario === "reload";
      fixture.containers[1].fileHidden = scenario === "unhide";
      const originalRead = bridge.getContainerFileHidden;
      let reads = 0;
      bridge.getContainerFileHidden = id => {
        if (scenario === "read-failure" || (scenario === "readback-failure" && reads++ > 0)) throw Error("read failed");
        return originalRead(id);
      };
      const calls = []; let release;
      bridge.setHideFileContainers = value => {
        calls.push(value);
        if (scenario === "setting-failure") throw Error("write failed");
        fixture.hideFileContainers = value;
      };
      bridge.setContainerFileHidden = async (id, hidden) => {
        calls.push([id, hidden]);
        if (scenario === "failure") throw Error("attribute failed");
        if (["pending", "session-change"].includes(scenario)) await new Promise(resolve => release = resolve);
        if (scenario !== "mismatch") fixture.containers.find(item => item.id === id).fileHidden = hidden;
      };
      if (scenario === "mounted") Object.assign(fixture.containers[1], {mounted:true, size:65536, drive:"Z:"});
      if (scenario === "partition") fixture.containers[1].partition = true;
      w.eval(read("ui/card_session.js")); await settle();
      const d = w.document;
      if (["setting", "setting-failure", "reload"].includes(scenario)) {
        d.querySelector('[data-section="settings"]').click(); await settle();
        const toggle = d.querySelector('[data-setting="hide-file-containers"]');
        if (scenario === "reload") assert.equal(toggle.getAttribute("aria-checked"), "true");
        else {
          toggle.click(); await settle();
          assert.equal(calls.length,1);
          assert.equal(toggle.getAttribute("aria-checked"),scenario === "setting-failure" ? "false" : "true");
        }
        continue;
      }
      const details = d.querySelector('[data-container-id="2"][data-container-action="details"]');
      assert.ok(details, scenario + ": " + d.querySelector("[data-session-state]")?.dataset.sessionState + " / " + JSON.stringify(fixture.pageEvents));
      details.click(); await settle();
      const action = "toggle-file-hidden";
      let button = d.querySelector(`[data-container-id="2"][data-container-action="${action}"]`);
      if (scenario === "partition") { assert.equal(button,null); continue; }
      assert.ok(button);
      assert.equal(d.querySelector('[data-container-action="hide-file"]'),null);
      assert.equal(d.querySelector('[data-container-action="unhide-file"]'),null);
      if (scenario === "read-failure") {
        assert.equal(button.disabled,true);
        assert.equal(button.getAttribute("aria-checked"),"mixed");
        assert.match(d.querySelector(".container-file-visibility output").textContent,/无法读取/);
        continue;
      }
      assert.equal(button.getAttribute("aria-checked"),scenario === "unhide" ? "true" : "false");
      if (scenario === "mounted") { assert.equal(button.disabled,true); button.click(); assert.equal(calls.length,0); continue; }
      const row = button.closest(".container-file-visibility");
      const previousFeedback = row.querySelector("output").textContent;
      button.focus();
      button.click();
      assert.equal(button.isConnected,true);
      assert.equal(button.getAttribute("aria-checked"),scenario === "unhide" ? "false" : "true");
      assert.equal(row.querySelector("output").textContent,previousFeedback);
      await settle();
      assert.deepEqual(calls,[[2,scenario !== "unhide"]]);
      if (["pending","session-change"].includes(scenario)) {
        button = d.querySelector(`[data-container-id="2"][data-container-action="${action}"]`);
        assert.equal(button.disabled,false);
        assert.equal(button.getAttribute("aria-disabled"),"true");
        assert.equal(button.closest(".container-file-visibility"),row);
        assert.equal(row.querySelector("output").textContent,previousFeedback);
        button.click(); assert.equal(calls.length,1);
        if (scenario === "session-change") { fixture.state = 2; w.dispatchEvent(new w.Event("usb-vault-session-changed")); await settle(); }
        release(); await settle();
      }
      const status = d.querySelector(".container-file-visibility output")?.textContent ?? "";
      if (scenario === "session-change") assert.ok(!status.includes("已隐藏"));
      else if (scenario === "readback-failure") {
        const current = d.querySelector(`[data-container-id="2"][data-container-action="${action}"]`);
        assert.equal(current,button);
        assert.equal(current.disabled,true);
        assert.equal(current.getAttribute("aria-checked"),"mixed");
        assert.match(status,/无法确认/);
      } else {
        assert.match(status,["failure","mismatch"].includes(scenario) ? /失败/ : scenario === "unhide" ? /取消隐藏/ : /已隐藏/);
        const current = d.querySelector(`[data-container-id="2"][data-container-action="${action}"]`);
        assert.equal(current.getAttribute("aria-checked"),scenario === "unhide" || ["failure","mismatch"].includes(scenario) ? "false" : "true");
        assert.equal(current.disabled,false);
        assert.equal(current,button);
        assert.equal(current.closest(".container-file-visibility"),row);
        assert.equal(d.activeElement,button);
      }
    } finally { w.close(); }
  }
  console.log("PASS: thirteen file visibility switch DOM scenarios");
})().catch(error => { console.error(error); process.exitCode=1; });
