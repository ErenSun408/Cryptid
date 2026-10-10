// Expanding file or partition details must not query or mutate file visibility.
const fs = require("node:fs"), path = require("node:path"), assert = require("node:assert/strict");
const { JSDOM } = require(path.resolve(process.argv[2]));
const read = file => fs.readFileSync(path.resolve(__dirname, "..", file), "utf8");
const settle = () => new Promise(resolve => setTimeout(resolve, 100));
(async () => {
  for (const scenario of ["file", "hidden", "mounted", "partition"]) {
    const dom = new JSDOM(read("ui/card_session.html"), {runScripts:"outside-only", pretendToBeVisual:true});
    const w = dom.window; w.TextEncoder = TextEncoder;
    try {
      w.eval(read("tests/card_product_ui_navigation_mock.js"));
      const fixture = w.navigationFixture, bridge = w.chrome.webview.hostObjects.nativeBridge;
      fixture.containers[1].fileHidden = scenario === "hidden";
      if (scenario === "mounted") Object.assign(fixture.containers[1], {mounted:true, size:65536, drive:"Z:"});
      if (scenario === "partition") fixture.containers[1].partition = true;
      const calls = [];
      for (const method of ["getContainerFileHidden", "setContainerFileHidden"])
        bridge[method] = () => { calls.push(method); throw Error("removed visibility control queried"); };
      w.eval(read("ui/card_session.js")); await settle();
      const d = w.document;
      d.querySelector('[data-container-id="2"][data-container-action="details"]').click(); await settle();
      const panel = d.querySelector('.container-details');
      assert.ok(panel);
      assert.ok(panel.querySelector('[data-container-action="browse"]'));
      assert.ok(panel.querySelector('[data-container-action="export"]'));
      assert.equal(panel.querySelector('.container-file-visibility'), null);
      assert.equal(panel.querySelector('[data-container-action="toggle-file-hidden"]'), null);
      assert.deepEqual(calls, []);
    } finally { w.close(); }
  }
  console.log("PASS: four details scenarios without visibility controls or attribute access");
})().catch(error => { console.error(error); process.exitCode=1; });
