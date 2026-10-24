#pragma once

#include <pgmspace.h>

// First-run setup wizard, served at "/" while the setup access point is up.
static const char kSetupHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Frame Setup</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1d2330;--muted:#677084;--line:#dde1e8;--accent:#4250af;--ok:#1b7f4c;--err:#b3261e;--sel:#e8ebfa}
@media (prefers-color-scheme:dark){:root{--bg:#11141a;--card:#1a1f28;--fg:#e6e9ef;--muted:#98a1b3;--line:#2b3240;--accent:#8c9cff;--ok:#5fd08f;--err:#ff8a80;--sel:#262d45}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:480px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:8px 0 4px}
.sub{color:var(--muted);margin:0 0 18px;font-size:14px}
section{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:14px}
h2{font-size:16px;margin:0 0 10px;display:flex;align-items:center;gap:8px}
.n{display:inline-grid;place-items:center;width:22px;height:22px;border-radius:50%;background:var(--accent);color:#fff;font-size:13px}
label{display:block;font-size:13px;color:var(--muted);margin:12px 0 4px}
input{width:100%;padding:11px 12px;border:1px solid var(--line);border-radius:10px;background:var(--bg);color:var(--fg);font:inherit}
button{width:100%;padding:13px;border-radius:10px;border:0;background:var(--accent);color:#fff;font:inherit;font-weight:600;cursor:pointer}
button:disabled{opacity:.55}
button.link{background:none;color:var(--accent);width:auto;padding:4px 0;font-weight:500}
ul{list-style:none;margin:0;padding:0;border:1px solid var(--line);border-radius:10px;max-height:260px;overflow:auto}
li{display:flex;align-items:center;gap:10px;padding:11px 12px;border-bottom:1px solid var(--line);cursor:pointer}
li:last-child{border-bottom:0}
li.sel{background:var(--sel)}
li .name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
li .meta{color:var(--muted);font-size:13px}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:14px}
.bars i{width:3px;background:var(--line);border-radius:1px}
.bars i.on{background:var(--fg)}
.row{display:flex;justify-content:space-between;align-items:center}
.hint{font-size:12px;color:var(--muted);margin-top:6px}
.msg{margin-top:12px;font-size:14px}
.ok{color:var(--ok)}.err{color:var(--err)}
.big{font-size:20px;font-weight:600;margin:4px 0}
.spin{display:inline-block;width:16px;height:16px;border:2px solid var(--line);border-top-color:var(--accent);border-radius:50%;animation:s 1s linear infinite;vertical-align:-3px;margin-right:6px}
@keyframes s{to{transform:rotate(360deg)}}
a{color:var(--accent)}
</style>
</head>
<body>
<main>
<h1>Set up your photo frame</h1>
<p class="sub">Connect the frame to your WiFi and your Immich server. It takes about a minute.</p>

<div id="form">
<section>
<h2><span class="n">1</span>WiFi network</h2>
<div class="row"><span class="hint" id="scanNote">Looking for networks...</span><button class="link" onclick="scan(true)">Rescan</button></div>
<ul id="nets"></ul>
<label for="ssid">Network name</label>
<input id="ssid" autocomplete="off" autocapitalize="none" placeholder="Pick above or type a hidden network">
<label for="pass">Password</label>
<input id="pass" type="password" autocomplete="off">
<div class="hint">The frame only works with 2.4 GHz WiFi.</div>
</section>

<section>
<h2><span class="n">2</span>Immich server</h2>
<label for="url">Server URL</label>
<input id="url" inputmode="url" autocomplete="off" autocapitalize="none" placeholder="http://192.168.1.10:2283">
<label for="key">API key</label>
<input id="key" type="password" autocomplete="off">
<div class="hint">In Immich: Account Settings &rarr; API Keys &rarr; New API Key.</div>
</section>

<button id="go" onclick="go()">Connect</button>
<div class="msg" id="msg"></div>
</div>

<section id="progress" hidden>
<div id="pState"></div>
<div class="msg" id="pDetail"></div>
<div id="pActions"></div>
</section>
<p class="hint">Need more options? <a href="/settings">Open full settings</a></p>
</main>

<script>
const $=id=>document.getElementById(id);
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const sleep=ms=>new Promise(f=>setTimeout(f,ms));
async function api(p,o){try{const r=await fetch(p,o);return await r.json();}catch(e){return {offline:true};}}
const post=(p,b)=>api(p,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b||{})});
let keySet=false;

function bars(rssi){const n=rssi>-55?4:rssi>-67?3:rssi>-78?2:1;let h='';for(let i=1;i<=4;i++)h+='<i class="'+(i<=n?'on':'')+'" style="height:'+(i*3+2)+'px"></i>';return '<span class="bars">'+h+'</span>';}

async function scan(fresh){
  $('scanNote').textContent='Looking for networks...';
  // A scan briefly takes the radio off the AP's channel, so the first list
  // comes from the scan the frame made before starting its network.
  let j=await api('/api/scan'+(fresh?'?refresh=1':''));
  for(let i=0;(j.scanning||j.offline)&&i<25;i++){await sleep(700);j=await api('/api/scan');}
  const list=(j.networks||[]).sort((a,b)=>b.rssi-a.rssi);
  $('scanNote').textContent=list.length?list.length+' networks found':'No networks found';
  $('nets').innerHTML=list.map(n=>'<li data-ssid="'+esc(n.ssid)+'">'+bars(n.rssi)+'<span class="name">'+esc(n.ssid)+'</span><span class="meta">'+(n.secure?'&#128274;':'open')+'</span></li>').join('');
  $('nets').querySelectorAll('li').forEach(li=>li.onclick=()=>pick(li));
  const cur=$('ssid').value;$('nets').querySelectorAll('li').forEach(li=>li.classList.toggle('sel',li.dataset.ssid===cur));
}
function pick(li){$('nets').querySelectorAll('li').forEach(x=>x.classList.remove('sel'));li.classList.add('sel');$('ssid').value=li.dataset.ssid;$('pass').focus();}

async function go(){
  const b={ssid:$('ssid').value.trim(),pass:$('pass').value,serverUrl:$('url').value.trim(),apiKey:$('key').value.trim()};
  if(!b.ssid){$('msg').innerHTML='<span class="err">Choose your WiFi network first.</span>';return;}
  if(!b.serverUrl||(!b.apiKey&&!keySet)){if(!confirm('Immich server details are missing. Continue and add them later?'))return;}
  $('go').disabled=true;$('msg').textContent='';
  const r=await post('/api/setup',b);
  if(!r.ok){$('go').disabled=false;$('msg').innerHTML='<span class="err">'+esc(r.error||'Could not start')+'</span>';return;}
  $('form').hidden=true;$('progress').hidden=false;
  watch();
}

async function watch(){
  for(;;){
    const s=await api('/api/setup');
    if(s.offline){$('pDetail').textContent='Reconnecting to the frame... your phone may briefly drop its WiFi while the frame switches channel.';await sleep(1500);continue;}
    if(s.state==='connecting'){
      $('pState').innerHTML='<div class="big"><span class="spin"></span>Connecting to '+esc(s.ssid)+'</div>';
      $('pDetail').textContent='Checking the password and joining the network.';
    }else if(s.state==='failed'){
      $('pState').innerHTML='<div class="big err">Couldn\'t join '+esc(s.ssid)+'</div>';
      $('pDetail').textContent=s.error||'';
      $('pActions').innerHTML='<button onclick="retry()">Try again</button>';
      return;
    }else if(s.state==='connected'){
      const url='http://'+s.ip,host='http://'+s.hostname+'.local';
      $('pState').innerHTML='<div class="big ok">Connected to '+esc(s.ssid)+'</div>';
      $('pDetail').innerHTML=(s.immichOk?'<span class="ok">&#10003; '+esc(s.immich)+'</span>':'<span class="err">Immich: '+esc(s.immich||'not checked')+'</span>')+
        '<p>After the restart, reconnect this phone to <b>'+esc(s.ssid)+'</b> and open <a href="'+url+'">'+url+'</a> (or <a href="'+host+'">'+host+'</a>) to pick albums and slideshow options.</p>'+
        '<p class="hint">The frame restarts in '+Math.max(0,s.rebootIn||0)+' s.</p>';
      $('pActions').innerHTML='<button onclick="finish()">Restart now</button>';
    }else{return;}
    await sleep(1000);
  }
}
function retry(){$('progress').hidden=true;$('form').hidden=false;$('go').disabled=false;$('pActions').innerHTML='';scan(true);}
async function finish(){await post('/api/setup/finish');$('pState').innerHTML='<div class="big">Restarting...</div>';$('pActions').innerHTML='';}

(async()=>{
  const s=await api('/api/setup');
  if(!s.offline){if(s.ssid)$('ssid').value=s.ssid;if(s.serverUrl)$('url').value=s.serverUrl;keySet=s.apiKeySet;
    if(keySet)$('key').placeholder='Already saved - leave blank to keep';
    if(s.state==='connecting'||s.state==='connected'){$('form').hidden=true;$('progress').hidden=false;watch();}}
  scan();
})();
</script>
</body>
</html>
)HTML";

// Single-page configuration portal served at "/" once on the home network,
// and at "/settings" in setup mode.
static const char kPortalHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Immich Frame</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1d2330;--muted:#677084;--line:#dde1e8;--accent:#4250af;--ok:#1b7f4c;--err:#b3261e}
@media (prefers-color-scheme:dark){:root{--bg:#11141a;--card:#1a1f28;--fg:#e6e9ef;--muted:#98a1b3;--line:#2b3240;--accent:#8c9cff;--ok:#5fd08f;--err:#ff8a80}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:640px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:4px 0 16px}
h2{font-size:15px;margin:0 0 12px;text-transform:uppercase;letter-spacing:.04em;color:var(--muted)}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin-bottom:14px}
label{display:block;font-size:13px;color:var(--muted);margin:10px 0 4px}
input,select{width:100%;padding:9px 10px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit}
.row{display:flex;gap:10px}.row>*{flex:1}
.check{display:flex;align-items:center;gap:8px;margin-top:12px;color:var(--fg);font-size:14px}
.check input{width:auto}
button{padding:9px 14px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--fg);font:inherit;cursor:pointer}
button.primary{background:var(--accent);border-color:var(--accent);color:#fff}
button.danger{color:var(--err)}
.btns{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px}
.msg{margin-top:10px;font-size:14px;min-height:1em}
.ok{color:var(--ok)}.err{color:var(--err)}
.kv{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;font-size:14px}
.kv span:nth-child(odd){color:var(--muted)}
.controls{display:flex;gap:8px;margin-top:12px}.controls button{flex:1;font-size:18px}
a{color:var(--accent)}
.hint{font-size:12px;color:var(--muted);margin-top:4px}
.thumb{display:block;max-width:100%;max-height:240px;margin:0 auto 12px;border-radius:8px}
</style>
</head>
<body>
<main>
<h1>Immich Frame</h1>

<section>
<h2>Now showing</h2>
<a id="thumbLink" target="_blank" rel="noopener" hidden><img id="thumb" class="thumb" alt="Photo on the frame"></a>
<div class="kv" id="status"><span>Status</span><span>Loading...</span></div>
<div class="controls">
<button onclick="cmd('prev')" title="Previous">&#9664;</button>
<button onclick="cmd('pause')" id="pauseBtn" title="Pause / resume">&#10073;&#10073;</button>
<button onclick="cmd('next')" title="Next">&#9654;</button>
</div>
<div class="msg" id="nowMsg"></div>
</section>

<section>
<h2>Slideshow</h2>
<label for="source">Show photos from</label>
<select id="source" onchange="sourceChanged()">
<option value="all">All photos</option>
<option value="album">An album</option>
<option value="person">A person</option>
<option value="tag">A tag</option>
<option value="favorites">Favorites</option>
</select>
<div id="itemWrap" hidden>
<label for="item" id="itemLabel">Album</label>
<div class="row"><select id="item"></select><button style="flex:0" onclick="loadItems(true)" title="Reload list">&#8635;</button></div>
</div>
<div class="row">
<div><label for="order">Order</label>
<select id="order"><option value="random">Shuffle</option><option value="newest">Newest first</option><option value="oldest">Oldest first</option></select></div>
<div><label for="interval">Seconds per photo</label><input id="interval" type="number" min="5" max="86400"></div>
</div>
<div class="row">
<div><label for="fill">Scaling</label>
<select id="fill"><option value="0">Fit whole photo</option><option value="1">Fill screen (crop)</option></select></div>
<div><label for="rotation">Orientation</label>
<select id="rotation"><option value="0">Portrait</option><option value="2">Portrait (flipped)</option><option value="1">Landscape</option><option value="3">Landscape (flipped)</option></select></div>
</div>
<label for="transition">Transition</label>
<select id="transition">
<option value="random">Random</option><option value="none">None (draw while loading)</option>
<option value="wipe-right">Wipe right</option><option value="wipe-left">Wipe left</option>
<option value="wipe-down">Wipe down</option><option value="wipe-up">Wipe up</option>
<option value="slide-left">Slide in from right</option><option value="slide-right">Slide in from left</option>
<option value="slide-down">Slide in from top</option><option value="slide-up">Slide in from bottom</option>
<option value="blinds">Blinds</option><option value="dissolve">Dissolve</option>
<option value="box-out">Box out</option><option value="diagonal">Diagonal</option>
</select>
<label class="check"><input id="showDate" type="checkbox"> Show the date the photo was taken</label>
<div class="btns"><button class="primary" onclick="save('show')">Save slideshow</button></div>
<div class="msg" id="showMsg"></div>
</section>

<section>
<h2>Display colours</h2>
<div class="row">
<div><label for="colorBgr">Colour order</label>
<select id="colorBgr" onchange="saveColors()"><option value="0">RGB</option><option value="1">BGR (swap red and blue)</option></select></div>
<div><label for="colorInvert">Inversion</label>
<select id="colorInvert" onchange="saveColors()"><option value="0">Normal</option><option value="1">Inverted</option></select></div>
</div>
<div class="hint">Show the test pattern, then change these until each bar matches its label and the background is black. Changes apply instantly.</div>
<div class="btns"><button onclick="cmd('pattern')">Show test pattern</button><button onclick="cmd('next')">Back to photos</button></div>
<div class="msg" id="colorMsg"></div>
</section>

<section>
<h2>Immich server</h2>
<label for="serverUrl">Server URL</label>
<input id="serverUrl" placeholder="http://192.168.1.10:2283" autocomplete="off">
<label for="apiKey">API key</label>
<input id="apiKey" type="password" autocomplete="off">
<div class="hint">Create one in Immich under Account Settings &rarr; API Keys. It needs asset, album, person, tag and user read access.</div>
<label class="check"><input id="insecureTls" type="checkbox"> Accept self-signed HTTPS certificates</label>
<div class="btns"><button onclick="test()">Test connection</button><button class="primary" onclick="save('immich')">Save server</button></div>
<div class="msg" id="immichMsg"></div>
</section>

<section>
<h2>WiFi</h2>
<label for="ssid">Network</label>
<div class="row"><input id="ssid" list="ssids" autocomplete="off"><button style="flex:0" onclick="scan()">Scan</button></div>
<datalist id="ssids"></datalist>
<label for="wifiPass">Password</label>
<input id="wifiPass" type="password" autocomplete="off">
<div class="btns"><button class="primary" onclick="save('wifi')">Save and reconnect</button></div>
<div class="msg" id="wifiMsg"></div>
</section>

<section>
<h2>Device</h2>
<div class="row">
<div><label for="hostname">Hostname</label><input id="hostname"></div>
<div><label for="adminPass">Portal password</label><input id="adminPass" type="password" autocomplete="new-password"></div>
</div>
<div class="hint">Log in as <b>admin</b> when a password is set. Leave it blank to keep the current one.</div>
<label class="check"><input id="clearAdminPass" type="checkbox"> Remove portal password</label>
<div class="btns">
<button class="primary" onclick="save('device')">Save device</button>
<button onclick="cmd('calibrate')">Calibrate touch</button>
<button onclick="cmd('reload')">Reload photos</button>
<button onclick="cmd('reboot')">Restart</button>
<button onclick="cmd('setup')" title="Restart into the setup WiFi network, keeping all settings">Re-run setup</button>
<button class="danger" onclick="if(confirm('Erase all settings including WiFi?'))cmd('factory')">Factory reset</button>
</div>
<label for="fw">Firmware update (.bin)</label>
<div class="row"><input id="fw" type="file" accept=".bin"><button style="flex:0" onclick="ota()">Upload</button></div>
<div class="msg" id="devMsg"></div>
</section>
</main>

<script>
const $=id=>document.getElementById(id);
let cfg={},itemsCache={};
function note(el,text,ok){const m=$(el);m.textContent=text;m.className='msg '+(ok?'ok':'err');}
async function api(path,opts){const r=await fetch(path,opts);let j={};try{j=await r.json();}catch(e){}if(!r.ok&&j.ok===undefined)j.ok=false;return j;}
function post(path,body){return api(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body||{})});}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}

async function load(){
  cfg=await api('/api/config');
  $('source').value=cfg.source;$('order').value=cfg.order;$('interval').value=cfg.intervalSec;
  $('fill').value=cfg.fill?'1':'0';$('rotation').value=String(cfg.rotation);$('showDate').checked=cfg.showDate;$('transition').value=cfg.transition||'random';
  $('colorBgr').value=cfg.colorBgr?'1':'0';$('colorInvert').value=cfg.colorInvert?'1':'0';
  $('serverUrl').value=cfg.serverUrl||'';$('apiKey').placeholder=cfg.apiKeySet?'Saved (leave blank to keep)':'Paste API key';
  $('insecureTls').checked=cfg.insecureTls;$('ssid').value=cfg.wifiSsid||'';
  $('wifiPass').placeholder=cfg.wifiPassSet?'Saved (leave blank to keep)':'';
  $('hostname').value=cfg.hostname;$('adminPass').placeholder=cfg.adminPassSet?'Set (leave blank to keep)':'None';
  sourceChanged(true);
}

function sourceChanged(initial){
  const t=$('source').value,needs=['album','person','tag'].includes(t);
  $('itemWrap').hidden=!needs;
  if(!needs)return;
  $('itemLabel').textContent={album:'Album',person:'Person',tag:'Tag'}[t];
  if(initial===true&&cfg.sourceId){$('item').innerHTML='<option value="'+esc(cfg.sourceId)+'">'+esc(cfg.sourceName||cfg.sourceId)+'</option>';}
  loadItems(false);
}

async function loadItems(force){
  const t=$('source').value,sel=$('item'),want=sel.value||cfg.sourceId;
  if(force||!itemsCache[t]){
    sel.innerHTML='<option value="" disabled selected>Loading...</option>';
    const r=await api('/api/sources?type='+t);
    if(!Array.isArray(r)){sel.innerHTML='';note('showMsg',r.error||'Could not load list',false);return;}
    r.sort((a,b)=>a.name.localeCompare(b.name));itemsCache[t]=r;
  }
  const list=itemsCache[t];
  sel.innerHTML=list.length?list.map(i=>'<option value="'+esc(i.id)+'">'+esc(i.name)+(i.count!==undefined?' ('+i.count+')':'')+'</option>').join(''):'<option value="">Nothing found</option>';
  if(list.some(i=>i.id===want))sel.value=want;
}

async function save(part){
  const b={},msg={show:'showMsg',immich:'immichMsg',wifi:'wifiMsg',device:'devMsg'}[part];
  if(part==='show'){
    const t=$('source').value;b.source=t;b.order=$('order').value;b.intervalSec=parseInt($('interval').value)||30;
    b.fill=$('fill').value==='1';b.rotation=parseInt($('rotation').value);b.showDate=$('showDate').checked;b.transition=$('transition').value;
    if(['album','person','tag'].includes(t)){
      const o=$('item').selectedOptions[0];
      if(!o||!o.value){note(msg,'Pick an item from the list first',false);return;}
      b.sourceId=o.value;b.sourceName=(itemsCache[t]||[]).find(i=>i.id===o.value)?.name||o.textContent;
    }
  }else if(part==='immich'){
    b.serverUrl=$('serverUrl').value;b.apiKey=$('apiKey').value;b.insecureTls=$('insecureTls').checked;
  }else if(part==='wifi'){
    if(!$('ssid').value){note(msg,'Enter a network name',false);return;}
    b.wifiSsid=$('ssid').value;b.wifiPass=$('wifiPass').value;
  }else{
    b.hostname=$('hostname').value;b.adminPass=$('adminPass').value;b.clearAdminPass=$('clearAdminPass').checked;
  }
  const r=await post('/api/config',b);
  if(!r.ok){note(msg,r.error||'Save failed',false);return;}
  note(msg,r.reboot?'Saved. Restarting - reconnect to your WiFi and open http://'+($('hostname').value||'immich-frame')+'.local':'Saved',true);
  if(!r.reboot){$('apiKey').value='';$('adminPass').value='';load();}
}

async function saveColors(){
  const r=await post('/api/config',{colorBgr:$('colorBgr').value==='1',colorInvert:$('colorInvert').value==='1'});
  note('colorMsg',r.ok?'Applied':'Failed: '+(r.error||''),r.ok);
}

async function test(){
  note('immichMsg','Testing...',true);
  const r=await post('/api/test',{serverUrl:$('serverUrl').value,apiKey:$('apiKey').value,insecureTls:$('insecureTls').checked});
  note('immichMsg',r.ok?r.info:r.error,r.ok);
}

async function scan(){
  note('wifiMsg','Scanning...',true);
  let j=await api('/api/scan?refresh=1');
  for(let i=0;j.scanning&&i<20;i++){await new Promise(f=>setTimeout(f,700));j=await api('/api/scan');}
  const r=j.networks;
  if(!Array.isArray(r)){note('wifiMsg','Scan failed',false);return;}
  r.sort((a,b)=>b.rssi-a.rssi);
  $('ssids').innerHTML=r.map(n=>'<option value="'+esc(n.ssid)+'">'+n.rssi+' dBm'+(n.secure?'':' (open)')+'</option>').join('');
  note('wifiMsg',r.length+' networks found - pick one from the Network field',true);
}

let nextWatch=0;
// After "next": if the photo wasn't prepared yet, say so until it appears.
async function watchNext(before){
  const id=++nextWatch;
  for(let i=0;i<60&&id===nextWatch;i++){
    await new Promise(f=>setTimeout(f,500));
    const s=await api('/api/status');
    if(s.assetId&&s.assetId!==before){note('nowMsg','Showing the next photo',true);setTimeout(()=>{if(id===nextWatch)$('nowMsg').textContent='';},2500);status();return;}
  }
  if(id===nextWatch)note('nowMsg','Still working on it - check the last error below',false);
}

async function cmd(c){
  const r=await post('/api/cmd?c='+c);
  if(r.ok&&(c==='next'||c==='prev')){
    if(c==='next'&&!r.nextReady)note('nowMsg','Downloading the next photo... it will appear in a few seconds',true);
    else if(c==='prev')note('nowMsg','Loading the previous photo...',true);
    else $('nowMsg').textContent='';
    watchNext(r.assetId);
    return;
  }
  if(!r.ok)note('devMsg',r.error||'Failed',false);
  else if(c==='calibrate')note('devMsg','Touch the arrows shown on the screen',true);
  else if(c==='reboot'||c==='factory')note('devMsg','Restarting...',true);
  else if(c==='setup')note('devMsg','Restarting into setup mode - join the WiFi network shown on the frame',true);
  setTimeout(status,800);
}

async function ota(){
  const f=$('fw').files[0];if(!f){note('devMsg','Choose a firmware .bin first',false);return;}
  const fd=new FormData();fd.append('firmware',f,f.name);
  note('devMsg','Uploading '+Math.round(f.size/1024)+' KB...',true);
  const r=await api('/update',{method:'POST',body:fd});
  note('devMsg',r.ok?'Update installed, restarting...':'Update failed: '+(r.error||'unknown'),r.ok);
}

let thumbId='';
// Fetched through the frame, only when the photo changes.
function thumb(s){
  if(!s.assetId||s.assetId===thumbId)return;
  thumbId=s.assetId;
  $('thumbLink').href=s.serverUrl+'/photos/'+s.assetId;
  $('thumb').onload=()=>{$('thumbLink').hidden=false;};
  $('thumb').onerror=()=>{$('thumbLink').hidden=true;thumbId='';};
  $('thumb').src='/api/thumb?id='+encodeURIComponent(s.assetId);
}

function ago(s){return s<60?s+' s ago':s<3600?Math.round(s/60)+' min ago':Math.round(s/3600)+' h ago';}

async function status(){
  const s=await api('/api/status');if(s.ok===false){if(s.error)note('nowMsg',s.error,false);return;}
  const rows=[];
  if(s.apMode)rows.push(['Mode','Setup access point - connect it to your WiFi below']);
  if(s.assetId)rows.push(['Photo',(s.assetDate?esc(s.assetDate)+' &middot; ':'')+'<a target="_blank" rel="noopener" href="'+esc(s.serverUrl)+'/photos/'+esc(s.assetId)+'">open in Immich</a>']);
  rows.push(['State',(s.paused?'Paused':'Playing')+(s.loading?' &middot; downloading a photo':s.nextReady?' &middot; next photo ready':'')]);
  if(s.lastError)rows.push(['Last error','<span class="err">'+esc(s.lastError)+'</span> <span class="hint">('+ago(s.lastErrorAgo)+')</span>']);
  rows.push(['Shown',s.shown+' photos since start'+(s.skipped?' &middot; '+s.skipped+' skipped':'')]);
  rows.push(['Network',(s.ssid?esc(s.ssid)+' ('+s.rssi+' dBm) &middot; ':'')+esc(s.ip)]);
  rows.push(['Device','fw '+esc(s.version)+' &middot; heap '+Math.round(s.freeHeap/1024)+' KB (min '+Math.round(s.minFreeHeap/1024)+')']);
  $('status').innerHTML=rows.map(r=>'<span>'+r[0]+'</span><span>'+r[1]+'</span>').join('');
  thumb(s);
  $('pauseBtn').innerHTML=s.paused?'&#9654;&#10073;':'&#10073;&#10073;';
}

// Polling holds this device's claim on the portal; a hidden tab lets it go.
load();status();setInterval(()=>{if(!document.hidden)status();},5000);
document.addEventListener('visibilitychange',()=>{if(!document.hidden)status();});
</script>
</body>
</html>
)HTML";
