// A finished mount offers Explorer and never opens it unasked: confirm or a
// click outside closes, Open shows the volume (or This PC for several).
const fs=require('fs'),path=require('path'),assert=require('node:assert/strict');
const {JSDOM}=require(path.resolve(process.argv[2]));
const read=f=>fs.readFileSync(path.resolve(__dirname,'..',f),'utf8');
const tick=(ms=20)=>new Promise(r=>setTimeout(r,ms));
const wait=async p=>{for(let i=0;i<200;i++){if(p())return;await tick();}throw Error('mount open UI timeout: '+p);};
(async()=>{for(const scenario of ['open','confirm','outside','failure','unmount','batch-one','batch-many','creation']){
 const dom=new JSDOM(read('ui/card_session.html'),{runScripts:'outside-only',pretendToBeVisual:true});
 const w=dom.window;w.TextEncoder=TextEncoder;
 try {
  w.eval(read('tests/card_product_ui_navigation_mock.js'));
  const f=w.navigationFixture;
  if(scenario==='creation')Object.assign(f.containers[1],{partition:true,conversion:2});
  if(scenario==='batch-many')Object.assign(f.containers[2],{mounted:false,size:0,drive:''});
  w.eval(read('ui/card_session.js'));
  const q=s=>w.document.querySelector(s),el=id=>w.document.getElementById(id);
  const dialog=el('volume-operation-dialog'),open=el('volume-operation-open'),confirm=el('volume-operation-confirm');
  const opened=()=>Array.from(f.explorerOpened||[]);
  await wait(()=>!el('authenticated-dashboard').hidden);
  if(scenario==='creation'){
   q('[data-container-id="2"][data-container-action="resume"]').click();
   await wait(()=>f.partitionResumed===2);f.pendingCreation.resolve();
   await wait(()=>!el('create-volume-open').hidden);
   assert.equal(el('create-volume-cancel').classList.contains('primary'),true);
   assert.equal(el('create-volume-cancel-label').textContent,'确定');
   assert.equal(el('create-volume-open').compareDocumentPosition(el('create-volume-cancel'))&w.Node.DOCUMENT_POSITION_FOLLOWING,w.Node.DOCUMENT_POSITION_FOLLOWING);
   el('create-volume-open').click();await wait(()=>el('create-volume-dialog').hidden);
   assert.deepEqual(opened(),[2]);
   assert.equal(el('create-volume-cancel').classList.contains('primary'),false);assert.equal(el('create-volume-open').hidden,true);
   console.log('PASS mount open:',scenario);continue;
  }
  if(scenario.startsWith('batch')){
   const all=el('mount-select-all');all.checked=true;all.dispatchEvent(new w.Event('change'));
   el('dashboard-mount').click();
   await wait(()=>!el('batch-result-dialog').hidden);
   const button=el('batch-result-open');
   assert.equal(button.hidden,false);assert.equal(button.nextElementSibling,el('batch-result-confirm'));
   button.click();await wait(()=>el('batch-result-dialog').hidden);
   assert.deepEqual(opened(),[scenario==='batch-one'?2:0]);
   console.log('PASS mount open:',scenario);continue;
  }
  q('#dashboard-section-dashboard [data-container-id="2"][data-container-action="mount"]').click();
  await wait(()=>dialog.dataset.result==='success');
  assert.equal(open.hidden,false);assert.equal(open.nextElementSibling,confirm);
  if(scenario==='open'){
   await tick(3300);assert.equal(dialog.hidden,false,'a finished mount closed itself');
   open.click();open.click();await wait(()=>dialog.hidden);assert.deepEqual(opened(),[2],"a double click opened twice");
  }
  if(scenario==='confirm'){confirm.click();assert.equal(dialog.hidden,true);assert.deepEqual(opened(),[]);}
  if(scenario==='outside'){
   dialog.dispatchEvent(new w.MouseEvent('pointerdown',{button:0,bubbles:true}));
   assert.equal(dialog.hidden,true);assert.deepEqual(opened(),[]);
  }
  if(scenario==='failure'){
   f.failExplorer=true;open.click();await wait(()=>/无法打开资源管理器/.test(el('volume-operation-note').textContent));
   assert.equal(dialog.hidden,false);
  }
  if(scenario==='unmount'){
   confirm.click();
   q('#dashboard-section-dashboard [data-container-id="2"][data-container-action="unmount"]').click();
   await wait(()=>dialog.dataset.result==='success');
   assert.equal(open.hidden,true);
   await wait(()=>dialog.hidden);assert.deepEqual(opened(),[]);
  }
 } finally {w.close();}
 console.log('PASS mount open:',scenario);
}console.log('PASS: 8 mount open DOM scenarios');})().catch(e=>{console.error(e);process.exitCode=1;});
