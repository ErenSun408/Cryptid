// Settings show the logged-in card's grant in local time and replace it with
// a new code; an expired grant is reported without ending the session.
const fs=require('node:fs'), path=require('node:path'), assert=require('node:assert/strict');
const {JSDOM,VirtualConsole}=require(path.resolve(process.argv[2]));
const read=f=>fs.readFileSync(path.resolve(__dirname,'..',f),'utf8');
const tick=()=>new Promise(r=>setTimeout(r,30));
const wait=async f=>{for(let i=0;i<150;i++){if(f())return;await tick();}throw Error('UI timeout: '+f);};
const expiry=1793375999; // 2026-10-30T15:59:59Z
const localText=s=>{const d=new Date(s*1000),p=v=>String(v).padStart(2,'0'),o=-d.getTimezoneOffset();
 return `${d.getFullYear()}-${p(d.getMonth()+1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}（UTC${o<0?'-':'+'}${p(Math.floor(Math.abs(o)/60))}:${p(Math.abs(o)%60)}）`;};
(async()=>{
 for(const scenario of ['active','expired']){
  const errors=[],vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e.message));
  const dom=new JSDOM(read('ui/card_session.html'),{runScripts:'outside-only',pretendToBeVisual:true,virtualConsole:vc});
  const w=dom.window;w.TextEncoder=TextEncoder;
  try{
   w.eval(read('tests/card_product_ui_navigation_mock.js'));
   const bridge=w.chrome.webview.hostObjects.nativeBridge,submitted=[];
   let status=scenario==='active'?1:2,until=expiry;
   bridge.getActivationStatus=()=>status;
   bridge.getActivationCardNumber=()=>'1A2B-3C4D';
   bridge.getActivationExpiry=()=>until;
   bridge.submitActivationCode=code=>{submitted.push(code);
    if(code!=='GOOD')return 1;status=1;until=expiry+86400;return 0;};
   w.eval(read('ui/card_session.js'));
   const d=w.document,$=id=>d.getElementById(id);
   await wait(()=>!$('authenticated-dashboard').hidden);
   d.querySelector('[data-section="settings"]').click();
   await wait(()=>$('license-card').textContent==='1A2B-3C4D');
   assert.equal($('license-expiry').textContent,
     localText(expiry)+(scenario==='expired'?'，已到期，退出后需更新激活码':''));
   assert.equal($('license-expiry').dataset.expired,scenario==='expired'?'true':'false');
   assert.equal($('license-save').disabled,true,'an empty code could be submitted');

   const code=$('license-code');
   code.value='BAD';code.dispatchEvent(new w.Event('input'));
   assert.equal($('license-save').disabled,false);
   $('license-save').click();
   await wait(()=>submitted.length===1&&!$('license-save').disabled);
   assert.equal($('license-status').textContent,'激活码无效，请确认已完整复制。');
   assert.equal($('license-status').dataset.tone,'error');

   code.value='GOOD';code.dispatchEvent(new w.Event('input'));
   $('license-save').click();
   await wait(()=>$('license-status').textContent==='激活码已更新。'&&$('license-expiry').textContent===localText(expiry+86400));
   assert.equal(code.value,'','the accepted code stayed in the field');
   assert.equal($('license-expiry').dataset.expired,'false');
   assert.equal(d.getElementById('card-session-surface').dataset.sessionState,'authenticated',
     'renewal changed the session state');
   assert.equal(errors.length,0,errors.join('\n'));
   console.log('PASS licence settings:',scenario);
  }finally{w.close();}
 }
 console.log('PASS: 2 licence settings DOM scenarios');
})().catch(e=>{console.error(e);process.exitCode=1;});
