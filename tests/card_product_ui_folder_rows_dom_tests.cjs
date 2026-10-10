const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const {JSDOM, VirtualConsole} = require(path.resolve(process.argv[2]));
const read = file => fs.readFileSync(path.resolve(__dirname, '..', file), 'utf8');
const wait = async check => { for (let i = 0; i < 150; i++) { if (check()) return; await new Promise(r => setTimeout(r, 20)); } throw Error('UI wait timed out'); };
async function run(cell) {
  const errors = [], vc = new VirtualConsole(); vc.on('jsdomError', e => errors.push(e.message));
  const dom = new JSDOM(read('ui/card_session.html'), {runScripts:'outside-only', pretendToBeVisual:true, virtualConsole:vc});
  const w = dom.window, d = w.document, $ = selector => d.querySelector(selector); w.TextEncoder = TextEncoder;
  try {
    w.eval(read('tests/card_product_ui_navigation_mock.js'));
    const f = w.navigationFixture, b = w.chrome.webview.hostObjects.nativeBridge;
    let transfers = 0, deletes = 0;
    b.moveContainerEntry = b.copyContainerEntry = () => { transfers++; };
    b.deleteContainerEntry = () => { deletes++; };
    b.listContainerDirectory = (id, directory) => {
      f.calls.push([id, directory]);
      return directory === '文档/子目录' ? '0' : '0\u001e' + (directory ? '子目录' : '文档') + '\u001f1\u001f0\u001f0\u001f0\u001e文件.txt\u001f0\u001f0\u001f10\u001f0';
    };
    w.eval(read('ui/card_session.js'));
    await wait(() => !$('#authenticated-dashboard').hidden);
    $('[data-section="files"]').click();
    $('#management-containers [data-row-container="1"] .container-state').click();
    await wait(() => $('#file-list-body tr[data-file-enter]'));
    const row = $('#file-list-body tr[data-file-enter]');
    assert.equal(row.draggable, false); assert.equal($('#file-drop-targets'), null);
    const drag = new w.Event('dragstart', {bubbles:true, cancelable:true}); row.dispatchEvent(drag);
    assert.equal(drag.defaultPrevented, true);
    row.dispatchEvent(new w.Event('dragend', {bubbles:true}));
    row.dispatchEvent(new w.Event('drop', {bubbles:true}));
    assert.equal(transfers, 0); assert.ok(!d.body.textContent.includes('没有移动。'));
    if (cell === 'actions') {
      row.querySelector('[data-file-action="delete"] svg').dispatchEvent(new w.MouseEvent('click', {bubbles:true}));
      await wait(() => deletes === 1);
      assert.ok(!f.calls.some(([, dir]) => dir === '文档'));
      const file = $('#file-list-body [data-file-entry="文件.txt"]');
      file.dispatchEvent(new w.MouseEvent('contextmenu', {bubbles:true}));
      $('#file-context-rename').click();
      assert.equal($('#file-name-input').value, '文件.txt'); $('#file-name-cancel').click();
      $('#file-list-body tr[data-file-enter] [data-file-action="export"] svg').dispatchEvent(new w.MouseEvent('click', {bubbles:true}));
      await wait(() => $('#export-container-list .xfiles')?.textContent.includes('文件.txt'));
      assert.ok(!f.calls.some(([, dir]) => dir === '文档'));
      assert.equal($('#file-name-dialog').hidden, true);
    } else {
      if (cell === 'keyboard') row.dispatchEvent(new w.KeyboardEvent('keydown', {key:'Enter', bubbles:true, cancelable:true}));
      else row.children[cell].click();
      await wait(() => f.calls.some(([, dir]) => dir === '文档'));
      await wait(() => $('#file-list-body tr[data-file-enter="子目录"]'));
      $('#file-list-body tr[data-file-enter="子目录"]').click();
      await wait(() => f.calls.some(([, dir]) => dir === '文档/子目录'));
      $('#file-breadcrumb [data-file-depth="0"]').click();
      await wait(() => $('#file-list-body tr[data-file-enter="文档"]'));
    }
    assert.deepEqual(errors, []);
  } finally { w.close(); }
}
(async () => { for (const cell of [0, 1, 2, 3, 'keyboard', 'actions']) await run(cell); console.log('PASS: all folder cells, nested navigation, breadcrumb, keyboard, action isolation, context rename, blocked drag'); })().catch(e => { console.error(e); process.exitCode = 1; });
