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
.mut{color:var(--mut)}input[type=range]{width:100%}
.bar{display:inline-block;width:84px;height:12px;background:#0d1117;border-radius:6px;overflow:hidden;vertical-align:middle;margin-left:8px}
.bar>div{height:100%;background:var(--ac)}
.bar>div.g{background:#a07840}
.sw2{position:relative;display:inline-flex;width:132px;height:40px;background:#2a3440;border-radius:10px;cursor:pointer;user-select:none}
.sw2 .lbl{flex:1;display:flex;align-items:center;justify-content:center;font-size:14px;color:var(--mut);z-index:1}
.sw2 .knob{position:absolute;top:3px;left:3px;width:63px;height:34px;border-radius:8px;background:var(--ac);
  display:flex;align-items:center;justify-content:center;font-size:14px;color:#fff;transition:left .18s ease;z-index:2}
.sw2.in .knob{left:66px}
.sw2.off .knob{background:#46535f}
.dimtbl{width:100%;border-collapse:collapse}
.dimtbl td{padding:5px 3px;vertical-align:middle}
.dimtbl td.nm{white-space:nowrap}
.dimtbl td.bt{width:74px}
.dimtbl button{width:100%;min-width:0;padding:9px 6px;font-size:14px}
.dimtbl input[type=range]{width:100%;margin:0;vertical-align:middle}
.dimtbl tr.mst td{border-top:1px solid #2a3440;padding-top:9px}
.row.sub2{padding-left:16px;font-size:14px}
.row.sub2 button{padding:8px 13px;font-size:14px}
#foot{font-size:12px;color:var(--mut);margin-top:16px;white-space:pre-wrap}
</style></head><body>
<div class="row" style="padding:0 2px"><h1 style="margin:0">Van Companion</h1><span class="sub" id="build"></span></div>

<div class="card"><div class="row"><div><div class="big" id="soc">--%</div><div class="sub" id="batt"></div></div>
<div style="text-align:right"><div class="big" id="tempin">--</div><div class="sub">inside</div></div></div></div>

<h2>Lights &amp; switches</h2><div class="card">
<table class="dimtbl"><tbody id="switches"></tbody></table></div>

<h2>Climate</h2><div class="card" id="climcard">
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
<div class="row"><span class="name">Fan</span>
<span><button id="ftog">off</button></span></div>
<div class="row"><span class="name">Fan airflow</span>
<span class="sw2" id="fdir"><span class="lbl">Out</span><span class="lbl">In</span><span class="knob" id="fknob">Out</span></span></div>
<div class="row"><span class="name">Fan speed</span><b id="sppct">--</b></div>
<div class="row"><input type="range" id="vrange" min="0" max="255" value="0"></div>
</div>

<h2>Power</h2><div class="card">
<div class="row"><span class="name">Inverter <span class="sub" id="invline"></span></span>
<span><button id="invtog">off</button></span></div>
<div class="row"><span class="name">Fresh water</span>
<span><b id="fresh">--</b><span class="bar"><div id="freshbar"></div></span></span></div>
<div class="row"><span class="name">Gray water</span>
<span><b id="gray">--</b><span class="bar"><div id="graybar" class="g"></div></span></span></div>
</div>

<div id="foot"></div>
<script>
// --- switch rows ---------------------------------------------------------------
// Rows 0-3 are the dimmable lights (shadow injection holds a level; the panel
// always wins if someone touches a switch). Row 4 is master. Rows 5-7 are
// plain wall-switch spoofs with no dimming.
// dimIdx maps a light row to the firmware's LIGHT_DO order; swIdx maps a
// plain row to the SW[] spoof table.
const LIGHTS=["cabin","garage","reading","awning"];
const LIGHT_CTL=[1,1,0,1];   // reading has no wall switch -> status only
const LIGHT_SW=[0,1,-1,5];   // index into the firmware's SW[] spoof table
const PLAIN=[{n:"aux",sw:3},{n:"pump",sw:2},{n:"recirc",sw:4}];
const lHold=[0,0,0,0], lOn=[0,0,0,0], lPct=[0,0,0,0];

const holdUntil=[0,0,0], curP=[0,0,0];

const tb=document.getElementById("switches");
LIGHTS.forEach((n,i)=>{
  const r=document.createElement("tr");
  // "reading" has no wall switch on this van and dimming is gone, so that
  // row is a status readout only — the panel is the way to control it.
  const btn=LIGHT_CTL[i]?`<button id="L${i}">off</button>`
                        :`<span class="sub" id="L${i}">panel</span>`;
  r.innerHTML=`<td class="nm">${n} <span class="sub" id="la${i}"></span></td>`+
              `<td class="bt">${btn}</td>`;
  tb.appendChild(r);
});
PLAIN.forEach((p,i)=>{
  const r=document.createElement("tr");
  r.innerHTML=`<td class="nm">${p.n} <span class="sub" id="a${i}"></span></td>`+
              `<td class="bt"><button id="s${i}">off</button></td>`;
  tb.appendChild(r);
});

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
let curFan=0, fanHold=0;     // 0 off, 1 out, 2 in
function paintVent(){
  const b=document.getElementById("vtog");
  b.textContent=curVent?"Open":"Closed";
  b.className=curVent?"on":"sel";
}
document.getElementById("vtog").onclick=()=>{
  curVent^=1; paintVent(); ventHold=Date.now()+15000;
  cmd("vent&open="+curVent);
};
// Fan on/off and airflow direction are independent: direction is remembered
// while the fan is off so turning it back on resumes the same airflow.
let fanDir=0;       // 0 = out, 1 = in
let lastVSpeed=100; // remembered so the slider shows a level while off
function paintFan(){
  const b=document.getElementById("ftog");
  b.textContent=curFan?"ON":"off"; b.className=curFan?"on":"";
  const d=document.getElementById("fdir");
  d.className="sw2"+(fanDir?" in":"")+(curFan?"":" off");
  document.getElementById("fknob").textContent=fanDir?"In":"Out";
}
document.getElementById("ftog").onclick=()=>{
  curFan=curFan?0:1; paintFan(); fanHold=Date.now()+3000;
  cmd(curFan?("vent&dir="+fanDir+"&speed="+fanSpeed()):"vent&speed=0");
};
document.getElementById("fdir").onclick=()=>{
  fanDir^=1; paintFan(); fanHold=Date.now()+3000;
  cmd("vent&dir="+fanDir+(curFan?"&speed="+fanSpeed():""));
};
function fanSpeed(){const v=parseInt(document.getElementById("vrange").value);return v>0?v:100;}
const vr=document.getElementById("vrange");
vr.oninput=()=>{document.getElementById("sppct").textContent=Math.round(vr.value/2.55)+"%";};
vr.onchange=()=>cmd("vent&speed="+vr.value);

// --- inverter ------------------------------------------------------------------------
// Single-shot latch, no status echo: button = last commanded state, the AC
// line sub-text next to it is the ground truth.
let curInv=-1;
function paintInv(){
  const b=document.getElementById("invtog");
  b.textContent=curInv===1?"ON":"off";
  b.className=curInv===1?"on":"";
}
document.getElementById("invtog").onclick=()=>{
  curInv=curInv===1?0:1; paintInv(); cmd("inv&on="+(curInv===1?1:0));
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
    document.getElementById("soc").textContent=j.soc!=null?j.soc.toFixed(0)+"%":"--";
    document.getElementById("batt").textContent=j.batt||"";
    document.getElementById("tempin").textContent=j.tempin!=null?j.tempin.toFixed(0)+"°F":"";
    if(j.lights) LIGHTS.forEach((n,i)=>{
      const L=j.lights[i];
      if(Date.now()>=lHold[i]&&L.on!==lOn[i]){lOn[i]=L.on;paintLight(i);}
      if(!LIGHT_CTL[i]){                     // status-only row
        const t=document.getElementById("L"+i);
        t.textContent=L.on?"on (panel)":"panel";
      }
      const am=document.getElementById("la"+i);
      if(am) am.textContent=L.amps>0.02?L.amps.toFixed(1)+" A":"";
    });
    PLAIN.forEach((p,i)=>{
      const st=j.sw[p.sw]?1:0;
      if(Date.now()>=holdUntil[i]&&st!==curP[i]){curP[i]=st;paint(i);}
      document.getElementById("a"+i).textContent=j.amps[p.sw]>0.02?j.amps[p.sw].toFixed(1)+" A":"";
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
    if(Date.now()>=spHold)
      document.getElementById("coolsp").textContent=j.coolsp!=null?j.coolsp+"°":"--";
    const sOpen=(j.ventst||"").indexOf("open")===0?1:0;
    if(Date.now()>=ventHold||sOpen===curVent){curVent=sOpen;paintVent();}
    document.getElementById("ventst").textContent=(j.ventst||"").indexOf("moving")>=0?"· moving":"";
    if(Date.now()>=fanHold){
      curFan=j.vspeed>0?1:0;
      if(j.fansp) fanDir=(j.fansp.indexOf("in")>=0)?1:0;
      paintFan();
    }
    if(document.activeElement!==vr&&!vr._drag){
      // Show the SET speed even when the fan is off, like the light sliders.
      if(j.vspeed>0) lastVSpeed=j.vspeed;
      const shown=j.vspeed>0?j.vspeed:lastVSpeed;
      vr.value=shown;
      document.getElementById("sppct").textContent=Math.round(shown/2.55)+"%";
    }
    if(j.invon!=null&&j.invon>=0&&curInv!==j.invon){curInv=j.invon;paintInv();}
    acPower=!!j.aclive; paintAcGate();
    document.getElementById("invline").textContent=
      j.shore?"· shore power · "+(j.invtext||"") : (j.invtext?"· "+j.invtext:"");
    document.getElementById("fresh").textContent=j.fresh!=null?j.fresh+"%":"--";
    document.getElementById("freshbar").style.width=(j.fresh||0)+"%";
    document.getElementById("gray").textContent=j.gray!=null?j.gray+"%":"--";
    document.getElementById("graybar").style.width=(j.gray||0)+"%";
    document.getElementById("foot").textContent=(j.foot||"")+(j.dbg?"\n"+j.dbg:"");
  }catch(e){}
}

// --- remaining buttons ------------------------------------------------------------------
function stepCool(d){
  spHold=Date.now()+1600;
  const el=document.getElementById("coolsp");
  const v=parseInt(el.textContent);
  if(!isNaN(v)) el.textContent=(v+d)+"°";
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
document.getElementById("coolup").onclick=()=>stepCool(1);
document.getElementById("cooldn").onclick=()=>stepCool(-1);
for(let i=0;i<4;i++)paintLight(i); for(let i=0;i<3;i++)paint(i);
paintAc(); paintAcGate(); paintComp(); paintAcFan(); paintVent(); paintFan(); paintInv();
setInterval(poll,1200);
poll();
</script></body></html>)rawliteral";
