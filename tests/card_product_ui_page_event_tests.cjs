// Real product script in jsdom with a recording native bridge. No card access.
// node tests/card_product_ui_page_event_tests.cjs <jsdom module path> [case name]
const fs = require("node:fs");
const path = require("node:path");
const assert = require("node:assert/strict");
const { JSDOM, VirtualConsole } = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, "..");
const script = fs.readFileSync(path.join(root, "ui/card_session.js"), "utf8");
const mock = fs.readFileSync(path.join(root, "tests/card_product_ui_navigation_mock.js"), "utf8");
const html = fs.readFileSync(path.join(root, "ui/card_session.html"), "utf8");
const sleep = (ms = 0) => new Promise(resolve => setTimeout(resolve, ms));
async function wait(condition) {
  for (let i = 0; i < 200; ++i) {
    if (condition()) return;
    await sleep(10);
  }
  throw new Error("page-event fixture did not settle");
}
async function page(prepare = () => {}) {
  const dom = new JSDOM(html, {runScripts: "outside-only", pretendToBeVisual: true,
    virtualConsole: new VirtualConsole(), url: "https://usb-vault.local/card_session.html"});
  const window = dom.window;
  window.TextEncoder = TextEncoder;
  window.eval(mock);
  const fixture = window.navigationFixture;
  const bridge = window.chrome.webview.hostObjects.nativeBridge;
  prepare(window, fixture, bridge);
  window.eval(script + "\n//# sourceURL=https://usb-vault.local/card_session.js");
  await wait(() => window.usbVaultPageStartupStatus !== 0);
  await sleep();
  return {window, fixture, bridge, close: () => dom.window.close()};
}
function scriptError(window, message = "invalid container status", line = 25) {
  window.dispatchEvent(new window.ErrorEvent("error", {error: new window.Error(message),
    filename: "https://usb-vault.local/card_session.js", lineno: line, colno: 3}));
}
function rejection(window, reason) {
  const event = new window.Event("unhandledrejection");
  Object.defineProperty(event, "reason", {value: reason});
  window.dispatchEvent(event);
}
const cases = {
  async passwordValidation() {
    const p = await page((window, fixture) => { fixture.state = 1; });
    try {
      const input = p.window.document.getElementById("card-password");
      const reports = () => p.fixture.pageEvents.filter(e => e.source === "password-strength-description");
      const enter = async value => { input.value = value; input.dispatchEvent(new p.window.Event("input", {bubbles: true})); await sleep(); };
      await enter("short");
      assert.equal(reports().length, 2, "short password must report validation and displayed error");
      assert.deepEqual(Array.from(reports(), e => e.category), [2, 1]);
      await enter("small");
      assert.equal(reports().length, 2, "same password validation conclusion must not repeat");
      await enter("SAFE_SENTINEL_LONG_".repeat(8));
      assert.equal(reports().length, 4, "changed length error must be reported");
      assert.ok(reports().at(-1).content.includes("超过 128"));
      await enter("ACCEPTABLE_SECRET");
      assert.equal(reports().length, 4, "acceptable password must not produce an error");
      await enter("");
      await enter("short");
      assert.equal(reports().length, 6, "a new validation episode must be reported");
      assert.ok(p.fixture.pageEvents.every(e => !/short|small|SAFE_SENTINEL|ACCEPTABLE_SECRET/.test(e.content)),
        "password validation reports must never include input values");
    } finally { p.close(); }
  },
  async creationSpecialMappings() {
    for (const kind of ["unmounted", "cleanup", "partition", "cancelled", "success"]) {
      let reads = 0;
      const code = kind === "cleanup" ? 15 : 12;
      const p = await page((window, fixture, bridge) => { bridge.getCreationError = () => { ++reads; return code; }; });
      try {
        const d = p.window.document;
        d.querySelector("[data-create-container]").click();
        await wait(() => !d.getElementById("partition-refresh").disabled && d.querySelector("#partition-list-body button"));
        if (kind === "partition") {
          d.querySelector("#partition-list-body button").click();
          await wait(() => !d.getElementById("create-volume-continue").disabled);
          d.getElementById("create-volume-continue").click();
          d.getElementById("partition-erase-consent").click();
        } else {
          d.querySelector('[data-create-storage="file"]').click();
          for (const [id, value] of [["create-container-name", "audit"], ["create-container-directory", "D:\\Audit"]]) {
            const input = d.getElementById(id); input.value = value;
            input.dispatchEvent(new p.window.Event("input", {bubbles: true}));
          }
        }
        d.getElementById("create-volume-continue").click();
        await wait(() => p.fixture.pendingCreation);
        if (kind === "unmounted" || kind === "partition")
          p.fixture.containers.push({id: 99, name: "audit", mounted: false, size: 0, drive: "", partition: kind === "partition"});
        if (kind === "success") p.fixture.pendingCreation.resolve();
        else p.fixture.creationProgress = `${kind === "cleanup" ? 5 : kind === "cancelled" ? 4 : 3},3,41,0,3`;
        const status = d.getElementById("create-volume-status");
        const tone = kind === "cancelled" ? "cancelled" : kind === "success" ? "success" : "error";
        await wait(() => status.dataset.tone === tone);
        await sleep();
        const reports = p.fixture.pageEvents.filter(e => e.category === 1 && e.source === status.id);
        if (tone === "error") {
          assert.equal(reads, 1, `${kind}: read the native error exactly once`);
          assert.equal(reports.length, 1, `${kind}: report a single displayed failure`);
          assert.ok(reports[0].content.includes(`原生错误码 ${code} → ${status.textContent}`), `${kind}: native code must map to actual custom copy`);
        } else {
          assert.equal(reads, 0, `${kind}: do not query an error for a normal outcome`);
          assert.equal(reports.length, 0, `${kind}: normal outcome must not report an error`);
        }
      } finally { p.close(); }
    }
  },
  async startupMapping() {
    const p = await page((window, fixture) => {
      fixture.state = 4;
      window.usbVaultStartupFailure = {code: 2101};
    });
    try {
      assert.ok(p.fixture.pageEvents.some(e => e.category === 1 &&
        e.content.includes("原生启动码 2101") && e.content.includes("错误码：2101")),
        "startup reports must join the native code to the displayed code");
    } finally { p.close(); }
  },
  async inputValidation() {
    const p = await page();
    try {
      p.window.document.querySelector("[data-create-container]").click();
      await wait(() => !p.window.document.getElementById("partition-refresh").disabled);
      p.window.document.querySelector('[data-create-storage="file"]').click();
      const target = p.window.document.getElementById("create-container-name-error");
      const input = p.window.document.getElementById("create-container-name");
      input.value = "invalid/name";
      input.dispatchEvent(new p.window.Event("input", {bubbles: true}));
      await sleep();
      assert.ok([1, 2].every(category => p.fixture.pageEvents.some(e => e.category === category &&
        e.source === target.id && e.content.includes(target.textContent))),
        "visible input validation must report both the failure and its displayed text");
    } finally { p.close(); }
  },
  async display() {
    const p = await page();
    try {
      const target = p.window.document.getElementById("dashboard-status");
      target.dataset.tone = "success"; target.textContent = "操作成功";
      await sleep();
      assert.equal(p.fixture.pageEvents.length, 0, "success must not produce an error event");
      target.hidden = true; target.dataset.tone = "error"; target.textContent = "错误码：1234";
      await sleep();
      assert.equal(p.fixture.pageEvents.length, 0, "hidden errors must not claim to be displayed");
      target.hidden = false;
      await wait(() => p.fixture.pageEvents.length === 1);
      assert.match(p.fixture.pageEvents[0].content, /显示错误码 1234.*错误码：1234/, "displayed code must be searchable");
      target.textContent = "错误码：1234";
      await sleep();
      assert.equal(p.fixture.pageEvents.length, 1, "duplicate render must not repeat an error");
      target.textContent = "错误码：1235";
      await wait(() => p.fixture.pageEvents.length === 2);
      target.textContent = "";
      await sleep();
      target.textContent = "错误码：1235";
      await wait(() => p.fixture.pageEvents.length === 3);
      target.textContent = "长错误\r\n" + "A".repeat(4000) + "（错误码1236）";
      await wait(() => p.fixture.pageEvents.length === 4);
      assert.ok(p.fixture.pageEvents.every(e => e.category === 1 && e.source === "dashboard-status" &&
        e.content.length <= 2000 && !/[\r\n]/.test(e.content)), "display reports must be bounded single-line events");
      assert.match(p.fixture.pageEvents[3].content, /显示错误码 1236/, "truncation must retain the displayed code");
    } finally { p.close(); }
  },
  async globals() {
    const p = await page();
    try {
      scriptError(p.window);
      const error = new p.window.TypeError("invalid creation progress");
      Object.defineProperty(error, "stack", {value: "SECRET_STACK_HEADER\n" +
        " at secretName (https://usb-vault.local/card_session.js:35:7)\n".repeat(20)});
      rejection(p.window, error);
      assert.equal(p.fixture.pageEvents.length, 2, "both global listeners must remain active after startup");
      assert.ok(p.fixture.pageEvents.every(e => e.category === 3), "global failures must use category three");
      assert.match(p.fixture.pageEvents[0].content, /invalid container status.*文件 card_session.js；行 25；列 3/);
      assert.match(p.fixture.pageEvents[1].content, /TypeError；invalid creation progress/);
      assert.equal((p.fixture.pageEvents[1].content.match(/card_session\.js:35:7/g) || []).length, 6,
        "stack frames must be truncated");
      assert.ok(!p.fixture.pageEvents[1].content.includes("secretName") &&
        !p.fixture.pageEvents[1].content.includes("SECRET_STACK_HEADER"), "stack payload must not leak");
      assert.equal(p.window.usbVaultPageStartupStatus, 1, "runtime errors must not rewrite startup success");
    } finally { p.close(); }
  },
  async secrets() {
    const p = await page();
    try {
      const sentinel = "PAGE_SECRET_7f9341";
      for (const id of ["card-password", "card-password-confirmation", "create-container-b-second",
        "create-container-b-second-confirmation", "hidden-create-review-password", "decoy-password",
        "setup-user-sn", "setup-api-key", "setup-user-pin"]) {
        const input = p.window.document.getElementById(id);
        assert.ok(input, `credential fixture missing: ${id}`);
        input.value = sentinel;
      }
      const error = new p.window.Error(sentinel);
      for (const input of p.window.document.querySelectorAll("input")) input.value = "";
      rejection(p.window, error);
      rejection(p.window, {password: sentinel, toString() { throw Error("do not coerce"); }});
      rejection(p.window, sentinel);
      let getterCalls = 0;
      rejection(p.window, {get message() { ++getterCalls; return sentinel; }, get stack() { ++getterCalls; return sentinel; }});
      assert.equal(p.fixture.pageEvents.length, 4, "unsafe rejections must still leave safe diagnostic events");
      assert.equal(getterCalls, 0, "diagnostics must not execute rejection getters");
      assert.ok(p.fixture.pageEvents.every(e => !e.content.includes(sentinel) &&
        e.content.includes("安全白名单")), "cleared credential values must not survive in diagnostics");
    } finally { p.close(); }
  },
  async queue() {
    const p = await page((window, fixture, bridge) => {
      let first = true;
      Object.defineProperty(window.chrome.webview.hostObjects, "nativeBridge", {get() {
        if (first) {
          first = false;
          for (let i = 0; i < 60; ++i) scriptError(window, "invalid container status", i + 1);
        }
        return bridge;
      }});
    });
    try {
      assert.equal(p.fixture.pageEvents.length, 50, "pre-bridge buffering must stop at fifty events");
      assert.match(p.fixture.pageEvents[0].content, /行 1；列 3/);
      assert.match(p.fixture.pageEvents[49].content, /行 50；列 3/);
      await sleep(300);
      assert.equal(p.fixture.pageEvents.length, 50, "flushed events must not be replayed");
      assert.equal(p.window.usbVaultPageStartupStatus, 1408, "startup exceptions must preserve watchdog failure");
    } finally { p.close(); }
  },
  async lateBridge() {
    const p = await page((window) => { window.chrome.webview.hostObjects.nativeBridge = undefined; });
    try {
      assert.equal(p.fixture.pageEvents.length, 0, "absent bridge must not claim delivery");
      p.window.chrome.webview.hostObjects.nativeBridge = p.bridge;
      await wait(() => p.fixture.pageEvents.some(e => e.category === 1 && e.content.includes("1402")));
      assert.ok(p.fixture.pageEvents.some(e => e.category === 2), "late bridge must receive buffered startup errors");
    } finally { p.close(); }
  },
  async reportingFailure() {
    let attempts = 0;
    const p = await page((window, fixture, bridge) => {
      bridge.reportPageEvent = () => { ++attempts; return window.Promise.reject(new window.Error("invalid status")); };
    });
    try {
      scriptError(p.window);
      await sleep(40);
      assert.equal(attempts, 1, "rejected reporting must not recurse");
      p.bridge.reportPageEvent = () => { ++attempts; throw new p.window.Error("invalid status"); };
      scriptError(p.window);
      await sleep(40);
      assert.equal(attempts, 2, "synchronous reporting failure must not recurse");
      assert.equal(p.window.document.getElementById("authenticated-dashboard").hidden, false,
        "reporting failure must not change the product flow");
    } finally { p.close(); }
  },
  async invalidBridgeData() {
    const p = await page((window, fixture, bridge) => { bridge.getContainerMounted = () => "invalid"; });
    try {
      assert.ok(p.fixture.pageEvents.some(e => e.category === 2 && e.content.includes("invalid container status")),
        "invalid container facts must produce a validation event");
      assert.ok(p.fixture.pageEvents.some(e => e.category === 1 && e.content.includes("页面启动阶段码 1405")),
        "startup fallback must be distinguished from a native error");
    } finally { p.close(); }
  },
  async configuration() {
    const p = await page((window, fixture, bridge) => {
      bridge.getStartupView = () => "configuration";
      bridge.getCredentialStatus = () => 2;
      bridge.continueToSession = () => { throw new window.Error("CONFIGURATION_SECRET"); };
    });
    try {
      assert.ok(p.fixture.pageEvents.some(e => e.category === 1 && e.source === "setup-status" &&
        e.content.includes("1108")), "configuration errors must use the configuration bridge");
      p.window.document.getElementById("setup-back").click();
      await wait(() => p.fixture.pageEvents.some(e => e.category === 2));
      assert.ok(p.fixture.pageEvents.every(e => !e.content.includes("CONFIGURATION_SECRET")),
        "configuration failures must not serialize credentials");
    } finally { p.close(); }
  },
};
(async () => {
  let count = 0;
  for (const [name, run] of Object.entries(cases)) {
    if (process.argv[3] && process.argv[3] !== name) continue;
    await run();
    ++count;
    console.log(`PASS: ${name}`);
  }
  assert.ok(count > 0, "no page event cases selected");
  console.log(`PASS: ${count} page event scenarios`);
})().catch(error => { console.error(error); process.exitCode = 1; });
