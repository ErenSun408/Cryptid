const fs=require('fs'),assert=require('node:assert/strict');
const {JSDOM}=require(require('path').resolve(process.argv[2]));
const tick=()=>new Promise(r=>setTimeout(r,30));
const wait=async predicate=>{for(let i=0;i<100;i++){if(predicate())return;await tick();}throw Error('UI timeout');};
(async()=>{for(const mode of ['success','partial','session']){
 const dom=new JSDOM(fs.readFileSync('ui/card_session.html','utf8'),{runScripts:'outside-only',pretendToBeVisual:true});const w=dom.window;w.TextEncoder=TextEncoder;
 try{
 w.eval(fs.readFileSync('tests/card_product_ui_navigation_mock.js','utf8'));let pending=[];w.chrome.webview.hostObjects.nativeBridge.exportContainerEntry=(...args)=>new Promise((resolve,reject)=>pending.push({resolve,reject,args}));
 w.eval(fs.readFileSync('ui/card_session.js','utf8'));await tick();const d=w.document,$=id=>d.getElementById(id);
 d.querySelector('[data-section=backup]').click();
 $('backup-export-confirm').dispatchEvent(new w.Event('click'));await tick();assert.equal(pending.length,0);
 d.querySelector('[data-export-select-all="1"]').click();await tick();
 $('backup-export-confirm').dispatchEvent(new w.Event('click'));await tick();assert.equal(pending.length,0);
 assert.equal($('export-dialog').hidden,false);assert.equal($('export-dialog-start').disabled,true);
 $('export-dialog-location').click();await tick();assert.equal($('export-confirm-dialog'),null);
 $('export-dialog-start').click();$('export-dialog-start').dispatchEvent(new w.Event('click'));
 await wait(()=>pending.length===1);assert.equal($('export-result-dialog').hidden,false);assert.equal($('export-result-confirm').disabled,true);assert.equal(d.querySelectorAll('#export-result-list [data-state=running]').length,2);assert.match($('export-result-summary').textContent,/0\/2/);
 $('export-result-dialog').dispatchEvent(new w.KeyboardEvent('keydown',{key:'Escape',bubbles:true}));assert.equal($('export-result-dialog').hidden,false);
 if(mode==='session'){
 w.navigationFixture.state=0;w.dispatchEvent(new w.Event('usb-vault-session-changed'));pending[0].resolve();await tick();await tick();assert.equal($('export-result-dialog').hidden,true);assert.equal($('export-result-list').children.length,0);assert.equal(pending.length,1);continue;
 }
 pending[0].resolve();await wait(()=>pending.length===2);assert.match($('export-result-summary').textContent,/1\/2/);assert.equal(d.querySelector('#export-result-list .batch-result-row').dataset.state,'success');
 if(mode==='partial')pending[1].reject(Error('simulated failure'));else pending[1].resolve();await wait(()=>!$('export-result-confirm').disabled);assert.match($('export-result-summary').textContent,mode==='partial'?/成功：1.*失败：1/:/成功：2.*失败：0/);
 assert.equal($('backup-export-confirm').textContent,'下一步');assert.equal($('backup-export-confirm').disabled,mode!=='partial');
 $('export-result-confirm').click();assert.equal($('export-result-list').children.length,0);assert.equal($('export-result-dialog').hidden,true);assert.equal($('authenticated-dashboard').hasAttribute('inert'),false);
 }finally{w.close();}
}console.log('PASS: export pending, per-entry results, totals, partial selection, close cleanup, session interruption');})().catch(e=>{console.error(e);process.exitCode=1});
