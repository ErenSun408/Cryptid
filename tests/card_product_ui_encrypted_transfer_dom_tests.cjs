// P5.9 backup centre: encrypted export and the two import paths, driven
// through the page against a mocked bridge. Usage: node <this> <jsdom path>
const fs = require('fs'), assert = require('node:assert/strict');
const {JSDOM} = require(require('path').resolve(process.argv[2]));
const tick = () => new Promise(r => setTimeout(r, 30));
const wait = async predicate => { for (let i = 0; i < 200; i++) { if (predicate()) return; await tick(); } throw Error('UI timeout'); };

const load = () => {
  const dom = new JSDOM(fs.readFileSync('ui/card_session.html', 'utf8'), {runScripts: 'outside-only', pretendToBeVisual: true});
  const w = dom.window;
  w.TextEncoder = TextEncoder;
  w.eval(fs.readFileSync('tests/card_product_ui_navigation_mock.js', 'utf8'));
  const bridge = w.chrome.webview.hostObjects.nativeBridge;
  const calls = [];
  // One transfer at a time; `script` is the sequence of status strings it
  // reports before settling on its last one.
  let script = [];
  const start = (name) => (...args) => {
    calls.push({name, args: args.map(a => Array.isArray(a) ? [...a] : a), arrays: args.filter(Array.isArray)});
    return true;
  };
  bridge.beginEncryptedExport = start('export');
  bridge.beginEncryptedImport = start('import');
  bridge.beginVerifyImport = start('verify');
  bridge.beginImportIntoNewContainerMib = start('new');
  bridge.beginImportIntoNewContainerBMib = start('newB');
  bridge.encryptedTransferStatus = () => script.length > 1 ? script.shift() : (script[0] || '0,0,0,0,0');
  bridge.cancelEncryptedTransfer = () => { calls.push({name: 'cancel'}); script = ['4,0,0,0,1']; return true; };
  bridge.chooseImportPackage = () => 'family photos';
  bridge.getVerifiedImportMinimumMib = () => 120;
  w.navigationFixture.chosenLocation = 'D:\\Vaults';
  w.eval(fs.readFileSync('ui/card_session.js', 'utf8'));
  return {w, d: w.document, $: id => w.document.getElementById(id), calls, setScript: s => { script = s; }};
};

(async () => {
  // Encrypted export: passwords leave their fields, each entry gets its own
  // arrays, which the page wipes after the bridge has them.
  {
    const {w, d, $, calls, setScript} = load();
    try {
      await tick();
      d.querySelector('[data-section=backup]').click();
      $('export-mode-encrypted').click(); await tick();
      assert.equal($('export-encrypted-fields').hidden, false);
      assert.equal($('import-pane').hidden, true);
      d.querySelector('[data-export-select-all="1"]').click(); await tick();
      $('backup-export-location').click(); await tick();
      assert.match($('backup-export-confirm').textContent, /^开始加密导出（\d+）$/);
      $('backup-export-confirm').click(); await tick();
      assert.equal(calls.length, 0, 'an export without passwords must not start');
      assert.match($('backup-export-status').textContent, /导出密码/);
      setScript(['1,0,10,0,2', '2,10,10,0,2']);
      $('encrypted-export-password').value = 'long enough export pass';
      $('encrypted-export-confirmation').value = 'long enough export pass';
      $('backup-export-confirm').click();
      await wait(() => !$('export-result-confirm').disabled);
      const exports = calls.filter(c => c.name === 'export');
      assert.ok(exports.length >= 1);
      assert.equal($('encrypted-export-password').value, '');
      assert.equal($('encrypted-export-confirmation').value, '');
      for (const call of exports) {
        assert.equal(call.args[3], 'D:\\Vaults');
        assert.equal(call.args[4], call.args[2], 'the package is named after its entry');
        assert.equal(String.fromCharCode(...call.args[5]), 'long enough export pass');
        for (const array of call.arrays) assert.ok(array.every(b => b === 0), 'export password arrays must be wiped');
      }
    } finally { w.close(); }
  }

  // Encrypted export failure maps to its message.
  {
    const {w, d, $, setScript} = load();
    try {
      await tick();
      d.querySelector('[data-section=backup]').click();
      $('export-mode-encrypted').click(); await tick();
      d.querySelector('[data-export-select-all="1"]').click(); await tick();
      $('backup-export-location').click(); await tick();
      setScript(['3,0,0,4,0']);
      $('encrypted-export-password').value = 'long enough export pass';
      $('encrypted-export-confirmation').value = 'long enough export pass';
      $('backup-export-confirm').click();
      await wait(() => !$('export-result-confirm').disabled);
      assert.match($('backup-export-status').textContent, /同名/);
    } finally { w.close(); }
  }

  // Stopping an encrypted export cancels the running package and does not
  // start the rest.
  {
    const {w, d, $, calls, setScript} = load();
    try {
      await tick();
      d.querySelector('[data-section=backup]').click();
      $('export-mode-encrypted').click(); await tick();
      d.querySelector('[data-export-select-all="1"]').click(); await tick();
      $('backup-export-location').click(); await tick();
      setScript(['1,0,10,0,2']);
      $('encrypted-export-password').value = 'long enough export pass';
      $('encrypted-export-confirmation').value = 'long enough export pass';
      $('backup-export-confirm').click();
      await wait(() => calls.some(c => c.name === 'export'));
      assert.equal($('export-result-cancel').hidden, false);
      $('export-result-cancel').click();
      await wait(() => !$('export-result-confirm').disabled);
      assert.equal(calls.filter(c => c.name === 'export').length, 1, 'a stopped run starts nothing more');
      assert.ok(calls.some(c => c.name === 'cancel'));
      assert.equal($('export-result-cancel').hidden, true);
    } finally { w.close(); }
  }

  // Import into an existing vault: only mounted vaults are offered, the
  // pick is shown by name, and success clears the pick and password.
  {
    const {w, d, $, calls, setScript} = load();
    try {
      await tick();
      d.querySelector('[data-section=backup]').click();
      $('export-mode-import').click(); await tick();
      assert.equal($('import-pane').hidden, false);
      assert.equal($('export-plain-pane').hidden, true);
      assert.equal($('import-container-list').querySelectorAll('.xrow').length, 2, 'only mounted vaults are offered');
      assert.equal($('import-confirm').disabled, true);
      $('import-package-browse').click(); await tick();
      assert.equal($('import-package-name').textContent, 'family photos');
      $('import-container-list').querySelector('.xrow-open').click(); await tick();
      assert.equal($('import-confirm').disabled, false);
      $('import-confirm').click(); await tick();
      assert.equal(calls.length, 0, 'an import without a password must not start');
      setScript(['1,5,10,0,1', '1,5,10,0,2', '2,10,10,0,2']);
      $('import-password').value = 'long enough export pass';
      $('import-confirm').click();
      await wait(() => /导入完成/.test($('import-status').textContent));
      const call = calls.find(c => c.name === 'import');
      assert.equal(typeof call.args[0], 'number');
      assert.equal(call.args[1], '');
      assert.ok(call.arrays[0].every(b => b === 0), 'the import password array must be wiped');
      assert.equal($('import-password').value, '');
      assert.equal($('import-package-name').textContent, '选择一个导出包…');
    } finally { w.close(); }
  }

  // Import into a new vault: verify first, then the form appears with the
  // minimum size; a failure resets the pick; cancel reaches the bridge.
  {
    const {w, d, $, calls, setScript} = load();
    try {
      await tick();
      d.querySelector('[data-section=backup]').click();
      $('export-mode-import').click(); await tick();
      $('import-target-new').click(); await tick();
      $('import-package-browse').click(); await tick();
      assert.equal($('import-confirm').textContent, '校验导出包');
      assert.equal($('import-new-fields').hidden, true);
      setScript(['1,3,10,0,1', '2,10,10,0,1']);
      $('import-password').value = 'long enough export pass';
      $('import-confirm').click();
      await wait(() => /校验通过/.test($('import-status').textContent));
      assert.equal(calls.filter(c => c.name === 'verify').length, 1);
      assert.equal($('import-new-fields').hidden, false);
      assert.equal($('import-new-size').value, '120');
      assert.equal($('import-confirm').textContent, '新建安全盘并导入');
      assert.equal($('import-confirm').disabled, true, 'a new vault needs a name and a location');
      $('import-new-name').value = 'Restored';
      $('import-new-name').dispatchEvent(new w.Event('input'));
      $('import-new-browse').click(); await tick();
      assert.equal($('import-confirm').disabled, false);
      $('import-new-size').value = '100';
      $('import-new-size').dispatchEvent(new w.Event('input'));
      assert.equal($('import-confirm').disabled, true, 'a size below the minimum is refused');
      $('import-new-size').value = '200';
      $('import-new-size').dispatchEvent(new w.Event('input'));
      assert.equal($('import-password').disabled, true, 'a used password field is not offered again');
      // The hidden-volume shape is held to the creation form's rules.
      $('import-new-hidden').checked = true;
      $('import-new-hidden').dispatchEvent(new w.Event('change'));
      $('import-new-hidden-size').value = '160';
      $('import-new-hidden-size').dispatchEvent(new w.Event('input'));
      $('import-new-second').value = 'decoy password 12';
      $('import-new-second-confirmation').value = 'decoy password 12';
      $('import-new-second').dispatchEvent(new w.Event('input'));
      $('import-new-acknowledge').checked = true;
      $('import-new-acknowledge').dispatchEvent(new w.Event('change'));
      assert.equal($('import-confirm').disabled, true, 'an inner volume over 3/4 is refused');
      $('import-new-hidden-size').value = '150';
      $('import-new-hidden-size').dispatchEvent(new w.Event('input'));
      assert.equal($('import-confirm').disabled, false);
      $('import-new-acknowledge').checked = false;
      $('import-new-acknowledge').dispatchEvent(new w.Event('change'));
      assert.equal($('import-confirm').disabled, true, 'the consequence must be acknowledged');
      $('import-new-acknowledge').checked = true;
      $('import-new-second').value = 'short';
      $('import-new-second').dispatchEvent(new w.Event('input'));
      assert.equal($('import-confirm').disabled, true, 'a short second password is refused');
      $('import-new-hidden').checked = false;
      $('import-new-hidden').dispatchEvent(new w.Event('change'));
      setScript(['1,1,10,0,2']);
      $('import-confirm').click();
      await wait(() => $('import-cancel').hidden === false);
      $('import-cancel').click();
      await wait(() => /已取消/.test($('import-status').textContent));
      const created = calls.find(c => c.name === 'new');
      assert.deepEqual(created.args, ['Restored', 'D:\\Vaults', 200]);
      assert.ok(calls.some(c => c.name === 'cancel'));
      assert.equal($('import-new-fields').hidden, true, 'a finished attempt needs a fresh verification');
      assert.equal($('import-confirm').textContent, '校验导出包');
    } finally { w.close(); }
  }

  // A session change forgets every import field.
  {
    const {w, d, $} = load();
    try {
      await tick();
      d.querySelector('[data-section=backup]').click();
      $('export-mode-import').click(); await tick();
      $('import-package-browse').click(); await tick();
      $('import-target-new').click(); await tick();
      $('import-password').value = 'typed';
      $('import-new-name').value = 'previous';
      $('import-new-hidden').checked = true;
      $('encrypted-export-password').value = 'typed';
      w.dispatchEvent(new w.Event('usb-vault-session-changed')); await tick();
      assert.equal($('import-password').value, '');
      assert.equal($('encrypted-export-password').value, '');
      assert.equal($('import-package-name').textContent, '选择一个导出包…');
      assert.equal($('import-new-name').value, '');
      assert.equal($('import-new-hidden').checked, false);
      assert.equal($('import-target-existing').getAttribute('aria-pressed'), 'true');
    } finally { w.close(); }
  }
  console.log('PASS: encrypted export, export failure text, export stop, import into an existing vault, verify-then-create with hidden-volume rules and cancel, session reset');
})().catch(e => { console.error(e); process.exitCode = 1; });
