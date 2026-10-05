const fs=require('fs'),assert=require('node:assert/strict');
const {JSDOM}=require(require('path').resolve(process.argv[2]));
const tick=()=>new Promise(r=>setTimeout(r,20));
const wait=async p=>{for(let i=0;i<200;i++){if(p())return;await tick();}throw Error('mount UI timeout');};
(async()=>{for(const scenario of ['ordinary','busy','failure','session','maintenance']){
 const dom=new JSDOM(fs.readFileSync('ui/card_session.html','utf8'),{runScripts:'outside-only',pretendToBeVisual:true});
 const w=dom.window;w.TextEncoder=TextEncoder;
 try {
  w.eval(fs.readFileSync('tests/card_product_ui_navigation_mock.js','utf8'));
  const fixture=w.navigationFixture,bridge=w.chrome.webview.hostObjects.nativeBridge;
  w.eval(fs.readFileSync('ui/card_session.js','utf8'));
  const q=s=>w.document.querySelector(s),el=id=>w.document.getElementById(id);
  await wait(()=>!el('authenticated-dashboard').hidden);
  const removed=id=>{const c=fixture.containers.find(c=>c.id===id);c.mounted=false;c.size=0;c.drive='';};
  const notice=()=>w.dispatchEvent(new w.Event('usb-vault-mounts-changed'));
  const state=id=>q(`#dashboard-section-dashboard [data-row-container="${id}"] .container-state`).textContent;
  if(scenario==='maintenance'){
   q('[data-section=decoy]').click();el('decoy-password').value='handover';el('decoy-verify-submit').click();el('decoy-confirm-accept').click();
   await wait(()=>!el('decoy-results').hidden);
   for(const id of [1,3]){
    q(`#decoy-container-list [data-decoy-container="${id}"] .container-actions button`).click();
    await wait(()=>fixture.measuredMaintenance.includes(id) && !el('decoy-maintenance').hidden && !el('decoy-maintenance-end').disabled);
    if(id===1)q('[data-decoy-back]').click();
   }
   const capacity=el('decoy-maintenance-writable').textContent;
   removed(1);notice();await wait(()=>state(1)==='未挂载');
   assert.equal(el('decoy-maintenance-container').value,'3');
   assert.equal(el('decoy-maintenance-writable').textContent,capacity);
   assert.equal(el('decoy-verify').hidden,true);
   removed(3);notice();await wait(()=>el('decoy-maintenance-drive').textContent.includes('该安全盘未挂载'));
   assert.equal(el('decoy-maintenance-writable').textContent,'—');
   assert.equal(fixture.dropArmed,false);assert.equal(fixture.surveyPasswords.length,1);
   q('[data-decoy-back]').click();assert.match(q('#decoy-container-list [data-decoy-container="3"] .container-actions button').textContent,/更新/);
   continue;
  }
  if(scenario==='busy'){
   fixture.delayVolume=true;
   q('#dashboard-section-dashboard [data-container-id="2"][data-container-action="mount"]').click();
   await wait(()=>typeof fixture.completeVolume==='function');
   removed(1);notice();await tick();assert.equal(state(1),'已挂载');
   fixture.completeVolume();await wait(()=>state(1)==='未挂载');
   await wait(()=>el('volume-operation-dialog').dataset.result!=='running');
   assert.equal(el('volume-operation-dialog').hidden,false);assert.equal(state(2),'已挂载');continue;
  }
  if(scenario==='session'){
   let release;const original=bridge.getContainerCount;
   bridge.getContainerCount=()=>new Promise(r=>release=r);
   notice();await wait(()=>release);
   fixture.state=0;w.dispatchEvent(new w.Event('usb-vault-session-changed'));
   bridge.getContainerCount=original;release(fixture.containers.length);
   await wait(()=>el('authenticated-dashboard').hidden);assert.equal(el('export-result-dialog').hidden,true);continue;
  }
  if(scenario==='failure'){
   const original=bridge.getContainerCount;let calls=0;
   bridge.getContainerCount=()=>{calls++;throw Error('E_PENDING');};removed(1);notice();await wait(()=>calls>0);
   assert.equal(state(1),'已挂载');assert.equal(el('authenticated-dashboard').hidden,false);
   bridge.getContainerCount=original;
   await wait(()=>state(1)==='未挂载');assert.equal(state(3),'已挂载');continue;
  }
  q('[data-section=backup]').click();q('[data-export-select-all="1"]').click();await tick();
  assert.match(el('backup-export-confirm').textContent,/2/);
  // Unsubmitted settings content must survive the mount-only refresh.
  el('exit-hotkey-key').value='unsaved draft';el('exit-hotkey-count').value='7';
  removed(1);notice();await wait(()=>state(1)==='未挂载');
  assert.equal(state(3),'已挂载');assert.equal(el('backup-export-confirm').textContent,'开始导出');
  assert.equal(el('exit-hotkey-key').value,'unsaved draft');assert.equal(el('exit-hotkey-count').value,'7');
  assert.equal(el('dashboard-section-backup').classList.contains('active'),true);
  assert.equal(fixture.pageEvents.some(e=>/无法确认安全会话/.test(e.content)),false);
 } finally {w.close();}
 console.log('PASS mount synchronization:',scenario);
}console.log('PASS: 5 mount synchronization DOM scenarios');})().catch(e=>{console.error(e);process.exitCode=1;});
