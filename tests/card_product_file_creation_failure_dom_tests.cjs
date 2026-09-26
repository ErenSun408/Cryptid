const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { JSDOM, VirtualConsole } = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, '..');
async function until(check) {
  for (let i = 0; i < 500; ++i) {
    if (check()) return;
    await new Promise(resolve => setTimeout(resolve, 10));
  }
  throw new Error('Timed out: ' + check);
}
async function run(kind) {
  const errors = [];
  const vc = new VirtualConsole();
  vc.on('jsdomError', error => errors.push(error.message));
  const dom = new JSDOM(fs.readFileSync(path.join(root, 'ui/card_session.html'), 'utf8'),
    { runScripts: 'outside-only', pretendToBeVisual: true, virtualConsole: vc });
  const w = dom.window;
  w.TextEncoder = TextEncoder;
  const $ = selector => w.document.querySelector(selector);
  try {
    w.eval(fs.readFileSync(path.join(root, 'tests/card_product_ui_navigation_mock.js'), 'utf8'));
    const bridge = w.chrome.webview.hostObjects.nativeBridge;
    let reads = 0;
    bridge.getCreationError = () => { ++reads; return kind === 'unknown' ? 123456 : 19; };
    if (kind === 'rejected') bridge.beginCreateContainerMib = () => { throw new w.Error('rejected'); };
    w.eval(fs.readFileSync(path.join(root, 'ui/card_session.js'), 'utf8'));
    await until(() => !$('#authenticated-dashboard').hidden && !$('[data-create-container]').disabled);
    $('[data-create-container]').click();
    await until(() => $('#partition-list-body button') && !$('#partition-refresh').disabled);
    $('[data-create-storage="file"]').click();
    $('[data-create-kind="encrypted"]').click();
    for (const [id, value] of [['name', 'work'], ['directory', 'D:\\Export'], ['capacity', '64']]) {
      const field = $('#create-container-' + id);
      field.value = value;
      field.dispatchEvent(new w.Event('input', { bubbles: true }));
    }
    await until(() => !$('#create-volume-continue').disabled);
    $('#create-volume-continue').click();
    if (kind !== 'rejected') {
      await until(() => w.navigationFixture.pendingCreation);
      w.navigationFixture.creationProgress = kind === 'cancel' ? '4,3,41,0,3'
        : kind === 'cleanup' ? '5,3,41,0,3' : '3,3,41,0,3';
    }
    await until(() => ['error', 'cancelled'].includes($('#create-volume-status').dataset.tone));
    const message = $('#create-volume-status').textContent;
    if (kind === 'full') assert.match(message, /剩余空间不足.*错误码19/);
    if (kind === 'unknown') assert.match(message, /原因无法确定.*错误码123456/);
    if (kind === 'rejected') { assert.match(message, /错误码2147500037/); assert.equal(reads, 0); }
    if (kind === 'cancel') { assert.match(message, /已取消创建，没有留下容器文件/); assert.equal(reads, 0); }
    if (kind === 'cleanup') { assert.doesNotMatch(message, /没有留下容器文件/); assert.equal(reads, 0); }
    if (kind !== 'cleanup') assert.match(message, /重新填写名称与路径/);
    assert.equal($('#create-container-name').value, '');
    assert.equal($('#create-container-directory').value, '');
    assert.deepEqual(errors, []);
  } finally { w.close(); }
}
(async () => {
  for (const kind of ['full', 'unknown', 'rejected', 'cancel', 'cleanup']) await run(kind);
  console.log('File creation failure DOM tests: 5 passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
