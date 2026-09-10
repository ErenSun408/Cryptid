// Build a browser-only fixture from the actual product page. No native card,
// vault, or filesystem operations are exposed by the mock bridge.
// Run: node tests/card_product_ui_navigation_fixture.cjs
// Open out/ui-navigation/index.html for assertions; preview.html for visual QA.
const fs = require("node:fs");
const path = require("node:path");
const root = path.resolve(__dirname, "..");
const out = path.join(root, "out", "ui-navigation");
fs.mkdirSync(out, { recursive: true });
const source = fs.readFileSync(path.join(root, "ui/card_session.html"), "utf8");
for (const mode of ["index", "preview"]) {
  fs.writeFileSync(path.join(out, `${mode}.html`), source.replace(
    /<script src="card_session.js[^\"]*"><\/script>/,
    '<script src="mock.js"></script><script src="card_session.js"></script>' +
    (mode === "index" ? '<script src="checks.js"></script>' : "")));
}
fs.copyFileSync(path.join(root, "ui/card_session.js"), path.join(out, "card_session.js"));
fs.copyFileSync(path.join(__dirname, "card_product_ui_navigation_mock.js"), path.join(out, "mock.js"));
fs.copyFileSync(path.join(__dirname, "card_product_ui_navigation_checks.js"), path.join(out, "checks.js"));
console.log(out);
