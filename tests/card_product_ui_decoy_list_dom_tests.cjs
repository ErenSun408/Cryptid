const fs=require('fs'),assert=require('node:assert/strict');const {JSDOM}=require(require('path').resolve(process.argv[2]));
const settle=()=>new Promise(r=>setTimeout(r,120));
(async()=>{for(const mode of ['drag','keyboard','failure']){
 const dom=new JSDOM(fs.readFileSync('ui/card_session.html','utf8'),{runScripts:'outside-only',pretendToBeVisual:true});const w=dom.window;w.TextEncoder=TextEncoder;try{
 w.eval(fs.readFileSync('tests/card_product_ui_navigation_mock.js','utf8'));w.eval(fs.readFileSync('ui/card_session.js','utf8'));await settle();const d=w.document;
 d.querySelector('[data-section=decoy]').click();d.getElementById('decoy-password').value='handover';d.getElementById('decoy-verify-submit').click();d.getElementById('decoy-confirm-accept').click();await settle();
 const list=d.getElementById('decoy-container-list');const rows=[...list.children];const grip=rows[0].querySelector('.grab');w.navigationFixture.failOrder=mode==='failure';
 rows.forEach((row,i)=>row.getBoundingClientRect=()=>({left:0,top:i*70,width:800,height:60}));grip.setPointerCapture=()=>{};
 if(mode==='keyboard')grip.dispatchEvent(new w.KeyboardEvent('keydown',{bubbles:true,altKey:true,key:'ArrowDown'}));
 else {const pointer=(name,y)=>{const e=new w.MouseEvent(name,{bubbles:true,button:0,clientY:y});Object.defineProperty(e,'pointerId',{value:1});return e;};grip.dispatchEvent(pointer('pointerdown',10));d.dispatchEvent(pointer('pointermove',200));d.dispatchEvent(pointer('pointerup',200));}
 await settle();assert.equal([...list.children].map(r=>r.dataset.decoyContainer).join(','),mode==='failure'?'1,3':'3,1');
 if(mode!=='failure')assert.equal(w.navigationFixture.orderIds.join(','),'3,2,1','noncandidate slot moved');
 d.getElementById('decoy-refresh').click();assert.equal(d.getElementById('decoy-refresh').dataset.refreshing,'true');assert.equal(w.getComputedStyle(d.getElementById('decoy-refresh')).cursor,'pointer');await settle();
 assert.equal([...list.children].map(r=>r.dataset.decoyContainer).join(','),mode==='failure'?'1,3':'3,1');
 if(mode==='keyboard') {
 d.getElementById('decoy-exit').click();assert.equal(d.getElementById('decoy-verify').hidden,false);assert.equal(d.getElementById('decoy-password').value,'');assert.equal(list.children.length,0);
 d.getElementById('decoy-password').value='handover';d.getElementById('decoy-verify-submit').click();d.getElementById('decoy-confirm-accept').click();await settle();
 }
 list.querySelector('[data-decoy-container="3"] .container-actions button').click();await settle();await settle();assert.equal(d.getElementById('decoy-maintenance-drive').textContent,'盘符 Y:');
 d.querySelector('[data-decoy-back]').click();
 if(mode==='failure') {w.navigationFixture.failEndMaintenance=true;d.getElementById('decoy-exit').click();await settle();await settle();assert.equal(d.getElementById('decoy-results').hidden,false);assert.match(d.getElementById('decoy-results-status').textContent,/无法退出/);w.navigationFixture.failEndMaintenance=false;}
 d.getElementById('decoy-exit').click();await settle();await settle();assert.equal(d.getElementById('decoy-verify').hidden,false);assert.equal(d.getElementById('decoy-password').value,'');assert.equal(list.children.length,0);assert.equal(w.navigationFixture.endedMaintenance.at(-1),3);
 }finally{w.close();}}
 console.log('PASS: advanced update drag, keyboard, rollback, refresh cursor, drive display');})().catch(e=>{console.error(e);process.exitCode=1});