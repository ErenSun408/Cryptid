// The removable entry reuses the partition picker: it scans external media
// only and shows in-place encryption for NTFS alone.
const fs=require('node:fs'), path=require('node:path'), assert=require('node:assert/strict');
const {JSDOM,VirtualConsole}=require(path.resolve(process.argv[2]));
const read=f=>fs.readFileSync(path.resolve(__dirname,'..',f),'utf8');
const tick=()=>new Promise(r=>setTimeout(r,30));
const wait=async f=>{for(let i=0;i<150;i++){if(f())return;await tick();}throw Error('UI timeout: '+f);};
(async()=>{
 for(const scenario of ['ntfs','exfat','empty']){
  const errors=[],vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e.message));
  const dom=new JSDOM(read('ui/card_session.html'),{runScripts:'outside-only',pretendToBeVisual:true,virtualConsole:vc});
  const w=dom.window;w.TextEncoder=TextEncoder;
  try{
   w.eval(read('tests/card_product_ui_navigation_mock.js'));
   const f=w.navigationFixture;
   if(scenario==='exfat')f.partitionNotNtfs=[1];
   if(scenario==='empty')f.partitionList='';
   w.eval(read('ui/card_session.js'));
   const d=w.document,$=id=>d.getElementById(id);
   await wait(()=>!$('authenticated-dashboard').hidden);
   d.querySelector('[data-create-container]').click();await tick();
   const tab=d.querySelector('[data-create-storage="disk"]');
   assert.equal(tab.textContent,'移动安全盘');
   assert.equal(tab.previousElementSibling,d.querySelector('[data-create-storage="file"]'));
   assert.equal($('create-volume-dialog').dataset.storage,'file');
   tab.click();
   await wait(()=>f.partitionScans===1&&!d.querySelector('#partition-list-body').textContent.includes('正在读取'));
   assert.equal(f.partitionScope,1,'removable scan included internal volumes');
   assert.equal($('create-volume-description').textContent,'安全盘为 U 盘、移动硬盘或 SD 卡上的分区');
   if(scenario==='empty'){
    assert.equal(d.querySelector('#partition-list-body .partition-empty').textContent,'暂无可用的移动介质');
   }else{
    const preserve=$('partition-preserve-data');
    if(scenario==='exfat'){preserve.checked=true;preserve.dispatchEvent(new w.Event('change'));}
    d.querySelector('[data-partition-id="1"]').click();
    await wait(()=>f.partitionSelected===1&&!preserve.closest('#create-disk-step').querySelector('[data-partition-id="1"]').disabled);
    assert.equal(preserve.closest('label').hidden,scenario==='exfat','in-place option does not follow NTFS');
    assert.equal(preserve.checked,false,'non-NTFS selection kept in-place encryption');
   }
   assert.equal(errors.length,0,errors.join('\n'));
   console.log('PASS removable:',scenario);
  }finally{w.close();}
 }
 console.log('PASS: 3 removable DOM scenarios');
})().catch(e=>{console.error(e);process.exitCode=1;});
