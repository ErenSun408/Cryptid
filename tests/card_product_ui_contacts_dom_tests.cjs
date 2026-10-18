// node tests/card_product_ui_contacts_dom_tests.cjs <local jsdom path>
// DOM only: the P5.14 contact list page against a scripted bridge.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {JSDOM} = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, '..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8');
const settle = () => new Promise(resolve => setTimeout(resolve, 30));
const RS = '\u001e', FS = '\u001f';
const KEY_A = 'a'.repeat(64), KEY_B = 'b'.repeat(64);

async function open(setup) {
  const dom = new JSDOM(read('ui/card_session.html'), {runScripts: 'outside-only'});
  const w = dom.window; w.TextEncoder = TextEncoder;
  w.eval(read('tests/card_product_ui_navigation_mock.js'));
  w.navigationFixture.state = 3;
  const bridge = w.chrome.webview.hostObjects.nativeBridge;
  const book = {state: 3, own: 'OWNF INGE RPRI NT00', entries: [[KEY_A, '爱丽丝', 'AAAA BBBB CCCC DDDD']]};
  const calls = [];
  const pack = () => ['0', `${book.state}${FS}${book.own}`,
    ...book.entries.map(entry => entry.join(FS))].join(RS);
  bridge.getContactBook = () => { calls.push(['get']); return pack(); };
  bridge.reloadContactBook = () => { calls.push(['reload']); return pack(); };
  setup && setup(bridge, book, calls);
  w.eval(read('ui/card_session.js')); await settle();
  const el = id => w.document.getElementById(id);
  w.document.querySelector('[data-section="contacts"]').click(); await settle();
  return {w, el, bridge, book, calls};
}

async function listAndManage() {
  const t = await open((bridge, book, calls) => {
    let held = false;
    bridge.chooseContactKeyFile = () => { held = true; return `0${RS}1${FS}EEEE FFFF 0000 1111`; };
    bridge.addChosenContact = remark => {
      calls.push(['add', remark]);
      if (!held) return '1';
      if (remark.trim() === '') return `0${RS}8${FS}`;
      held = false;
      book.entries.push([KEY_B, remark.trim(), 'EEEE FFFF 0000 1111']);
      return `0${RS}0${FS}`;
    };
    bridge.renameContact = (key, remark) => {
      calls.push(['rename', key, remark]);
      book.entries.find(entry => entry[0] === key)[1] = remark;
      return `0${RS}0`;
    };
    bridge.removeContact = key => {
      calls.push(['remove', key]);
      book.entries = book.entries.filter(entry => entry[0] !== key);
      return `0${RS}0`;
    };
    bridge.exportOwnContactKey = () => { calls.push(['export']); return `0${RS}1`; };
  });
  const {w, el, calls} = t;
  try {
    assert.equal(el('dashboard-section-contacts').classList.contains('active'), true);
    assert.equal(el('contacts-own-fingerprint').textContent, 'OWNF INGE RPRI NT00');
    const rows = () => Array.from(el('contacts-body').querySelectorAll('tr[data-contact-key]'));
    assert.deepEqual(rows().map(row => row.cells[0].textContent), ['爱丽丝']);
    assert.equal(rows()[0].cells[1].textContent, 'AAAA BBBB CCCC DDDD');

    el('contacts-add').click(); await settle();
    assert.equal(el('contact-add-dialog').hidden, false);
    assert.equal(el('contact-add-fingerprint').textContent, 'EEEE FFFF 0000 1111');
    el('contact-add-remark').value = '  ';
    el('contact-add-confirm').click(); await settle();
    assert.equal(el('contact-add-status').textContent, '请填写备注。');
    assert.equal(el('contact-add-dialog').hidden, false, 'a refused remark keeps the dialog');
    assert.equal(el('contact-add-confirm').disabled, false, 'a refused remark can be retried');
    el('contact-add-remark').value = ' Bob ';
    el('contact-add-confirm').click(); await settle();
    assert.equal(el('contact-add-dialog').hidden, true);
    assert.deepEqual(rows().map(row => row.cells[0].textContent), ['爱丽丝', 'Bob']);
    assert.equal(el('contacts-status').textContent, '已添加联系人。');

    rows()[1].querySelector('[data-contact-action="rename"]').click(); await settle();
    assert.equal(el('contact-rename-remark').value, 'Bob');
    el('contact-rename-remark').value = '鲍勃';
    el('contact-rename-confirm').click(); await settle();
    assert.deepEqual(calls.find(call => call[0] === 'rename'), ['rename', KEY_B, '鲍勃']);
    assert.equal(rows()[1].cells[0].textContent, '鲍勃');

    rows()[0].querySelector('[data-contact-action="remove"]').click(); await settle();
    assert.equal(el('contact-remove-name').textContent, '爱丽丝');
    el('contact-remove-cancel').click(); await settle();
    assert.equal(calls.some(call => call[0] === 'remove'), false, 'cancelling removed a contact');
    rows()[0].querySelector('[data-contact-action="remove"]').click(); await settle();
    el('contact-remove-confirm').click(); await settle();
    assert.deepEqual(calls.find(call => call[0] === 'remove'), ['remove', KEY_A]);
    assert.deepEqual(rows().map(row => row.cells[0].textContent), ['鲍勃']);

    el('contacts-export-own').click(); await settle();
    assert.equal(el('contacts-status').textContent, '已保存本人公钥文件，可发送给联系人导入。');

    // Nothing of the list reaches a page event.
    const events = JSON.stringify(w.navigationFixture.pageEvents);
    for (const secret of ['爱丽丝', '鲍勃', 'Bob', 'AAAA', 'EEEE', KEY_A, KEY_B, 'OWNF']) {
      assert.equal(events.includes(secret), false, `a page event carried ${secret}`);
    }
  } finally { w.close(); }
}

async function refusals() {
  const t = await open(bridge => {
    let pick = 0;
    bridge.chooseContactKeyFile = () => [`0${RS}0${FS}`, `0${RS}4${FS}`, `0${RS}1${FS}AAAA BBBB CCCC DDDD`][pick++];
    bridge.addChosenContact = () => `0${RS}6${FS}爱丽丝`;
  });
  const {w, el} = t;
  try {
    el('contacts-add').click(); await settle();
    assert.equal(el('contact-add-dialog').hidden, true, 'a cancelled pick opened the dialog');
    el('contacts-add').click(); await settle();
    assert.equal(el('contacts-status').textContent, '公钥文件校验失败，可能已损坏或被改动。');
    el('contacts-add').click(); await settle();
    el('contact-add-remark').value = 'Again';
    el('contact-add-confirm').click(); await settle();
    assert.equal(el('contact-add-status').textContent, '该联系人已在通讯录中，备注为“爱丽丝”。');
    assert.equal(el('contact-add-confirm').disabled, true, 'a listed key cannot be retried');
  } finally { w.close(); }
}

async function states() {
  const small = await open((bridge, book) => { book.state = 1; book.entries = []; });
  try {
    assert.equal(small.el('contacts-notice').hidden, false);
    assert.match(small.el('contacts-notice-text').textContent, /容量不足/);
    assert.equal(small.el('contacts-add').disabled, true);
    assert.equal(small.el('contacts-clear').hidden, true);
  } finally { small.w.close(); }

  let cleared = 0;
  const damaged = await open((bridge, book) => {
    book.state = 4; book.entries = [];
    bridge.clearContactBook = () => { ++cleared; book.state = 2; return '0'; };
  });
  try {
    assert.match(damaged.el('contacts-notice-text').textContent, /已损坏/);
    assert.equal(damaged.el('contacts-clear').hidden, false);
    assert.equal(damaged.el('contacts-add').disabled, true);
    damaged.el('contacts-clear').click(); await settle();
    assert.equal(cleared, 1);
    assert.equal(damaged.el('contacts-notice').hidden, true);
    assert.equal(damaged.el('contacts-add').disabled, false);
    assert.match(damaged.el('contacts-body').textContent, /尚无联系人/);
  } finally { damaged.w.close(); }

  const failing = await open(bridge => { bridge.getContactBook = () => '2'; });
  try {
    assert.equal(failing.el('contacts-status').textContent,
                 '此安全卡容量不足，无法使用通讯录，其余功能不受影响。');
    assert.equal(failing.el('contacts-body').querySelectorAll('tr[data-contact-key]').length, 0);
  } finally { failing.w.close(); }

  const changed = await open((bridge, book, calls) => {
    bridge.renameContact = () => '3';
  });
  try {
    changed.el('contacts-body').querySelector('[data-contact-action="rename"]').click(); await settle();
    changed.el('contact-rename-remark').value = 'X';
    changed.el('contact-rename-confirm').click(); await settle();
    assert.match(changed.el('contact-rename-status').textContent, /已重新读取/);
    assert.equal(changed.calls.filter(call => call[0] === 'get').length, 2, 'a changed list was not read again');
  } finally { changed.w.close(); }
}

(async () => {
  await listAndManage();
  await refusals();
  await states();
  console.log('PASS: contact list page lists, adds, renames, removes, exports, refuses and shows list states without leaking contacts');
})().catch(error => { console.error(error); process.exit(1); });
