#pragma once
// The single-page UI. Lives in a header because the Arduino .ino
// preprocessor misparses 'async function' inside the raw string
// as a C++ declaration.
//
// UI RULES (owner-specified, 2026-08-25):
//  - Every highlighted button means STATE (bus truth or commanded-within-
//    hold-off). Never a decorative class in markup.
//  - GREEN = active/on only. Off / closed / inactive-selected = gray .sel.
//  - Binary controls are ONE toggle button that shows current state, never
//    an Off button next to an On button.
//  - State text labels next to a control are redundant; the control shows it.
#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Van Companion</title>
<style>
:root{color-scheme:dark;--bg:#101418;--card:#1b222b;--on:#2e7d4f;--tx:#e8edf2;--mut:#8b98a5;--ac:#3b82c4}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:16px/1.4 system-ui,sans-serif;padding:10px;max-width:520px;margin:auto}
h1{font-size:20px;margin:6px 0 12px}h2{font-size:14px;color:var(--mut);text-transform:uppercase;letter-spacing:.08em;margin:18px 0 6px}
.card{background:var(--card);border-radius:12px;padding:10px;margin-bottom:8px}
.row{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:6px 2px}
.name{flex:1}.sub{font-size:12px;color:var(--mut)}
button{background:#2a3440;color:var(--tx);border:0;border-radius:10px;padding:10px 16px;font-size:15px;min-width:56px;cursor:pointer}
button.on{background:var(--on)}
button.sel{background:#46535f}
button:active{opacity:.8}button:disabled{opacity:.45}
.big{font-size:26px;font-weight:600}
.bl{padding:2px 2px;color:var(--mut)}
#drawline,#lifeline,#invline{white-space:pre}
.mut{color:var(--mut)}input[type=range]{width:100%}
.bar{display:inline-block;width:84px;height:12px;background:#0d1117;border-radius:6px;overflow:hidden;vertical-align:middle;margin-left:8px}
.bar>div{height:100%;background:var(--ac)}
.bar>div.g{background:#a07840}
.bar>div.gr{background:var(--on)}
.sw2{position:relative;display:inline-flex;width:132px;height:40px;background:#2a3440;border-radius:10px;cursor:pointer;user-select:none}
.sw2 .lbl{flex:1;display:flex;align-items:center;justify-content:center;font-size:14px;color:var(--mut);z-index:1}
.sw2 .knob{position:absolute;top:3px;left:3px;width:63px;height:34px;border-radius:8px;background:#5a6773;
  display:flex;align-items:center;justify-content:center;font-size:14px;color:var(--tx);transition:left .18s ease;z-index:2}
.sw2.in .knob{left:66px}
.dimtbl{width:100%;border-collapse:collapse}
.dimtbl td{padding:5px 3px;vertical-align:middle}
.dimtbl td.nm{white-space:nowrap}
.dimtbl td.bt{width:74px}
.dimtbl button{width:100%;min-width:0;padding:9px 6px;font-size:14px}
.ro{font-size:15px;color:var(--mut);display:inline-block;line-height:1.2}
.dimtbl input[type=range]{width:100%;margin:0;vertical-align:middle}
.dimtbl tr.mst td{border-top:1px solid #2a3440;padding-top:9px}
.row.sub2{padding-left:16px;font-size:14px}
.row.sub2 button{padding:8px 13px;font-size:14px}
#foot{font-size:12px;color:var(--mut);margin-top:16px;white-space:pre-wrap}
</style></head><body>
<div class="row" style="padding:0 2px"><h1 style="margin:0">Van Companion</h1><span class="sub" id="build"></span></div>

<h2>Battery</h2><div class="card">
<div class="row"><span class="name">SoC:</span>
<span><b id="socpct">--</b></span><span class="bar"><div id="socbar" class="gr"></div></span></div>
<div class="row"><span>Power flow:</span>
<span><span id="drawline">--</span><span id="lifeline"></span></span></div>
<div class="sub" id="batt"></div></div>

<h2>Lights &amp; switches</h2><div class="card">
<table class="dimtbl"><tbody id="switches"></tbody></table></div>

<h2 style="display:flex;justify-content:space-between;align-items:baseline">Climate
<span id="tempin" style="text-transform:none;letter-spacing:0;color:var(--tx)">--</span></h2>
<div class="card" id="climcard">
<div class="row" id="acgate" style="display:none"><span class="sub">needs the inverter or shore power &mdash; the overhead unit runs on AC</span></div>
<div class="row"><span class="name">Roof A/C</span>
<span><button id="m0">Off</button> <button id="m1">Cool</button> <button id="m2">Heat</button></span></div>
<div class="row sub2" id="comprow" style="display:none"><span class="name">Compressor</span>
<span><button id="comp">off</button></span></div>
<div class="row sub2" id="acfanrow" style="display:none"><span class="name">Fan</span>
<span><button id="cf0">Auto</button> <button id="cf1">Low</button> <button id="cf2">High</button></span></div>
<div class="row sub2"><span class="name">Cool setpoint</span>
<span><button id="cooldn">&minus;</button> <b id="coolsp">--</b> <button id="coolup">+</button></span></div>
</div>

<h2>Roof vent</h2><div class="card">
<div class="row"><span class="name">Vent <span class="sub" id="ventst"></span></span>
<span><button id="vtog">—</button></span></div>
<div class="row"><span class="name">Fan <span class="sub" id="fannote"></span></span>
<span><button id="ftog">off</button></span></div>
<div class="row"><span class="name">Fan airflow</span>
<span class="sw2" id="fdir"><span class="lbl">Out</span><span class="lbl">In</span><span class="knob" id="fknob">Out</span></span></div>
<div class="row"><span class="name">Fan speed</span><b id="sppct">--</b></div>
<div class="row"><input type="range" id="vrange" min="0" max="200" value="0"></div>
</div>

<h2>Power</h2><div class="card">
<div class="row"><span class="name">Inverter <span class="sub" id="invline"></span></span>
<span><button id="invtog">off</button></span></div>
<div class="row"><span class="name">Fresh water</span>
<span><b id="fresh">--</b><span class="bar"><div id="freshbar"></div></span></span></div>
<div class="row"><span class="name">Gray water</span>
<span><b id="gray">--</b><span class="bar"><div id="graybar" class="g"></div></span></span></div>
</div>

<h2>Rixen hydronic heat <span class="sub" style="text-transform:none;letter-spacing:0">(read-only, use panel)</span></h2>
<div class="card" id="rixcard">
<div class="row"><span class="name">Cabin <span class="sub" id="rixheat"></span></span><b id="rixcur">--</b></div>
<div class="row"><span class="name">Target</span><b id="rixtgt">--</b></div>
<div class="row sub2"><span class="name">Furnace</span><b id="rixfurn">--</b></div>
<div class="row sub2"><span class="name">Hot water</span><b id="rixhw">--</b></div>
<div class="row sub2"><span class="name">Heater fan</span><b id="rixfan">--</b></div>
</div>

<h2>Phone app integration board temp</h2><div class="card">
<div class="row"><span class="name" id="tempsum">collecting…</span>
<button id="tempdemo" style="min-width:0;padding:4px 10px;font-size:12px">demo</button></div>
<div id="tempchart" style="overflow-x:auto"></div>
</div>

<div id="foot"></div>
<script>
// --- switch rows ---------------------------------------------------------------
// Rows 0-3 are the dimmable lights (shadow injection holds a level; the panel
// always wins if someone touches a switch). Row 4 is master. Rows 5-7 are
// plain wall-switch spoofs with no dimming.
// dimIdx maps a light row to the firmware's LIGHT_DO order; swIdx maps a
// plain row to the SW[] spoof table.
const LIGHTS=["cabin lights","cargo lights","reading lights","awning lights"];
const LIGHT_CTL=[1,1,0,1];   // reading has no wall switch -> status only
const LIGHT_SW=[0,1,-1,5];   // index into the firmware's SW[] spoof table
const PLAIN=[{n:"aux",sw:3},{n:"water pump",sw:2},{n:"hot water circ.",sw:4}];
const lHold=[0,0,0,0], lOn=[0,0,0,0], lPct=[0,0,0,0];

const holdUntil=[0,0,0], curP=[0,0,0];

// Per-fixture draw, shown as "-0.25A (-13W)". Units are compressed against
// the numbers, draws are negative, and a surplus would carry no + sign.
// The underlying current is quantised to 0.125 A counts, so the watts figure
// steps in ~1.5 W increments.
function fmtDraw(a,w){
  const as=(Math.abs(a)<1 ? a.toFixed(2) : a.toFixed(1))+"A";
  const ws=(Math.abs(w)<10 ? w.toFixed(1) : w.toFixed(0))+"W";
  return as+" ("+ws+")";
}
const tb=document.getElementById("switches");
LIGHTS.forEach((n,i)=>{
  const r=document.createElement("tr");
  // "reading" has no wall switch on this van and dimming is gone, so that
  // row is a status readout only — the panel is the way to control it.
  const btn=LIGHT_CTL[i]?`<button id="L${i}">off</button>`
                        :`<span id="L${i}" class="ro">use panel</span>`;
  r.innerHTML=`<td class="nm">${n} <span class="sub" id="la${i}"></span></td>`+
              `<td class="bt"${LIGHT_CTL[i]?"":' style="text-align:center"'}>${btn}</td>`;
  tb.appendChild(r);
});
PLAIN.forEach((p,i)=>{
  const r=document.createElement("tr");
  r.innerHTML=`<td class="nm">${p.n} <span class="sub" id="a${i}"></span></td>`+
              `<td class="bt"><button id="s${i}">off</button></td>`;
  tb.appendChild(r);
});
// Read-only: the battery-compartment fans are a standing-feed channel we never
// write to. Reported, never controlled.
{
  const r=document.createElement("tr");
  r.innerHTML=`<td class="nm">battery&rarr;galley fans</td>`+
              `<td class="bt" style="text-align:center"><span id="gfan" class="ro">--</span></td>`;
  tb.appendChild(r);
}

function paintLight(i){
  if(!LIGHT_CTL[i])return;
  const b=document.getElementById("L"+i);
  b.textContent=lOn[i]?"ON":"off"; b.className=lOn[i]?"on":"";
}
function paint(i){
  const b=document.getElementById("s"+i);
  b.textContent=curP[i]?"ON":"off"; b.className=curP[i]?"on":"";
}
LIGHTS.forEach((n,i)=>{
  if(!LIGHT_CTL[i])return;
  document.getElementById("L"+i).onclick=()=>{
    if(Date.now()<lHold[i])return;
    lOn[i]^=1; paintLight(i); lHold[i]=Date.now()+1600;
    cmd("toggle&i="+LIGHT_SW[i]);
  };
});
PLAIN.forEach((p,i)=>{
  document.getElementById("s"+i).onclick=()=>toggleSw(i);
});
async function toggleSw(i){
  if(Date.now()<holdUntil[i])return;
  curP[i]^=1; paint(i);
  holdUntil[i]=Date.now()+1600;
  cmd("toggle&i="+PLAIN[i].sw);
}

// --- climate ---------------------------------------------------------------------
// Wire truth (0x19FEF903 byte1 nibble): 0=off, 1=cool, 2=heat; whole byte 0x04
// = running with compressor off (the compressor toggle here). 3-way selector
// per owner spec; compressor row appears only while Cool is selected.
const AC_CMD=["ac&mode=off","ac&mode=on","ac&mode=heat"];
let acSeg=0, compOn=false, acHold=0;
function segFromMode(m){return (m==="cool"||m==="on"||m==="on, compressor off")?1:m==="heat"?2:0;}
let acPower=true;   // AC available (inverter or shore) — gates the whole card
function paintAcGate(){
  document.getElementById("acgate").style.display=acPower?"none":"flex";
  ["m0","m1","m2","comp","cf0","cf1","cf2","cooldn","coolup"].forEach(id=>{
    const e=document.getElementById(id);
    if(e) e.disabled=!acPower;
  });
}
function paintAc(){
  for(let k=0;k<3;k++)document.getElementById("m"+k).className=k===acSeg?(acSeg===0?"sel":"on"):"";
  document.getElementById("comprow").style.display=acSeg===1?"flex":"none";
  document.getElementById("acfanrow").style.display=acSeg===0?"none":"flex";
}
function paintComp(){
  const b=document.getElementById("comp");
  b.textContent=compOn?"ON":"off"; b.className=compOn?"on":"";
}
for(let k=0;k<3;k++){
  document.getElementById("m"+k).onclick=()=>{
    if(Date.now()<acHold)return;
    acSeg=k; compOn=(k===1); paintAc(); paintComp(); acHold=Date.now()+1600;
    cmd(AC_CMD[k]);
  };
}
document.getElementById("comp").onclick=()=>{
  if(Date.now()<acHold)return;
  compOn=!compOn; paintComp(); acHold=Date.now()+1600;
  cmd(compOn?"ac&mode=on":"ac&mode=comp");
};
// A/C fan mode = byte1 bits 4-5. Only 0 (auto) is wire-verified; low/high stay
// disabled until a panel capture pins their bit values, and the row reports
// whatever the bus echo currently shows so an unknown value is visible.
// 0 = auto, 1 = low, 2 = high. Auto is byte1 high nibble 0; low/high are
// manual (nibble 1) with byte2 speed 0x64 / 0xC8.
const FAN_CMD=["acfan&m=auto","acfan&m=low","acfan&m=high"];
let acFan=0;
function paintAcFan(){
  for(let k=0;k<3;k++)
    document.getElementById("cf"+k).className=(k===acFan)?"on":"";
}
for(let k=0;k<3;k++){
  document.getElementById("cf"+k).onclick=()=>{
    if(Date.now()<acHold)return;
    acFan=k; paintAcFan(); acHold=Date.now()+1600; cmd(FAN_CMD[k]);
  };
}

// --- roof vent / fan ----------------------------------------------------------------
// Vent lid is one binary toggle (motion takes seconds: commanded state stays
// highlighted until the status frame confirms it, hold expires at 15 s).
let curVent=0, ventHold=0;   // 0 closed, 1 open
let curFan=0, fanHold=0;     // 0 = off, 1 = running
let fanLockUntil=0;          // airflow-change lockout (see setFanLock)
// Lid is 3-state: closed / moving / open. The firmware reports which, from
// the status flags (bit 3 = in motion), so the UI no longer guesses with a
// blind 15 s timer -- it shows the transit and follows the real position.
let lidMoving=false;
function paintVent(){
  const b=document.getElementById("vtog");
  b.textContent=lidMoving?"···":(curVent?"Open":"Closed");
  b.className=lidMoving?"sel":(curVent?"on":"sel");
  b.disabled=lidMoving;                     // no commands mid-transit
}
document.getElementById("vtog").onclick=()=>{
  if(lidMoving)return;
  curVent^=1;
  if(!curVent){curFan=0;fanHold=Date.now()+3000;}   // closed lid => fan off
  lidMoving=true;                           // optimistic: motion starts now
  paintVent(); paintFan();
  ventHold=Date.now()+3000;                 // brief, just until the report moves
  cmd("vent&open="+curVent);
};
// FAN MODEL (rewritten 2026-08-26 after the whole section misbehaved).
//
// The rule that fixes everything here: the fan's RUN STATE is commanded, never
// inferred. docs/climate-control.md records that vent status byte 2 "oscillates
// between the setpoint and 0 on a rough 5-10 s cycle" while the command byte
// holds steady -- so the reported speed cannot answer "is the fan on?". Reading
// it as truth is what made the button say "off" while the fan was spinning.
//
// State, all owned by the firmware and mirrored here:
//   vfan  - commanded run state (authoritative)
//   vset  - the speed SETPOINT; survives fan-off, lid-close, and reboots
//   vdir  - airflow direction; independent of run state
//   vrep  - reported speed: DIAGNOSTIC ONLY, never drives UI state
let fanDir=0;       // 0 = out, 1 = in
let fanSet=0;       // slider position = setpoint; 0 until the bus tells us
function paintFan(){
  const locked=fanLocked();
  const shut=!curVent;                 // a closed lid means the fan cannot RUN
  const b=document.getElementById("ftog");
  b.textContent=curFan?"ON":"off"; b.className=curFan?"on":"";
  b.disabled=locked||shut;             // only the run button is gated by the lid
  const d=document.getElementById("fdir");
  d.className="sw2"+(fanDir?" in":"");
  d.style.opacity=locked?"0.45":"";    // airflow stays usable with the lid shut
  document.getElementById("fknob").textContent=fanDir?"In":"Out";
  document.getElementById("vrange").disabled=locked;
  document.getElementById("fannote").textContent=
    shut ? "· open vent to enable fan"
         : (locked ? "· temporarily stopping for airflow change" : "");
}
// The fan physically stops and restarts when airflow reverses, and reports
// speed 0 while it does. Rather than decode that transient off the wire,
// lock the fan controls for 10 s and say plainly what is happening.
function fanLocked(){return Date.now()<fanLockUntil;}
function setFanLock(){
  fanLockUntil=Date.now()+13000;
  if(fanHold<fanLockUntil) fanHold=fanLockUntil;   // never shorten the lock
  paintFan();
  const tick=()=>{
    if(fanLocked()){setTimeout(tick,250);return;}
    paintFan();
  };
  setTimeout(tick,250);
}
// on/off: an explicit fan= command. Speed is not mentioned, so the firmware
// starts at its remembered setpoint and the setpoint cannot be clobbered.
document.getElementById("ftog").onclick=()=>{
  if(fanLocked()||!curVent)return;
  curFan=curFan?0:1;
  fanHold=Date.now()+3000;
  paintFan();
  cmd("vent&fan="+curFan);
};
document.getElementById("fdir").onclick=()=>{
  if(fanLocked())return;
  const wasRunning=curFan;        // only a SPINNING fan has to stop and restart
  fanDir^=1;
  paintFan();
  cmd("vent&dir="+fanDir);        // direction only; run state untouched
  if(wasRunning) setFanLock();    // fan off: nothing stops, so no lockout
};
const vr=document.getElementById("vrange");
// Mobile browsers do not reliably fire "change" on a range input, and reading
// vr.value inside a release handler proved unreliable on the phone. Track the
// value in a variable, send throttled on input and on every release event.
let ventPend=0;
function sendVentSpeed(){
  const v=ventPend|0;
  fanHold=Date.now()+3000;
  if(v<=0){                       // slider at zero = stop, setpoint preserved
    if(curFan){curFan=0;paintFan();cmd("vent&speed=0");}
    return;
  }
  // Setting a speed does NOT start the fan; only the fan button does that.
  fanSet=v; paintFan();
  cmd("vent&speed="+v);
}
vr.oninput=()=>{
  if(fanLocked())return;
  ventPend=parseInt(vr.value,10)||0;
  document.getElementById("sppct").textContent=Math.round(ventPend/2)+"%";
  const now=Date.now();
  if(now-(vr._last||0)>250){vr._last=now;sendVentSpeed();}
};
["change","pointerup","touchend","mouseup","keyup"].forEach(ev=>
  vr.addEventListener(ev,()=>{
    if(fanLocked())return;
    ventPend=parseInt(vr.value,10)||ventPend;
    vr._last=Date.now();
    sendVentSpeed();
  }));

// --- inverter ------------------------------------------------------------------------
// The inverter is a single-shot latch with no status echo, so its state comes
// from the AC line: live mains means it is running. That reading is only
// meaningful while FRESH -- the frame comes from the inverter itself and stops
// when it powers down, so a held-over reading would report a dead inverter as
// running. The firmware sends -1 when it has no current reading, and that is
// shown as "?" rather than guessed at.
let curInv=-1;          // 1 = on, 0 = off, -1 = unknown
let invHold=0;
function paintInv(){
  const b=document.getElementById("invtog");
  b.textContent=curInv===1?"ON":(curInv===0?"off":"?");
  b.className=curInv===1?"on":"";
}
document.getElementById("invtog").onclick=()=>{
  // Optimistic, but briefly: the AC line is authoritative and will confirm or
  // contradict this within a couple of seconds.
  curInv=curInv===1?0:1; paintInv(); invHold=Date.now()+4000;
  cmd("inv&on="+(curInv===1?1:0));
};

// --- command + poll plumbing ----------------------------------------------------------
async function cmd(q){
  try{await fetch("/api/cmd?c="+q,{method:"POST"});}catch(e){}
  setTimeout(poll,300);
  setTimeout(poll,1200);
}
let spHold=0;
async function poll(){
  try{
    const j=await (await fetch("/api/state")).json();
    document.getElementById("build").textContent=j.build||"";
    document.getElementById("batt").textContent=j.batt||"";
    document.getElementById("drawline").textContent=j.drawline||"--";
    document.getElementById("socpct").textContent=j.soc!=null?j.soc.toFixed(0)+"%":"--";
    document.getElementById("lifeline").textContent=j.lifeline||"";
    document.getElementById("socbar").style.width=(j.soc!=null?j.soc:0)+"%";
    // Battery-compartment fans: report the commanded level, not current.
    const gf=document.getElementById("gfan");
    if(gf) gf.textContent=j.gfan==null?"--":(j.gfan>0?"on, auto":"off, auto");
    document.getElementById("tempin").textContent=
      j.tempin!=null?(j.tempin.toFixed(0)+"°F inside"):"";
    if(j.lights) LIGHTS.forEach((n,i)=>{
      const L=j.lights[i];
      if(Date.now()>=lHold[i]&&L.on!==lOn[i]){lOn[i]=L.on;paintLight(i);}
      if(!LIGHT_CTL[i]){                     // status-only row
        const t=document.getElementById("L"+i);
        t.textContent="use panel";
      }
      // Feedback current is quantised to 0.125 A per count, so a single count
      // (0.125 A) renders as "0.1" at one decimal. Show two decimals below 1 A
      // to keep the smallest readable step visible.
      const am=document.getElementById("la"+i);
      if(am) am.textContent=Math.abs(L.watts)>0.5?fmtDraw(L.amps,L.watts):"";
    });
    PLAIN.forEach((p,i)=>{
      const st=j.sw[p.sw]?1:0;
      if(Date.now()>=holdUntil[i]&&st!==curP[i]){curP[i]=st;paint(i);}
      document.getElementById("a"+i).textContent=
        Math.abs(j.watts[p.sw])>0.5?fmtDraw(j.amps[p.sw],j.watts[p.sw]):"";
    });
    if(Date.now()>=acHold){
      const m=j.acmode||"off";
      acSeg=segFromMode(m); paintAc();
      compOn=(m==="cool"||m==="on"); paintComp();
      if(j.acfan!=null){
        acFan=(j.acfan===0)?0:((j.acfanspd||0)>=0x96?2:1);
      }
      paintAcFan();
    }
    if(Date.now()>=spHold){
      coolSp=(j.coolsp!=null)?j.coolsp:null;
      document.getElementById("coolsp").textContent=coolSp!=null?coolSp+"°":"--";
    }
    const sOpen=j.vopen?1:0;          // explicit field, not parsed prose
    if(Date.now()>=ventHold){
      const was=curVent, wasMoving=lidMoving;
      lidMoving=!!j.vmoving;
      if(!lidMoving) curVent=sOpen;      // trust position only when settled
      if(was!==curVent||wasMoving!==lidMoving){paintVent();paintFan();}
    }
    document.getElementById("ventst").textContent=lidMoving?"· moving":"";
    if(Date.now()>=fanHold){
      // Authoritative fields only. vrep is deliberately ignored: it reports 0
      // on a 5-10 s cycle while the fan is genuinely running.
      if(j.vfan!=null) curFan=j.vfan?1:0;
      if(j.vdir!=null) fanDir=j.vdir?1:0;
      paintFan();
    }
    if(document.activeElement!==vr&&!vr._drag&&!fanLocked()){
      // The slider shows the SETPOINT, which persists across fan-off and
      // lid-close. While the panel still owns the vent (vown=0) the firmware
      // adopts the observed speed into vset, so this shows reality on a
      // fresh boot instead of an invented default.
      if(j.vset!=null&&j.vset>0){
        fanSet=j.vset;
        vr.value=fanSet;
        document.getElementById("sppct").textContent=Math.round(fanSet/2)+"%";
      }else{
        document.getElementById("sppct").textContent="--";
      }
    }
    // -1 means the box has no fresh AC reading: show "?" rather than keeping a
    // stale value on screen.
    if(j.invon!=null&&Date.now()>=invHold&&curInv!==j.invon){curInv=j.invon;paintInv();}
    acPower=!!j.aclive; paintAcGate();
    document.getElementById("invline").textContent=
      j.shore?"•  shore power  •  "+(j.invtext||"") : (j.invtext?"•  "+j.invtext:"");
    // Rixen: read-only mirror of the heater's own state
    document.getElementById("rixcur").textContent=j.rixcur!=null?j.rixcur.toFixed(1)+"°F":"--";
    document.getElementById("rixtgt").textContent=j.rixtgt!=null?j.rixtgt.toFixed(1)+"°F":"--";
    document.getElementById("rixheat").textContent=j.rixheat?"· calling for heat":"";
    document.getElementById("rixfurn").textContent=j.rixfurn?"on":"off";
    document.getElementById("rixhw").textContent=j.rixhw?"on":"off";
    document.getElementById("rixfan").textContent=j.rixfan?String(j.rixfan):"off";
    document.getElementById("fresh").textContent=j.fresh!=null?j.fresh+"%":"--";
    document.getElementById("freshbar").style.width=(j.fresh||0)+"%";
    document.getElementById("gray").textContent=j.gray!=null?j.gray+"%":"--";
    document.getElementById("graybar").style.width=(j.gray||0)+"%";
    // Frame counters and raw channel levels were debugging aids; the pack
    // summary and any PDM fault are what is still worth surfacing.
    // Pack summary, then the bus counters. CAN2's age is the one that matters:
    // the battery readings above are suppressed when it goes stale, and this
    // says how long it has been quiet.
    // DEMO: set window.tempDemo=1 in the console to render synthetic history
    // and check the layout without waiting days for real samples.
    // Demo data is built once and reused; regenerating it each poll made the
    // chart twitch every second and hid whether the layout was right.
    if(window.tempDemo){
      if(!window.tempDemoData){
        const d=[];
        for(let i=0;i<121;i++){
          const hour=(i%24);
          const base=118+14*Math.sin((hour-9)/24*2*Math.PI);
          d.push(Math.round(base+(Math.random()*3-1.5)));
        }
        window.tempDemoData=d;
      }
      drawTemp(window.tempDemoData,121);
    }
    else if(j.temphist) drawTemp(j.temphist, j.tempfill||0);
    document.getElementById("foot").textContent=
      (j.packline?j.packline+"\n":"")+(j.foot||"");
  }catch(e){}
}

// --- board temperature chart ------------------------------------------------------
// 121 hourly buckets: 120 whole hours plus the one in progress. The board has
// no calendar -- millis() resets on boot -- so the axis is relative, "-5d" to
// "now". Hours with too little data arrive as null and are drawn as gaps, not
// interpolated across.
function drawTemp(hist, fill){
  if(!hist||!hist.length)return;
  const W=480,H=110,PAD=18,YW=30;   // YW: gutter for the y-axis labels
  const vals=hist.filter(v=>v!=null);
  if(!vals.length){
    document.getElementById("tempchart").innerHTML=
      '<div class="sub">no samples yet</div>';
    return;
  }
  let lo=Math.min.apply(null,vals), hi=Math.max.apply(null,vals);
  if(hi-lo<4){const m=(hi+lo)/2;lo=m-2;hi=m+2;}          // avoid a flat scale
  const PW=W-YW;                     // plot width, right of the label gutter
  const bw=PW/hist.length;
  let bars="";
  hist.forEach((v,i)=>{
    if(v==null)return;                                    // gap: draw nothing
    const h=Math.max(1,(v-lo)/(hi-lo)*(H-PAD));
    bars+=`<rect x="${(YW+i*bw).toFixed(2)}" y="${(H-PAD-h).toFixed(2)}" `+
          `width="${Math.max(1,bw).toFixed(2)}" height="${h.toFixed(2)}" fill="var(--ac)"/>`;
  });
  // ESP32-S3 maximum rated ambient is 85 °C = 185 °F. Drawn only when the
  // current scale actually reaches it -- an off-scale line would be
  // misleading, and a line pinned to the top edge doubly so.
  const TMAX=185;
  let yaxis="";
  if(TMAX>=lo&&TMAX<=hi){
    const y=H-PAD-(TMAX-lo)/(hi-lo)*(H-PAD);
    yaxis+=`<line x1="${YW}" y1="${y.toFixed(1)}" x2="${W}" y2="${y.toFixed(1)}" `+
           `stroke="#c0392b" stroke-width="1.5"/>`;
    yaxis+=`<text x="${W-52}" y="${(y-3).toFixed(1)}" fill="#c0392b" font-size="10">`+
           `85°C max</text>`;
  }
  [0,0.5,1].forEach(f=>{
    const v=lo+(hi-lo)*f;
    const y=H-PAD-f*(H-PAD);
    yaxis+=`<line x1="${YW}" y1="${y.toFixed(1)}" x2="${W}" y2="${y.toFixed(1)}" `+
           `stroke="#2a3440" stroke-width="1"/>`;
    yaxis+=`<text x="0" y="${(y+3).toFixed(1)}" fill="#8b98a5" font-size="10">`+
           `${v.toFixed(0)}°</text>`;
  });
  // day gridlines + labels, oldest (-5d) at the left through to now
  let axis="";
  for(let d=0;d<=5;d++){
    const x=YW+(d*24)*bw;
    axis+=`<line x1="${x.toFixed(1)}" y1="0" x2="${x.toFixed(1)}" y2="${H-PAD}" `+
          `stroke="#2a3440" stroke-width="1"/>`;
    const lbl=d===5?"now":("-"+(5-d)+"d");
    // The last label sits on the right edge, so anchor it inward or it clips.
    const anc=d===5?'text-anchor="end"':'';
    const lx=d===5?x-2:x+2;
    axis+=`<text x="${lx.toFixed(1)}" y="${H-6}" fill="#8b98a5" font-size="10" ${anc}>${lbl}</text>`;
  }
  const box=document.getElementById("tempchart");
  // Keep the newest data in view. The chart is wider than a phone screen, and
  // with only a few hours of history everything sits at the right-hand edge --
  // which looked blank until you scrolled. Anchor right, and leave the user
  // scrolled where they put it if they have gone looking at older data.
  const wasRight = box.scrollWidth - box.clientWidth - box.scrollLeft < 4;
  box.innerHTML=
    `<svg width="${W}" height="${H}" style="min-width:${W}px">${yaxis}${axis}${bars}</svg>`;
  if(wasRight||box._first===undefined){box.scrollLeft=box.scrollWidth;box._first=1;}
  document.getElementById("tempsum").textContent=
    `${lo.toFixed(0)}–${hi.toFixed(0)}°F over ${fill<121?fill+"h so far":"5 days"}`;
}

// --- remaining buttons ------------------------------------------------------------------
// Setpoint lives in a variable; reading it back out of the rendered text
// made the DOM the source of truth (and relied on parseInt stopping at the
// degree sign). Same failure family as the vent bugs.
let coolSp=null;
function stepCool(d){
  spHold=Date.now()+1600;
  if(coolSp!=null){
    coolSp+=d;
    document.getElementById("coolsp").textContent=coolSp+"°";   // optimistic
  }
  cmd("cool&d="+d);
}
// Touch drags do not always set activeElement, so mark the slider as held
// for the duration of the gesture and let polls leave it alone.
document.querySelectorAll('input[type=range]').forEach(el=>{
  const down=()=>{el._drag=true;};
  const up=()=>{setTimeout(()=>{el._drag=false;},400);};
  el.addEventListener("pointerdown",down);
  el.addEventListener("touchstart",down,{passive:true});
  el.addEventListener("pointerup",up);
  el.addEventListener("touchend",up,{passive:true});
  el.addEventListener("blur",up);
});
// Tap to fill the chart with a synthetic five-day cycle -- lets the layout and
// scrolling be checked now instead of after days of real samples. Tap again
// to return to live data.
document.getElementById("tempdemo").onclick=()=>{
  window.tempDemo=!window.tempDemo;
  document.getElementById("tempdemo").className=window.tempDemo?"on":"";
  const box=document.getElementById("tempchart"); box._first=undefined;
  poll();
};
document.getElementById("coolup").onclick=()=>stepCool(1);
document.getElementById("cooldn").onclick=()=>stepCool(-1);
for(let i=0;i<4;i++)paintLight(i); for(let i=0;i<3;i++)paint(i);
paintAc(); paintAcGate(); paintComp(); paintAcFan(); paintVent(); paintFan(); paintInv();
document.getElementById("sppct").textContent="--";   // no invented value pre-poll
setInterval(poll,1200);
poll();
</script></body></html>)rawliteral";
