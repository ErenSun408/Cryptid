const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const { JSDOM } = require(path.resolve(process.argv[2]));
const read = file => fs.readFileSync(path.resolve(__dirname, '..', file), 'utf8');
const settle = () => new Promise(resolve => setTimeout(resolve, 100));
(async () => {
 for (const scenario of ['dashboard', 'management', 'vertical', 'short', 'cancel', 'mounted', 'keyboard', 'session']) {
  const dom = new JSDOM(read('ui/card_session.html'), {runScripts:'outside-only', pretendToBeVisual:true});
  const w = dom.window; w.TextEncoder = TextEncoder;
  try {
   w.eval(read('tests/card_product_ui_navigation_mock.js'));
   w.eval(read('ui/card_session.js')); await settle();
   const d = w.document;
   if (scenario === 'management') d.querySelector('[data-section=files]').click();
   const list = scenario === 'management' ? d.getElementById('management-containers') : d.querySelector('[data-container-list]');
   const row = list.querySelector(`[data-row-container="${scenario === 'mounted' ? 1 : 2}"]`);
   const rail = row.parentElement, remove = rail.querySelector('.container-swipe-actions');
   const forget = remove.querySelector('[data-container-action=forget]'), erase = remove.querySelector('[data-container-action=delete-file]');
   assert.equal(forget.textContent,'从列表移除'); assert.equal(erase.textContent,'从磁盘删除');
   assert.ok(remove.hidden); assert.equal(row.querySelector('[data-container-action=forget]'), null);
   const facts = row.querySelector('.container-facts');
   const pointer = (name,x,y) => { const e=new w.MouseEvent(name,{bubbles:true,cancelable:true,button:0,clientX:x,clientY:y}); Object.defineProperty(e,'pointerId',{value:1}); return e; };
   if (scenario === 'keyboard') {
    row.querySelector('.container-name').dispatchEvent(new w.KeyboardEvent('keydown',{key:'ArrowLeft',bubbles:true,cancelable:true}));
   } else {
    facts.dispatchEvent(pointer('pointerdown',250,50));
    if (['dashboard', 'management'].includes(scenario)) {
     d.dispatchEvent(pointer('pointermove',226,50));
     assert.equal(row.style.transform,'translateX(-24px)');
     assert.equal(remove.style.clipPath,'inset(0 0 0 152px)','the reveal must match the 24px drag');
     assert.equal(w.getComputedStyle(remove).width,'176px');
    }
    d.dispatchEvent(pointer('pointermove',scenario === 'short' ? 235 : 100,scenario === 'vertical' ? 300 : 50));
    d.dispatchEvent(pointer(scenario === 'cancel' ? 'pointercancel' : 'pointerup',100,50));
    row.dispatchEvent(new w.MouseEvent('click',{bubbles:true,cancelable:true}));
   }
   if (['vertical','short','cancel'].includes(scenario)) { assert.ok(remove.hidden); continue; }
   assert.equal(remove.hidden,false); assert.equal(row.querySelector('.container-facts'),facts);
   assert.equal(list.querySelector('.container-details'),null,'swipe must not expand details');
   if (scenario === 'mounted') { assert.ok(forget.disabled && erase.disabled); continue; }
   if (scenario === 'session') {
    w.navigationFixture.state=2; w.dispatchEvent(new w.Event('usb-vault-session-changed')); await settle();
    assert.ok(remove.hidden || !remove.isConnected); continue;
   }
   if (scenario === 'keyboard') {
    assert.equal(d.activeElement,forget);
    forget.dispatchEvent(new w.KeyboardEvent('keydown',{key:'Escape',bubbles:true,cancelable:true}));
    assert.ok(remove.hidden); continue;
   }
   if (scenario === 'dashboard') {
    // Closing an open rail takes a drag past the middle of "从列表移除"
    // (a quarter of the 176px rail); a shorter drag leaves it open.
    const drag = dx => {
     facts.dispatchEvent(pointer('pointerdown',300,50));
     d.dispatchEvent(pointer('pointermove',300 + dx,50));
     d.dispatchEvent(pointer('pointerup',300 + dx,50));
    };
    drag(40); assert.equal(remove.hidden,false,'a drag short of the middle keeps the rail open');
    drag(60); assert.ok(remove.hidden,'a drag past the middle of the first action closes the rail');
    continue;
   }
   forget.click(); assert.equal(d.getElementById('forget-container-dialog').hidden,false);
   d.getElementById('forget-container-cancel').click();
   assert.ok(w.navigationFixture.containers.some(item=>item.id===2),'cancel removed record');
  } finally { w.close(); }
 }
 console.log('container swipe DOM: 8 scenarios passed');
})().catch(error=>{ console.error(error);process.exitCode=1; });
