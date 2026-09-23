const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {JSDOM,VirtualConsole}=require(path.resolve(process.argv[2]));
const root=path.resolve(__dirname,'..'),sleep=ms=>new Promise(r=>setTimeout(r,ms));
async function until(f){for(let i=0;i<500;++i){if(f())return;await sleep(10);}throw Error('Timed out: '+f);}
async function run(fail){
 const errors=[],vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e.message));
 const dom=new JSDOM(fs.readFileSync(path.join(root,'ui/card_session.html'),'utf8'),{runScripts:'outside-only',pretendToBeVisual:true,virtualConsole:vc});
 const w=dom.window,$=s=>w.document.querySelector(s);w.TextEncoder=TextEncoder;
 try{
  for(const file of ['tests/card_product_ui_navigation_mock.js','ui/card_session.js'])w.eval(fs.readFileSync(path.join(root,file),'utf8'));
  await until(()=>!$('#authenticated-dashboard').hidden&&!$('[data-create-container]').disabled);$('[data-create-container]').click();$('#partition-refresh').click();
  await until(()=>$('#partition-list-body button')&&!$('#partition-refresh').disabled);$('#partition-list-body button').click();
  await until(()=>!$('#partition-erase-consent').disabled);$('#partition-erase-consent').click();$('#partition-preserve-data').click();
  assert.equal($('#partition-erase-consent').checked,false);assert.equal($('#create-volume-continue').disabled,true);
  assert.match($('#partition-consent-text').textContent,/原地加密/);assert.doesNotMatch($('#partition-consent-text').textContent,/清空/);
  $('#partition-erase-consent').click();$('#create-volume-continue').click();await until(()=>w.navigationFixture.partitionConverted);
  assert.equal(w.navigationFixture.partitionCreated,undefined);assert.match($('#creation-cancel-work').textContent,/暂停/);
  if(fail){
   w.navigationFixture.pendingCreation.reject();await until(()=>!$('#create-volume-cancel').disabled);
   assert.match($('#create-volume-status').textContent,/未确认正常暂停/);$('#create-volume-cancel').click();
   assert.equal($('[data-container-id="101"][data-container-action="resume"]'),null);
   assert.equal($('[data-container-id="101"][data-container-action="mount"]').disabled,true);
   assert.equal($('[data-container-id="101"][data-container-action="forget"]').disabled,true);
  }else{
   $('#creation-cancel-work').click();await until(()=>$('#creation-cancel-work').disabled);
   assert.match($('#creation-cancel-work').textContent,/保存进度/);
   assert.doesNotMatch($('#create-volume-status').textContent,/进度已保存/);
   w.navigationFixture.pendingCreation.pause();await until(()=>!$('#create-volume-cancel').disabled);
   assert.match($('#create-volume-status').textContent,/进度已保存/);
   assert.equal($('#creation-progress [role="progressbar"]').getAttribute('aria-valuenow'),'23');
   assert.match($('#create-volume-title').textContent,/暂停/);$('#create-volume-cancel').click();
   assert.equal($('[data-container-id="101"][data-container-action="forget"]').disabled,true);
   $('[data-container-id="101"][data-container-action="resume"]').click();await until(()=>w.navigationFixture.partitionResumed===101);
   w.navigationFixture.pendingCreation.resolve();await until(()=>!$('#create-volume-cancel').disabled);
   assert.match($('#create-volume-status').textContent,/加密已完成/);
   assert.equal(w.navigationFixture.containers.find(c=>c.id===101).conversion,0);
  }
  assert.equal(errors.length,0,errors.join('\n'));
 }finally{w.close();}
}
(async()=>{await run(false);await run(true);console.log('PASS: preserve-data consent, pause acknowledgement, resumed completion and uncertain-state guards');})().catch(e=>{console.error(e);process.exitCode=1;});
