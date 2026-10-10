const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const { JSDOM, VirtualConsole } = require(path.resolve(process.argv[2]));
const read = file => fs.readFileSync(path.resolve(__dirname, '..', file), 'utf8');
const settle = () => new Promise(resolve => setTimeout(resolve, 100));
const scenarios = ['enter', 'blur', 'escape', 'unchanged', 'empty', 'invalid', 'ime', 'ime-blur',
  'failure', 'pending', 'session-edit', 'session-save', 'session-save-success', 'persistence-off', 'persistence-error',
  'management', 'refresh', 'readback-failure', 'keyboard'];
(async () => {
 for (const scenario of scenarios) {
  const errors = [], console = new VirtualConsole();
  console.on('jsdomError', error => errors.push(error.message));
  const dom = new JSDOM(read('ui/card_session.html'), { runScripts: 'outside-only', pretendToBeVisual: true, virtualConsole: console });
  const w = dom.window; w.TextEncoder = TextEncoder;
  try {
   w.eval(read('tests/card_product_ui_navigation_mock.js'));
   const fixture = w.navigationFixture, bridge = w.chrome.webview.hostObjects.nativeBridge;
   const writes = []; let resolve, reject, fail = scenario === 'failure';
   const pending = ['pending', 'session-save', 'session-save-success'].includes(scenario);
   const nativeName = fixture.containers[0].name;
   bridge.getRememberContainerPaths = () => {
    if (scenario === 'persistence-error') throw new Error('settings unavailable');
    return scenario !== 'persistence-off';
   };
   bridge.renameContainer = (id, name) => {
    writes.push({ id, name });
    if (fail) return Promise.reject(new Error('write failed'));
    const apply = () => { fixture.containers.find(item => item.id === id).name = name || 'original.hc'; };
    if (pending) return new Promise((success, failure) => { resolve = () => { apply(); success(); }; reject = failure; });
    apply();
    if (scenario === 'readback-failure') bridge.getSessionState = () => { throw new Error('read failed'); };
   };
   w.eval(read('ui/card_session.js')); await settle();
   const d = w.document;
   if (scenario === 'management') { d.querySelector('[data-section=files]').click(); await settle(); }
   const list = scenario === 'management' ? d.getElementById('management-containers') : d.querySelector('[data-container-list]');
   assert.ok(list, 'list missing');
   const row = list.querySelector('[data-row-container="1"]');
   const name = row.querySelector('.container-name');
   assert.equal(name.hasAttribute('title'), false, 'name should not show a hover tooltip');
   name.click();
   assert.equal(list.querySelector('.container-details'), null, 'single name click expanded details');
   if (scenario === 'keyboard') name.dispatchEvent(new w.KeyboardEvent('keydown', { key:'F2', bubbles:true }));
   else name.dispatchEvent(new w.MouseEvent('dblclick', { bubbles:true }));
   let input = row.querySelector('.container-name-input');
   if (scenario.startsWith('persistence-')) {
    assert.equal(input, null); assert.equal(writes.length, 0);
    assert.match(d.getElementById('dashboard-status').textContent, /持久化安全盘列表/); continue;
   }
   assert.ok(input, `${scenario}: editor missing`);
   assert.equal(input.hasAttribute('title'), false, 'editor should not show a hover tooltip');
   assert.equal(d.activeElement, input); assert.equal(input.value, nativeName);
   assert.equal(input.maxLength, 100);
   assert.equal(list.querySelector('.container-details'), null);
   const key = (key, extra={}) => input.dispatchEvent(new w.KeyboardEvent('keydown', { key, bubbles:true, cancelable:true, ...extra }));
   const change = value => { input.value = value; input.dispatchEvent(new w.Event('input', {bubbles:true})); };
   if (scenario === 'escape') {
    change('discard'); key('Escape'); assert.equal(writes.length, 0);
    assert.equal(input.value, ''); assert.equal(list.querySelector('.container-name').textContent, nativeName); continue;
   }
   if (scenario === 'unchanged') {
    key('Enter'); await settle(); assert.equal(writes.length, 0); assert.equal(input.isConnected, false); continue;
   }
   if (scenario === 'session-edit') {
    change('secret draft'); fixture.state=2; w.dispatchEvent(new w.Event('usb-vault-session-changed')); await settle();
    assert.equal(writes.length,0); assert.equal(input.value,''); assert.equal(input.isConnected,false); continue;
   }
   if (scenario === 'invalid') {
    for (const invalid of ['  ', 'a/b', 'a\\b', 'a\0b', '\ud800', 'x'.repeat(101)]) {
     change(invalid); key('Enter'); await settle();
     assert.equal(writes.length,0); assert.equal(input.getAttribute('aria-invalid'),'true'); assert.equal(d.activeElement,input);
    }
    key('Escape'); continue;
   }
   const wanted = scenario === 'empty' ? '' : '资料 🔒 <img src=x>';
   change(wanted);
   if (scenario === 'refresh') {
    input.setSelectionRange(2,4);
    d.querySelector('[data-section=dashboard]').click();
    assert.equal(list.querySelector('.container-name-input'),input,'refresh replaced active editor');
    assert.equal(input.selectionStart,2); assert.equal(input.selectionEnd,4); assert.equal(writes.length,0);
   }
   if (scenario === 'ime' || scenario === 'ime-blur') {
    input.dispatchEvent(new w.CompositionEvent('compositionstart',{bubbles:true}));
    key('Enter',{isComposing:true}); key('Escape',{keyCode:229});
    if (scenario === 'ime-blur') input.blur();
    assert.equal(writes.length,0); assert.equal(input.isConnected,true);
    input.dispatchEvent(new w.CompositionEvent('compositionend',{bubbles:true}));
    if (scenario === 'ime') key('Enter');
   } else if (scenario === 'blur') input.blur();
   else key('Enter');
   if (pending) {
    assert.equal(row.querySelector('.container-name-status').textContent, '', 'saving should not display a progress message');
    assert.equal(writes.length,1); assert.equal(input.readOnly,true);
    assert.ok(d.getElementById('authenticated-dashboard').hasAttribute('inert'));
    key('Enter'); input.dispatchEvent(new w.Event('blur')); assert.equal(writes.length,1);
    if (scenario.startsWith('session-save')) {
     fixture.state=2; w.dispatchEvent(new w.Event('usb-vault-session-changed'));
     assert.equal(input.value,'');
     if (scenario === 'session-save-success') resolve(); else reject(new Error('old session'));
     await settle();
     assert.equal(d.getElementById('authenticated-dashboard').hidden,true);
     assert.equal(d.getElementById('authenticated-dashboard').hasAttribute('inert'),false); continue;
    }
    resolve();
   }
   await settle();
   if (scenario === 'failure') {
    assert.equal(input.value,wanted); assert.equal(input.readOnly,false); assert.equal(d.activeElement,input);
    assert.equal(fixture.containers[0].name,nativeName);
    assert.match(row.querySelector('.container-name-status').textContent,/保存失败/);
    fail=false; key('Enter'); await settle(); assert.equal(writes.length,2);
   } else assert.equal(writes.length,1);
   assert.equal(writes[0].id,1); assert.equal(writes[0].name,wanted);
   assert.equal(input.value,''); assert.equal(input.isConnected,false);
   assert.equal(d.getElementById('authenticated-dashboard').hasAttribute('inert'),false);
   if (scenario === 'readback-failure') {
    assert.match(d.getElementById('dashboard-status').textContent,/已保存.*刷新失败/); continue;
   }
   for (const label of d.querySelectorAll('[data-container-list] [data-row-container="1"] .container-name')) {
    assert.equal(label.textContent,wanted || 'original'); assert.equal(label.querySelector('img'),null);
   }
   assert.equal(fixture.containers[0].mounted,true,'renaming changed mount state');
   assert.equal(JSON.stringify(fixture.pageEvents).includes(wanted), wanted === '', 'alias leaked into page diagnostics');
  } finally {
   w.close(); assert.deepEqual(errors, [], `${scenario}: browser script error`);
  }
 }
 console.log(`container rename DOM: ${scenarios.length} scenarios passed`);
})().catch(error => { console.error(error); process.exitCode=1; });
