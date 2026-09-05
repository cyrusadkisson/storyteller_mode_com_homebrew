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
#drawline,#invline{white-space:pre}
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
.qual{font-size:13px;color:var(--mut)}
.warn{font-size:13px;color:#e0a458;background:#2a2318;border-radius:8px;padding:8px;margin-bottom:6px}
.stale{opacity:.45}
.ph{font-size:13px;color:var(--mut);font-style:italic;padding:2px 2px 6px;text-align:center}
.ro{font-size:15px;color:var(--mut);display:inline-block;line-height:1.2}
.dimtbl input[type=range]{width:100%;margin:0;vertical-align:middle}
.dimtbl tr.mst td{border-top:1px solid #2a3440;padding-top:9px}
.row.sub2{padding-left:16px;font-size:14px}
.row.sub2 button{padding:8px 13px;font-size:14px}
#foot{font-size:12px;color:var(--mut);margin-top:16px;white-space:pre-wrap}
</style></head><body>
<div class="row" style="padding:0 2px"><h1 style="margin:0">Van Companion</h1><span class="sub" id="build"></span></div>

<h2>Battery &amp; Power</h2><div class="card">
<div id="cellwarn" class="warn" style="display:none"></div>
<div id="vsocwarn" class="warn" style="display:none"></div>
<div class="row"><span class="name">State of charge</span>
<span><b id="socpct">--</b></span><span class="bar"><div id="socbar" class="gr"></div></span></div>
<div class="row"><span>Power flow</span><span id="drawline">--</span></div>
<div id="pwrchart" style="overflow-x:auto"></div>
<div class="row"><span>&#x231B; <span class="qual">(derived from current flow)</span></span><span id="remnow">--</span></div>
<div class="row" id="remhalfrow" style="display:none"><span>&#x231B; <span class="qual" id="remlab">(derived from past 15m usage)</span></span><span id="remhalf">--</span></div>
<div class="row"><span class="name">Pack temperature</span><b id="packf">--</b></div>
<div id="packchart" style="overflow-x:auto"></div>
<div class="row"><span class="name">Inverter <span class="sub" id="invline"></span></span>
<span><button id="invtog">off</button></span></div>
<div class="sub" id="batt"></div></div>

<h2>Lights &amp; switches <span class="sub" style="text-transform:none;letter-spacing:0">(use panel for dimming)</span></h2><div class="card">
<div id="can1warn" class="warn" style="display:none">Control bus not responding — states below are last known, and controls will not take effect.</div>
<table class="dimtbl"><tbody id="switches"></tbody></table></div>

<h2>Climate</h2>
<div class="card" id="climcard">
<div class="row" id="acgate" style="display:none"><span class="sub">needs the inverter or shore power &mdash; the overhead unit runs on AC</span></div>
<div class="row"><span class="name">Roof A/C</span>
<span><button id="m0">Off</button> <button id="m1">Cool</button> <button id="m2">Heat</button></span></div>
<div class="row sub2" id="comprow" style="display:none"><span class="name">Compressor</span>
<span><button id="comp">off</button></span></div>
<div class="row sub2" id="acfanrow" style="display:none"><span class="name">Fan</span>
<span><button id="cf0">Auto</button> <button id="cf1">Low</button> <button id="cf2">High</button></span></div>
<div class="row sub2"><span class="name">Temp setpoint</span>
<span><button id="cooldn">&minus;</button> <b id="coolsp">--</b> <button id="coolup">+</button></span></div>
<div class="row"><span class="name">Cabin temp</span><b id="tempin">--</b></div>
<div id="ambchart" style="overflow-x:auto"></div>
</div>

<h2>Roof vent</h2><div class="card">
<div class="row"><span class="name">Vent <span class="sub" id="ventst"></span></span>
<span><button id="vtog">—</button></span></div>
<div class="row"><span class="name">Fan <span class="sub" id="fannote"></span></span>
<span><button id="ftog">off</button></span></div>
<div class="row"><span class="name">Fan airflow</span>
<span class="sw2" id="fdir"><span class="lbl">Out</span><span class="lbl">In</span><span class="knob" id="fknob">Out</span></span></div>
<div class="row"><span class="name">Fan speed</span><b id="sppct">--</b></div>
<div class="row"><button id="vradj" style="min-width:0;padding:6px 12px;font-size:13px">enable slider</button>
<input type="range" id="vrange" min="0" max="200" value="0" disabled style="flex:1"></div>
</div>

<h2>Tanks</h2><div class="card">
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

<h2>Cell monitor</h2><div class="card">
<div class="row"><span class="name">Lowest cell</span><b id="celllow">--</b></div>
<div class="row"><span class="name">Spread across the pack</span><b id="cellspread">--</b></div>
<div id="cellgrid" class="sub" style="font-family:monospace;white-space:pre;overflow-x:auto"></div>
<div class="sub" id="cellnote" style="margin-top:8px"></div>
<div class="sub" id="cellraw" style="font-family:monospace;white-space:pre;overflow-x:auto;font-size:11px;margin-top:8px;opacity:.6"></div>
</div>

<h2>Phone app board temp (ok up to 185&deg;F)</h2><div class="card">
<div id="tempchart" style="overflow-x:auto"></div>
<div class="row" id="hotwarnrow" style="display:none">
<span class="warn" style="flex:1;margin:0" id="hotwarn"></span>
<button id="hotdismiss" style="min-width:0;padding:6px 12px;font-size:13px">dismiss</button>
</div>
</div>

<div id="foot"></div>
<script>
// --- switch rows ---------------------------------------------------------------
// Rows 0-3 are the dimmable lights (shadow injection holds a level; the panel
// always wins if someone touches a switch). Row 4 is master. Rows 5-7 are
// plain wall-switch spoofs with no dimming.
// dimIdx maps a light row to the firmware's LIGHT_DO order; swIdx maps a
// plain row to the SW[] spoof table.
const LIGHTS=["Cabin lights","Cargo lights","Reading lights","Awning lights"];
const LIGHT_CTL=[1,1,0,1];   // reading has no wall switch -> status only
const LIGHT_SW=[0,1,-1,5];   // index into the firmware's SW[] spoof table
const PLAIN=[{n:"Aux",sw:3},{n:"Water pump",sw:2},{n:"Hot water circ.",sw:4}];
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
  r.innerHTML=`<td class="nm">Battery&rarr;galley fans</td>`+
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
  // curVent is -1 when the position is unknown. -1 is TRUTHY in JS, so the
  // old `curVent?"Open":"Closed"` rendered a green "Open" for an unknown lid
  // -- the exact "says open while it is shut" failure, from a second cause.
  // Compare explicitly.
  b.textContent=lidMoving?"···":(curVent<0?"?":(curVent>0?"Open":"Closed"));
  b.className=lidMoving?"sel":(curVent>0?"on":"sel");
  b.disabled=lidMoving;                     // no commands mid-transit
}
document.getElementById("vtog").onclick=()=>{
  if(lidMoving)return;
  // From unknown (-1), the useful command is CLOSE: it drives the vent to its
  // limit, which is what re-homes it. XOR on -1 would give 0 by accident
  // rather than by intent, so say it outright.
  curVent = (curVent<0) ? 0 : (curVent^1);
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
  document.getElementById("fannote").textContent=
    shut ? "· open vent to enable fan"
         : (locked ? "· temporarily stopping for airflow change" : "");
}
// The fan physically stops and restarts when airflow reverses, and reports
// speed 0 while it does. Rather than decode that transient off the wire,
// lock the fan controls for 10 s and say plainly what is happening.
function fanLocked(){return Date.now()<fanLockUntil;}
// The speed slider sits disabled so a swipe across the page cannot drag it;
// "adjust" arms it for 60 s of inactivity, then it locks again. Any slider
// activity pushes the deadline out.
let vrArmUntil=0;
const VR_ARM_MS=20000;
function vrArmed(){return Date.now()<vrArmUntil;}
function setSliderArm(on){
  if(on) vrArmUntil=Date.now()+VR_ARM_MS; else vrArmUntil=0;
  paintSlider();
}
// Single owner of the slider's disabled state. paintFan used to set it too,
// and the two fought: paintFan re-enabled the slider while the arm tick
// re-disabled it, so the control blinked. Disabled = not armed OR airflow-
// change lockout.
function paintSlider(){
  // Exactly as specified: the button always reads "enable slider"; it is
  // disabled while the slider is armed, and re-enabled when the arm lapses.
  // Independent of the fan's run state and the airflow lockout -- the slider
  // shows and sets the SETPOINT, which nothing transient may veto.
  const armed=vrArmed();
  vr.disabled=!armed;
  const b=document.getElementById("vradj");
  b.disabled=armed;
  b.textContent=armed?"slider enabled":"enable slider";
}
function touchSliderArm(){ if(vrArmed()) setSliderArm(true); }
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
  if(!vrArmed())return;
  touchSliderArm();
  ventPend=parseInt(vr.value,10)||0;
  document.getElementById("sppct").textContent=Math.round(ventPend/2)+"%";
  const now=Date.now();
  if(now-(vr._last||0)>250){vr._last=now;sendVentSpeed();}
};
["change","pointerup","touchend","mouseup","keyup"].forEach(ev=>
  vr.addEventListener(ev,()=>{
    if(!vrArmed())return;
    touchSliderArm();
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
    // Two extrapolations: the instantaneous one, which swings as the
    // compressor and heater cycle, and a 30-minute mean that rides through it.
    {
      // Plain hours rather than d/h -- shorter, and the qualifiers matter more
      // than the precision. Qualifiers are set smaller and muted so the
      // numbers read first.
      // Direction comes from an explicit field, never from parsing prose.
      // The trailing space on "til full " keeps the two figures aligned
      // against each other, since "full" is a character shorter than "empty".
      const suffix=d=>d==="full"?" til full ":(d==="empty"?" til empty":"");
      // Past 999h the number is dominated by noise around zero current, so it
      // is not an estimate -- but it is not an error either. A van sitting at
      // any state of charge with solar input balancing its draw is in a real,
      // healthy equilibrium, and so is a full pack at rest. Rather than guess
      // which, state the only thing actually known: the figure is off the top
      // of the scale. No direction is appended, because at that magnitude the
      // sign is noise too.
      const OVER=">999h";
      const fig=(mins,dir)=>{
        const h=mins/60;
        return h>999?OVER:h.toFixed(1)+"h"+suffix(dir);
      };
      // The two rows may disagree, direction included: plug in after a long
      // discharge and this one flips to "til full" while the hour-long mean
      // below still reads "til empty". It corrects itself as samples accrue.
      document.getElementById("remnow").textContent=
        j.instdir?fig(j.instmin,j.instdir):OVER;
      // The window grows 15m -> 1h; label and value track it. The whole row
      // stays hidden until 15 minutes of data exist -- showing a figure before
      // that would be noise.
      {
        // windir: "" = the window has not filled yet, and the row stays
        // hidden -- a figure from under 15 minutes of samples would be noise.
        // "full"/"empty" = a real figure. "error" = filled, but the net flow is
        // inside the deadband, which reads as off-scale rather than as a fault.
        const row=document.getElementById("remhalfrow");
        const n=j.pwrwinN||0;
        const wmin=Math.min(60,Math.round(n*5/60));
        if(!j.windir){
          row.style.display="none";
        }else{
          document.getElementById("remlab").textContent=
            "(derived from past "+wmin+"m usage)";
          document.getElementById("remhalf").textContent=
            (j.windir==="error")?OVER:fig(j.winmin,j.windir);
          row.style.display="flex";
        }
      }
    }
    document.getElementById("socbar").style.width=(j.soc!=null?j.soc:0)+"%";
    // Battery-compartment fans: report the commanded level, not current.
    const gf=document.getElementById("gfan");
    if(gf) gf.textContent=(j.gfan==null||j.gfan<0)?"--":(j.gfan>0?"on, auto":"off, auto");
    document.getElementById("tempin").textContent=
      j.tempin!=null?j.tempin.toFixed(0)+"°F":"--";
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
    // -1 = vent status stale: show "?" rather than holding a stale position.
    if(j.vopen<0){
      if(curVent!==-1){curVent=-1;lidMoving=false;paintVent();}
    }else if(Date.now()>=ventHold){
      const sOpen=j.vopen?1:0;        // explicit field, not parsed prose
      const was=curVent, wasMoving=lidMoving;
      lidMoving=!!j.vmoving;
      if(!lidMoving) curVent=sOpen;      // trust position only when settled
      if(was!==curVent||wasMoving!==lidMoving){paintVent();paintFan();}
    }
    document.getElementById("ventst").textContent=
      j.vunsure?"· position unknown — tap to re-home"
               :(j.vopen<0?"· vent status lost":(lidMoving?"· moving":""));
    if(Date.now()>=fanHold){
      // Authoritative fields only. vrep is deliberately ignored: it reports 0
      // on a 5-10 s cycle while the fan is genuinely running.
      if(j.vfan!=null) curFan=j.vfan?1:0;
      if(j.vdir!=null) fanDir=j.vdir?1:0;
      paintFan();
    }
    // Arming gates EDITING, never DISPLAY: the slider always shows the
    // current setpoint, armed or not. Gating this on vrArmed() (as shipped)
    // froze the position at the markup default of 0 whenever the slider was
    // locked -- showing 0 while the fan ran.
    if(document.activeElement!==vr&&!vr._drag){
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
    if(j.pwrhist) drawPower(j.pwrhist, j.tempfill||0, j.temphours);
    // CAN1 liveness: the head unit re-asserts PDM levels at ~91 Hz, so silence
    // means the bus is gone, not that nothing changed. Say so rather than
    // leaving stale switch states looking actionable.
    {
      const live=j.can1!==0;
      document.getElementById("can1warn").style.display=live?"none":"block";
      document.getElementById("switches").className=live?"":"stale";
      const cc=document.getElementById("climcard");
      if(cc) cc.className=live?"card":"card stale";
    }
    // Heat warning: latched in firmware until a NEW excursion updates it.
    // Dismiss keys on the excursion's identity (hotat), so a dismissal hides
    // this event but a fresh one re-shows it.
    {
      const row=document.getElementById("hotwarnrow");
      window.hotSeenAt=j.hotat||0;
      if(j.hotat&&j.hotat!==window.hotDismissed){
        const ago=j.hotago||0, d=Math.floor(ago/86400), h=Math.floor((ago%86400)/3600);
        document.getElementById("hotwarn").textContent=
          `Warning: Board was >185°F for more than an hour ${d}d ${h}h ago`;
        row.style.display="flex";
      } else row.style.display="none";
    }
    // Cell monitor. The decoded volts use the PROPOSED scaling (2.00 + b/100)
    // and are shown only as a convenience -- the raw bytes below them are the
    // evidence, and the spread is the answer. Nothing here is treated as fact.
    {
      // Confirmed per-cell on 2026-09-05: one byte read 0x7B against fifteen
      // 0x7D, matching the phone app exactly (docs/energy-can2.md). So this
      // section reports the WEAKEST CELL rather than debating what the bytes
      // are -- the BMS opens the contactor on the weakest cell, never on the
      // pack average, which is the whole reason this van dies at 80 % showing.
      const cs=j.cells||[];
      let lo=999,hi=-1,loI=-1;
      cs.forEach((v,i)=>{ if(v<lo){lo=v;loI=i;} if(v>hi)hi=v; });
      const have=j.cellfresh&&cs.length&&loI>=0;
      // The weak-cell banner sits at the top of the battery card, above the
      // V/SoC proxy: this is the condition that actually opens the contactor.
      {
        const cw=document.getElementById("cellwarn");
        if(j.cellbad&&have){
          const ago=j.cellbadago||0;
          const h=Math.floor(ago/3600), m=Math.floor((ago%3600)/60);
          cw.textContent=
            `Warning: cell ${loI+1} is at ${(2+lo/100).toFixed(2)}V, `+
            `${(j.cellspread/100).toFixed(2)}V below the strongest cell. The BMS opens the `+
            `contactor on this cell alone — expect a total power loss while the gauge `+
            `still reads a useful charge. Started ${h}h ${m}m ago.`;
          cw.style.display="block";
        } else cw.style.display="none";
      }
      document.getElementById("celllow").textContent=
        have?("c"+(loI+1)+"  "+(2+lo/100).toFixed(2)+"V"):"--";
      document.getElementById("cellspread").textContent=
        have?((j.cellspread/100).toFixed(2)+"V  ("+j.cellspread+" counts)"):"--";
      let g="";
      cs.forEach((v,i)=>{
        // Mark the low cell. With a failing cell in this pack, WHICH one is
        // lowest matters more than any of the sixteen values.
        const mark=(have&&i===loI&&j.cellspread>0)?"*":" ";
        g+=(i%4===0?(i?"\n":""):"  ")+
           mark+("c"+(i+1)).padStart(3," ")+" "+(2+v/100).toFixed(2);
      });
      document.getElementById("cellgrid").textContent=have?g:"";
      document.getElementById("cellnote").textContent=
        !have?"No cell monitor frames on CAN2.":
        (j.cellspread>0
          ?("c"+(loI+1)+" is "+(j.cellspread/100).toFixed(2)+
            "V below the highest cell. A gap that grows as the pack drains is the "+
            "shutdown signature: the BMS trips on this cell while the gauge still "+
            "reports what the other fifteen hold.")
          :"All sixteen equal. Cells diverge as the pack drains — near a full charge they genuinely are alike, so no spread here means little.");
      document.getElementById("cellraw").textContent=
        have&&j.cellraw?j.cellraw.join("\n"):"";
    }
    if(j.temphist) drawTemp(j.temphist, j.tempfill||0, j.temphours);
    if(j.ambhist) drawTemp(j.ambhist, j.tempfill||0, j.temphours, "ambchart");
    if(j.packhist) drawTemp(j.packhist, j.tempfill||0, j.temphours, "packchart", 0);
    document.getElementById("packf").textContent=
      j.packf!=null?j.packf.toFixed(0)+"°F":"--";
    // Voltage/SoC disagreement. Worded as what it MEANS -- a cell near its
    // floor -- because "low voltage at high charge" reads like a gauge fault,
    // which is the wrong conclusion and the one that cost three shutdowns.
    {
      const w=document.getElementById("vsocwarn");
      if(j.vsoc&&j.battV!=null&&j.soc!=null){
        const ago=j.vsocago||0;
        const h=Math.floor(ago/3600), m=Math.floor((ago%3600)/60);
        w.textContent=
          `Warning: pack is ${j.battV.toFixed(1)}V (${(j.vcell||j.battV/16).toFixed(2)}V/cell average) `+
          `while the gauge reads ${j.soc.toFixed(0)}%. That gap means a cell is near its floor, `+
          `not that the pack is empty — the BMS can open the contactor and kill all power at any `+
          `indicated charge. Started ${h}h ${m}m ago.`;
        w.style.display="block";
      } else w.style.display="none";
    }
    document.getElementById("foot").textContent=
      (j.packline?j.packline+"\n":"")+(j.foot||"");
  }catch(e){}
}

// 4-6 y-axis ticks anchored to sane values. Pick the coarsest step from a
// ladder of human-friendly increments that still yields at least 4 ticks; the
// first tick is the first step-multiple inside the range, so lines land on
// round numbers (115, 120, 125...) rather than arbitrary fractions of the span.
function niceTicks(lo,hi){
  const span=hi-lo;
  const steps=[0.5,1,2,2.5,5,10,20,25,50,100,200,250,500,1000,2000,2500,5000];
  let step=steps[steps.length-1];
  for(const s of steps){ if(span/s<=6){ step=s; break; } }
  const t=[];
  for(let v=Math.ceil(lo/step)*step; v<=hi+1e-9; v+=step)
    t.push(Math.round(v*100)/100);
  return t;
}
const fmtTick=v=>(Math.abs(v%1)>0.001?v.toFixed(1):v.toFixed(0));

// --- power flow chart ---------------------------------------------------------------
// Same buckets as the temperature charts, but signed: a draw is negative and a
// surplus positive, so bars grow from a zero baseline rather than the floor.
// Scale snaps to a round step and always includes zero.
function drawPower(hist, fill, days){
  if(!hist||!hist.length)return;
  const DAYS=days||Math.round((hist.length-1)/24);
  const box=document.getElementById("pwrchart");
  const vals=hist.filter(v=>v!=null);
  if(!vals.length){
    box.innerHTML='<div class="ph">Chart will appear here as data becomes '+
                  'available (15m)</div>';
    return;
  }
  const H=118,PAD=18,YW=14,YR=38,TOP=8;
  const W=Math.max(300,Math.floor(box.clientWidth)||480);
  // FIXED scale, owner call: a resizing axis made people misread the shape.
  const lo=-2200, hi=2200;
  const dlo=Math.min.apply(null,vals), dhi=Math.max.apply(null,vals);
  const PW=W-YW-YR, bw=PW/hist.length;
  const yOf=v=>H-PAD-(v-lo)/(hi-lo)*(H-PAD-TOP);
  const zero=yOf(0);
  let grid="";
  niceTicks(lo,hi).forEach(v=>{
    const y=yOf(v);
    grid+=`<line x1="${YW}" y1="${y.toFixed(1)}" x2="${W-YR}" y2="${y.toFixed(1)}" `+
          `stroke="${v===0?"#4a5560":"#2a3440"}" stroke-width="${v===0?1.5:1}"/>`;
    grid+=`<text x="${W-YR+4}" y="${(y+3).toFixed(1)}" fill="#8b98a5" font-size="10">${fmtTick(v)}W</text>`;
  });
  let bars="";
  hist.forEach((v,i)=>{
    if(v==null)return;
    const y=yOf(v), top=Math.min(y,zero), h=Math.max(1,Math.abs(zero-y));
    bars+=`<rect x="${(YW+i*bw).toFixed(2)}" y="${top.toFixed(2)}" `+
          `width="${Math.max(1,bw).toFixed(2)}" height="${h.toFixed(2)}" `+
          `fill="${v<0?"#c0623b":"var(--on)"}"/>`;
  });
  // Gridlines measured from "now" at the RIGHT EDGE of the newest bar, so the
  // newest bar sits INSIDE the now line. Labels centered under their lines.
  let axis="";
  const N=hist.length, DIV=48;                 // 48 bars = 12 h
  for(let k=0;k*DIV<N;k++){
    const x=YW+(N-k*DIV)*bw;
    axis+=`<line x1="${x.toFixed(1)}" y1="${TOP}" x2="${x.toFixed(1)}" y2="${H-PAD}" `+
          `stroke="#2a3440" stroke-width="1"/>`;
    const lbl=k===0?"now":("-"+(k*12)+"h");
    axis+=`<text x="${x.toFixed(1)}" y="${H-6}" fill="#8b98a5" font-size="10" `+
          `text-anchor="middle">${lbl}</text>`;
  }
  box.innerHTML=`<svg width="${W}" height="${H}" style="max-width:100%">${grid}${axis}${bars}</svg>`;
  // Summary line removed by owner.
}

// --- board temperature chart ------------------------------------------------------
// One bucket per hour: DAYS*24 whole hours plus the one in progress. The board
// has no calendar -- millis() resets on boot -- so the axis is relative,
// "-Nd" through to "now". Hours with too little data arrive as null and are drawn as gaps, not
// interpolated across.
function drawTemp(hist, fill, days, elId, tmax){
  if(!hist||!hist.length)return;
  const DAYS=days||Math.round((hist.length-1)/24);   // derived, never hardcoded
  // YW/YR: label gutters. The chart scrolls horizontally, so the axis is
  // repeated on the right -- otherwise reading a value means scrolling five
  // days back to find the scale.
  const H=118,PAD=18,YW=14,YR=34,TOP=8;
  const box=document.getElementById(elId||"tempchart");
  const W=Math.max(300,Math.floor(box.clientWidth)||480);   // fit the card
  const vals=hist.filter(v=>v!=null);
  if(!vals.length){
    document.getElementById(elId||"tempchart").innerHTML=
      '<div class="ph">Chart will appear here as data becomes available (15m)</div>';
    return;
  }
  // Keep the real data range for the summary, and scale the axis to the tens
  // that enclose it -- 103..131 gives 100..140. No extra padding: snapping
  // outward already leaves room, and adding 5 first pushed it a whole step too
  // far.
  const dlo=Math.min.apply(null,vals), dhi=Math.max.apply(null,vals);
  let lo=Math.floor(dlo/10)*10;
  let hi=Math.ceil(dhi/10)*10;
  if(hi-lo<20){hi=lo+20;}                                // never a flat scale
  const PW=W-YW-YR;                  // plot width, between the two gutters
  const bw=PW/hist.length;
  let bars="";
  hist.forEach((v,i)=>{
    if(v==null)return;                                    // gap: draw nothing
    const h=Math.max(1,(v-lo)/(hi-lo)*(H-PAD-TOP));
    bars+=`<rect x="${(YW+i*bw).toFixed(2)}" y="${(H-PAD-h).toFixed(2)}" `+
          `width="${Math.max(1,bw).toFixed(2)}" height="${h.toFixed(2)}" fill="var(--ac)"/>`;
  });
  // ESP32-S3 maximum rated ambient is 85 °C = 185 °F. Drawn only when the
  // current scale actually reaches it -- an off-scale line would be
  // misleading, and a line pinned to the top edge doubly so.
  // Default is the ESP32-S3 die limit. Other sensors pass their own, or 0 for
  // none: an "85°C max" line on a BATTERY chart would be actively wrong, not
  // merely unused, so the pack chart disables it rather than relying on the
  // limit happening to sit off-scale.
  const TMAX=(tmax===undefined)?185:tmax;
  let yaxis="";
  if(TMAX&&TMAX>=lo&&TMAX<=hi){
    const y=H-PAD-(TMAX-lo)/(hi-lo)*(H-PAD-TOP);
    yaxis+=`<line x1="${YW}" y1="${y.toFixed(1)}" x2="${W-YR}" y2="${y.toFixed(1)}" `+
           `stroke="#c0392b" stroke-width="1.5"/>`;
    yaxis+=`<text x="${YW+4}" y="${(y-3).toFixed(1)}" fill="#c0392b" font-size="10">`+
           `85°C max</text>`;
  }
  niceTicks(lo,hi).forEach(v=>{
    const f=(v-lo)/(hi-lo);
    const y=H-PAD-f*(H-PAD-TOP);
    yaxis+=`<line x1="${YW}" y1="${y.toFixed(1)}" x2="${W-YR}" y2="${y.toFixed(1)}" `+
           `stroke="#2a3440" stroke-width="1"/>`;
    yaxis+=`<text x="${W-YR+4}" y="${(y+3).toFixed(1)}" fill="#8b98a5" font-size="10">`+
           `${fmtTick(v)}°</text>`;
  });
  // Gridlines measured from "now" at the RIGHT EDGE of the newest bar, so the
  // newest bar sits INSIDE the now line. Labels centered under their lines.
  let axis="";
  const N=hist.length, DIV=48;                 // 48 bars = 12 h
  for(let k=0;k*DIV<N;k++){
    const x=YW+(N-k*DIV)*bw;
    axis+=`<line x1="${x.toFixed(1)}" y1="${TOP}" x2="${x.toFixed(1)}" y2="${H-PAD}" `+
          `stroke="#2a3440" stroke-width="1"/>`;
    const lbl=k===0?"now":("-"+(k*12)+"h");
    axis+=`<text x="${x.toFixed(1)}" y="${H-6}" fill="#8b98a5" font-size="10" `+
          `text-anchor="middle">${lbl}</text>`;
  }
  // Keep the newest data in view if it ever does overflow. The chart is wider than a phone screen, and
  // with only a few hours of history everything sits at the right-hand edge --
  // which looked blank until you scrolled. Anchor right, and leave the user
  // scrolled where they put it if they have gone looking at older data.
  box.innerHTML=
    `<svg width="${W}" height="${H}" style="max-width:100%">${yaxis}${axis}${bars}</svg>`;
  // Summary line removed by owner: the chart carries the information.
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
document.getElementById("hotdismiss").onclick=()=>{
  window.hotDismissed=window.hotSeenAt||0;
  poll();
};
document.getElementById("vradj").onclick=()=>setSliderArm(!vrArmed());
setInterval(()=>{ paintSlider(); },500);
document.getElementById("coolup").onclick=()=>stepCool(1);
document.getElementById("cooldn").onclick=()=>stepCool(-1);
for(let i=0;i<4;i++)paintLight(i); for(let i=0;i<3;i++)paint(i);
paintAc(); paintAcGate(); paintComp(); paintAcFan(); paintVent(); paintFan(); paintInv();
document.getElementById("sppct").textContent="--";   // no invented value pre-poll
setInterval(poll,1200);
poll();
</script></body></html>)rawliteral";
