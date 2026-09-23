const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const {JSDOM, VirtualConsole} = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, '..');
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(f) { for (let i=0;i<500;i++) { if(f())return; await sleep(10); } throw Error('Timeout: '+f); }
async function run(preserve, cancel) {
 const errors=[], vc=new VirtualConsole(); vc.on('jsdomError',e=>errors.push(e.message));
 const dom=new JSDOM(fs.readFileSync(path.join(root,'ui/card_session.html'),'utf8'),{runScripts:'outside-only',pretendToBeVisual:true,virtualConsole:vc});
 const w=dom.window; w.TextEncoder=TextEncoder; const $=s=>w.document.querySelector(s);
 try {
  for(const f of ['tests/card_product_ui_navigation_mock.js','ui/card_session.js'])w.eval(fs.readFileSync(path.join(root,f),'utf8'));
  await until(()=>!$('#authenticated-dashboard').hidden && !$('[data-create-container]').disabled);
  $('[data-create-container]').click(); $('#partition-refresh').click();
  await until(()=>$('#partition-list-body button') && !$('#partition-refresh').disabled);
  $('#partition-list-body button').click(); await until(()=>!$('#partition-erase-consent').disabled);
  if(preserve)$('#partition-preserve-data').click();
  $('#partition-erase-consent').click();
  w.navigationFixture.bitlockerProgress='1,1,100';
  $('#create-volume-continue').click(); await until(()=>!$('#creation-bitlocker').hidden);
  assert.equal($('#create-volume-dialog').hidden,false);
  assert.equal($('#creation-progress').hidden,true);
  assert.match($('#bitlocker-description').textContent,/已被 Windows BitLocker 完全加密/);
  assert.match($('#bitlocker-description').textContent, /T:/);
  assert.equal($('#bitlocker-description img'),null);
  assert.equal($('#bitlocker-continue').disabled,true);
  $('#bitlocker-continue').dispatchEvent(new w.MouseEvent('click'));
  assert.equal(w.navigationFixture.bitlockerConfirmed,undefined);
  $('#bitlocker-help').click(); await until(()=>w.navigationFixture.bitlockerHelpOpened);
  if(cancel) {
   $('#bitlocker-cancel').click();
   assert.equal(w.navigationFixture.bitlockerConfirmed,undefined);
   w.navigationFixture.creationProgress='4,0,0,0,0';
   await until(()=>!$('#create-volume-cancel').disabled);
   assert.equal($('#creation-bitlocker').hidden,true);
  } else {
   $('#bitlocker-consent').click(); assert.equal($('#bitlocker-continue').disabled,false);
   $('#bitlocker-continue').click(); await until(()=>!$('#bitlocker-decryption').hidden);
   assert.equal(w.navigationFixture.bitlockerConfirmed,true);
   assert.match($('#bitlocker-progress-text').textContent,/20%/);
   assert.equal($('#bitlocker-continue').hidden,true);
   assert.equal($('#creation-progress').hidden,true);
   w.navigationFixture.bitlockerProgress='2,3,40';
   await until(()=>$('#bitlocker-progress-text').textContent.includes('60%'));
   w.navigationFixture.bitlockerProgress='0,0,0';
   await until(()=>$('#creation-bitlocker').hidden && !$('#creation-progress').hidden);
   w.navigationFixture.pendingCreation.resolve();
   await until(()=>!$('#create-volume-cancel').disabled);
   assert.match($('#create-volume-status').textContent,/创建成功|分区加密已完成/);
  }
  assert.equal(errors.length,0,errors.join('\n'));
 } finally {w.close();}
}
(async()=>{for(const p of [false,true])for(const c of [false,true])await run(p,c);console.log('PASS: BitLocker consent, fixed help, cancellation, progress and both creation continuations');})().catch(e=>{console.error(e);process.exitCode=1;});

