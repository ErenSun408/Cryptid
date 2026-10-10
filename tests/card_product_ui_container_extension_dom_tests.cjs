// Retired settings must not read or write legacy preferences from the page.
const fs = require("node:fs"), path = require("node:path"), assert = require("node:assert/strict");
const { JSDOM } = require(path.resolve(process.argv[2]));
const read = file => fs.readFileSync(path.resolve(__dirname, "..", file), "utf8");
const settle = () => new Promise(resolve => setTimeout(resolve, 100));
(async () => {
  for (const state of [2, 3]) {
    const dom = new JSDOM(read("ui/card_session.html"), {runScripts:"outside-only", pretendToBeVisual:true});
    const w = dom.window; w.TextEncoder = TextEncoder;
    try {
      w.eval(read("tests/card_product_ui_navigation_mock.js"));
      w.navigationFixture.state = state;
      const bridge = w.chrome.webview.hostObjects.nativeBridge;
      const calls = [];
      for (const method of ["getContainerExtension", "setContainerExtension", "getHideFileContainers", "setHideFileContainers"])
        bridge[method] = () => { calls.push(method); throw Error("retired preference queried"); };
      w.eval(read("ui/card_session.js")); await settle();
      w.document.querySelector('[data-section="settings"]').click(); await settle();
      assert.equal(w.document.getElementById("settings-container-extension"), null);
      assert.equal(w.document.getElementById("settings-container-extension-status"), null);
      assert.equal(w.document.querySelector('[data-setting="hide-file-containers"]'), null);
      assert.ok(w.document.querySelector('[data-setting="remember-containers"]'));
      assert.ok(w.document.querySelector('[data-setting="operation-logging"]'));
      assert.deepEqual(calls, []);
    } finally { w.close(); }
  }
  console.log("PASS: retired file creation settings do not access legacy preferences");
})().catch(error => { console.error(error); process.exitCode=1; });
