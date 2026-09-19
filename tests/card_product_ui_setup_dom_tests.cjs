// Pure DOM simulation of the configuration route: no browser, native host,
// card access or credential storage. It runs the product page against the
// setup bridge mock, once with no stored credential set and once with a
// readable one.
// node tests/card_product_ui_setup_dom_tests.cjs <jsdom module path>
const fs = require("node:fs");
const path = require("node:path");
const { JSDOM, VirtualConsole } = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, "..");
const page = fs.readFileSync(path.join(root, "ui/card_session.html"), "utf8");
const sources = ["tests/card_product_ui_setup_mock.js", "ui/card_session.js",
                 "tests/card_product_ui_setup_checks.js"]
  .map((file) => fs.readFileSync(path.join(root, file), "utf8"));

const run = (status) => new Promise((resolve) => {
  const errors = [];
  const virtualConsole = new VirtualConsole();
  virtualConsole.on("jsdomError", (error) => errors.push(error.message));
  const dom = new JSDOM(page, {
    runScripts: "outside-only", virtualConsole, pretendToBeVisual: true,
  });
  const window = dom.window;
  window.TextEncoder = TextEncoder;
  window.setupFixtureStatus = status;
  for (const source of sources) window.eval(source);
  const deadline = Date.now() + 30000;
  const timer = setInterval(() => {
    const output = window.document.getElementById("setup-test-results");
    if (!output && Date.now() <= deadline) return;
    clearInterval(timer);
    const passed = Boolean(output) &&
      window.document.title.startsWith("PASS:") && errors.length === 0;
    console.log(output ? output.textContent : "FAIL: test completion timed out");
    if (errors.length) console.error(errors.join("\n"));
    window.close();
    resolve(passed);
  }, 20);
});

(async () => {
  let passed = true;
  for (const status of [1, 0]) {
    console.log(`--- stored credential status ${status} ---`);
    passed = (await run(status)) && passed;
  }
  process.exitCode = passed ? 0 : 1;
})();
