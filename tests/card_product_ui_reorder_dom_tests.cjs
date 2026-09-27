const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const { JSDOM } = require(path.resolve(process.argv[2]));
const read = file => fs.readFileSync(path.resolve(__dirname, '..', file), 'utf8');
const settle = () => new Promise(resolve => setTimeout(resolve, 100));
(async () => {
 for (const scenario of ['success', 'failure', 'cancel', 'session-change', 'keyboard']) {
  const dom = new JSDOM(read('ui/card_session.html'), {runScripts:'outside-only', pretendToBeVisual:true});
  const w = dom.window; w.TextEncoder = TextEncoder;
  try {
   w.eval(read('tests/card_product_ui_navigation_mock.js'));
   const bridge = w.chrome.webview.hostObjects.nativeBridge;
   let complete, reject, writes = 0, reads = 0;
   bridge.reorderContainers = () => { writes++; return new Promise((resolve, fail) => { complete = resolve; reject = fail; }); };
   w.eval(read('ui/card_session.js')); await settle();
   const originalRead = bridge.getSessionState;
   bridge.getSessionState = () => { reads++; return originalRead(); };
   const lists = [...w.document.querySelectorAll('[data-container-list]')];
   const rows = lists.map(list => [...list.querySelectorAll('.container-row')]);
   const menu = w.document.getElementById('mount-items');
   const menuRows = [...menu.children];
   const dashboard = w.document.getElementById('authenticated-dashboard');
   const row = rows[0][0], grip = row.querySelector('.grab');
   const heading = w.document.getElementById('dashboard-volume-state');
   for (const [i, node] of rows[0].entries()) node.getBoundingClientRect = () => ({left:100,top:100+i*70,width:800,height:60});
   lists[0].parentElement.scrollTop = 45;
   grip.setPointerCapture = () => {};
   const pointer = (name, y) => { const e = new w.MouseEvent(name,{bubbles:true,button:0,clientX:110,clientY:y}); Object.defineProperty(e,'pointerId',{value:1}); return e; };
   if (scenario === 'keyboard') grip.dispatchEvent(new w.KeyboardEvent('keydown',{bubbles:true,altKey:true,key:'ArrowDown'}));
   else {
    grip.dispatchEvent(pointer('pointerdown',110));
    w.document.dispatchEvent(pointer('pointermove',1000));
    w.document.dispatchEvent(pointer(scenario === 'cancel' ? 'pointercancel' : 'pointerup',1000));
   }
   if (scenario === 'cancel') { assert.equal(writes,0); assert.equal(lists[0].querySelector('.container-row'),row); continue; }
   assert.equal(writes,1); assert.ok(dashboard.hasAttribute('inert'));
   for (let i=0;i<lists.length;i++) for (const node of rows[i]) assert.ok(node.isConnected && lists[i].contains(node), 'old card was rebuilt');
   assert.equal(w.document.getElementById('dashboard-volume-state'),heading);
   assert.equal(lists[0].parentElement.scrollTop,45);
   grip.dispatchEvent(new w.KeyboardEvent('keydown',{bubbles:true,altKey:true,key:'ArrowDown'})); assert.equal(writes,1);
   if (scenario === 'session-change') {
    w.navigationFixture.state=2;
    w.dispatchEvent(new w.Event('usb-vault-session-changed'));
    reject(new Error('old session')); await settle();
    assert.equal(dashboard.hidden,true); assert.equal(dashboard.hasAttribute('inert'),false); continue;
   }
   if (scenario === 'failure') reject(new Error('save failed')); else complete();
   await settle();
   assert.equal(reads,0,'sorting reloaded the full native snapshot');
   assert.equal(dashboard.hasAttribute('inert'),false);
   const expectedMenu = scenario==='failure'?menuRows:scenario==='keyboard'?[menuRows[1],menuRows[0],...menuRows.slice(2)]:[...menuRows.slice(1),menuRows[0]];
   [...menu.children].forEach((node,index)=>assert.equal(node,expectedMenu[index], 'batch menu order or identity changed'));
   for(let i=0;i<lists.length;i++) {
    const actual=[...lists[i].querySelectorAll('.container-row')];
    const expected=scenario==='failure'?rows[i]:scenario==='keyboard'?[rows[i][1],rows[i][0],...rows[i].slice(2)]:[...rows[i].slice(1),rows[i][0]];
    assert.equal(actual.length,expected.length); actual.forEach((node,index)=>assert.equal(node,expected[index]));
   }
   if(scenario==='failure') assert.match(w.document.getElementById('dashboard-status').textContent,/排序保存失败/);
  } finally { w.close(); }
 }
 console.log('local reorder DOM: 5 scenarios passed');
})().catch(error => { console.error(error); process.exitCode=1; });
