// P5.14 import of transfer packages: the check before any password, the
// sender line shown only after verification, adding an unknown sender,
// refusal of forged and damaged packages, and the password fallback.
// Usage: node <this> <jsdom path>
const fs = require('fs'), assert = require('node:assert/strict');
const {JSDOM} = require(require('path').resolve(process.argv[2]));
const tick = () => new Promise(r => setTimeout(r, 30));
const wait = async predicate => { for (let i = 0; i < 300; i++) { if (predicate()) return; await tick(); } throw Error('UI timeout'); };
const RS = '\u001e', FS = '\u001f';

const load = ({check, sender}) => {
  const dom = new JSDOM(fs.readFileSync('ui/card_session.html', 'utf8'), {runScripts: 'outside-only', pretendToBeVisual: true});
  const w = dom.window;
  w.TextEncoder = TextEncoder;
  w.eval(fs.readFileSync('tests/card_product_ui_navigation_mock.js', 'utf8'));
  const bridge = w.chrome.webview.hostObjects.nativeBridge;
  const calls = [];
  let script = [check];
  const start = name => (...args) => { calls.push({name, args}); return true; };
  bridge.chooseImportFile = () => '2|family photos';
  bridge.renameImportPackage = name => { calls.push({name: 'rename', args: [name]}); return 0; };
  bridge.beginVerifyTransferImport = start('check');
  bridge.transferImportSender = () => sender;
  bridge.getVerifiedImportMinimumMib = () => 64;
  bridge.beginImportVerified = start('verified');
  bridge.beginEncryptedImport = start('password');
  bridge.beginVerifyImport = start('verify');
  bridge.beginImportIntoNewContainerMib = start('new');
  bridge.addTransferSender = remark => { calls.push({name: 'addSender', args: [remark]}); return `0${RS}0${FS}`; };
  bridge.encryptedTransferStatus = () => script.length > 1 ? script.shift() : (script[0] || '0,0,0,0,0');
  w.navigationFixture.chosenLocation = 'D:\\Vaults';
  w.eval(fs.readFileSync('ui/card_session.js', 'utf8'));
  return {w, d: w.document, $: id => w.document.getElementById(id), calls, setScript: s => { script = s; }};
};
const openImport = async (d, $, calls) => {
  await tick();
  d.querySelector('[data-mount-existing]').click();
  await wait(() => !$('import-dialog').hidden && calls.some(c => c.name === 'check') && !/正在识别/.test($('import-status').textContent));
  await tick();
};

(async () => {
  // A listed sender: named by remark, no password, imported as verified.
  {
    const {w, d, $, calls, setScript} = load({check: '2,0,0,0,1', sender: `2${RS}0${FS}Alice${FS}`});
    try {
      await openImport(d, $, calls);
      assert.deepEqual(calls.find(c => c.name === 'rename').args, ['family photos']);
      assert.equal($('import-sender').hidden, false);
      assert.equal($('import-sender-text').textContent, '来自 Alice');
      assert.equal($('import-sender-add-fields').hidden, true);
      assert.equal($('import-password-field').hidden, true, 'a transfer package asked for a password');
      assert.equal($('import-package-name').disabled, true);
      $('import-container-list').querySelector('.xrow-open').click(); await tick();
      setScript(['2,10,10,0,2']);
      $('import-confirm').click();
      await wait(() => $('import-status').textContent === '导入成功');
      const verified = calls.filter(c => c.name === 'verified');
      assert.equal(verified.length, 1);
      assert.equal(verified[0].args[1], '');
      assert.equal(calls.some(c => c.name === 'password'), false);
    } finally { w.close(); }
  }

  // An unknown sender: named by fingerprint and can be added.
  {
    const {w, d, $, calls} = load({check: '2,0,0,0,1', sender: `2${RS}0${FS}${FS}ABCD EFGH JKMN PQRS`});
    try {
      await openImport(d, $, calls);
      assert.equal($('import-sender-text').textContent, '未知发件人 · 指纹 ABCD EFGH JKMN PQRS');
      assert.equal($('import-sender-add-fields').hidden, false);
      $('import-sender-remark').value = 'Bob';
      $('import-sender-add').click(); await tick();
      assert.deepEqual(calls.find(c => c.name === 'addSender').args, ['Bob']);
      assert.equal($('import-sender-status').textContent, '已加入通讯录。');
      assert.equal($('import-sender-text').textContent, '来自 Bob');
      assert.equal($('import-sender-add-fields').hidden, true);
    } finally { w.close(); }
  }

  // This card itself.
  {
    const {w, d, $, calls} = load({check: '2,0,0,0,1', sender: `2${RS}1${FS}${FS}`});
    try {
      await openImport(d, $, calls);
      assert.equal($('import-sender-text').textContent, '来自本人');
      assert.equal($('import-sender-add-fields').hidden, true);
    } finally { w.close(); }
  }

  // Not addressed here, or no usable contact list: the password path as before.
  for (const sender of ['1', '3']) {
    const {w, d, $, calls, setScript} = load({check: '2,0,0,0,1', sender});
    try {
      await openImport(d, $, calls);
      assert.equal($('import-sender').hidden, true);
      assert.equal($('import-password-field').hidden, false);
      assert.equal($('import-package-name').disabled, false);
      $('import-container-list').querySelector('.xrow-open').click(); await tick();
      $('import-password').value = 'export password';
      setScript(['2,10,10,0,2']);
      $('import-confirm').click();
      await wait(() => $('import-status').textContent === '导入成功');
      assert.equal(calls.filter(c => c.name === 'password').length, 1);
      assert.equal(calls.some(c => c.name === 'verified'), false);
    } finally { w.close(); }
  }

  // Forged and damaged packages are refused before anything else.
  for (const [code, text] of [[11, /签名无效/], [10, /已损坏/]]) {
    const {w, d, $, calls} = load({check: `3,0,0,${code},1`, sender: '0'});
    try {
      await openImport(d, $, calls);
      assert.match($('import-status').textContent, text);
      assert.equal($('import-sender').hidden, true, 'a refused package named a sender');
      assert.equal($('import-password-field').hidden, true);
      $('import-container-list').querySelector('.xrow-open').click(); await tick();
      assert.equal($('import-confirm').disabled, true);
    } finally { w.close(); }
  }

  // Into a new container: verified already, so the creation fields come first.
  {
    const {w, d, $, calls, setScript} = load({check: '2,0,0,0,1', sender: `2${RS}0${FS}Alice${FS}`});
    try {
      await openImport(d, $, calls);
      $('import-target-new').click(); await tick();
      assert.equal($('import-new-fields').hidden, false);
      assert.equal($('import-new-size').value, '64');
      $('import-new-name').value = 'Restored'; $('import-new-name').dispatchEvent(new w.Event('input'));
      $('import-new-browse').click(); await tick();
      setScript(['2,10,10,0,2']);
      $('import-confirm').click();
      await wait(() => $('import-status').textContent === '导入成功');
      assert.equal(calls.filter(c => c.name === 'new').length, 1);
      assert.equal(calls.some(c => c.name === 'verify'), false, 'a verified transfer was verified with a password');
    } finally { w.close(); }
  }
  console.log('PASS: transfer import check, sender by remark/fingerprint/own, adding a sender, password fallback, forged and damaged refusal, new container');
})().catch(error => { console.error(error); process.exit(1); });
