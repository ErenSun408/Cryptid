const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const { JSDOM } = require(path.resolve(process.argv[2]));
const read = file => fs.readFileSync(path.resolve(__dirname, '..', file), 'utf8');
const settle = () => new Promise(resolve => setTimeout(resolve, 100));
(async () => {
 for (const scenario of ['enable', 'disable', 'failure', 'unauthenticated']) {
  const dom = new JSDOM(read('ui/card_session.html'), {runScripts:'outside-only', pretendToBeVisual:true});
  const w = dom.window; w.TextEncoder = TextEncoder;
  try {
   w.eval(read('tests/card_product_ui_navigation_mock.js'));
   const fixture = w.navigationFixture;
   fixture.state = scenario === 'unauthenticated' ? 2 : 3;
   fixture.closeToTray = scenario === 'disable';
   const bridge = w.chrome.webview.hostObjects.nativeBridge, writes = [];
   let exits = 0;
   bridge.setCloseToTrayEnabled = value => {
    writes.push(value);
    if (scenario === 'failure') throw new Error('cannot save');
    fixture.closeToTray = value;
   };
   bridge.exitApplication = () => { exits++; };
   w.eval(read('ui/card_session.js')); await settle();
   const toggle = w.document.querySelector('[data-setting="close-to-tray"]');
   assert.equal(toggle.getAttribute('aria-checked'), String(fixture.closeToTray));
   toggle.click(); await settle();
   if (scenario === 'unauthenticated') { assert.deepEqual(writes, []); continue; }
   assert.deepEqual(writes, [scenario !== 'disable']);
   assert.equal(toggle.getAttribute('aria-checked'), String(scenario === 'enable'));
   if (scenario === 'failure') assert.match(w.document.getElementById('dashboard-status').textContent, /无法更改/);
   if (scenario === 'enable') {
    w.document.getElementById('app-exit-toggle').click();
    w.document.getElementById('exit-application').click(); await settle();
    assert.equal(exits, 1, 'tray preference swallowed explicit exit');
   }
  } finally { w.close(); }
 }
 console.log('tray settings DOM: 4 scenarios passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
