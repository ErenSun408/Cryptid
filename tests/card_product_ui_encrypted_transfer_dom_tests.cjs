// P5.9 export page and generic import: the sticky toolbar, the export dialog
// that "next" opens, cipher export, and the import button that tells
// containers from cipher export packages. Usage: node <this> <jsdom path>
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
  let script = [];
  const start = (name) => (...args) => {
    calls.push({name, args: args.map(a => Array.isArray(a) ? [...a] : a), arrays: args.filter(Array.isArray)});
    return true;
  };
  bridge.beginExportSelection = start('export');
  bridge.beginEncryptedImport = start('import');
  bridge.beginVerifyImport = start('verify');
  bridge.beginImportIntoNewContainerMib = start('new');
  bridge.beginImportIntoNewContainerBMib = start('newB');
  bridge.encryptedTransferStatus = () => script.length > 1 ? script.shift() : (script[0] || '0,0,0,0,0');
  bridge.cancelEncryptedTransfer = () => { calls.push({name: 'cancel'}); script = ['4,0,0,0,1']; return true; };
  let importChoice = '2|family photos';
  bridge.chooseImportFile = () => { calls.push({name: 'choose'}); return importChoice; };
  bridge.adoptUndecidedImportFile = () => { calls.push({name: 'adopt'}); return true; };
  bridge.treatUndecidedImportAsPackage = () => { calls.push({name: 'asPackage'}); return 'unnamed'; };
  bridge.getVerifiedImportMinimumMib = () => 120;
  bridge.renameImportPackage = name => { calls.push({name: 'rename', args: [name]}); return 0; };
  w.navigationFixture.chosenLocation = 'D:\\Vaults';
  w.eval(fs.readFileSync('ui/card_session.js', 'utf8'));
  return {w, d: w.document, $: id => w.document.getElementById(id), calls,
          setScript: s => { script = s; }, setImportChoice: c => { importChoice = c; }};
};

const openBackup = async (d, $, mode) => {
  d.querySelector('[data-section=backup]').click();
  if (mode) $(mode).click();
  await tick();
};

(async () => {
  // The toolbar: three ways and one "next", without an import entry.
  {
    const {w, d, $} = load();
    try {
      await tick();
      await openBackup(d, $);
      const toolbar = d.querySelector('#dashboard-section-backup .export-toolbar');
      assert.ok(toolbar.contains($('export-mode-plain')) && toolbar.contains($('export-mode-encrypted')) &&
                toolbar.contains($('export-mode-migrate')) && toolbar.contains($('backup-export-confirm')));
      assert.equal($('export-mode-encrypted').querySelector('h4').textContent, '密文导出');
      assert.match($('export-mode-encrypted').textContent, /不需要原安全卡/);
      assert.equal($('export-mode-migrate').querySelector('h4').textContent, '安全盘迁移');
      assert.equal($('export-mode-import'), null);
      assert.equal($('backup-export-confirm').textContent, '下一步');
      assert.equal($('backup-export-location'), null, 'no location field stays on the page');
      assert.equal($('export-merge').getAttribute('aria-checked'), 'true');
      $('export-mode-migrate').click();
      assert.equal($('export-merge').disabled, true);
      $('export-mode-plain').click();
      assert.equal($('export-merge').disabled, false);
    } finally { w.close(); }
  }

  // Cipher export: the dialog asks for location and passphrase; several
  // entries keep their own names; passphrase errors stay in the dialog.
  {
    const {w, d, $, calls, setScript} = load();
    try {
      await tick();
      await openBackup(d, $, 'export-mode-encrypted');
      d.querySelector('[data-export-select-all="1"]').click(); await tick();
      $('backup-export-confirm').click(); await tick();
      assert.equal($('export-dialog').hidden, false);
      assert.equal($('export-dialog-title').textContent, '密文导出');
      assert.equal($('export-dialog-secret-fields').hidden, false);
      assert.equal($('export-dialog-name').disabled, false, 'merged selections may be named');
      assert.equal($('export-dialog-start').disabled, true, 'a location comes first');
      $('export-dialog-location').click(); await tick();
      $('export-dialog-start').click(); await tick();
      assert.match($('export-dialog-error').textContent, /两次加密口令/);
      $('export-dialog-passphrase').value = 'long enough export pass';
      $('export-dialog-passphrase-confirmation').value = 'long enough export pazz';
      $('export-dialog-start').click(); await tick();
      assert.match($('export-dialog-error').textContent, /不一致/);
      assert.equal(calls.length, 0);
      setScript(['1,0,10,0,2', '2,10,10,0,2']);
      $('export-dialog-passphrase-confirmation').value = 'long enough export pass';
      $('export-dialog-start').click();
      await wait(() => !$('export-result-confirm').disabled);
      assert.equal($('export-dialog').hidden, true);
      assert.equal($('export-dialog-passphrase').value, '');
      const exports = calls.filter(c => c.name === 'export');
      assert.ok(exports.length >= 1);
      for (const call of exports) {
        assert.equal(call.args[2], 'D:\\Vaults');
        assert.equal(call.args[3], '导出项目');
        assert.equal(call.args[1].split('\u001e').length, 2, 'both projects enter one package');
        assert.equal(String.fromCharCode(...call.args[4]), 'long enough export pass');
        for (const array of call.arrays) assert.ok(array.every(b => b === 0), 'passphrase arrays must be wiped');
      }
    } finally { w.close(); }
  }

  for (const mode of ['plain', 'encrypted']) {
    const {w, d, $, calls, setScript} = load();
    try {
      await tick(); await openBackup(d, $, 'export-mode-' + mode);
      $('export-merge').click();
      assert.equal($('export-merge').getAttribute('aria-checked'), 'false');
      d.querySelector('[data-export-select-all="1"]').click(); await tick();
      $('backup-export-confirm').click(); await tick();
      assert.equal($('export-dialog-name-field').hidden, false);
      assert.equal($('export-dialog-name').disabled, true);
      $('export-dialog-name').value = 'ignored';
      $('export-dialog-location').click(); await tick();
      if (mode === 'encrypted') {
        $('export-dialog-passphrase').value = 'long enough export pass';
        $('export-dialog-passphrase-confirmation').value = 'long enough export pass';
      }
      setScript(['2,1,1,0,2']); $('export-dialog-start').click();
      await wait(() => !$('export-result-confirm').disabled);
      const exports = calls.filter(call => call.name === 'export');
      assert.equal(exports.length, 2);
      for (const {args} of exports) {
        assert.equal(args[0], mode === 'encrypted');
        assert.equal(args[1].split('\u001e').length, 1);
        assert.equal(args[3], args[1].split('\u001f')[2]);
        assert.notEqual(args[3], 'ignored');
        if (mode === 'plain') assert.equal(args[4].length, 0);
      }
      w.dispatchEvent(new w.Event('usb-vault-session-changed')); await tick();
      assert.equal($('export-merge').getAttribute('aria-checked'), 'true');
    } finally { w.close(); }
  }

  // One entry may be given a package name.
  {
    const {w, d, $, calls, setScript} = load();
    try {
      await tick();
      await openBackup(d, $, 'export-mode-encrypted');
      d.querySelector('[data-export-toggle="1"] .xrow-open').click();
      await wait(() => d.querySelector('#export-container-list [data-export-pick]'));
      d.querySelector('#export-container-list [data-export-pick]').click(); await tick();
      $('backup-export-confirm').click(); await tick();
      assert.equal($('export-dialog-name').disabled, false);
      $('export-dialog-name').value = '家庭照片';
      $('export-dialog-location').click(); await tick();
      $('export-dialog-passphrase').value = 'long enough export pass';
      $('export-dialog-passphrase-confirmation').value = 'long enough export pass';
      setScript(['2,1,1,0,2']);
      $('export-dialog-start').click();
      await wait(() => !$('export-result-confirm').disabled);
      assert.equal(calls.find(c => c.name === 'export').args[3], '家庭照片');
    } finally { w.close(); }
  }

  // Failure text, and stopping a run.
  {
    const {w, d, $, calls, setScript} = load();
    try {
      await tick();
      await openBackup(d, $, 'export-mode-encrypted');
      d.querySelector('[data-export-select-all="1"]').click(); await tick();
      const run = async (status) => {
        $('backup-export-confirm').click(); await tick();
        $('export-dialog-location').click(); await tick();
        $('export-dialog-passphrase').value = 'long enough export pass';
        $('export-dialog-passphrase-confirmation').value = 'long enough export pass';
        setScript(status);
        $('export-dialog-start').click();
      };
      await run(['3,0,0,4,0']);
      await wait(() => !$('export-result-confirm').disabled);
      assert.match($('backup-export-status').textContent, /同名/);
      $('export-result-confirm').click(); await tick();
      const before = calls.filter(c => c.name === 'export').length;
      await run(['1,0,10,0,2']);
      await wait(() => calls.filter(c => c.name === 'export').length === before + 1);
      assert.equal($('export-result-cancel').hidden, false);
      $('export-result-cancel').click();
      await wait(() => !$('export-result-confirm').disabled);
      assert.equal(calls.filter(c => c.name === 'export').length, before + 1, 'a stopped run starts nothing more');
    } finally { w.close(); }
  }

  // Unmounting a container clears what was selected in it.
  {
    const {w, d, $} = load();
    try {
      await tick();
      await openBackup(d, $, 'export-mode-plain');
      d.querySelector('[data-export-toggle="1"] .xrow-open').click(); await tick();
      d.querySelector('[data-export-select-all="1"]').click();
      await wait(() => !$('backup-export-confirm').disabled);
      // Unmount it from its own row on the export page.
      d.querySelector('#export-container-list [data-row-mount="1"]').click();
      await wait(() => !w.navigationFixture.containers.find(c => c.id === 1).mounted &&
                       d.querySelectorAll('#export-container-list .cbx.on').length === 0);
      assert.equal($('backup-export-confirm').disabled, true, 'nothing selected is left to export');
      assert.equal(d.querySelector('[data-export-toggle="1"]').getAttribute('aria-expanded'), 'false');
    } finally { w.close(); }
  }

  // The import button: a package opens the import dialog; importing into a
  // mounted vault remains open with success feedback until manually closed.
  {
    const {d, $, w, calls, setScript} = load();
    try {
      await tick();
      d.querySelector('[data-mount-existing]').click();
      await wait(() => !$('import-dialog').hidden);
      assert.equal($('import-package-name').value, 'family photos');
      assert.equal($('import-container-list').querySelectorAll('.xrow').length, 3, 'all vaults are offered');
      assert.equal($('import-container-list').querySelectorAll('[data-reorder-grip]').length, 0);
      assert.equal($('import-package-name').tagName, 'INPUT');
      assert.equal($('import-status').parentElement, $('import-dialog').querySelector('.modal'));
      assert.equal($('import-confirm').textContent, '下一步');
      const unmounted = () => $('import-container-list').querySelector('[data-row-container="2"]');
      assert.equal(unmounted().querySelector('.xrow-open').disabled, true, 'unmounted vaults cannot be selected');
      assert.ok(unmounted().classList.contains('import-unmounted'));
      assert.equal(unmounted().querySelector('[data-row-mount]').disabled, false);
      unmounted().querySelector('.cbx').click();
      assert.equal($('import-confirm').disabled, true);
      unmounted().querySelector('[data-row-mount]').click();
      await wait(() => w.navigationFixture.containers.find(c => c.id === 2).mounted &&
        !unmounted().querySelector('.xrow-open').disabled);
      $('volume-operation-confirm').click();
      assert.equal($('import-dialog').hidden, false, 'mounting keeps the import dialog open');
      $('import-container-list').querySelector('.xrow-open').click(); await tick();
      $('import-confirm').click(); await tick();
      assert.equal(calls.filter(c => c.name === 'import').length, 0, 'no import without a passphrase');
      assert.match($('import-status').textContent, /加密口令/);
      const removedTarget = w.navigationFixture.containers.find(c => c.id === 1);
      removedTarget.mounted = false; removedTarget.drive = ''; removedTarget.size = 0;
      w.dispatchEvent(new w.Event('usb-vault-mounts-changed'));
      await wait(() => $('import-confirm').disabled && $('import-container-list').querySelectorAll('.cbx.on').length === 0);
      unmounted().querySelector('.xrow-open').click();
      setScript(['1,5,10,0,1', '1,5,10,0,2', '2,10,10,0,2']);
      $('import-password').value = 'long enough export pass';
      $('import-confirm').click();
      await wait(() => $('import-status').textContent === '导入成功' && $('import-cancel').textContent === '关闭');
      assert.equal($('import-dialog').hidden, false);
      assert.equal($('import-status').dataset.tone, 'success');
      assert.equal($('import-confirm').hidden, true);
      assert.equal($('import-password').value, '');
      assert.equal($('import-package-name').disabled, true);
      assert.equal(d.activeElement, $('import-cancel'));
      const importCount = calls.filter(c => c.name === 'import').length;
      $('import-confirm').dispatchEvent(new w.Event('click'));
      await tick();
      assert.equal(calls.filter(c => c.name === 'import').length, importCount, 'success cannot re-import');
      $('import-cancel').click();
      assert.equal($('import-dialog').hidden, true);
      assert.equal($('authenticated-dashboard').hasAttribute('inert'), false);
      const call = calls.find(c => c.name === 'import');
      assert.equal(call.args[1], '');
      assert.ok(call.arrays[0].every(b => b === 0), 'the import passphrase array must be wiped');
    } finally { w.close(); }
  }

  // The editable name is applied natively only on next; a collision can be retried.
  {
    const {w, d, $, calls, setScript} = load();
    try {
      await tick(); d.querySelector('[data-mount-existing]').click();
      await wait(() => !$('import-dialog').hidden);
      $('import-container-list').querySelector('.xrow-open').click();
      $('import-package-name').value = '';
      $('import-package-name').dispatchEvent(new w.Event('input'));
      assert.equal($('import-confirm').disabled, true);
      $('import-package-name').value = 'Renamed package';
      $('import-package-name').dispatchEvent(new w.Event('input'));
      assert.equal(calls.filter(c => c.name === 'rename').length, 0);
      const bridge = w.chrome.webview.hostObjects.nativeBridge;
      bridge.renameImportPackage = name => { calls.push({name: 'rename', args: [name]}); return 4; };
      $('import-password').value = 'export password'; $('import-confirm').click();
      await wait(() => /已有同名/.test($('import-status').textContent) && !$('import-password').disabled);
      assert.equal(calls.filter(c => c.name === 'import').length, 0);
      bridge.renameImportPackage = name => { calls.push({name: 'rename', args: [name]}); return 0; };
      setScript(['3,0,0,1,1']);
      $('import-password').value = 'wrong password'; $('import-confirm').click();
      await wait(() => /加密口令错误/.test($('import-status').textContent) && !$('import-password').disabled);
      assert.equal($('import-package-name').value, 'Renamed package');
      setScript(['2,10,10,0,2']);
      $('import-password').value = 'correct password'; $('import-confirm').click();
      await wait(() => $('import-status').textContent === '导入成功' && $('import-cancel').textContent === '关闭');
      assert.equal($('import-dialog').hidden, false);
      assert.equal(calls.filter(c => c.name === 'rename').length, 2, 'retry must reuse the renamed source path');
    } finally { w.close(); }
  }

  // A file the native side cannot place is left to the user.
  {
    const {d, $, w, calls, setImportChoice} = load();
    try {
      await tick();
      setImportChoice('3|unnamed');
      d.querySelector('[data-mount-existing]').click();
      await wait(() => !$('import-kind-dialog').hidden);
      assert.equal($('import-kind-name').textContent, 'unnamed');
      $('import-kind-package').click();
      await wait(() => !$('import-dialog').hidden);
      assert.ok(calls.some(c => c.name === 'asPackage'));
      $('import-cancel').click(); await tick();
      assert.equal($('import-dialog').hidden, true);
      d.querySelector('[data-mount-existing]').click();
      await wait(() => !$('import-kind-dialog').hidden);
      $('import-kind-container').click();
      await wait(() => calls.some(c => c.name === 'adopt'));
      // Let the container action finish before the window goes.
      for (let i = 0; i < 10; i++) await tick();
      assert.equal($('import-dialog').hidden, true);
    } finally { w.close(); }
  }

  // Import into a new vault: verify, then the creation form with the
  // hidden-volume rules; cancel reaches the bridge.
  {
    const {w, d, $, calls, setScript} = load();
    try {
      await tick();
      d.querySelector('[data-mount-existing]').click();
      await wait(() => !$('import-dialog').hidden);
      $('import-target-new').click(); await tick();
      assert.equal($('import-confirm').textContent, '下一步');
      setScript(['1,3,10,0,1', '2,10,10,0,1']);
      $('import-password').value = 'long enough export pass';
      $('import-confirm').click();
      await wait(() => /校验通过/.test($('import-status').textContent));
      assert.equal($('import-new-fields').hidden, false);
      assert.equal($('import-new-size').value, '120');
      assert.equal($('import-password').disabled, true);
      $('import-new-name').value = 'Restored';
      $('import-new-name').dispatchEvent(new w.Event('input'));
      $('import-new-browse').click(); await tick();
      $('import-new-size').value = '200';
      $('import-new-size').dispatchEvent(new w.Event('input'));
      assert.equal($('import-confirm').disabled, false);
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
      $('import-new-hidden').checked = false;
      $('import-new-hidden').dispatchEvent(new w.Event('change'));
      setScript(['1,1,10,0,2']);
      $('import-confirm').click();
      await wait(() => $('import-cancel').textContent === '停止');
      $('import-cancel').click();
      await wait(() => /已取消/.test($('import-status').textContent));
      assert.deepEqual(calls.find(c => c.name === 'new').args, ['Restored', 'D:\\Vaults', 200]);
      assert.ok(calls.some(c => c.name === 'cancel'));
      assert.equal($('import-confirm').disabled, false, 'the same package can be verified again');
      assert.equal($('import-confirm').textContent, '下一步');
      assert.equal($('import-password').disabled, false);
      assert.equal($('import-package-name').value, 'family photos');
      setScript(['2,10,10,0,1']);
      $('import-password').value = 'retry export pass';
      $('import-confirm').click();
      await wait(() => /校验通过/.test($('import-status').textContent));
      assert.equal(calls.filter(c => c.name === 'verify').length, 2);
      $('import-target-existing').click(); await tick();
      assert.equal($('import-package-name').value, 'family photos');
      assert.equal($('import-password').disabled, false, 'changing target asks for a fresh password');
      assert.equal($('import-confirm').textContent, '下一步');
    } finally { w.close(); }
  }

  // Failed import attempts keep the same selection and accept a new password.
  {
    const {w, d, $, calls, setScript} = load();
    try {
      await tick();
      d.querySelector('[data-mount-existing]').click();
      await wait(() => !$('import-dialog').hidden);
      $('import-container-list').querySelector('.xrow-open').click();
      for (const [code, message] of [[1, '加密口令错误。'], [10, '导出包已损坏或格式不受支持。'], [4, '目标位置已有同名的文件或文件夹。']]) {
        setScript([`3,0,0,${code},1`]);
        $('import-password').value = 'retry passphrase';
        $('import-confirm').click();
        await wait(() => $('import-status').textContent.startsWith(message) && !$('import-password').disabled);
        assert.equal($('import-dialog').hidden, false);
        assert.equal($('import-package-name').value, 'family photos');
        assert.equal($('import-password').value, '');
        assert.equal($('import-confirm').disabled, false);
      }
      const bridge = w.chrome.webview.hostObjects.nativeBridge;
      const beginImport = bridge.beginEncryptedImport;
      bridge.beginEncryptedImport = () => { throw Error('transient start refusal'); };
      $('import-password').value = 'retry passphrase';
      $('import-confirm').click();
      await wait(() => $('import-status').textContent.startsWith('没有开始') && !$('import-password').disabled);
      assert.equal($('import-package-name').value, 'family photos');
      bridge.beginEncryptedImport = beginImport;
      setScript(['2,10,10,0,2']);
      $('import-password').value = 'correct passphrase';
      $('import-confirm').click();
      await wait(() => $('import-status').textContent === '导入成功' && $('import-cancel').textContent === '关闭');
      assert.equal($('import-dialog').hidden, false);
      assert.equal($('import-status').dataset.tone, 'success');
      $('import-cancel').click();
      assert.equal($('import-dialog').hidden, true);
      assert.equal(calls.filter(c => c.name === 'choose').length, 1);
      assert.equal(calls.filter(c => c.name === 'import').length, 4);
      assert.ok(calls.filter(c => c.name === 'import').every(c => c.arrays[0].every(b => b === 0)));
    } finally { w.close(); }
  }

  {
    const {w, d, $, calls, setScript} = load();
    try {
      await tick(); d.querySelector('[data-mount-existing]').click();
      await wait(() => !$('import-dialog').hidden);
      $('import-target-new').click();
      $('import-password').value = 'export password';
      setScript(['2,1,1,0,1']); $('import-confirm').click();
      await wait(() => !$('import-new-fields').hidden && !$('import-new-name').disabled);
      $('import-new-name').value = 'Imported'; $('import-new-name').dispatchEvent(new w.Event('input'));
      $('import-new-browse').click(); await tick();
      setScript(['2,1,1,0,2']); $('import-confirm').click();
      await wait(() => $('import-status').textContent === '导入成功' && $('import-cancel').textContent === '关闭');
      assert.equal($('import-dialog').hidden, false);
      assert.equal($('import-status').dataset.tone, 'success');
      assert.equal(calls.filter(call => call.name === 'new').length, 1);
      $('import-cancel').click();
      d.querySelector('[data-mount-existing]').click(); await wait(() => !$('import-dialog').hidden);
      assert.equal($('import-confirm').hidden, false);
      assert.equal($('import-package-name').disabled, false);
      assert.equal($('import-status').textContent, '');
      assert.equal($('import-cancel').textContent, '取消');
    } finally { w.close(); }
  }

  // The export dialog closes with a reason when its selection goes, a
  // session change wipes its passphrase fields, and it hands back only the
  // inert state it took.
  {
    const {w, d, $} = load();
    try {
      await tick();
      await openBackup(d, $, 'export-mode-encrypted');
      d.querySelector('[data-export-toggle="1"] .xrow-open').click(); await tick();
      d.querySelector('[data-export-select-all="1"]').click();
      await wait(() => !$('backup-export-confirm').disabled);
      $('backup-export-confirm').click(); await tick();
      assert.equal($('authenticated-dashboard').hasAttribute('inert'), true);
      $('export-dialog-passphrase').value = 'typed passphrase';
      w.dispatchEvent(new w.Event('usb-vault-session-changed'));
      for (let i = 0; i < 20; i++) await tick();
      assert.equal($('export-dialog').hidden, true);
      assert.equal($('export-dialog-passphrase').value, '');
      assert.equal($('authenticated-dashboard').hasAttribute('inert'), false);

      await openBackup(d, $, 'export-mode-encrypted');
      d.querySelector('[data-export-toggle="1"] .xrow-open').click(); await tick();
      d.querySelector('[data-export-select-all="1"]').click();
      await wait(() => !$('backup-export-confirm').disabled);
      $('backup-export-confirm').click(); await tick();
      // Something outside these dialogs that already holds the dashboard
      // inert keeps it after the dialog closes.
      $('export-dialog-cancel').click();
      $('authenticated-dashboard').setAttribute('inert', '');
      $('backup-export-confirm').disabled = false;
      $('backup-export-confirm').click(); await tick();
      $('export-dialog-cancel').click();
      assert.equal($('authenticated-dashboard').hasAttribute('inert'), true, 'inert another owner took stays');
      $('authenticated-dashboard').removeAttribute('inert');

      $('backup-export-confirm').click(); await tick();
      assert.equal($('export-dialog').hidden, false);
      w.navigationFixture.containers.find(c => c.id === 1).mounted = false;
      w.dispatchEvent(new w.Event('usb-vault-session-changed'));
      await wait(() => $('export-dialog').hidden);
      // Let the session refresh finish before the window goes.
      for (let i = 0; i < 20; i++) await tick();
    } finally { w.close(); }
  }

  // A migration is named in the dialog: the vault's name by default, any
  // name the user types otherwise, and that name reaches the bridge.
  {
    const {w, d, $} = load();
    try {
      const migrations = [];
      w.chrome.webview.hostObjects.nativeBridge.exportContainerFile = (...args) => { migrations.push(args); return true; };
      await tick();
      await openBackup(d, $, 'export-mode-migrate');
      d.querySelector('[data-migrate-select="2"]').click();
      await wait(() => !$('backup-export-confirm').disabled);
      $('backup-export-confirm').click(); await tick();
      assert.equal($('export-dialog-title').textContent, '安全盘迁移');
      assert.equal($('export-dialog-name-field').hidden, false);
      assert.equal($('export-dialog-name-label').textContent, '导出名称');
      assert.equal($('export-dialog-name').value, '影像存档', 'the vault name is the default');
      assert.equal($('export-dialog-name-hint').hidden, true, 'a migration shows no name hint');
      assert.equal($('export-dialog-secret-fields').hidden, true);
      $('export-dialog-name').value = '影像存档 迁移';
      $('export-dialog-location').click(); await tick();
      $('export-dialog-start').click();
      await wait(() => migrations.length === 1 && /^导出成功$/.test($('backup-export-status').textContent));
      assert.deepEqual(migrations[0], [2, 'D:\\Vaults', '影像存档 迁移']);
      for (let i = 0; i < 10; i++) await tick();
    } finally { w.close(); }
  }

  // A swipe offers both removing the entry and deleting the file; deleting
  // asks for an acknowledgement and calls its own bridge member.
  {
    const {w, d, $} = load();
    try {
      const deleted = [];
      const forgotten = [];
      const bridge = w.chrome.webview.hostObjects.nativeBridge;
      bridge.deleteContainerFromDisk = (id) => { deleted.push(id); return true; };
      const originalForget = bridge.forgetContainer;
      bridge.forgetContainer = (id) => { forgotten.push(id); return originalForget ? originalForget(id) : true; };
      await tick();
      const rail = d.querySelector('[data-container-list] .container-swipe:has([data-row-container="2"])') ||
        [...d.querySelectorAll('[data-container-list] .container-swipe')].find(item => item.querySelector('[data-row-container="2"]'));
      const actions = rail.querySelector('.container-swipe-actions');
      const buttons = [...actions.querySelectorAll('button')].map(button => button.textContent);
      assert.deepEqual(buttons, ['从列表移除', '从磁盘删除']);
      const mountedRail = [...d.querySelectorAll('[data-container-list] .container-swipe')]
        .find(item => item.querySelector('[data-row-container="1"]'));
      assert.ok([...mountedRail.querySelectorAll('.container-swipe-actions button')].every(button => button.disabled),
                'a mounted vault offers neither action');
      actions.querySelector('[data-container-action="delete-file"]').click(); await tick();
      assert.equal($('forget-container-dialog').hidden, false);
      assert.equal($('forget-container-title').textContent, '从磁盘删除');
      assert.equal($('forget-container-confirm').disabled, true, 'deleting waits for the acknowledgement');
      $('forget-container-acknowledge').checked = true;
      $('forget-container-acknowledge').dispatchEvent(new w.Event('change'));
      assert.equal($('forget-container-confirm').disabled, false);
      $('forget-container-confirm').click();
      await wait(() => deleted.length === 1);
      assert.deepEqual(deleted, [2]);
      assert.equal(forgotten.length, 0, 'deleting did not go through the plain removal');
      for (let i = 0; i < 10; i++) await tick();
      // The plain removal keeps its own wording and needs no acknowledgement.
      const again = [...d.querySelectorAll('[data-container-list] .container-swipe')]
        .find(item => item.querySelector('[data-row-container="2"]'));
      again.querySelector('[data-container-action="forget"]').click(); await tick();
      assert.equal($('forget-container-title').textContent, '从列表移除');
      assert.equal($('forget-container-acknowledge-field').hidden, true);
      assert.equal($('forget-container-confirm').disabled, false);
      $('forget-container-cancel').click(); await tick();
    } finally { w.close(); }
  }

  // A session change closes the dialogs and forgets their fields.
  {
    const {w, d, $} = load();
    try {
      await tick();
      d.querySelector('[data-mount-existing]').click();
      await wait(() => !$('import-dialog').hidden);
      $('import-target-new').click(); await tick();
      $('import-password').value = 'typed';
      $('import-new-name').value = 'previous';
      $('import-new-hidden').checked = true;
      w.dispatchEvent(new w.Event('usb-vault-session-changed')); await tick();
      assert.equal($('import-dialog').hidden, true);
      assert.equal($('import-password').value, '');
      assert.equal($('import-new-name').value, '');
      assert.equal($('import-new-hidden').checked, false);
    } finally { w.close(); }
  }
  console.log('PASS: export toolbar, cipher export dialog, package name, failure and stop, unmount clears selection, generic import of packages and undecided files, verify-then-create, session reset');
})().catch(e => { console.error(e); process.exitCode = 1; });
