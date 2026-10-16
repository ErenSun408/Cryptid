// Activation reuses the login surface: state 5 shows the card number and a
// code field, reports native rejections, and opens login once accepted.
const fs=require('node:fs'), path=require('node:path'), assert=require('node:assert/strict');
const {JSDOM,VirtualConsole}=require(path.resolve(process.argv[2]));
const read=f=>fs.readFileSync(path.resolve(__dirname,'..',f),'utf8');
const tick=()=>new Promise(r=>setTimeout(r,30));
const wait=async f=>{for(let i=0;i<150;i++){if(f())return;await tick();}throw Error('UI timeout: '+f);};
(async()=>{
 for(const scenario of ['missing','expired','rewound']){
  const errors=[],vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e.message));
  const dom=new JSDOM(read('ui/card_session.html'),{runScripts:'outside-only',pretendToBeVisual:true,virtualConsole:vc});
  const w=dom.window;w.TextEncoder=TextEncoder;
  try{
   w.eval(read('tests/card_product_ui_navigation_mock.js'));
   const f=w.navigationFixture,bridge=w.chrome.webview.hostObjects.nativeBridge;
   f.state=5;
   const submitted=[];
   bridge.getActivationStatus=()=>({missing:0,expired:2,rewound:3})[scenario];
   bridge.getActivationCardNumber=()=>f.state===5?'1A2B-3C4D':'';
   bridge.submitActivationCode=code=>{
    submitted.push(code);
    if(code.trim()==='OTHER')return 2;
    if(code.trim()!=='GOOD')return 1;
    f.state=2;return 0;
   };
   w.eval(read('ui/card_session.js'));
   const d=w.document,$=id=>d.getElementById(id);
   await wait(()=>$('card-session-surface').dataset.sessionState==='activation-required'&&$('card-session-surface').dataset.busy==='false');
   assert.equal($('card-state-title').textContent,'需要激活');
   assert.equal($('activation-fields').hidden,false,'activation fields hidden');
   assert.equal($('password-fields').hidden,true,'password fields shown on the activation route');
   assert.equal($('activation-card-number').textContent,'1A2B-3C4D');
   assert.match($('activation-reason').textContent,
     scenario==='expired'?/已到期/:scenario==='rewound'?/系统时间/:/尚未激活/);
   assert.equal($('card-session-action-label').textContent,'激活');
   assert.equal($('card-session-action').disabled,true,'empty code could be submitted');

   const code=$('activation-code');
   code.value='OTHER';code.dispatchEvent(new w.Event('input'));
   assert.equal($('card-session-action').disabled,false);
   $('card-session-action').click();
   await wait(()=>submitted.length===1&&$('card-session-surface').dataset.busy==='false');
   assert.equal($('card-session-error').hidden,false);
   assert.match($('card-session-error').textContent,/其他安全卡/);
   assert.equal($('card-session-surface').dataset.sessionState,'activation-required');

   code.value='bad';code.dispatchEvent(new w.Event('input'));
   code.dispatchEvent(new w.KeyboardEvent('keydown',{key:'Enter',bubbles:true}));
   await wait(()=>submitted.length===2&&$('card-session-surface').dataset.busy==='false');
   assert.match($('card-session-error').textContent,/激活码无效/);

   code.value='  GOOD\n';code.dispatchEvent(new w.Event('input'));
   $('card-session-action').click();
   await wait(()=>$('card-session-surface').dataset.sessionState==='login-required'&&$('card-session-surface').dataset.busy==='false');
   assert.equal(submitted[2],'  GOOD\n','the page altered the pasted code');
   assert.equal($('activation-fields').hidden,true,'activation fields stayed after acceptance');
   assert.equal($('password-fields').hidden,false,'login did not open after acceptance');
   assert.equal(code.value,'','the accepted code stayed in the field');
   assert.match($('card-session-status').textContent,/激活成功/);
   assert.equal(errors.length,0,errors.join('\n'));
   console.log('PASS activation:',scenario);
  }finally{w.close();}
 }
 console.log('PASS: 3 activation DOM scenarios');
})().catch(e=>{console.error(e);process.exitCode=1;});
