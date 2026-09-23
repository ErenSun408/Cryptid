const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {JSDOM}=require(path.resolve(process.argv[2]));
const dom=new JSDOM(fs.readFileSync('ui/card_session.html','utf8'),{runScripts:'outside-only',pretendToBeVisual:true});
const w=dom.window;w.TextEncoder=TextEncoder;
w.chrome={webview:{hostObjects:{nativeBridge:new Proxy({getStartupView:()=>"permission-required"},{get:(o,k)=>{if(k in o)return o[k];throw Error('Denied page reached '+String(k));}})}}};
w.eval(fs.readFileSync('ui/card_session.js','utf8'));
setTimeout(()=>{try{assert.equal(w.usbVaultPageStartupStatus,1);assert.match(w.document.body.textContent,/需要管理员权限/);assert.match(w.document.body.textContent,/安全组件当前不可用/);assert.equal(w.document.querySelector('#authenticated-dashboard').hidden,true);console.log('PASS: denied startup needs only the inert bridge and displays administrator requirement');}catch(e){console.error(e);process.exitCode=1;}finally{w.close();}},100);
