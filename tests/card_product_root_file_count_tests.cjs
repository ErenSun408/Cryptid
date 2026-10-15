const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const source = fs.readFileSync(path.join(__dirname, "../ui/card_session.js"), "utf8");
const start = source.indexOf("const readContainerSnapshot = async");
const end = source.indexOf("const refreshNativeState = async", start);
assert.ok(start >= 0 && end > start);
const makeReader = new Function("bridge", "readContainerRoot", "displayContainerName",
  "isMaintenanceContainer", `${source.slice(start, end)}; return readContainerSnapshot;`);

function testActivationRefresh() {
  const handlers = new Map();
  const window = { addEventListener: (name, handler) => handlers.set(name, handler) };
  const document = { hidden: false,
    addEventListener: (name, handler) => handlers.set(name, handler) };
  let now = 3000;
  let requests = 0;
  const start = source.lastIndexOf('window.addEventListener("usb-vault-window-activated"');
  const end = source.indexOf('window.addEventListener("pagehide"', start);
  assert.ok(start >= 0 && end > start);
  new Function("window", "document", "Date", "scheduleDecoyActivationRefresh",
    "requestContainerInventoryRefresh", `let decoyLastActivationNotice = 0;
      const kDecoyActivationGapMs = 2500;
      ${source.slice(start, end)}`)(window, document, { now: () => now },
        () => {}, () => requests++);
  handlers.get("usb-vault-window-activated")();
  assert.equal(requests, 1);
  for (now of [4000, 5000, 6000]) handlers.get("usb-vault-window-activated")();
  assert.equal(requests, 1);
  handlers.get("blur")();
  now = 7000;
  handlers.get("usb-vault-window-activated")();
  assert.equal(requests, 2);
  handlers.get("focus")();
  assert.equal(requests, 3);
  document.hidden = true;
  handlers.get("visibilitychange")();
  assert.equal(requests, 3);
  document.hidden = false;
  handlers.get("visibilitychange")();
  assert.equal(requests, 4);
}

function testRefreshCoalescing() {
  const scheduleStart = source.indexOf("const scheduleMountRefresh =");
  const scheduleEnd = source.indexOf("const refreshMountInventory =", scheduleStart);
  const requestStart = source.indexOf("const requestContainerInventoryRefresh =");
  const requestEnd = source.indexOf('window.addEventListener("usb-vault-mounts-changed"', requestStart);
  assert.ok(scheduleStart >= 0 && scheduleEnd > scheduleStart && requestStart >= 0 && requestEnd > requestStart);
  const makeRequest = new Function("bridge", "currentState", "mountRefreshDisposed",
    "setTimeout", "refreshMountInventory", `const nativeState = { authenticated: 42 };
      let mountRefreshPending = false;
      let mountRefreshTimer = null;
      ${source.slice(scheduleStart, scheduleEnd)}
      ${source.slice(requestStart, requestEnd)}
      return requestContainerInventoryRefresh;`);
  const timers = [];
  let refreshes = 0;
  const timer = (callback, delay) => { assert.equal(delay, 200); timers.push(callback); return 1; };
  const request = makeRequest({}, 42, false, timer, () => refreshes++);
  request(); request(); request();
  assert.equal(timers.length, 1);
  timers.shift()();
  assert.equal(refreshes, 1);
  for (const [bridge, state, disposed] of [[null, 42, false], [{}, 0, false], [{}, 42, true]]) {
    makeRequest(bridge, state, disposed, timer, () => refreshes++)();
  }
  assert.equal(timers.length, 0);
}

async function snapshot({ mounted = true, unavailable = false, maintenance = false,
  listing = { truncated: false, entries: [] } } = {}) {
  let reads = 0;
  const bridge = {
    getContainerCount: async () => 1,
    getContainerId: async () => 1,
    getContainerName: async () => "sample",
    getContainerMounted: async () => mounted,
    getContainerSizeMib: async () => unavailable || !mounted ? 0 : 64,
    getContainerDriveLetter: async () => mounted ? "D:" : "",
    getContainerPartition: async () => false,
    getContainerConversion: async () => 0,
    getContainerVolumeUnavailable: async () => unavailable,
  };
  const read = makeReader(bridge, async id => {
    assert.equal(id, 1);
    reads++;
    return listing;
  }, name => name, () => maintenance);
  const [container] = await read(mounted);
  return { count: container.rootItemCount, reads };
}

(async () => {
  testActivationRefresh();
  testRefreshCoalescing();
  assert.deepEqual(await snapshot(), { count: 0, reads: 1 });
  assert.deepEqual(await snapshot({ listing: { truncated: false, entries: [
    { isDirectory: false }, { isDirectory: true }, { isDirectory: false },
  ] } }), { count: 3, reads: 1 });
  for (const state of [{ mounted: false }, { unavailable: true }, { maintenance: true }]) {
    assert.deepEqual(await snapshot(state), { count: null, reads: 0 });
  }
  assert.deepEqual(await snapshot({ listing: null }), { count: null, reads: 1 });
  assert.deepEqual(await snapshot({ listing: { truncated: true,
    entries: [{ isDirectory: false }] } }), { count: null, reads: 1 });
  console.log("card_product_root_file_count_tests: PASS");
})().catch(error => { console.error(error); process.exitCode = 1; });
