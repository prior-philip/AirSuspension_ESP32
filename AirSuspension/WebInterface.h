// WebInterface.h - WiFi Access Point + Web Dashboard / Control Interface
//
// Creates its own WiFi AP — no router needed.
// Connect phone or laptop to:  SSID "AirSuspension"  PW "leveling1"
// Then open:  http://192.168.4.1
//
// Three-tab single-page app:
//   Dashboard   — live left/right pressure, target, regulation status, bag init
//   Target      — slider + number input to set target %, saves to EEPROM
//   Calibration — current stored values, live ADC readings, manual value entry
//
// REST API (used internally by the page, also usable from curl/Postman):
//   GET  /api/status       → JSON live sensor + state snapshot
//   GET  /api/calibration  → JSON stored calibration values
//   POST /api/target       → body: target=<0-100>   saves to EEPROM
//   POST /api/calibration  → body: lmin=&lmax=&rmin=&rmax=  saves to EEPROM
//
// ADC note: WiFi TX injects ~50-100 count noise on internal ADC.
// The rolling average in Sensors.h partially mitigates this.
// For sub-1% accuracy under active WiFi, add an external ADS1115.
//
#ifndef WEB_INTERFACE_H
#define WEB_INTERFACE_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>     // OTA firmware update (built into the ESP32 core)
#include <esp_ota_ops.h> // running-partition info for /api/fwinfo
#include "Config.h"
#include "Types.h"
#include "Sensors.h"
#include "EEPROM_Manager.h"

extern SensorData       sensors;
extern CalibrationData  calibration;
extern RegulationState  regulation;
extern BagInitState     bagInit;
extern PowerSaveState   powerSave;
extern AntiHuntingState antiHunt;
extern byte             targetPercent;
extern bool             calibrationMode;
extern void             resetRegulationState();
extern void             notifyUserChange(unsigned long);

static WebServer webServer(80);

// ============================================================================
// HTML — single-page app, dark theme, mobile-first
// ============================================================================

static const char HTML_INDEX[] = R"rawhtml(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Air Suspension</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#0f0f0f;--card:#1a1a1a;--bdr:#2c2c2c;--txt:#dedede;--dim:#777;--acc:#3a8fd4;--ok:#4caf50;--warn:#ff9800;--err:#ef5350;--okbg:#0e2310;--warnbg:#2c1c00;--errbg:#2c0a0a}
body{background:var(--bg);color:var(--txt);font:16px system-ui,sans-serif;min-height:100vh;max-width:520px;margin:0 auto}
header{display:flex;align-items:center;gap:10px;padding:14px 16px;border-bottom:1px solid var(--bdr)}
header h1{font-size:1rem;font-weight:500;flex:1}
#conn{width:8px;height:8px;border-radius:50%;background:var(--dim);flex-shrink:0}
#conn.ok{background:var(--ok)}
#conn.err{background:var(--err)}
nav{display:flex;border-bottom:1px solid var(--bdr)}
nav button{flex:1;padding:11px 4px;background:none;border:none;border-bottom:3px solid transparent;color:var(--dim);font-size:.9rem;cursor:pointer}
nav button.on{color:var(--acc);border-bottom-color:var(--acc)}
.tab{display:none;padding:14px}
.tab.on{display:block}
.card{background:var(--card);border:1px solid var(--bdr);border-radius:10px;padding:14px;margin-bottom:12px}
.row{display:flex;gap:12px}
.gauge{flex:1;text-align:center;padding:6px 0}
.big{font-size:2.6rem;font-weight:600;line-height:1;letter-spacing:-.5px}
.bar-wrap{height:6px;background:var(--bdr);border-radius:3px;margin:8px 2px 4px;overflow:hidden}
.bar{height:100%;border-radius:3px;background:var(--acc);transition:.4s}
.glbl{font-size:.78rem;color:var(--dim);margin-top:4px}
.graw{font-size:.72rem;color:var(--dim);margin-top:1px}
.kv{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid var(--bdr);gap:8px}
.kv:last-child{border:none}
.k{color:var(--dim);font-size:.85rem;flex-shrink:0}
.v{font-weight:500;text-align:right}
.badge{padding:2px 9px;border-radius:20px;font-size:.75rem;font-weight:500;display:inline-block}
.b-ok{background:var(--okbg);color:var(--ok)}
.b-warn{background:var(--warnbg);color:var(--warn)}
.b-err{background:var(--errbg);color:var(--err)}
.b-dim{background:var(--bdr);color:var(--dim)}
label{display:block;font-size:.82rem;color:var(--dim);margin:12px 0 5px}
input[type=number],input[type=range]{width:100%}
input[type=number]{background:var(--card);border:1px solid var(--bdr);border-radius:7px;color:var(--txt);padding:9px 12px;font-size:1rem}
input[type=number]:focus{outline:none;border-color:var(--acc)}
input[type=range]{accent-color:var(--acc);height:28px}
.tval{text-align:center;font-size:2rem;font-weight:600;margin:6px 0}
.btn{display:block;width:100%;padding:13px;border:none;border-radius:8px;font-size:1rem;font-weight:500;cursor:pointer;margin-top:10px}
.btn-pri{background:var(--acc);color:#fff}
.btn-pri:active{filter:brightness(.85)}
.cal-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:4px}
.ci label{margin-top:0}
.cv-row{display:grid;grid-template-columns:1fr 1fr 1fr 1fr;gap:0;text-align:center}
.cv-row>div>div:first-child{font-size:.72rem;color:var(--dim);margin-bottom:2px}
.cv-row>div>div:last-child{font-weight:600}
.live-row{display:flex;gap:10px;text-align:center;margin-top:6px}
.live-row>div{flex:1;background:var(--bg);border-radius:8px;padding:10px 4px}
.live-row .ln{font-size:1.6rem;font-weight:600}
.live-row .ll{font-size:.72rem;color:var(--dim);margin-top:2px}
.msg{margin-top:10px;padding:9px 12px;border-radius:7px;font-size:.85rem;display:none}
.msg.ok{background:var(--okbg);color:var(--ok);display:block}
.msg.err{background:var(--errbg);color:var(--err);display:block}
.sec-hd{font-size:.8rem;color:var(--dim);text-transform:uppercase;letter-spacing:.05em;margin-bottom:10px}
/* ── axle visualization ── */
.viz{width:100%;height:auto;display:block}
.bag-fill{transition:y .5s ease,height .5s ease}
.valve{transition:fill .15s,opacity .15s}
.valve-off{fill:var(--bdr);opacity:.5}
.valve-inf{fill:var(--ok);opacity:1}
.valve-def{fill:var(--err);opacity:1}
.vlabel{fill:var(--dim);font-size:11px}
.vpct{fill:#fff;font-size:15px;font-weight:600}
.tgt-line{stroke:var(--warn);stroke-width:2;stroke-dasharray:6 4}
/* ── OTA progress bar ── */
.fwbar{width:100%;height:18px;background:var(--bdr);border-radius:9px;overflow:hidden;margin:14px 0 8px;display:none}
.fwbar.on{display:block}
.fwfill{height:100%;width:0;background:var(--acc);transition:width .2s;border-radius:9px}
</style></head>
<body>
<header>
  <h1>Air Suspension</h1>
  <div id="conn" title="Connection status"></div>
</header>
<nav>
  <button class="on" onclick="go('dash',this)">Dashboard</button>
  <button onclick="go('tgt',this)">Target</button>
  <button onclick="go('cal',this)">Calibration</button>
  <button onclick="go('upd',this)">Update</button>
</nav>

<!-- ── Dashboard ─────────────────────────────────────────────────── -->
<div id="dash" class="tab on">
  <div class="card" id="bicard" style="display:none">
    <div class="sec-hd">Bag init</div>
    <div class="kv"><span class="k" id="bitxt">--</span><span class="v" id="bistate"></span></div>
    <button class="btn btn-pri" id="biyes"    onclick="biAct('yes')">Yes</button>
    <button class="btn"         id="bino"     onclick="biAct('no')">No</button>
    <button class="btn"         id="bicancel" onclick="biAct('cancel')">Cancel</button>
    <button class="btn btn-pri" id="bistart"  onclick="biAct('start')">Start bag init</button>
  </div>
  <div class="card">
    <div class="sec-hd">Rear axle</div>
    <svg class="viz" viewBox="0 0 340 200" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <pattern id="tread" width="14" height="10" patternUnits="userSpaceOnUse">
          <path d="M0 0 L7 5 L0 10 M7 0 L14 5 L7 10" fill="none" stroke="#444" stroke-width="1.2"/>
        </pattern>
      </defs>
      <!-- axle beam — spans under both tyres -->
      <rect x="60" y="150" width="220" height="9" rx="2" fill="#747474"/>

      <!-- LEFT tyre (static decoration) -->
      <rect x="45" y="80" width="34" height="100" rx="11" fill="#1a1a1a"/>
      <rect x="45" y="80" width="34" height="100" rx="11" fill="url(#tread)"/>
      <rect x="45" y="80" width="34" height="100" rx="11" fill="none" stroke="#001DBC" stroke-width="1.2"/>
      <text class="vlabel" x="62" y="195" text-anchor="middle">Left tyre</text>

      <!-- RIGHT tyre (static decoration) -->
      <rect x="261" y="80" width="34" height="100" rx="11" fill="#1a1a1a"/>
      <rect x="261" y="80" width="34" height="100" rx="11" fill="url(#tread)"/>
      <rect x="261" y="80" width="34" height="100" rx="11" fill="none" stroke="#001DBC" stroke-width="1.2"/>
      <text class="vlabel" x="278" y="195" text-anchor="middle">Right tyre</text>

      <!-- LEFT bag (fills from bottom; interior y=70..150, 80px tall) -->
      <clipPath id="clipL"><rect x="100" y="70" width="44" height="80" rx="6"/></clipPath>
      <rect id="bagL" class="bag-fill" x="100" y="110" width="44" height="40" fill="#1ba1e2" clip-path="url(#clipL)"/>
      <rect x="100" y="70" width="44" height="80" rx="6" fill="none" stroke="#006EAF" stroke-width="1.5"/>
      <line id="tgtL" class="tgt-line" x1="94" y1="94" x2="150" y2="94"/>
      <text id="pctL" class="vpct" x="122" y="115" text-anchor="middle">--%</text>
      <!-- left valves -->
      <circle id="vLI" class="valve valve-off" cx="122" cy="52" r="12"/>
      <path d="M122 46 L128 57 L116 57 Z" fill="#fff"/>
      <text class="vlabel" x="122" y="30" text-anchor="middle">inflate</text>
      <circle id="vLD" class="valve valve-off" cx="122" cy="170" r="12"/>
      <path d="M122 176 L128 165 L116 165 Z" fill="#fff"/>

      <!-- RIGHT bag -->
      <clipPath id="clipR"><rect x="196" y="70" width="44" height="80" rx="6"/></clipPath>
      <rect id="bagR" class="bag-fill" x="196" y="110" width="44" height="40" fill="#1ba1e2" clip-path="url(#clipR)"/>
      <rect x="196" y="70" width="44" height="80" rx="6" fill="none" stroke="#006EAF" stroke-width="1.5"/>
      <line id="tgtR" class="tgt-line" x1="190" y1="94" x2="246" y2="94"/>
      <text id="pctR" class="vpct" x="218" y="115" text-anchor="middle">--%</text>
      <!-- right valves -->
      <circle id="vRI" class="valve valve-off" cx="218" cy="52" r="12"/>
      <path d="M218 46 L224 57 L212 57 Z" fill="#fff"/>
      <text class="vlabel" x="218" y="30" text-anchor="middle">inflate</text>
      <circle id="vRD" class="valve valve-off" cx="218" cy="170" r="12"/>
      <path d="M218 176 L224 165 L212 165 Z" fill="#fff"/>

      <!-- target label on the beam -->
      <text id="tgtTxt" class="vlabel" x="170" y="145" text-anchor="middle" style="fill:var(--warn);font-weight:600">TGT --%</text>
    </svg>
  </div>
  <div class="card">
    <div class="row">
      <div class="gauge">
        <div class="big" id="pl">--</div>
        <div class="bar-wrap"><div class="bar" id="bl" style="width:0"></div></div>
        <div class="glbl">Left</div>
        <div class="graw" id="rl">raw --</div>
      </div>
      <div class="gauge">
        <div class="big" id="pr">--</div>
        <div class="bar-wrap"><div class="bar" id="br" style="width:0"></div></div>
        <div class="glbl">Right</div>
        <div class="graw" id="rr">raw --</div>
      </div>
    </div>
  </div>
  <div class="card">
    <div class="kv"><span class="k">Target</span><span class="v" id="dtgt">--%</span></div>
    <div class="kv"><span class="k">Regulation</span><span class="v" id="dreg"></span></div>
    <div class="kv"><span class="k">Anti-hunt lockout</span><span class="v" id="dlck"></span></div>
    <div class="kv"><span class="k">Calibration</span><span class="v" id="dcal"></span></div>
  </div>
</div>

<!-- ── Target ────────────────────────────────────────────────────── -->
<div id="tgt" class="tab">
  <div class="card">
    <div class="sec-hd">Current target</div>
    <div class="tval" id="tcur">--%</div>
    <label>New target (%)</label>
    <input type="range" id="trange" min="0" max="100" step="10" value="50"
           oninput="sync('trange','tnum')">
    <input type="number" id="tnum" min="0" max="100" step="10" value="50" style="margin-top:8px"
           oninput="sync('tnum','trange')">
    <button class="btn btn-pri" onclick="setTarget()">Set target</button>
    <button class="btn" onclick="setOff()" style="margin-top:8px">OFF (standby — stop regulating)</button>
    <div id="tmsg" class="msg"></div>
  </div>
</div>

<!-- ── Calibration ───────────────────────────────────────────────── -->
<div id="cal" class="tab">
  <div class="card">
    <div class="sec-hd">Stored values</div>
    <div class="cv-row">
      <div><div>L min</div><div id="cvlmin">--</div></div>
      <div><div>L max</div><div id="cvlmax">--</div></div>
      <div><div>R min</div><div id="cvrmin">--</div></div>
      <div><div>R max</div><div id="cvrmax">--</div></div>
    </div>
  </div>
  <div class="card">
    <div class="sec-hd">Live ADC</div>
    <div class="live-row">
      <div><div class="ln" id="cll">--</div><div class="ll">Left raw</div></div>
      <div><div class="ln" id="clr">--</div><div class="ll">Right raw</div></div>
    </div>
  </div>
  <div class="card">
    <div class="sec-hd">Manual entry (12-bit, 0 – 4095)</div>
    <div class="cal-grid">
      <div class="ci"><label>Left min</label><input type="number" id="ilmin" min="0" max="4095" placeholder="e.g. 684"></div>
      <div class="ci"><label>Left max</label><input type="number" id="ilmax" min="0" max="4095" placeholder="e.g. 3810"></div>
      <div class="ci"><label>Right min</label><input type="number" id="irmin" min="0" max="4095" placeholder="e.g. 699"></div>
      <div class="ci"><label>Right max</label><input type="number" id="irmax" min="0" max="4095" placeholder="e.g. 3795"></div>
    </div>
    <button class="btn btn-pri" onclick="saveCal()">Save calibration</button>
    <div id="calmsg" class="msg"></div>
  </div>
</div>

<!-- ── Firmware Update ───────────────────────────────────────────── -->
<div id="upd" class="tab">
  <div class="card">
    <div class="sec-hd">Firmware update (OTA)</div>
    <p style="font-size:.85rem;color:var(--dim);margin:0 0 12px">
      Select a compiled <b>.bin</b> built for this board, then upload.
      The controller reboots automatically when done. Do not close this
      page or power off during the update.
    </p>
    <div class="kv"><span class="k">Running</span><span class="v" id="fwver">–</span></div>
    <div class="kv"><span class="k">Image / OTA slot</span><span class="v" id="fwslot">–</span></div>
    <input type="file" id="fwfile" accept=".bin" style="margin:10px 0">
    <button class="btn btn-pri" id="fwbtn" onclick="uploadFW()">Upload &amp; flash</button>
    <div class="fwbar" id="fwbar"><div class="fwfill" id="fwfill"></div></div>
    <div id="updmsg" class="msg"></div>
  </div>
</div>

<script>
// ── tab switching ──────────────────────────────────────────────────
function go(id,btn){
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('on'));
  document.querySelectorAll('nav button').forEach(b=>b.classList.remove('on'));
  document.getElementById(id).classList.add('on');
  btn.classList.add('on');
  if(id==='cal') loadCal();
}

// ── slider ↔ number sync ──────────────────────────────────────────
function sync(src,dst){
  const v=document.getElementById(src).value;
  document.getElementById(dst).value=v;
}

// ── badge helpers ─────────────────────────────────────────────────
const B=(c,t)=>`<span class="badge ${c}">${t}</span>`;

// Target 45 is the OFF/standby sentinel (regulation disabled).
const fmtTgt=(v)=> v==45 ? 'OFF' : v+'%';

// ── poll dashboard ────────────────────────────────────────────────
const conn=document.getElementById('conn');
async function poll(){
  try{
    const d=await(await fetch('/api/status')).json();
    conn.className='ok';
    document.getElementById('pl').textContent=d.lp+'%';
    document.getElementById('pr').textContent=d.rp+'%';
    document.getElementById('bl').style.width=d.lp+'%';
    document.getElementById('br').style.width=d.rp+'%';
    document.getElementById('rl').textContent='raw '+d.la;
    document.getElementById('rr').textContent='raw '+d.ra;
    document.getElementById('dtgt').textContent=fmtTgt(d.tgt);
    document.getElementById('tcur').textContent=fmtTgt(d.tgt);
    // trange/tnum intentionally NOT updated — they hold user intent, not server state
    document.getElementById('dreg').innerHTML=d.reg?B('b-warn','Regulating'):B('b-ok','Idle');
    updateBagInit(d);
    document.getElementById('dlck').innerHTML=d.lck?B('b-warn','Locked out'):B('b-dim','None');
    document.getElementById('dcal').innerHTML=d.cal?B('b-ok','Valid'):B('b-err','Not calibrated');
    // live ADC on calibration tab
    document.getElementById('cll').textContent=d.la;
    document.getElementById('clr').textContent=d.ra;
    // ── axle visualization ──
    updateAxle(d);
    // ── adaptive poll rate: fast when anything is happening ──
    pollActive = d.reg || (d.bi>0 && d.bi!=6) || d.vli || d.vld || d.vri || d.vrd;
  }catch(e){conn.className='err';}
}

// ── bag init (out-of-range) ───────────────────────────────────────
// bi: 0 idle, 1 prompt, 2 deflate probe, 3 deflate extra, 4 inflate,
//     5 retry prompt, 6 FAULT.  bir: reason text, bis: seconds left, bst: step
const BI_TXT=['','Out of range – run init?','Deflate probe','Deflating further',
              'Inflating','Retry 3×10 s?','FAULT – regulation blocked'];
function updateBagInit(d){
  const c=document.getElementById('bicard');
  if(!d.bi){c.style.display='none';return;}
  c.style.display='';
  let t=BI_TXT[d.bi]||'';
  if(d.bi==4) t+=' '+(d.bst+1)+'/3';
  if(d.bi==3) t+=' +'+d.bst;
  document.getElementById('bitxt').textContent=t;
  const st=(d.bi==6)?B('b-err',d.bir):(d.bis>0?B('b-warn',d.bis+' s'):B('b-warn',d.bir));
  document.getElementById('bistate').innerHTML=st;
  const ask=(d.bi==1||d.bi==5), run=(d.bi>=2&&d.bi<=4);
  document.getElementById('biyes').style.display=ask?'':'none';
  document.getElementById('bino').style.display=ask?'':'none';
  document.getElementById('bicancel').style.display=run?'':'none';
  document.getElementById('bistart').style.display=(d.bi==6)?'':'none';
}
async function biAct(a){
  try{
    await fetch('/api/baginit',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action='+a});
  }catch(e){}
  poll();
}

// ── axle visualization ────────────────────────────────────────────
// Bag interior spans y=70 (top, 100%) to y=150 (bottom, 0%) — 80px tall.
const BAG_TOP=70, BAG_H=80;
function bagFill(bagId,pctId,pct){
  const h=Math.round(BAG_H*pct/100);
  const y=BAG_TOP+(BAG_H-h);
  const r=document.getElementById(bagId);
  r.setAttribute('y',y);
  r.setAttribute('height',h);
  document.getElementById(pctId).textContent=pct+'%';
}
function tgtLine(lineId,pct){
  const y=BAG_TOP+(BAG_H-BAG_H*pct/100);
  const el=document.getElementById(lineId);
  el.setAttribute('y1',y);el.setAttribute('y2',y);
}
function valve(id,on,kind){
  const el=document.getElementById(id);
  el.setAttribute('class','valve '+(on?(kind==='inf'?'valve-inf':'valve-def'):'valve-off'));
}
function updateAxle(d){
  bagFill('bagL','pctL',d.lp);
  bagFill('bagR','pctR',d.rp);
  const off=(d.tgt==45);
  document.getElementById('tgtL').style.display=off?'none':'';
  document.getElementById('tgtR').style.display=off?'none':'';
  if(!off){ tgtLine('tgtL',d.tgt); tgtLine('tgtR',d.tgt); }
  document.getElementById('tgtTxt').textContent=off?'OFF':'TGT '+d.tgt+'%';
  valve('vLI',d.vli,'inf');
  valve('vLD',d.vld,'def');
  valve('vRI',d.vri,'inf');
  valve('vRD',d.vrd,'def');
}

// ── load calibration values ───────────────────────────────────────
async function loadCal(){
  try{
    const d=await(await fetch('/api/calibration')).json();
    document.getElementById('cvlmin').textContent=d.lmin??'--';
    document.getElementById('cvlmax').textContent=d.lmax??'--';
    document.getElementById('cvrmin').textContent=d.rmin??'--';
    document.getElementById('cvrmax').textContent=d.rmax??'--';
    if(d.valid){
      ['ilmin','ilmax','irmin','irmax'].forEach((id,i)=>{
        document.getElementById(id).placeholder=[d.lmin,d.lmax,d.rmin,d.rmax][i];
      });
    }
  }catch(e){}
}

// ── set OFF (standby) ──────────────────────────────────────────────
async function setOff(){
  try{
    const r=await fetch('/api/target',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'target=45'
    });
    if(r.ok){
      document.getElementById('tcur').textContent='OFF';
      document.getElementById('dtgt').textContent='OFF';
    }
    msg('tmsg',r.ok?'ok':'err',r.ok?'Regulation OFF (standby)':'Server error');
  }catch(e){msg('tmsg','err','Request failed');}
}

// ── set target ────────────────────────────────────────────────────
async function setTarget(){
  let v=parseInt(document.getElementById('tnum').value);
  if(isNaN(v)||v<0||v>100){msg('tmsg','err','Enter a value between 0 and 100');return;}
  v=Math.round(v/10)*10;   // snap to nearest 10% step
  // reflect the snapped value back into both inputs
  document.getElementById('tnum').value=v;
  document.getElementById('trange').value=v;
  try{
    const r=await fetch('/api/target',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'target='+v
    });
    if(r.ok){
      document.getElementById('tcur').textContent=v+'%';
      document.getElementById('dtgt').textContent=v+'%';
    }
    msg('tmsg',r.ok?'ok':'err',r.ok?'Target set to '+v+'%':'Server error');
  }catch(e){msg('tmsg','err','Request failed');}
}

// ── save calibration ──────────────────────────────────────────────
async function saveCal(){
  const get=id=>document.getElementById(id).value;
  const lmin=+get('ilmin'),lmax=+get('ilmax'),rmin=+get('irmin'),rmax=+get('irmax');
  if([lmin,lmax,rmin,rmax].some(v=>v===''||isNaN(v))){
    msg('calmsg','err','All four values are required');return;
  }
  if(lmax-lmin<800||rmax-rmin<800){msg('calmsg','err','Span too small — minimum 800 counts');return;}
  if(lmin<200||rmin<200){msg('calmsg','err','Min value must be ≥ 200');return;}
  if(lmax>4050||rmax>4050){msg('calmsg','err','Max value must be ≤ 4050');return;}
  try{
    const r=await fetch('/api/calibration',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:`lmin=${lmin}&lmax=${lmax}&rmin=${rmin}&rmax=${rmax}`
    });
    if(r.ok){msg('calmsg','ok','Calibration saved');loadCal();}
    else     msg('calmsg','err','Save failed — check serial output');
  }catch(e){msg('calmsg','err','Request failed');}
}

// ── toast helper ──────────────────────────────────────────────────
function msg(id,cls,txt){
  const el=document.getElementById(id);
  el.textContent=txt;el.className='msg '+cls;
  setTimeout(()=>el.className='msg',4000);
}
// like msg() but does not auto-clear (for persistent status e.g. rebooting)
function msgHold(id,cls,txt){
  const el=document.getElementById(id);
  el.textContent=txt;el.className='msg '+cls;
}

// ── firmware OTA upload ───────────────────────────────────────────
// Firmware info: running version, current image size, free OTA slot.
let fwSlot=0;
async function loadFw(){
  try{
    const d=await (await fetch('/api/fwinfo')).json();
    fwSlot=d.slot;
    document.getElementById('fwver').textContent=d.ver+' ('+d.part+')';
    document.getElementById('fwslot').textContent=
      Math.round(d.used/1024)+' KB / '+Math.round(d.slot/1024)+' KB ('+Math.round(d.used*100/d.slot)+' %)';
  }catch(e){}
}

function uploadFW(){
  const f=document.getElementById('fwfile').files[0];
  if(!f){msg('updmsg','err','Choose a .bin file first');return;}
  if(!f.name.endsWith('.bin')){msg('updmsg','err','File must be a .bin');return;}
  // Refuse before sending anything if the image cannot fit the OTA slot.
  if(fwSlot && f.size>fwSlot){
    msg('updmsg','err','Image is '+Math.round(f.size/1024)+' KB but the OTA slot holds only '+
        Math.round(fwSlot/1024)+' KB — use a partition scheme with larger app slots.');
    return;
  }

  const bar=document.getElementById('fwbar');
  const fill=document.getElementById('fwfill');
  const btn=document.getElementById('fwbtn');
  bar.classList.add('on');
  fill.style.width='0%';
  btn.disabled=true;
  msgHold('updmsg','','Uploading…');

  const fd=new FormData();
  fd.append('update',f,f.name);

  const xhr=new XMLHttpRequest();
  xhr.open('POST','/update',true);

  xhr.upload.onprogress=e=>{
    if(e.lengthComputable){
      const pct=Math.round(e.loaded/e.total*100);
      fill.style.width=pct+'%';
      msgHold('updmsg','','Uploading… '+pct+'%');
    }
  };
  xhr.onload=()=>{
    btn.disabled=false;
    if(xhr.status===200){
      fill.style.width='100%';
      msgHold('updmsg','ok','Update OK — rebooting. Reconnect WiFi in ~10 s, then turn WiFi back on from the menu.');
    }else{
      msg('updmsg','err','Update failed: '+(xhr.responseText||('HTTP '+xhr.status)));
    }
  };
  xhr.onerror=()=>{
    btn.disabled=false;
    // The socket drops as the device reboots — expected at the very end of a
    // good flash, so only treat it as an error if we hadn't reached 100%.
    if(fill.style.width!=='100%')
      msg('updmsg','err','Connection lost during upload');
  };
  xhr.send(fd);
}

// Adaptive poll loop: 400 ms while something is active (regulating,
// bag init running/prompting, or any valve firing), 1000 ms when idle.  pollActive is
// updated at the end of each successful poll().  Using setTimeout (not
// setInterval) so the interval can change between cycles and so a slow
// response never lets requests pile up.
var pollActive=false;
async function pollLoop(){
  await poll();
  setTimeout(pollLoop, pollActive ? 400 : 1000);
}
pollLoop();
loadFw();
</script>
</body></html>
)rawhtml";

// ============================================================================
// JSON HELPERS
// ============================================================================

static void sendJSON(const String &json) {
  webServer.sendHeader("Access-Control-Allow-Origin", "*");
  webServer.send(200, "application/json", json);
}

static void sendOK()   { webServer.send(200, "text/plain", "ok"); }
static void sendErr(const char *msg) { webServer.send(400, "text/plain", msg); }

// ============================================================================
// ROUTE HANDLERS
// ============================================================================

static void handleRoot() {
  webServer.sendHeader("Content-Encoding", "identity");
  webServer.send_P(200, "text/html", HTML_INDEX);
}

static void handleStatus() {
  // Browser polls this every second — count it as user activity to prevent
  // idle sleep while the web interface is open.
  powerSave.lastActivityTime = millis();

  // Read raw ADC without going through the rolling average so we get live values
  int rawL = analogRead(SENSOR_LEFT_PIN);
  int rawR = calibration.rightInverted
             ? (ADC_MAX_RAW - analogRead(SENSOR_RIGHT_PIN))
             :  analogRead(SENSOR_RIGHT_PIN);

  // Live valve states straight off the GPIO pins.  This reflects what is
  // ACTUALLY energized right now — auto-regulation, manual mode, and serial
  // control all show up here, because they all drive these same pins.
  bool vLI = digitalRead(INFLATE_LEFT_PIN)  == HIGH;  // left inflate
  bool vLD = digitalRead(DEFLATE_LEFT_PIN)  == HIGH;  // left deflate
  bool vRI = digitalRead(INFLATE_RIGHT_PIN) == HIGH;  // right inflate
  bool vRD = digitalRead(DEFLATE_RIGHT_PIN) == HIGH;  // right deflate

  char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"lp\":%d,\"rp\":%d,\"la\":%d,\"ra\":%d,"
    "\"tgt\":%d,\"reg\":%s,\"lck\":%s,\"cal\":%s,"
    "\"bi\":%d,\"bst\":%d,\"bis\":%u,\"bir\":\"%s\","
    "\"vli\":%s,\"vld\":%s,\"vri\":%s,\"vrd\":%s}",
    sensors.leftPercent,
    sensors.rightPercent,
    rawL,
    rawR,
    (int)targetPercent,
    regulation.isRegulating   ? "true" : "false",
    antiHunt.inLockout        ? "true" : "false",
    calibration.valid         ? "true" : "false",
    (int)bagInit.phase,
    (int)bagInit.step,
    biSecondsLeft(millis()),
    biReasonText(),
    vLI ? "true" : "false",
    vLD ? "true" : "false",
    vRI ? "true" : "false",
    vRD ? "true" : "false"
  );
  sendJSON(buf);
}

static void handleGetCalibration() {
  char buf[160];
  snprintf(buf, sizeof(buf),
    "{\"lmin\":%d,\"lmax\":%d,\"rmin\":%d,\"rmax\":%d,\"valid\":%s}",
    calibration.leftMinRaw,
    calibration.leftMaxRaw,
    calibration.rightMinRaw,
    calibration.rightMaxRaw,
    calibration.valid ? "true" : "false"
  );
  sendJSON(buf);
}

// POST /api/baginit  action = yes | no | cancel | start
static void handleWebBagInit() {
  if (!webServer.hasArg("action")) { sendErr("missing action"); return; }
  String a = webServer.arg("action");
  unsigned long now = millis();
  if      (a == "yes")    bagInitAnswer(true,  now);
  else if (a == "no")     bagInitAnswer(false, now);
  else if (a == "cancel") bagInitCancel(now);
  else if (a == "start")  { if (!biIsRunning(bagInit)) bagInitStart(now); }
  else { sendErr("unknown action"); return; }
  Serial.print(F("[Web] Bag init: ")); Serial.println(a);
  sendOK();
}

static void handleSetTarget() {
  if (!webServer.hasArg("target")) { sendErr("missing target"); return; }
  int v = webServer.arg("target").toInt();
  if (v < 0 || v > 100) { sendErr("target out of range 0-100"); return; }
  // Web OFF button: remember the current setpoint so a later long-UP on the
  // buttons resumes to it (same as the long-DOWN OFF gesture does).
  if (isSetpointOff((byte)v)) rememberSetpointForResume();
  targetPercent = (byte)v;
  saveSetpoint();
  resetRegulationState();
  notifyUserChange(millis());
  Serial.print(F("[Web] Target → ")); Serial.print(v); Serial.println('%');
  sendOK();
}

static void handleSetCalibration() {
  if (!webServer.hasArg("lmin") || !webServer.hasArg("lmax") ||
      !webServer.hasArg("rmin") || !webServer.hasArg("rmax")) {
    sendErr("missing lmin/lmax/rmin/rmax"); return;
  }
  int lmin = webServer.arg("lmin").toInt();
  int lmax = webServer.arg("lmax").toInt();
  int rmin = webServer.arg("rmin").toInt();
  int rmax = webServer.arg("rmax").toInt();

  // Safety range checks (same limits as Calibration.h)
  if (lmin < 200 || rmin < 200) { sendErr("min value below 200"); return; }
  if (lmax > 4050 || rmax > 4050) { sendErr("max value above 4050"); return; }
  if (lmax - lmin < 800 || rmax - rmin < 800) { sendErr("span < 800 counts"); return; }

  calibration.leftMinRaw  = lmin;
  calibration.leftMaxRaw  = lmax;
  calibration.rightMinRaw = rmin;
  calibration.rightMaxRaw = rmax;
  calibration.valid       = true;
  saveCalibration();

  Serial.println(F("[Web] Calibration saved"));
  Serial.print(F("  L: ")); Serial.print(lmin); Serial.print(F(" – ")); Serial.println(lmax);
  Serial.print(F("  R: ")); Serial.print(rmin); Serial.print(F(" – ")); Serial.println(rmax);
  sendOK();
}

static void handleNotFound() {
  webServer.send(404, "text/plain", "Not found");
}

// ============================================================================
// OTA FIRMWARE UPDATE
// ============================================================================
// Two-part handler (WebServer upload pattern):
//   handleUpdateUpload() — called repeatedly with chunks of the .bin as it
//                          streams in; writes them to the inactive OTA
//                          partition via the Update library.
//   handleUpdateDone()   — called once after upload completes; sends the
//                          result and (on success) reboots into the new image.
//
// Safety:
//   • Valves are forced OFF before flashing begins (a suspension controller
//     must never be left with a valve energized across a reboot).
//   • The watchdog is disabled for the duration — a multi-second flash write
//     would otherwise trip the 4 s WDT and reset mid-update.
//   • Dual OTA partitions (default 4MB-with-spiffs scheme) mean the running
//     image stays bootable if the upload is interrupted; the swap only
//     happens on a fully verified image.

// ── OTA state ────────────────────────────────────────────────────────────
// The whole upload runs synchronously inside one webServer.handleClient()
// call, so loop() (and with it the watchdog feed) is blocked for its
// duration — hence the watchdog is disabled while uploading.  EVERY exit path
// (success → reboot, failure, client abort) must re-enable it; before this
// fix an aborted upload left the controller running without a watchdog.
static bool   otaActive        = false;
static bool   otaHeaderChecked = false;
static String otaError;               // first error wins; empty = OK so far

// ESP32 app image: byte 0 = 0xE9 magic (checked by Update itself),
// bytes 12..13 = chip ID.  Reject images built for another chip early, with
// a readable message, instead of failing at activation.
#ifdef CONFIG_IDF_FIRMWARE_CHIP_ID
static const uint16_t OTA_EXPECTED_CHIP_ID = CONFIG_IDF_FIRMWARE_CHIP_ID;
#else
static const uint16_t OTA_EXPECTED_CHIP_ID = 0x0009;   // ESP32-S3
#endif

static void otaFail(const String& why) {
  if (otaError.length() == 0) otaError = why;          // keep the first cause
  Serial.print(F("[OTA] Error: ")); Serial.println(why);
}

static void otaFinishFailed() {
  if (Update.isRunning()) Update.abort();
  otaActive = false;
  enableWatchdog();                                      // back to protected
  Serial.println(F("[OTA] Watchdog re-enabled — continuing with current firmware"));
}

// GET /api/fwinfo — running version/partition, image size, free OTA slot
static void handleFwInfo() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  char buf[224];
  snprintf(buf, sizeof(buf),
    "{\"ver\":\"%s\",\"descr\":\"%s\",\"part\":\"%s\",\"used\":%u,\"slot\":%u}",
    VERSION, DESCR, run ? run->label : "?",
    (unsigned)ESP.getSketchSize(), (unsigned)ESP.getFreeSketchSpace());
  sendJSON(String(buf));
}

static void handleUpdateUpload() {
  HTTPUpload& up = webServer.upload();

  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] Start: %s\n", up.filename.c_str());
    otaActive = true; otaHeaderChecked = false; otaError = "";
    // SAFETY: valves off and no stale activity when the loop resumes after
    // a failed upload: end any regulation run, cancel a running bag init.
    allValvesOff();
    abortRegulationRun();
    if (biIsRunning(bagInit)) bagInitCancel(millis());
    disableWatchdog();
    // Size is discovered from the stream; writes beyond the slot fail.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) otaFail(Update.errorString());

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaError.length()) return;              // already failed: drain only
    if (!otaHeaderChecked && up.currentSize >= 16) {
      otaHeaderChecked = true;
      uint16_t chip = up.buf[12] | (up.buf[13] << 8);
      if (up.buf[0] == 0xE9 && chip != OTA_EXPECTED_CHIP_ID) {
        otaFail(String("Wrong chip: image is for chip ID ") + String(chip) +
                ", this board is " + String(OTA_EXPECTED_CHIP_ID) + " (ESP32-S3)");
        Update.abort();
        return;
      }
    }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      otaFail(Update.errorString());            // e.g. no space / bad magic
      Update.abort();
    }

  } else if (up.status == UPLOAD_FILE_END) {
    if (otaError.length() == 0) {
      if (Update.end(true)) {                   // true = set as boot image
        Serial.printf("[OTA] Success: %u bytes\n", up.totalSize);
      } else {
        otaFail(Update.errorString());
      }
    }

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    otaFail("Upload aborted (connection lost)");
    otaFinishFailed();                          // the request handler may not run
  }
}

static void handleUpdateDone() {
  bool ok = otaActive && otaError.length() == 0 && !Update.hasError();
  webServer.sendHeader("Connection", "close");
  if (ok) {
    webServer.send(200, "text/plain", "OK");
    Serial.println(F("[OTA] Rebooting into new firmware..."));
    delay(200);          // let the response flush before reset
    allValvesOff();      // belt-and-braces before reboot
    reboot();            // esp_restart()
  } else {
    String why = otaError.length() ? otaError : String("Update failed");
    webServer.send(400, "text/plain", why);
    if (otaActive) otaFinishFailed();           // re-enable WDT, keep old image
  }
}

// ============================================================================
// PUBLIC API
// ============================================================================
//
// WiFi/web is OFF by default and started on demand from the button menu.
// initWebInterface()  — registers routes ONCE (call from setup()).  Does NOT
//                       start the radio.
// startWebInterface() — brings up the SoftAP + server.  Idempotent.
// stopWebInterface()  — tears the AP down and turns the radio off.
// handleWebInterface()— call every loop; no-op when WiFi is off.
//
extern bool wifiEnabled;   // defined in the .ino

static bool webRoutesRegistered = false;

static void registerWebRoutes() {
  if (webRoutesRegistered) return;
  webServer.on("/",               HTTP_GET,  handleRoot);
  webServer.on("/api/status",     HTTP_GET,  handleStatus);
  webServer.on("/api/calibration",HTTP_GET,  handleGetCalibration);
  webServer.on("/api/target",     HTTP_POST, handleSetTarget);
  webServer.on("/api/baginit",    HTTP_POST, handleWebBagInit);
  webServer.on("/api/calibration",HTTP_POST, handleSetCalibration);
  // OTA: handleUpdateDone runs after the upload completes; handleUpdateUpload
  // receives the streamed .bin chunks.
  webServer.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  webServer.on("/api/fwinfo",     HTTP_GET,  handleFwInfo);
  webServer.onNotFound(handleNotFound);
  webRoutesRegistered = true;
}

// Register routes once.  Does not start the radio — WiFi stays off until
// startWebInterface() is called from the button menu.
void initWebInterface() {
  registerWebRoutes();
  Serial.println(F("[Web] Routes registered (WiFi off until enabled)"));
}

// Bring up the AP + web server.  Safe to call repeatedly.
void startWebInterface() {
  if (wifiEnabled) return;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
  WiFi.setTxPower((wifi_power_t)WIFI_TX_POWER_QDBM);

  registerWebRoutes();
  webServer.begin();

  wifiEnabled = true;
  Serial.print(F("[Web] AP ON: "));
  Serial.print(WIFI_AP_SSID);
  Serial.print(F("  IP: "));
  Serial.println(WiFi.softAPIP());
}

// Tear the AP down and power the radio off.
void stopWebInterface() {
  if (!wifiEnabled) return;

  webServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  wifiEnabled = false;
  Serial.println(F("[Web] AP OFF"));
}

// Call every loop iteration — non-blocking.  No-op when WiFi is off.
void handleWebInterface() {
  if (!wifiEnabled) return;
  webServer.handleClient();
}

#endif // WEB_INTERFACE_H