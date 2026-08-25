#pragma once
// The single-page UI. Lives in a header because the Arduino .ino
// preprocessor misparses 'async function' inside the raw string
// as a C++ declaration.
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
button{background:#2a3440;color:var(--tx);border:0;border-radius:10px;padding:10px 16px;font-size:15px;min-width:64px;cursor:pointer}
button.on{background:var(--on)}button.blue{background:var(--ac)}button:active{opacity:.8}
.big{font-size:26px;font-weight:600}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.mut{color:var(--mut)}input[type=range]{width:100%}
#foot{font-size:12px;color:var(--mut);margin-top:16px;white-space:pre-wrap}
</style></head><body>
<h1>Van Companion</h1>

<div class="card"><div class="row"><div><div class="big" id="soc">--%</div><div class="sub" id="batt"></div></div>
<div style="text-align:right"><div class="big" id="tempin">--</div><div class="sub" id="inv"></div></div></div></div>

<h2>Lights &amp; switches</h2><div class="card" id="switches"></div>

<h2>Climate</h2><div class="card">
<div class="row"><span class="name">A/C <span class="sub" id="acmode"></span></span>
<span><button id="acoff">Off</button> <button id="acon" class="blue">On</button> <button id="accomp">Comp off</button></span></div>
<div class="row"><span class="name">Cool setpoint <span class="sub">panel shows 2&deg;F less</span></span>
<span><button id="cooldn">&minus;</button> <b id="coolsp">--</b> <button id="coolup">+</button></span></div>
</div>

<h2>Roof vent</h2><div class="card">
<div class="row"><span class="name">Vent <span class="sub" id="ventst"></span></span>
<span><button id="vclose">Close</button> <button id="vopen" class="blue">Open</button></span></div>
<div class="row"><span class="name">Fan <span class="sub" id="fansp"></span></span>
<span><button id="vout">Air out</button> <button id="vin">Air in</button></span></div>
<div class="row"><input type="range" id="vrange" min="0" max="200" value="0"></div>
</div>

<h2>Power</h2><div class="card">
<div class="row"><span class="name">Inverter</span><span><button id="invoff">Off</button> <button id="invon" class="blue">On</button></span></div>
<div class="row"><span class="name">Fresh water</span><b id="fresh">--</b></div>
<div class="row"><span class="name">Gray water</span><b id="gray">--</b></div>
</div>

<div id="foot"></div>
<script>
const swNames=["cabin","garage","pump","aux","recirc","awning"];
let swBox=document.getElementById("switches");
swNames.forEach((n,i)=>{
  let r=document.createElement("div");r.className="row";
  r.innerHTML=`<span class="name">${n} <span class="sub" id="a${i}"></span></span><button id="s${i}">—</button>`;
  swBox.appendChild(r);
  r.querySelector("button").onclick=()=>cmd(`toggle&i=${i}`);
});
async function cmd(q){await fetch("/api/cmd?c="+q,{method:"POST"});setTimeout(poll,300);}
async function poll(){
  try{
    const j=await (await fetch("/api/state")).json();
    document.getElementById("soc").textContent=j.soc!=null?j.soc.toFixed(0)+"%":"--";
    document.getElementById("batt").textContent=j.batt||"";
    document.getElementById("tempin").textContent=j.tempin!=null?j.tempin.toFixed(0)+"°F":"";
    document.getElementById("inv").textContent=j.invtext||"";
    swNames.forEach((n,i)=>{
      let b=document.getElementById("s"+i);
      b.textContent=j.sw[i]?"ON":"off";b.className=j.sw[i]?"on":"";
      document.getElementById("a"+i).textContent=j.amps[i]>0.02?j.amps[i].toFixed(1)+" A":"";
    });
    document.getElementById("acmode").textContent=j.acmode||"";
    document.getElementById("coolsp").textContent=j.coolsp!=null?j.coolsp+"°":"--";
    document.getElementById("ventst").textContent=j.ventst||"";
    document.getElementById("fansp").textContent=j.fansp||"";
    document.getElementById("vrange").value=j.vspeed||0;
    document.getElementById("fresh").textContent=j.fresh!=null?j.fresh+"%":"--";
    document.getElementById("gray").textContent=j.gray!=null?j.gray+"%":"--";
    document.getElementById("foot").textContent=j.foot||"";
  }catch(e){}
  setTimeout(poll,1200);
}
document.getElementById("acon").onclick=()=>cmd("ac&mode=on");
document.getElementById("acoff").onclick=()=>cmd("ac&mode=off");
document.getElementById("accomp").onclick=()=>cmd("ac&mode=comp");
document.getElementById("coolup").onclick=()=>cmd("cool&d=1");
document.getElementById("cooldn").onclick=()=>cmd("cool&d=-1");
document.getElementById("vopen").onclick=()=>cmd("vent&open=1");
document.getElementById("vclose").onclick=()=>cmd("vent&open=0");
document.getElementById("vin").onclick=()=>cmd("vent&dir=1");
document.getElementById("vout").onclick=()=>cmd("vent&dir=0");
document.getElementById("vrange").onchange=e=>cmd("vent&speed="+e.target.value);
document.getElementById("invon").onclick=()=>cmd("inv&on=1");
document.getElementById("invoff").onclick=()=>cmd("inv&on=0");
poll();
</script></body></html>)rawliteral";
