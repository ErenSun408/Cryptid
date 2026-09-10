// Pure DOM simulation: no browser, native host, card access, resource loading,
// or layout engine. Supply a local jsdom module path to run the same assertions
// used by the optional browser fixture.
// node tests/card_product_ui_navigation_dom_tests.cjs <jsdom module path>
const fs = require("node:fs");
const path = require("node:path");
const { JSDOM, VirtualConsole } = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, "..");
const errors = [];
const virtualConsole = new VirtualConsole();
virtualConsole.on("jsdomError", (error) => errors.push(error.message));
const dom = new JSDOM(fs.readFileSync(path.join(root, "ui/card_session.html"), "utf8"), {
  runScripts: "outside-only", virtualConsole,
});
const window = dom.window;
window.TextEncoder = TextEncoder;
// jsdom has no pointer capture or layout. Assertions exercise event routing
// and resulting state; actual pointer hit-testing remains a browser check.
window.PointerEvent = class extends window.MouseEvent {
  constructor(type, options = {}) {
    super(type, options);
    this.pointerId = options.pointerId || 0;
  }
};
for (const file of ["tests/card_product_ui_navigation_mock.js", "ui/card_session.js",
                    "tests/card_product_ui_navigation_checks.js"]) {
  window.eval(fs.readFileSync(path.join(root, file), "utf8"));
}
const deadline = Date.now() + 10000;
const timer = setInterval(() => {
  const output = window.document.getElementById("navigation-test-results");
  if (output || Date.now() > deadline) {
    clearInterval(timer);
    console.log(output ? output.textContent : "FAIL: test completion timed out");
    if (errors.length) console.error(errors.join("\n"));
    process.exitCode = output && window.document.title.startsWith("PASS:") && !errors.length ? 0 : 1;
    window.close();
  }
}, 20);
