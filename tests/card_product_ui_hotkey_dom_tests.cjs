const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const { JSDOM } = require(path.resolve(process.argv[2]));
const read = file => fs.readFileSync(path.resolve(__dirname, '..', file), 'utf8');
const settle = () => new Promise(resolve => setTimeout(resolve, 100));
(async () => {
 for (const scenario of ['save', 'failure', 'readback', 'unauthenticated']) {
  const dom = new JSDOM(read('ui/card_session.html'), {runScripts:'outside-only', pretendToBeVisual:true});
  const w = dom.window; w.TextEncoder = TextEncoder;
  try {
   w.eval(read('tests/card_product_ui_navigation_mock.js'));
   w.navigationFixture.state = scenario === 'unauthenticated' ? 2 : 3;
   if (scenario === 'readback') w.navigationFixture.exitHotkey = 65536 + 5 * 256 + 65;
   const bridge = w.chrome.webview.hostObjects.nativeBridge, calls = [];
   bridge.setExitHotkey = (enabled, key, count) => {
    calls.push([enabled, key, count]);
    if (scenario === 'failure') throw new Error('denied');
    w.navigationFixture.exitHotkey = (enabled ? 65536 : 0) + count * 256 + key;
   };
   w.eval(read('ui/card_session.js')); await settle();
   const el = id => w.document.getElementById(id), toggle = el('exit-hotkey-enabled');
   if (scenario === 'readback') {
    assert.equal(toggle.getAttribute('aria-checked'), 'true');
    assert.equal(el('exit-hotkey-key').value, '65'); assert.equal(el('exit-hotkey-count').value, '5');
    toggle.click(); await settle(); assert.deepEqual(calls, [[false,65,5]]);
    assert.equal(el('exit-hotkey-options').hidden, true); continue;
   }
   assert.equal(el('exit-hotkey-options').hidden, true);
   toggle.click(); toggle.click(); await settle();
   if (scenario === 'unauthenticated') { assert.deepEqual(calls, []); continue; }
   assert.deepEqual(calls, [[true,123,3]]);
   if (scenario === 'failure') {
    assert.equal(toggle.getAttribute('aria-checked'), 'false');
    assert.equal(el('exit-hotkey-options').hidden, true);
    assert.match(el('exit-hotkey-status').textContent, /无法保存/); continue;
   }
   assert.equal(el('exit-hotkey-options').hidden, false);
   el('exit-hotkey-key').value = '66'; el('exit-hotkey-count').value = '2';
   el('exit-hotkey-save').click(); await settle();
   assert.deepEqual(calls[1], [true,66,2]);
   assert.equal(el('exit-hotkey-key').value, '66');
   toggle.click(); await settle(); assert.deepEqual(calls[2], [false,66,2]);
  } finally { w.close(); }
 }
 console.log('exit hotkey DOM: 4 scenarios passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
