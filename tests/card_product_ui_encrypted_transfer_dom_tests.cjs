const fs=require('fs'),assert=require('node:assert/strict');
const {JSDOM}=require(require('path').resolve(process.argv[2]));
const tick=()=>new Promise(resolve=>setTimeout(resolve,30));
const wait=async predicate=>{for(let i=0;i<200;i++){if(predicate())return;await tick();}throw Error('UI timeout');};
const load=()=>{
 const dom=new JSDOM(fs.readFileSync('ui/card_session.html','utf8'),{runScripts:'outside-only',pretendToBeVisual:true});
 const w=dom.window;w.TextEncoder=TextEncoder;
 w.eval(fs.readFileSync('tests/card_product_ui_navigation_mock.js','utf8'));
 const bridge=w.chrome.webview.hostObjects.nativeBridge,calls=[];let choice='2|family photos',status='2,10,10,0,2';
 for(const name of ['beginEncryptedExport','beginEncryptedImport','beginVerifyImport','beginImportIntoNewContainerMib','beginImportIntoNewContainerBMib'])
  bridge[name]=(...args)=>{calls.push({name,args:args.map(a=>Array.isArray(a)?[...a]:a),arrays:args.filter(Array.isArray)});return true;};
 bridge.encryptedTransferStatus=()=>status;bridge.cancelEncryptedTransfer=()=>{status='4,0,0,0,1';return true;};
 bridge.chooseImportFile=()=>choice;bridge.adoptUndecidedImportFile=()=>{calls.push({name:'adopt'});return true;};
 bridge.treatUndecidedImportAsPackage=()=> 'family photos';bridge.getVerifiedImportMinimumMib=()=>120;
 w.navigationFixture.chosenLocation='D:\\Export';w.eval(fs.readFileSync('ui/card_session.js','utf8'));
 return {w,d:w.document,$:id=>w.document.getElementById(id),calls,setChoice:value=>{choice=value;},setStatus:value=>{status=value;}};
};
(async()=>{
 {
  const {w,d,$,calls}=load();try{
   await tick();d.querySelector('[data-section=backup]').click();$('export-mode-encrypted').click();
   const toolbar=d.querySelector('.export-toolbar');
   for(const id of ['export-mode-plain','export-mode-encrypted','export-mode-migrate','backup-export-confirm'])assert.ok(toolbar.contains($(id)));
   assert.equal($('export-mode-import'),null);assert.equal($('backup-export-location'),null);
   d.querySelector('[data-export-select-all="1"]').click();await wait(()=>!$('backup-export-confirm').disabled);
   $('backup-export-confirm').click();assert.equal($('export-dialog').hidden,false);assert.equal($('export-dialog-name').disabled,true);
   $('export-dialog-location').click();await tick();$('export-dialog-start').click();assert.match($('export-dialog-error').textContent,/加密口令/);
   $('export-dialog-passphrase').value='long enough export pass';$('export-dialog-passphrase-confirmation').value='long enough export pazz';
   $('export-dialog-start').click();assert.match($('export-dialog-error').textContent,/不一致/);assert.equal(calls.length,0);
   $('export-dialog-passphrase-confirmation').value='long enough export pass';$('export-dialog-start').click();
   await wait(()=>!$('export-result-confirm').disabled);
   const exports=calls.filter(call=>call.name==='beginEncryptedExport');assert.equal(exports.length,2);
   for(const call of exports){assert.equal(call.args[3],'D:\\Export');assert.equal(call.args[4],call.args[2]);assert.ok(call.arrays.every(a=>a.every(b=>b===0)));}
   assert.equal($('export-dialog-passphrase').value,'');$('export-result-confirm').click();
  }finally{for(let i=0;i<10;i++)await tick();w.close();}
 }
 {
  const {w,d,$}=load();try{
   await tick();d.querySelector('[data-section=backup]').click();$('export-mode-encrypted').click();
   d.querySelector('[data-export-toggle="1"] .xrow-open').click();await tick();d.querySelector('[data-export-select-all="1"]').click();await tick();
   $('backup-export-confirm').click();$('export-dialog-passphrase').value='typed passphrase';
   $('export-dialog-passphrase-confirmation').focus();$('export-dialog').dispatchEvent(new w.KeyboardEvent('keydown',{key:'Tab',bubbles:true}));
   assert.ok($('export-dialog').contains(d.activeElement));
   const removed=w.navigationFixture.containers.find(c=>c.id===1);removed.mounted=false;removed.drive='';removed.size=0;w.dispatchEvent(new w.Event('usb-vault-mounts-changed'));
   await wait(()=>$('export-dialog').hidden);assert.match($('backup-export-status').textContent,/不可用/);assert.equal($('export-dialog-passphrase').value,'');
  }finally{for(let i=0;i<10;i++)await tick();w.close();}
 }
 {
  const {w,d,$,calls}=load();try{
   await tick();d.querySelector('[data-mount-existing]').click();await wait(()=>!$('import-dialog').hidden);
   assert.equal($('import-package-name').textContent,'family photos');d.querySelector('#import-container-list .xrow-open').click();
   $('import-password').value='import passphrase';$('import-confirm').click();await wait(()=>$('import-dialog').hidden);
   assert.equal(calls.filter(call=>call.name==='beginEncryptedImport').length,1);assert.ok(calls[0].arrays[0].every(b=>b===0));
  }finally{for(let i=0;i<10;i++)await tick();w.close();}
 }
 for(const asPackage of [false,true]){
  const {w,d,$,calls,setChoice}=load();try{
   await tick();setChoice('3|unnamed');d.querySelector('[data-mount-existing]').click();await wait(()=>!$('import-kind-dialog').hidden);
   $(asPackage?'import-kind-package':'import-kind-container').click();
   if(asPackage){await wait(()=>!$('import-dialog').hidden);$('import-password').value='pending';w.dispatchEvent(new w.Event('usb-vault-session-changed'));await wait(()=>$('import-dialog').hidden);assert.equal($('import-password').value,'');}
   else await wait(()=>calls.some(call=>call.name==='adopt'));
  }finally{for(let i=0;i<10;i++)await tick();w.close();}
 }
 console.log('PASS: export dialog, per-entry names, passphrase wiping, unavailable selection, unified imports and session reset');
})().catch(error=>{console.error(error);process.exitCode=1});
