// Export page folder tree: a folder row opens one level per click and its box
// ticks the whole folder; inside a ticked folder rows show as ticked and
// cannot be ticked alone; ticking a folder or a whole container takes back
// what was ticked below it; a listing that arrives after its folder closed is
// dropped. Usage: node <this> <jsdom path>
const fs = require('fs'), assert = require('node:assert/strict');
const {JSDOM} = require(require('path').resolve(process.argv[2]));
const tick = () => new Promise(r => setTimeout(r, 30));
const wait = async predicate => { for (let i = 0; i < 200; i++) { if (predicate()) return; await tick(); } throw Error('UI timeout'); };

const folder = (name) => `${name}\u001f1\u001f0\u001f0\u001f0`;
const file = (name, size) => `${name}\u001f0\u001f0\u001f${size}\u001f0`;
const tree = {
  '': ['0', folder('文档'), file('报告.xlsx', 9)],
  '文档': ['0', folder('子目录'), folder('空'), file('a.txt', 3)],
  '文档/空': ['0'],
  '文档/子目录': ['0', file('b.txt', 4)],
};

const load = () => {
  const dom = new JSDOM(fs.readFileSync('ui/card_session.html', 'utf8'), {runScripts: 'outside-only', pretendToBeVisual: true});
  const w = dom.window;
  w.TextEncoder = TextEncoder;
  w.eval(fs.readFileSync('tests/card_product_ui_navigation_mock.js', 'utf8'));
  const bridge = w.chrome.webview.hostObjects.nativeBridge;
  const listed = [];
  const held = new Map();
  bridge.listContainerDirectory = (id, directory) => {
    listed.push(directory);
    const answer = () => {
      if (!(directory in tree)) throw new Error('no such folder');
      return tree[directory].join('\u001e');
    };
    if (held.has(directory)) return new Promise((resolve, reject) => held.get(directory).push(() => {
      try { resolve(answer()); } catch (error) { reject(error); }
    }));
    return answer();
  };
  const calls = [];
  bridge.beginExportSelection = (...args) => { calls.push(args); return true; };
  bridge.encryptedTransferStatus = () => '2,1,1,0,2';
  w.navigationFixture.chosenLocation = 'D:\\Out';
  w.eval(fs.readFileSync('ui/card_session.js', 'utf8'));
  const d = w.document;
  return {w, d, listed, calls,
    hold: (directory) => held.set(directory, []),
    release: (directory) => { for (const go of held.get(directory)) go(); held.delete(directory); },
    row: (name) => [...d.querySelectorAll('#export-container-list .xfile')].find(r => r.dataset.exportPick === name),
    rows: () => [...d.querySelectorAll('#export-container-list .xfile')].map(r => r.dataset.exportPick)};
};

const ticked = (row) => row.querySelector('.cbx').classList.contains('on');
const depth = (row) => row.style.getPropertyValue('--xdepth');

const openCard = async ({d, row}) => {
  await tick();
  d.querySelector('[data-section=backup]').click();
  d.querySelector('[data-export-toggle="1"] .xrow-open').click();
  await wait(() => row('文档'));
};

(async () => {
  // Opening folders level by level, and closing a folder closes what is below.
  {
    const page = load();
    const {d, listed, row, rows} = page;
    try {
      await openCard(page);
      assert.deepEqual(rows(), ['文档', '报告.xlsx']);
      assert.equal(row('文档').getAttribute('aria-expanded'), 'false');
      row('文档').click();
      assert.ok(![...d.querySelectorAll('#export-container-list .xnote')].some(n => n.textContent === '正在读取…'),
        'opening a folder does not flash a reading note');
      await wait(() => row('a.txt'));
      assert.equal(listed.at(-1), '文档');
      assert.deepEqual(rows(), ['文档', '子目录', '空', 'a.txt', '报告.xlsx']);
      assert.equal(depth(row('子目录')), '1');
      assert.equal(ticked(row('文档')), false, 'opening a folder does not tick it');
      row('子目录').click();
      await wait(() => row('b.txt'));
      assert.equal(listed.at(-1), '文档/子目录', 'deeper folders are read with a slash-joined path');
      assert.equal(depth(row('b.txt')), '2');
      row('空').click();
      await wait(() => d.querySelector('#export-container-list .xempty'));
      const empty = d.querySelector('#export-container-list .xempty');
      assert.equal(empty.textContent, '这个文件夹是空的。');
      assert.equal(depth(empty), '2', 'the empty note sits one level below its folder, like an entry');
      assert.ok(empty.classList.contains('xfile') && empty.tagName === 'DIV' && !empty.dataset.exportPick,
        'the empty note takes the shape of an entry without being one');
      assert.ok(empty.firstElementChild.classList.contains('n') && empty.querySelector('.cbx[aria-hidden="true"]'),
        'the empty note starts where an entry icon starts and keeps the row height of an entry');
      assert.ok(![...d.querySelectorAll('#export-container-list .xnote')].some(n => n.textContent === '这个文件夹是空的。'));
      row('空').click(); await tick();
      row('文档').click(); await tick();
      assert.deepEqual(rows(), ['文档', '报告.xlsx']);
      row('文档').click();
      await wait(() => row('a.txt'));
      assert.equal(row('b.txt'), undefined, 'reopening a folder does not reopen what was below it');
      // Closing the card forgets every opened folder.
      d.querySelector('[data-export-toggle="1"] .xrow-open').click(); await tick();
      d.querySelector('[data-export-toggle="1"] .xrow-open').click();
      await wait(() => row('文档'));
      assert.equal(row('a.txt'), undefined);
    } finally { page.w.close(); }
  }

  // Ticking: a folder's box, a ticked folder carrying its entries, and taking
  // back what was ticked below a folder or a container.
  {
    const page = load();
    const {d, row, calls} = page;
    try {
      await openCard(page);
      row('文档').click();
      await wait(() => row('a.txt'));
      row('子目录').click();
      await wait(() => row('b.txt'));
      row('b.txt').click(); await tick();
      row('a.txt').click(); await tick();
      assert.ok(ticked(row('b.txt')) && ticked(row('a.txt')));
      row('文档').querySelector('[data-export-check]').click(); await tick();
      assert.ok(ticked(row('文档')), 'the box ticks the folder');
      assert.ok(row('a.txt') && row('b.txt'), 'ticking through the box does not close the folder');
      assert.ok(ticked(row('a.txt')) && row('a.txt').querySelector('.cbx').classList.contains('implied'),
        'entries inside a ticked folder show as carried by it');
      row('a.txt').click(); await tick();
      assert.ok(ticked(row('a.txt')), 'an entry inside a ticked folder cannot be unticked alone');
      assert.equal(row('子目录').querySelector('[data-export-check]'), null,
        'a folder inside a ticked folder has no box of its own');
      d.querySelector('[data-export-select-all="1"]').click(); await tick();
      d.querySelector('[data-export-select-all="1"]').click(); await tick();
      assert.ok(!ticked(row('文档')) && !ticked(row('a.txt')));
      // The export carries the folder once, not the entries below it too.
      row('b.txt').click(); await tick();
      row('文档').querySelector('[data-export-check]').click(); await tick();
      d.getElementById('backup-export-confirm').click(); await tick();
      d.getElementById('export-dialog-location').click(); await tick();
      d.getElementById('export-dialog-start').click();
      await wait(() => calls.length === 1);
      const entries = calls[0][1].split('\u001e').map(row => row.split('\u001f'));
      assert.deepEqual(entries, [['1', '', '文档']], 'ticking a folder took back what was ticked inside it');
    } finally { page.w.close(); }
  }

  // Select-all replaces deeper picks with the root, and an entry ticked deep
  // in a closed folder stays listed with its folder.
  {
    const page = load();
    const {d, row, calls} = page;
    try {
      await openCard(page);
      row('文档').click();
      await wait(() => row('a.txt'));
      row('a.txt').click(); await tick();
      row('文档').click(); await tick();
      const handed = [...d.querySelectorAll('#export-container-list .xfile')]
        .find(r => r.querySelector('.n').textContent === '文档 / a.txt');
      assert.ok(handed && ticked(handed), 'a ticked entry in a closed folder stays on screen');
      d.querySelector('[data-export-select-all="1"]').click(); await tick();
      assert.ok(ticked(row('文档')) && ticked(row('报告.xlsx')));
      d.getElementById('backup-export-confirm').click(); await tick();
      d.getElementById('export-dialog-location').click(); await tick();
      d.getElementById('export-dialog-start').click();
      await wait(() => calls.length === 1);
      const names = calls[0][1].split('\u001e').map(row => row.split('\u001f').slice(1).join('/'));
      assert.deepEqual(names.sort(), ['/报告.xlsx', '/文档'].sort(), 'select-all exports the root alone');
    } finally { page.w.close(); }
  }

  // A listing that arrives after its folder closed is dropped, and a failed
  // listing says so under its folder.
  {
    const page = load();
    const {row, hold, release} = page;
    try {
      await openCard(page);
      const reading = () => [...page.d.querySelectorAll('#export-container-list .xnote')].some(n => n.textContent === '正在读取…');
      hold('文档');
      row('文档').click(); await tick();
      assert.equal(row('文档').getAttribute('aria-expanded'), 'true', 'the arrow turns at once');
      assert.ok(!reading(), 'a listing in flight says nothing at first, so a quick one does not flash');
      await wait(reading);
      row('文档').click(); await tick();
      release('文档'); await tick(); await tick();
      assert.equal(row('a.txt'), undefined, 'a late listing does not reopen a closed folder');
      delete tree['文档/子目录'];
      page.w.navigationFixture.directoryError = 4;
      row('文档').click();
      await wait(() => row('子目录'));
      row('子目录').click();
      await wait(() => [...page.d.querySelectorAll('#export-container-list .xnote')]
        .some(n => n.textContent === '这个文件夹已经不在了，可能已被删除或移动。'));
      page.w.navigationFixture.directoryError = 0;
      tree['文档/子目录'] = ['0', file('b.txt', 4)];
    } finally { page.w.close(); }
  }
  console.log('PASS: folder tree opens level by level, folder box ticks, carried entries, picks taken back, select-all, closed-folder picks, late and failed listings');
})().catch(e => { console.error(e); process.exitCode = 1; });
