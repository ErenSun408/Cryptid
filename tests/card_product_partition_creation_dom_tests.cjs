const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {JSDOM, VirtualConsole} = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, '..');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(check) { for(let i=0;i<500;i++){if(check())return; await sleep(10);} throw Error('Timed out: '+check); }
async function run(outcome) {
 const errors=[]; const vc=new VirtualConsole(); vc.on('jsdomError', e=>errors.push(e.message));
 const dom = new JSDOM(fs.readFileSync(path.join(root,'ui/card_session.html'),'utf8'),{runScripts:'outside-only',pretendToBeVisual:true,virtualConsole:vc});
 const w=dom.window; w.TextEncoder=TextEncoder; const $=s=>w.document.querySelector(s);
 try {
  for(const file of ['tests/card_product_ui_navigation_mock.js','ui/card_session.js']) w.eval(fs.readFileSync(path.join(root,file),'utf8'));
  await until(()=>!$('#authenticated-dashboard').hidden && !$('[data-create-container]').disabled);
  $('[data-create-container]').click(); $('#partition-refresh').click();
  await until(()=>$('#partition-list-body button') && !$('#partition-refresh').disabled);
  $('#partition-list-body button').click(); await until(()=>!$('#partition-erase-consent').disabled);
  assert.equal($('#create-volume-continue').disabled,true);
  $('#create-volume-continue').dispatchEvent(new w.MouseEvent('click'));
  assert.equal(w.navigationFixture.partitionCreated,undefined);
  $('#partition-erase-consent').click(); assert.equal($('#create-volume-continue').disabled,false);
  $('#create-volume-continue').click(); await until(()=>w.navigationFixture.pendingCreation);
  assert.equal(w.navigationFixture.partitionCreated,1);
  assert.equal($('#partition-erase-consent').checked,false);
  assert.equal($('#create-volume-continue').disabled,true);
  w.navigationFixture.pendingCreation[outcome]();
  await until(()=>!$('#create-volume-cancel').disabled);
  if(outcome==='resolve') {
    assert.match($('#create-volume-status').textContent,/创建成功/);
    $('#create-volume-cancel').click();
    w.navigationFixture.containers.find(c=>c.id===100).mounted=false;
    w.navigationFixture.containers.find(c=>c.id===100).drive='';
    w.navigationFixture.containers.find(c=>c.id===100).size=0;
    w.dispatchEvent(new w.Event('usb-vault-session-changed'));
    await until(()=>w.document.querySelector('[data-container-id="100"][data-container-action="forget"]') && !w.document.querySelector('[data-container-id="100"][data-container-action="forget"]').disabled);
    const migrate = $('[data-migrate-select="100"]');
    assert.ok(migrate && migrate.classList.contains('off'));
    assert.match(migrate.title,/暂不支持分区密文导出/);
    $('[data-container-id="100"][data-container-action="forget"]').click();
    assert.match($('#forget-container-description').textContent,/无法通过文件导入找回分区/);
    assert.doesNotMatch($('#forget-container-description').textContent,/可通过「导入」/);
  }
  else {
    assert.equal($('#create-volume-status').textContent, `创建失败。错误码${outcome === "cancel" ? 16 : 17}`);
    assert.doesNotMatch($('#create-volume-status').textContent,/没有留下|未修改/);
  }
  assert.equal($('#partition-erase-consent').checked,false);
  assert.equal(errors.length,0,errors.join('\n'));
 } finally {w.close();}
}
(async()=>{for(const outcome of ['resolve','reject','cancel']) await run(outcome);console.log('PASS: explicit partition consent, success, failure, cancellation and no automatic retry');})().catch(e=>{console.error(e);process.exitCode=1;});
