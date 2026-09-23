const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {JSDOM}=require(path.resolve(process.argv[2]));
(async()=>{
 for(const mode of ['permission-bridge','permission-flag','handshake-failed']) {
  const dom=new JSDOM(fs.readFileSync('ui/card_session.html','utf8'),{runScripts:'outside-only',pretendToBeVisual:true});
  const w=dom.window; w.TextEncoder=TextEncoder;
  if(mode==='permission-flag') w.usbVaultPermissionRequired=true;
  w.chrome={webview:{hostObjects:{nativeBridge:new Proxy({}, {get:(_,key)=>{
   if(mode==='permission-flag') throw Error('Denied launch touched host bridge');
   if(key==='getStartupView') return ()=>{if(mode==='handshake-failed') throw Error('handshake failed');return 'permission-required';};
   throw Error('Unavailable page reached '+String(key));
  }})}}};
  try {
   w.eval(fs.readFileSync('ui/card_session.js','utf8'));
   await new Promise(resolve=>setTimeout(resolve,100));
   assert.equal(w.usbVaultPageStartupStatus,mode==='handshake-failed'?1402:1);
   assert.match(w.document.body.textContent,/安全组件当前不可用/);
   if(mode!=='handshake-failed') assert.match(w.document.body.textContent,/需要管理员身份/);
   assert.equal(w.document.querySelector('#authenticated-dashboard').hidden,true);
   assert.equal(w.document.querySelector('#card-session-action').hidden,true);
   console.log('PASS: '+mode+' stays on an inert unavailable page without unmount actions');
  } finally {w.close();}
 }
})().catch(error=>{console.error(error);process.exitCode=1;});
