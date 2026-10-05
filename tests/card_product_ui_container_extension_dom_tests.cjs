// Offline preference scenarios; no real settings, card, or driver writes.
const fs = require("node:fs"), path = require("node:path"), assert = require("node:assert/strict");
const { JSDOM } = require(path.resolve(process.argv[2]));
const read = file => fs.readFileSync(path.resolve(__dirname, "..", file), "utf8");
const settle = () => new Promise(resolve => setTimeout(resolve, 100));
(async () => {
  for (const scenario of ["fixed", "random", "write-failure", "read-failure", "pending", "reload", "unauthenticated"]) {
    const dom = new JSDOM(read("ui/card_session.html"), { runScripts: "outside-only", pretendToBeVisual: true });
    const w = dom.window; w.TextEncoder = TextEncoder;
    try {
      w.eval(read("tests/card_product_ui_navigation_mock.js"));
      w.navigationFixture.state = scenario === "unauthenticated" ? 2 : 3;
      const bridge = w.chrome.webview.hostObjects.nativeBridge;
      let saved = scenario === "reload" ? 2 : 0, writes = 0, release;
      bridge.getContainerExtension = () => {
        if (scenario === "read-failure") throw new Error("unreadable");
        return saved;
      };
      bridge.setContainerExtension = async value => {
        ++writes;
        if (scenario === "write-failure") throw new Error("write failed");
        if (scenario === "pending") await new Promise(resolve => { release = resolve; });
        saved = value;
      };
      w.eval(read("ui/card_session.js")); await settle();
      const select = w.document.getElementById("settings-container-extension");
      const status = w.document.getElementById("settings-container-extension-status");
      assert.deepEqual(Array.from(select.options, option => option.textContent), [".sys", ".dat", ".bin", "随机"]);
      const row = select.closest(".settings-row"), group = select.closest(".settings-group");
      assert.equal(group.querySelector("h3").textContent, "安全");
      assert.equal(group.querySelector(".settings-card").lastElementChild, row);
      assert.equal(row.querySelector("b").textContent, "文件安全盘后缀");
      assert.equal(row.querySelector("small"), null);
      assert.equal(w.getComputedStyle(select.options[0]).color, "rgb(232, 234, 237)");
      assert.equal(w.getComputedStyle(select.options[0]).backgroundColor, "rgb(20, 23, 28)");
      if (scenario === "unauthenticated") {
        select.value = "0"; select.dispatchEvent(new w.Event("change")); await settle();
        assert.equal(writes, 0); continue;
      }
      w.document.querySelector('[data-section="settings"]').click(); await settle();
      if (scenario === "read-failure") { assert.equal(select.disabled, true); assert.match(status.textContent, /无法读取/); continue; }
      if (scenario === "reload") { assert.equal(select.value, "2"); assert.equal(writes, 0); continue; }
      assert.equal(select.value, "0");
      const requested = scenario === "random" ? "3" : "2";
      select.value = requested; select.dispatchEvent(new w.Event("change")); await settle();
      if (scenario === "pending") {
        assert.equal(select.disabled, true);
        select.dispatchEvent(new w.Event("change")); assert.equal(writes, 1);
        release(); await settle();
      }
      assert.equal(writes, 1); assert.equal(select.disabled, false);
      if (scenario === "write-failure") { assert.equal(select.value, "0"); assert.equal(saved, 0); assert.match(status.textContent, /无法保存/); }
      else { assert.equal(saved, Number(requested)); assert.equal(select.value, requested); assert.match(status.textContent, /已保存/); }
    } finally { w.close(); }
  }
  console.log("PASS: seven container extension settings DOM scenarios");
})().catch(error => { console.error(error); process.exitCode = 1; });
