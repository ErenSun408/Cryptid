// Presentation changes must preserve native names, IDs, and existing resume access.
const fs=require('node:fs'), path=require('node:path'), assert=require('node:assert/strict');
const {JSDOM,VirtualConsole}=require(path.resolve(process.argv[2]));
const read=f=>fs.readFileSync(path.resolve(__dirname,'..',f),'utf8');
const tick=()=>new Promise(r=>setTimeout(r,30));
const wait=async f=>{for(let i=0;i<150;i++){if(f())return;await tick();}throw Error('UI timeout: '+f);};
(async()=>{
 for(const suffix of ['.sys','.DAT','.bin','.bat','.hc','.unknown']){
  const errors=[],vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e.message));
  const dom=new JSDOM(read('ui/card_session.html'),{runScripts:'outside-only',pretendToBeVisual:true,virtualConsole:vc});
  const w=dom.window;w.TextEncoder=TextEncoder;
  try{
   w.eval(read('tests/card_product_ui_navigation_mock.js'));
   const f=w.navigationFixture,b=w.chrome.webview.hostObjects.nativeBridge;
   f.containers[0].name='资料'+suffix; f.containers[1].name='资料.dat';
   f.containers[2].name='分区.sys';f.containers[2].partition=true;
   let writes=0;b.renameContainer=()=>{writes++;};
   b.getOperationCount=()=>1;b.getOperationCompletedAtUnixMs=()=>Date.now();
   b.getOperationKind=()=>1;b.getOperationOutcome=()=>0;
   b.getOperationContainerName=()=>f.containers[0].name;b.getOperationDriveLetter=()=>'';
   w.eval(read('ui/card_session.js'));
   const d=w.document,$=id=>d.getElementById(id),expected=suffix==='.unknown'?'资料.unknown':'资料';
   await wait(()=>!$('authenticated-dashboard').hidden);
   assert.equal(d.querySelector('[data-row-container="1"] .container-name').textContent,expected);
   assert.equal(d.querySelector('[data-row-container="2"] .container-name').textContent,'资料');
   assert.equal(d.querySelector('[data-row-container="3"] .container-name').textContent,'分区.sys');
   d.querySelector('[data-container-id="1"][data-container-action="details"]').click();await tick();
   assert.ok(d.querySelector('.container-details').textContent.includes(expected));
   assert.equal(f.containers[0].name,'资料'+suffix);assert.equal(writes,0);
   d.querySelector('[data-create-container]').click();await tick();
   assert.equal($('create-volume-dialog').dataset.storage,'file');
   assert.equal(d.querySelector('[data-create-storage="disk"]'),null);
   assert.equal(d.querySelectorAll('[data-create-storage]').length,1);
   assert.equal($('create-disk-step').hidden,true);assert.equal(f.partitionScans,undefined);
   assert.equal($('create-container-form').hidden,false);
   d.querySelector('[data-create-kind="hidden"]').click();await tick();
   assert.equal($('create-container-b-form').hidden,false);assert.equal($('create-disk-step').hidden,true);
   assert.equal(errors.length,0,errors.join('\n'));
  }finally{w.close();}
 }
 const dom=new JSDOM(read('ui/card_session.html'),{runScripts:'outside-only',pretendToBeVisual:true});
 const w=dom.window;w.TextEncoder=TextEncoder;
 try{
  w.eval(read('tests/card_product_ui_navigation_mock.js'));const f=w.navigationFixture;
  Object.assign(f.containers[1],{partition:true,conversion:2});w.eval(read('ui/card_session.js'));
  await tick();w.document.querySelector('[data-container-id="2"][data-container-action="resume"]').click();
  await wait(()=>f.partitionResumed===2);assert.equal(w.document.getElementById('create-volume-dialog').dataset.storage,'disk');
  f.pendingCreation.resolve();await wait(()=>!w.document.getElementById('create-volume-cancel').disabled);
  assert.equal(f.containers[1].conversion,0);
 }finally{w.close();}
 console.log('PASS: suffix display, native names and IDs, file-only creation, both file shapes, existing partition resume');
})().catch(e=>{console.error(e);process.exitCode=1;});
