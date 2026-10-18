// P5.14 cipher export to chosen contacts: the export dialog's protection
// choice, the recipient list read from the card, the 15-recipient bound and
// the bridge call. Usage: node <this> <jsdom path>
const fs = require('fs'), assert = require('node:assert/strict');
const {JSDOM} = require(require('path').resolve(process.argv[2]));
const tick = () => new Promise(r => setTimeout(r, 30));
const wait = async predicate => { for (let i = 0; i < 200; i++) { if (predicate()) return; await tick(); } throw Error('UI timeout'); };
const RS = '\u001e', FS = '\u001f';
const key = n => n.toString(16).padStart(2, '0').repeat(32);

const load = (book) => {
  const dom = new JSDOM(fs.readFileSync('ui/card_session.html', 'utf8'), {runScripts: 'outside-only', pretendToBeVisual: true});
  const w = dom.window;
  w.TextEncoder = TextEncoder;
  w.eval(fs.readFileSync('tests/card_product_ui_navigation_mock.js', 'utf8'));
  const bridge = w.chrome.webview.hostObjects.nativeBridge;
  const calls = [];
  let script = [];
  bridge.beginExportSelection = (...args) => { calls.push({name: 'password', args}); return true; };
  bridge.beginRecipientExport = (...args) => { calls.push({name: 'recipients', args}); return true; };
  bridge.encryptedTransferStatus = () => script.length > 1 ? script.shift() : (script[0] || '0,0,0,0,0');
  bridge.getContactBook = () => { calls.push({name: 'book'}); return book(); };
  w.navigationFixture.chosenLocation = 'D:\\Vaults';
  w.eval(fs.readFileSync('ui/card_session.js', 'utf8'));
  return {w, d: w.document, $: id => w.document.getElementById(id), calls, setScript: s => { script = s; }};
};
const listed = count => () => ['0', `3${FS}OWNF`,
  ...Array.from({length: count}, (_, i) => [key(i + 1), `联系人${i + 1}`, `FP${i + 1}`].join(FS))].join(RS);

const openDialog = async (d, $) => {
  d.querySelector('[data-section=backup]').click();
  $('export-mode-encrypted').click(); await tick();
  d.querySelector('[data-export-select-all="1"]').click(); await tick();
  $('backup-export-confirm').click(); await tick();
};

(async () => {
  // Choosing recipients replaces the password, reads the list once per choice,
  // and seals for exactly the chosen contacts.
  {
    const {w, d, $, calls, setScript} = load(listed(3));
    try {
      await tick();
      await openDialog(d, $);
      assert.equal($('export-dialog-protection').hidden, false);
      assert.equal($('export-protection-password').getAttribute('aria-pressed'), 'true');
      assert.equal($('export-dialog-recipient-fields').hidden, true);
      assert.equal(calls.some(c => c.name === 'book'), false, 'the list was read before it was asked for');
      $('export-dialog-passphrase').value = 'typed then dropped';
      $('export-protection-recipients').click(); await tick();
      assert.equal($('export-dialog-secret-fields').hidden, true);
      assert.equal($('export-dialog-passphrase').value, '', 'a typed password survived the switch');
      assert.equal($('export-dialog-recipient-fields').hidden, false);
      const boxes = () => Array.from($('export-recipient-list').querySelectorAll('input[type=checkbox]'));
      assert.deepEqual(boxes().map(b => b.parentElement.textContent), ['联系人1', '联系人2', '联系人3']);
      $('export-dialog-location').click(); await tick();
      assert.equal($('export-dialog-start').disabled, true, 'no recipient, no export');
      boxes()[2].click(); boxes()[0].click(); await tick();
      assert.match($('export-recipient-hint').textContent, /已选 2 \/ 15/);
      setScript(['1,0,10,0,2', '2,10,10,0,2']);
      $('export-dialog-start').click();
      await wait(() => !$('export-result-confirm').disabled);
      const sent = calls.filter(c => c.name === 'recipients');
      assert.equal(calls.some(c => c.name === 'password'), false);
      assert.equal(sent.length, 1);
      assert.equal(sent[0].args[0].split(RS).length, 2, 'both projects enter one package');
      assert.equal(sent[0].args[1], 'D:\\Vaults');
      assert.equal(sent[0].args[2], '导出项目');
      assert.deepEqual(sent[0].args[3].split(RS).sort(), [key(1), key(3)]);
      // The dialog forgets the choice for the next export.
      $('export-result-confirm').click(); await tick();
      await openDialog(d, $);
      assert.equal($('export-protection-password').getAttribute('aria-pressed'), 'true');
      assert.equal($('export-recipient-list').children.length, 0);
    } finally { w.close(); }
  }

  // Fifteen at most; an empty or unusable list says so.
  {
    const {w, d, $} = load(listed(16));
    try {
      await tick();
      await openDialog(d, $);
      $('export-protection-recipients').click(); await tick();
      // Each change redraws the list, so every click looks the box up again.
      for (let i = 0; i < 15; ++i) {
        $('export-recipient-list').querySelectorAll('input[type=checkbox]')[i].click(); await tick();
      }
      const fresh = Array.from($('export-recipient-list').querySelectorAll('input[type=checkbox]'));
      assert.equal(fresh[15].disabled, true, 'a sixteenth recipient could be chosen');
      assert.match($('export-recipient-hint').textContent, /已选 15 \/ 15/);
    } finally { w.close(); }
  }
  for (const [book, message] of [[listed(0), /尚无|还没有联系人/], [() => '2', /容量不足/]]) {
    const {w, d, $} = load(book);
    try {
      await tick();
      await openDialog(d, $);
      $('export-protection-recipients').click(); await tick();
      assert.match($('export-recipient-hint').textContent, message);
      $('export-dialog-location').click(); await tick();
      assert.equal($('export-dialog-start').disabled, true);
    } finally { w.close(); }
  }
  // A recipient the native side refuses (code 8) is reported, not hidden.
  {
    const {w, d, $, setScript} = load(listed(1));
    try {
      await tick();
      await openDialog(d, $);
      $('export-protection-recipients').click(); await tick();
      $('export-recipient-list').querySelector('input').click(); await tick();
      $('export-dialog-location').click(); await tick();
      setScript(['3,0,0,8,2']);
      $('export-dialog-start').click();
      await wait(() => !$('export-result-confirm').disabled);
      assert.match($('backup-export-status').textContent, /位置、名称或容量/);
    } finally { w.close(); }
  }
  console.log('PASS: recipient export protection choice, recipient list, 15 bound, empty and unusable lists, bridge call');
})().catch(error => { console.error(error); process.exit(1); });
