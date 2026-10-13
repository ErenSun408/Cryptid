// node tests/card_product_ui_card_backup_dom_tests.cjs <local jsdom path>
// DOM only: no native host, credentials, card, or mounted filesystem.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {JSDOM} = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, '..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8');
const settle = () => new Promise(resolve => setTimeout(resolve, 30));
async function run(state, fail) {
  const dom = new JSDOM(read('ui/card_session.html'), {runScripts:'outside-only'});
  const w = dom.window; w.TextEncoder = TextEncoder;
  try {
    w.eval(read('tests/card_product_ui_navigation_mock.js'));
    w.navigationFixture.state = state;
    const bridge = w.chrome.webview.hostObjects.nativeBridge;
    let backups=0, logins=0, unmounts=0;
    bridge.submitLoginPasswordBytes = () => { ++logins; };
    bridge.unmountAll = () => { ++unmounts; };
    bridge.openCardBackup = async () => { ++backups; if(fail) throw new Error('injected refusal'); };
    w.eval(read('ui/card_session.js')); await settle();
    const button=w.document.getElementById('card-backup-settings');
    if(state===3) {
      w.document.querySelector('[data-section="settings"]').click(); await settle();
      assert.equal(w.document.getElementById('dashboard-section-settings').hidden,false);
      assert.equal(button.closest('.settings-group').querySelector('h3').textContent,'安全');
    }
    assert.equal(button.disabled,false);
    button.click(); await settle();
    assert.equal(backups,1); assert.equal(logins,0); assert.equal(unmounts,0);
    assert.equal(w.document.getElementById('card-password').value,'');
    if(fail) assert.equal(button.disabled,false,'refusal must allow retry');
    else {
      button.click(); await settle(); assert.equal(backups,1,'pending wizard must exclude duplicate requests');
      w.dispatchEvent(new w.Event('usb-vault-card-backup-closed')); await settle();
      assert.equal(button.disabled,false,'cancellation must restore controls');
      if(state===3) assert.equal(w.document.getElementById('dashboard-section-settings').hidden,false,'cancellation must preserve settings page');
      button.click(); await settle(); assert.equal(backups,2,'cancelled wizard must allow reopening');
      if(state===3) {
        const document=w.document;
        w.navigationFixture.state=2;
        w.dispatchEvent(new w.CustomEvent('usb-vault-card-backup-closed',{detail:{sessionChanged:true}}));
        await settle(); await settle();
        assert.equal(w.document,document,'card exchange return must keep the document');
        assert.equal(w.document.getElementById('card-password').disabled,false,'return must show ready password input');
        assert.equal(w.document.body.dataset.view,'session','revoked session must leave dashboard');
        assert.equal(logins,0,'metadata handoff must not authenticate');
      }
    }
  } finally { w.close(); }
}
async function checkLoginMenu(state, available) {
  const dom = new JSDOM(read('ui/card_session.html'), {runScripts:'outside-only'});
  const w = dom.window; w.TextEncoder = TextEncoder;
  try {
    w.eval(read('tests/card_product_ui_navigation_mock.js'));
    w.navigationFixture.state = state;
    const bridge = w.chrome.webview.hostObjects.nativeBridge;
    bridge.canLoginReset = () => available;
    let backups = 0, resets = 0;
    bridge.openCardBackup = () => { ++backups; };
    bridge.prepareLoginReset = () => { ++resets; };
    w.eval(read('ui/card_session.js')); await settle();
    assert.equal(w.document.getElementById('card-backup-login'), null);
    const tools = w.document.getElementById('session-tools');
    assert.equal(tools.hidden, false, 'login/error menu remains available');
    w.document.getElementById('session-tools-toggle').click();
    assert.equal(w.document.getElementById('session-tools-menu').hidden, false);
    const forgot = w.document.getElementById('forgot-password-open');
    assert.equal(forgot.hidden, false, 'forgot password stays visible on errors');
    assert.equal(forgot.disabled, !available, 'native reset availability still controls permission');
    forgot.click(); await settle();
    assert.equal(w.document.getElementById('forgot-password-dialog').hidden, !available);
    assert.equal(backups, 0);
    assert.equal(resets, 0, 'opening the menu or dialog must not reset the card');
  } finally { w.close(); }
}
(async()=>{
  await run(3,false); await run(3,true);
  for (const state of [0,1,2,4]) {
    await checkLoginMenu(state,false); await checkLoginMenu(state,true);
  }
  console.log('PASS: settings-only backup entry, cancellation, login/error forgot-password visibility and native reset gating');
})().catch(error=>{console.error(error);process.exitCode=1;});

