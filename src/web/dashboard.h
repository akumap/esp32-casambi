/**
 * Status & Control Dashboard - the browser page served at GET /
 *
 * A single self-contained page (HTML + CSS + JS, no external resources — the
 * gateway has no internet access and must work on a plain LAN). It shows the
 * two link states of the gateway (Bluetooth side = Casambi mesh, API side =
 * Wi-Fi/REST/WebSocket) plus every unit with its generic, cloud-derived
 * control names and current values, and it CONTROLS those units: the on/off
 * pill is a button, every writable control gets a slider, and `temperature`
 * gets a Kelvin slider spanning the fixture's own min..max with a warm→cold
 * gradient track.
 *
 * Which endpoint a widget writes is derived from the control type, mirroring
 * what the FHEM module does:
 *   dimmer (single)   -> POST /api/units/:id/level        {"level":0-255}
 *   dimmer (multiple) -> POST /api/units/:id/state        {"dimmer0":0-255}
 *   vertical, slider  -> POST /api/units/:id/vertical|slider {"value":0-255}
 *   white, others     -> POST /api/units/:id/state        {"<name>":raw}
 *   temperature       -> POST /api/units/:id/temperature  {"kelvin":min..max}
 *   on/off            -> POST /api/units/:id/on|off, or one atomic /state
 *                        write over all channels on multi-dimmer fixtures
 * Unknown control types stay display-only: their value range is not part of
 * the interface, so a slider would be guesswork.
 *
 * The page itself is static and served unauthenticated; ALL data it displays
 * and every write it performs go through the authenticated endpoints
 * (GET /api/status, GET /api/units, the POST control routes and the /ws push
 * channel), so it exposes nothing and can do nothing that a client without the
 * API token could not. When a Casambi password is stored, the page asks for it
 * once and derives the API token the same way the FHEM module does —
 * SHA-256("casambi-api:" + password), computed in the browser (a small JS
 * implementation, because window.crypto.subtle is unavailable over plain
 * http://) and kept in localStorage. The password never goes on the wire.
 *
 * Two properties of the gateway shape the control logic:
 *   - The BLE command queue is BLE_CMD_QUEUE_DEPTH deep and the loop task
 *     executes ONE command per pass, so a dragged slider is throttled to one
 *     write per 250 ms (newest value wins, the released value always ships).
 *   - A write is only queued (202); the resulting state arrives later as a
 *     unit_state push. The value the user set therefore stays on screen for a
 *     grace period until the device echoes it, so knobs never jump around.
 *
 * The page consumes the /api/* + /ws interface exactly as documented; it adds
 * no endpoint, message type or field of its own, so it is NOT covered by the
 * VERSIONING CONTRACT at FHEM_API_VERSION_MAJOR in config.h. It does have to
 * FOLLOW that interface though: when a field it reads or an endpoint it writes
 * is renamed or removed, update the JavaScript below in the same commit.
 *
 * Served with the (const uint8_t*, len) response overload, which streams
 * straight from flash (AsyncProgmemResponse) — the plain const char* overload
 * would copy the whole page into a String and need a ~36 kB contiguous heap
 * block on every request.
 *
 * To work on the page: copy the raw string into a .html file and open it
 * against a gateway (or a mock serving /api/status, /api/units, the control
 * routes and /ws).
 */

#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <Arduino.h>

static const char DASHBOARD_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="light dark">
<title>Casambi Gateway</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><circle cx='8' cy='8' r='7' fill='%230b7a4b'/></svg>">
<style>
:root{--bg:#eef1f5;--card:#fff;--fg:#182029;--mut:#69737f;--line:#dde2e8;--ok:#0b7a4b;--bad:#c22e39;--warn:#a06308;--idle:#69737f;--shadow:0 1px 2px rgba(16,24,32,.08),0 1px 8px rgba(16,24,32,.05)}
@media (prefers-color-scheme:dark){:root{--bg:#10141a;--card:#1a2027;--fg:#e7ebef;--mut:#98a3af;--line:#2a323b;--ok:#3fbd85;--bad:#ef6b73;--warn:#e0a13a;--idle:#8b96a2;--shadow:none}}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--fg);
 font:400 16px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Ubuntu,sans-serif;
 padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)}
a{color:inherit}
/* ---------- header ---------- */
header{position:sticky;top:0;z-index:3;display:flex;flex-wrap:wrap;gap:6px 12px;align-items:center;
 justify-content:space-between;padding:10px clamp(12px,3vw,28px);
 background:color-mix(in srgb,var(--card) 88%,transparent);backdrop-filter:blur(10px);
 border-bottom:1px solid var(--line)}
@supports not (background:color-mix(in srgb,red,blue)){header{background:var(--card)}}
.brand{display:flex;align-items:baseline;gap:8px;min-width:0}
h1{margin:0;font-size:clamp(1.02rem,1.6vw + .6rem,1.3rem);font-weight:650;letter-spacing:-.01em;
 white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.brand span{color:var(--mut);font-size:.82rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.pills{display:flex;flex-wrap:wrap;gap:6px}
/* ---------- pills ---------- */
.pill{display:inline-flex;align-items:center;gap:6px;padding:3px 10px;border-radius:999px;
 border:1px solid var(--line);background:var(--card);font-size:.8rem;font-weight:600;white-space:nowrap}
.pill::before{content:"";width:8px;height:8px;border-radius:50%;background:var(--idle);flex:none}
.pill.ok{color:var(--ok);border-color:color-mix(in srgb,var(--ok) 35%,var(--line))}
.pill.ok::before{background:var(--ok)}
.pill.bad{color:var(--bad);border-color:color-mix(in srgb,var(--bad) 35%,var(--line))}
.pill.bad::before{background:var(--bad)}
.pill.warn{color:var(--warn);border-color:color-mix(in srgb,var(--warn) 35%,var(--line))}
.pill.warn::before{background:var(--warn)}
.pill.plain::before{display:none}
/* ---------- layout ---------- */
main{max-width:1240px;margin:0 auto;padding:clamp(12px,2.6vw,24px) clamp(12px,3vw,28px) 32px}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(min(100%,270px),1fr))}
.grid.units{grid-template-columns:repeat(auto-fill,minmax(min(100%,250px),1fr));align-items:start}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px 16px;box-shadow:var(--shadow)}
.card h2{margin:0 0 10px;font-size:.78rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--mut)}
.sec{display:flex;flex-wrap:wrap;align-items:baseline;gap:8px 12px;margin:24px 0 10px}
.sec h2{margin:0;font-size:1.02rem;font-weight:650}
.sec .note{color:var(--mut);font-size:.85rem}
/* ---------- key/value rows ---------- */
.row{display:flex;gap:10px;justify-content:space-between;align-items:baseline;
 padding:5px 0;border-top:1px solid var(--line);font-size:.92rem}
.row:first-child{border-top:0}
.row .k{color:var(--mut);flex:0 1 auto}
.row .v{text-align:right;font-weight:600;overflow-wrap:anywhere}
.row .v small{display:block;font-weight:400;color:var(--mut);font-size:.78rem}
.mono{font-variant-numeric:tabular-nums;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.86em}
/* ---------- unit cards ---------- */
.unit{display:flex;flex-direction:column;gap:8px}
.unit.off{opacity:.62}
.uhead{display:flex;gap:8px;align-items:flex-start;justify-content:space-between}
.uhead h3{margin:0;font-size:1rem;font-weight:600;line-height:1.25;overflow-wrap:anywhere}
.uhead .id{display:block;color:var(--mut);font-size:.76rem;font-weight:400}
.ctrls{display:flex;flex-direction:column}
.bar{height:4px;border-radius:2px;background:var(--line);overflow:hidden;margin-top:2px}
.bar i{display:block;height:100%;background:var(--ok);border-radius:2px}
.empty{color:var(--mut);font-size:.9rem;padding:6px 0}
/* ---------- controls: on/off pill button + sliders ---------- */
button.tgl{width:auto;margin:0;min-height:38px;padding:7px 14px;font-size:.8rem;
 font-weight:600;background:var(--card);color:inherit}
button.tgl:disabled{cursor:default;opacity:.55}
/* touch-action:pan-y keeps vertical page scrolling working when a swipe starts
   on a slider, while the horizontal gesture still moves the knob. */
.rng{-webkit-appearance:none;appearance:none;display:block;width:100%;height:32px;
 margin:0;padding:0;border:0;background:transparent;cursor:pointer;touch-action:pan-y}
.rng:focus-visible{outline:2px solid #2f6fbb;outline-offset:1px;border-radius:6px}
.rng::-webkit-slider-runnable-track{height:8px;border-radius:4px;
 background:linear-gradient(90deg,var(--ok) 0 calc(var(--p,0)*1%),var(--line) calc(var(--p,0)*1%) 100%)}
.rng::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;margin-top:-7px;
 border-radius:50%;background:var(--card);border:1px solid var(--mut);
 box-shadow:0 1px 3px rgba(16,24,32,.35)}
.rng::-moz-range-track{height:8px;border-radius:4px;background:var(--line)}
.rng::-moz-range-progress{height:8px;border-radius:4px;background:var(--ok)}
.rng::-moz-range-thumb{width:20px;height:20px;border-radius:50%;background:var(--card);
 border:1px solid var(--mut);box-shadow:0 1px 3px rgba(16,24,32,.35)}
/* Colour temperature: the track itself carries the warm→cold scale, so the
   knob position reads as a colour, and no fill is drawn over it. */
.rng.temp::-webkit-slider-runnable-track{background:linear-gradient(90deg,
 #ffab52,#ffd6a1,#f4f1ea,#d3e5ff,#a5caff)}
.rng.temp::-moz-range-track{background:linear-gradient(90deg,
 #ffab52,#ffd6a1,#f4f1ea,#d3e5ff,#a5caff)}
.rng.temp::-moz-range-progress{background:transparent}
.rng:disabled{cursor:default;opacity:.45}
#toast{position:fixed;left:50%;transform:translateX(-50%);bottom:calc(16px + env(safe-area-inset-bottom));
 z-index:5;max-width:min(92vw,460px);padding:10px 14px;border-radius:10px;font-size:.88rem;
 color:#fff;background:#33404e;box-shadow:0 4px 16px rgba(16,24,32,.35)}
#toast.err{background:var(--bad)}
/* ---------- auth / banner ---------- */
#auth{max-width:420px;margin:8vh auto 0}
#auth p{margin:0 0 12px;color:var(--mut);font-size:.9rem}
label{display:block;font-size:.85rem;color:var(--mut);margin-bottom:4px}
/* Scoped to the auth card on purpose — a global input rule would also frame
   the range sliders in the device cards. */
#auth input{width:100%;padding:10px 12px;font-size:1rem;color:var(--fg);background:var(--bg);
 border:1px solid var(--line);border-radius:9px}
button{margin-top:12px;width:100%;padding:11px 14px;font-size:1rem;font-weight:600;color:#fff;
 background:#2f6fbb;border:0;border-radius:9px;cursor:pointer;min-height:44px}
button.link{width:auto;margin:0;padding:0;background:none;color:var(--mut);font-weight:400;
 font-size:.82rem;text-decoration:underline;min-height:0}
#err{margin-top:10px;color:var(--bad);font-size:.88rem;min-height:1.2em}
#banner{margin-bottom:12px;padding:9px 12px;border-radius:10px;font-size:.9rem;color:var(--fg);
 border:1px solid var(--line);background:var(--card);
 border-color:color-mix(in srgb,var(--warn) 40%,var(--line));
 background:color-mix(in srgb,var(--warn) 10%,var(--card))}
footer{display:flex;flex-wrap:wrap;gap:8px 16px;align-items:center;justify-content:space-between;
 margin-top:26px;padding-top:12px;border-top:1px solid var(--line);color:var(--mut);font-size:.8rem}
.hide{display:none!important}
/* Landscape phones: a sticky header eats too much of a ~390 px tall viewport. */
@media (max-height:480px){header{position:static}#auth{margin-top:3vh}}
</style></head><body>

<header>
  <div class="brand"><h1 id="title">Casambi Gateway</h1><span id="sub"></span></div>
  <div class="pills">
    <span class="pill" id="pBt">Bluetooth</span>
    <span class="pill" id="pApi">API</span>
  </div>
</header>

<main>
  <section id="auth" class="card hide">
    <h2>Authentication</h2>
    <p>This gateway is protected. Enter the Casambi network password — the API
       token is derived from it locally in your browser, the password itself is
       never sent to the device.</p>
    <form id="authForm" autocomplete="on">
      <label for="pw">Casambi network password</label>
      <input id="pw" name="casambi-password" type="password" autocomplete="current-password">
      <button type="submit">Connect</button>
    </form>
    <div id="err"></div>
  </section>

  <div id="content" class="hide">
    <div id="banner" class="hide"></div>

    <div class="grid">
      <section class="card">
        <h2>Bluetooth side</h2>
        <div id="btRows"></div>
      </section>
      <section class="card">
        <h2>API side</h2>
        <div id="apiRows"></div>
      </section>
      <section class="card">
        <h2>System</h2>
        <div id="sysRows"></div>
      </section>
    </div>

    <div class="sec">
      <h2>Devices</h2>
      <span class="pill plain" id="devCount">–</span>
      <span class="note" id="devNote"></span>
    </div>
    <div class="grid units" id="units"></div>

    <footer>
      <span id="stamp">–</span>
      <span><button class="link" id="forget">Forget password</button></span>
    </footer>
  </div>
</main>
<div id="toast" class="hide"></div>

<script>
"use strict";
const $=id=>document.getElementById(id);
const TOKEN_KEY="casambiApiToken";
const POLL_MS=5000;

let token=localStorage.getItem(TOKEN_KEY)||"";
let ws=null, wsRetry=0, wsTimer=null;
let httpOk=false, wsUp=false, booted=false;
let status=null, hello=null;
const units=new Map();           // id -> unit object (last known state)
let renderQueued=false;

/* ---------------------------------------------------------------- helpers */
const esc=s=>String(s==null?"":s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
const pct=v=>Math.min(100,Math.round(v/2.55));

function dur(ms){
  if(ms==null) return "–";
  let s=Math.floor(ms/1000);
  const d=Math.floor(s/86400); s-=d*86400;
  const h=Math.floor(s/3600);  s-=h*3600;
  const m=Math.floor(s/60);    s-=m*60;
  if(d) return d+" d "+h+" h";
  if(h) return h+" h "+m+" min";
  if(m) return m+" min "+s+" s";
  return s+" s";
}
const kb=b=>b==null?"–":Math.round(b/1024)+" kB";

// Control types whose raw 0-255 value has a meaningful percentage (the same
// mapping the FHEM module applies); unknown types are shown raw only.
const SCALED=new Set(["vertical","white","slider"]);
const BLE_STATE={0:"idle",1:"connected",2:"key exchanged",3:"authenticated",99:"error"};
const DISCONNECT={1:"requested by user",2:"Bluetooth link loss",3:"authentication failed",
                  4:"key exchange failed",5:"timeout",6:"internal error"};

// Rows are given as [key, value, sub] triples; value/sub are already escaped
// by the callers that build them from device data.
function rows(el,list){
  el.innerHTML=list.filter(r=>r).map(r=>
    '<div class="row"><span class="k">'+esc(r[0])+'</span><span class="v">'+r[1]+
    (r[2]?'<small>'+r[2]+'</small>':'')+'</span></div>').join("");
}
const mono=v=>'<span class="mono">'+esc(v)+'</span>';

function setPill(el,text,cls){
  el.textContent=text;
  el.className="pill"+(cls?" "+cls:"");
}

/* ------------------------------------------------------------ SHA-256 (hex)
   The API token is SHA-256("casambi-api:" + password). crypto.subtle is not
   available over plain http:// (no secure context), so the digest is computed
   here instead — the password never leaves the browser. */
const SHA_K=new Uint32Array([
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
function sha256hex(str){
  const H=new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                           0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
  const m=new TextEncoder().encode(str), bits=m.length*8;
  const total=(m.length+9+63)&~63;
  const buf=new Uint8Array(total); buf.set(m); buf[m.length]=0x80;
  const dv=new DataView(buf.buffer);
  dv.setUint32(total-8,Math.floor(bits/4294967296));
  dv.setUint32(total-4,bits>>>0);
  const w=new Uint32Array(64), rot=(x,n)=>(x>>>n)|(x<<(32-n));
  for(let off=0;off<total;off+=64){
    for(let i=0;i<16;i++) w[i]=dv.getUint32(off+i*4);
    for(let i=16;i<64;i++){
      const a=w[i-15], b=w[i-2];
      const s0=rot(a,7)^rot(a,18)^(a>>>3), s1=rot(b,17)^rot(b,19)^(b>>>10);
      w[i]=(w[i-16]+s0+w[i-7]+s1)>>>0;
    }
    let a=H[0],b=H[1],c=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7];
    for(let i=0;i<64;i++){
      const S1=rot(e,6)^rot(e,11)^rot(e,25), ch=(e&f)^(~e&g);
      const t1=(h+S1+ch+SHA_K[i]+w[i])>>>0;
      const S0=rot(a,2)^rot(a,13)^rot(a,22), mj=(a&b)^(a&c)^(b&c);
      const t2=(S0+mj)>>>0;
      h=g;g=f;f=e;e=(d+t1)>>>0;d=c;c=b;b=a;a=(t1+t2)>>>0;
    }
    H[0]=(H[0]+a)>>>0;H[1]=(H[1]+b)>>>0;H[2]=(H[2]+c)>>>0;H[3]=(H[3]+d)>>>0;
    H[4]=(H[4]+e)>>>0;H[5]=(H[5]+f)>>>0;H[6]=(H[6]+g)>>>0;H[7]=(H[7]+h)>>>0;
  }
  return Array.from(H,x=>x.toString(16).padStart(8,"0")).join("");
}

/* --------------------------------------------------------------- transport */
class AuthError extends Error{}

async function get(path){
  const r=await fetch(path,{cache:"no-store",headers:token?{"X-API-Key":token}:{}});
  if(r.status===401) throw new AuthError("unauthorized");
  if(!r.ok) throw new Error("HTTP "+r.status);
  return r.json();
}

function openWs(){
  if(ws||!booted) return;
  const url=(location.protocol==="https:"?"wss://":"ws://")+location.host+"/ws"+
            (token?"?k="+encodeURIComponent(token):"");
  let sock;
  try{ sock=new WebSocket(url); }catch(e){ scheduleWs(); return; }
  ws=sock;
  sock.onopen=()=>{ wsRetry=0; wsUp=true; queueRender(); };
  sock.onmessage=ev=>{ try{ onPush(JSON.parse(ev.data)); }catch(e){} };
  sock.onclose=()=>{ if(ws===sock){ ws=null; wsUp=false; queueRender(); scheduleWs(); } };
  sock.onerror=()=>{ /* onclose follows */ };
}
function scheduleWs(){
  if(wsTimer) return;
  const delay=Math.min(30000,1000*Math.pow(2,wsRetry++));
  wsTimer=setTimeout(()=>{ wsTimer=null; openWs(); },delay);
}

function onPush(msg){
  if(msg.type==="hello"){
    hello=msg;
    if(Array.isArray(msg.units)){
      // A truncated hello carries only the first units; the periodic
      // GET /api/units then fills in the rest.
      if(!msg.units_truncated) units.clear();
      msg.units.forEach(u=>mergeUnit(u));
    }
    if(msg.units_truncated) loadUnits();
  }else if(msg.type==="unit_state"){
    // Explicit patch — a blind merge would let the message's own "type" field
    // overwrite the unit's fixture type from GET /api/units.
    const p={id:msg.id,level:msg.level,online:msg.online,on:msg.on};
    if(msg.vertical!=null) p.vertical=msg.vertical;
    if(msg.colorTemp!=null){ p.colorTemp=msg.colorTemp; p.cctMin=msg.cctMin; p.cctMax=msg.cctMax; }
    if(Array.isArray(msg.controls)) p.controls=msg.controls;
    mergeUnit(p);
  }else if(msg.type==="connection_state"){
    if(hello){ hello.ble_connected=msg.connected; hello.gateway=msg.gateway; }
    // The BLE diagnostics live in /api/status — pull a fresh copy right away
    // instead of waiting for the next poll tick.
    poll();
  }
  queueRender();
}

// Merge a partial update (unit_state) into the last known unit state.
function mergeUnit(u){
  const id=u.id!=null?u.id:u.deviceId;
  if(id==null) return;
  const prev=units.get(id)||{id:id};
  const next=Object.assign(prev,u);
  next.id=id;
  units.set(id,next);
  clearEchoed(next);   // device confirmed a value the user set → stop holding it
}

/* ----------------------------------------------------------------- polling */
async function poll(){
  try{
    status=await get("/api/status");
    httpOk=true;
  }catch(e){
    if(e instanceof AuthError){ needAuth(); return; }
    httpOk=false;
  }
  queueRender();
}

async function loadUnits(){
  try{
    const d=await get("/api/units");
    httpOk=true;
    if(Array.isArray(d.units)){
      const seen=new Set();
      d.units.forEach(u=>{ mergeUnit(u); seen.add(u.id); });
      Array.from(units.keys()).forEach(id=>{ if(!seen.has(id)) units.delete(id); });
    }
  }catch(e){
    if(e instanceof AuthError){ needAuth(); return; }
    httpOk=false;
  }
  queueRender();
}

let ticking=false;
async function tick(){
  if(ticking||!booted) return;       // never let slow requests pile up
  ticking=true;
  try{
    await poll();
    // Without a WebSocket the unit states have to be polled as well.
    if(!wsUp && httpOk) await loadUnits();
  }finally{ ticking=false; }
}

// Give the WebSocket slot back while the page is not visible (the gateway
// allows only WS_MAX_CLIENTS connections and drops the OLDEST one beyond
// that — a forgotten background tab would otherwise keep pushing FHEM out).
document.addEventListener("visibilitychange",()=>{
  if(document.hidden){
    if(wsTimer){ clearTimeout(wsTimer); wsTimer=null; }
    if(ws){ const s=ws; ws=null; wsUp=false; s.close(); }
  }else if(booted){
    wsRetry=0; openWs(); tick();
  }
});

/* ----------------------------------------------------------------- control */
// Writes go to the documented control endpoints; the resulting state comes
// back asynchronously as a unit_state push (or with the next poll), never in
// the HTTP response — the gateway only queues the BLE command (202).
const GRACE_MS=2500;   // how long a value the user set survives on screen
const SEND_MS=250;     // minimum spacing between two writes of one control

const overrides=new Map();   // "unit:control" -> {v,until} (user value in flight)
const splits=new Map();      // unit -> per-channel values remembered across "off"
const sends=new Map();       // "unit:control" -> throttle state
let dragging=null, renderDeferred=false, expiryTimer=null, toastTimer=null;

// Control is pointless without the BLE link (the endpoints answer 503) or
// without a reachable gateway — the widgets are disabled instead of failing.
function ctlEnabled(){ return bleConnected() && (httpOk||wsUp); }

function override(id,name){
  const k=id+":"+name, o=overrides.get(k);
  if(!o) return null;
  if(o.until<=Date.now()){ overrides.delete(k); return null; }
  return o.v;
}
function setOverride(id,name,v){
  overrides.set(id+":"+name,{v:v,until:Date.now()+GRACE_MS});
  if(expiryTimer) clearTimeout(expiryTimer);
  expiryTimer=setTimeout(()=>{ expiryTimer=null; queueRender(); },GRACE_MS+100);
}
// Drop the override once the device reports (nearly) the value we asked for.
// Temperature needs a wide tolerance: the operation carries kelvin/50 and the
// unit reports its state as a 0-255 raw value, so the echo is quantised twice.
function clearEchoed(u){
  const hit=(name,dev,tol)=>{
    const o=overrides.get(u.id+":"+name);
    if(o&&dev!=null&&Math.abs(dev-o.v)<=tol) overrides.delete(u.id+":"+name);
  };
  if(Array.isArray(u.controls)){
    u.controls.forEach(c=>{
      const name=c.name||c.type;
      if(c.type==="temperature") hit(name,c.kelvin,60);
      else                       hit(name,c.value,2);
    });
  }
  hit("level",u.level,2);
  hit("vertical",u.vertical,2);
  if(u.colorTemp!=null&&u.cctMin!=null&&u.cctMax>u.cctMin)
    hit("temperature",Math.round(u.cctMin+u.colorTemp*(u.cctMax-u.cctMin)/255),60);
}

function unitOn(u,dims){
  if(dims&&dims.length) return dims.some(c=>c.val>0);
  return !!u.on||u.level>0;
}

function toast(msg,isErr){
  const t=$("toast");
  t.textContent=msg;
  t.className=isErr?"err":"";
  if(toastTimer) clearTimeout(toastTimer);
  toastTimer=setTimeout(()=>{ t.className="hide"; },4500);
}

async function post(path,body){
  const h={};
  if(token) h["X-API-Key"]=token;
  const opts={method:"POST",headers:h};
  if(body){ h["Content-Type"]="application/json"; opts.body=JSON.stringify(body); }
  const r=await fetch(path,opts);
  if(r.status===401){ needAuth(); throw new AuthError("unauthorized"); }
  if(!r.ok){
    let m="HTTP "+r.status;
    try{ const j=await r.json(); if(j&&j.error) m=j.error; }catch(e){}
    throw new Error(m);
  }
  httpOk=true;
  return true;
}
// Fire-and-report: a rejected write (503 BLE down / queue full, 409 without a
// fixture layout) surfaces as a toast instead of a silent no-op.
function write(path,body){
  return post(path,body).catch(e=>{ if(!(e instanceof AuthError)) toast(e.message,true); });
}

function sendControl(id,name,kind,v){
  if(kind==="temperature") return write("/api/units/"+id+"/temperature",{kelvin:v});
  if(kind==="level")       return write("/api/units/"+id+"/level",{level:v});
  if(kind==="vertical")    return write("/api/units/"+id+"/vertical",{value:v});
  if(kind==="slider")      return write("/api/units/"+id+"/slider",{value:v});
  const b={}; b[name]=v;              // "state": named control, atomic write
  return write("/api/units/"+id+"/state",b);
}

// Dragging a slider must not turn into one request per pixel: the BLE command
// queue is 8 deep and one command is executed per loop pass. At most one write
// per SEND_MS goes out, the newest value wins, and the value the finger stops
// at is always sent (final).
function throttleSend(key,v,final,fn){
  let s=sends.get(key);
  if(!s){ s={t:0,timer:null,v:v}; sends.set(key,s); }
  s.v=v;
  const wait=SEND_MS-(Date.now()-s.t);
  if(final||wait<=0){
    if(s.timer){ clearTimeout(s.timer); s.timer=null; }
    s.t=Date.now();
    fn(s.v);
  }else if(!s.timer){
    s.timer=setTimeout(()=>{ s.timer=null; s.t=Date.now(); fn(s.v); },wait);
  }
}

function onSlide(el,final){
  const id=+el.dataset.u, name=el.dataset.c, v=+el.value;
  const lo=+el.min, span=(+el.max)-lo;
  // Update this widget in place — a full re-render would rip the element the
  // finger is currently on out of the DOM (see the dragging guard in render).
  el.style.setProperty("--p",span>0?Math.round((v-lo)*100/span):0);
  const t=ctrlText(el.dataset.fmt,v,el.dataset.sub||"");
  const vEl=el.parentNode.querySelector(".v");
  if(vEl) vEl.innerHTML=t[0]+(t[1]?'<small>'+t[1]+'</small>':'');
  setOverride(id,name,v);
  throttleSend(id+":"+name,v,final,x=>sendControl(id,name,el.dataset.kind,x));
}

function onToggle(btn){
  const id=+btn.dataset.u, u=units.get(id);
  if(!u) return;
  const dims=controlSpecs(u).filter(c=>c.dimmer);
  const on=unitOn(u,dims);
  if(dims.length>1){
    // Multi-dimmer fixtures: all channels in ONE atomic state write (the
    // level endpoint only reaches the first channel). "off" remembers the
    // current split so the next "on" restores it, like the FHEM module.
    const body={};
    if(on){
      splits.set(id,dims.map(c=>c.val));
      dims.forEach(c=>{ body[c.name]=0; });
    }else{
      const s=splits.get(id);
      dims.forEach((c,i)=>{ body[c.name]=(s&&s[i]>0)?s[i]:255; });
    }
    dims.forEach(c=>setOverride(id,c.name,body[c.name]));
    write("/api/units/"+id+"/state",body);
  }else{
    // /on and /off are level writes (255 / 0) — mirror that locally so the
    // card reacts immediately instead of waiting for the mesh echo.
    if(dims.length===1) setOverride(id,dims[0].name,on?0:255);
    write("/api/units/"+id+"/"+(on?"off":"on"));
  }
  queueRender();
}

/* ------------------------------------------------------------------ render */
function queueRender(){
  if(renderQueued) return;
  renderQueued=true;
  requestAnimationFrame(()=>{ renderQueued=false; render(); });
}

function bleConnected(){
  if(status) return !!status.ble_connected;
  return hello?!!hello.ble_connected:false;
}

function render(){
  // Never rebuild the cards while a knob is under the finger — the element
  // would be replaced mid-gesture. The render runs when the drag ends.
  if(dragging){ renderDeferred=true; return; }
  const s=status||{}, h=hello||{};
  const net=s.network_name||h.network||"";
  $("sub").textContent=net;
  document.title=net?"Casambi Gateway – "+net:"Casambi Gateway";

  /* --- header pills --- */
  const bt=bleConnected();
  // Without any working channel to the gateway the Bluetooth state is unknown
  // rather than "connected" — everything on screen is then last-known data.
  if(!httpOk&&!wsUp) setPill($("pBt"),"Bluetooth: unknown","");
  else               setPill($("pBt"),"Bluetooth: "+(bt?"connected":"disconnected"),bt?"ok":"bad");
  if(!httpOk)      setPill($("pApi"),"API: unreachable","bad");
  else if(wsUp)    setPill($("pApi"),"API: live","ok");
  else             setPill($("pApi"),"API: polling","warn");

  /* --- Bluetooth side --- */
  const gw=h.gateway||{};
  const gwMac=s.gateway_mac||gw.mac||"";
  const online=Array.from(units.values()).filter(u=>u.online).length;
  const proto=h.casambi_protocol_version;
  rows($("btRows"),[
    ["Link",bt?'<span class="pill ok">connected</span>':'<span class="pill bad">disconnected</span>',
      s.ble_state!=null?"state: "+esc(BLE_STATE[s.ble_state]||s.ble_state):""],
    ["Casambi network",esc(net||"–")],
    ["Gateway unit",gwMac?mono(gwMac):"–",gw.name?esc(gw.name):""],
    bt?["Signal",s.gateway_rssi!=null?esc(s.gateway_rssi)+" dBm":"–"]:null,
    bt?["Link uptime",esc(dur(s.connection_uptime_ms))]:null,
    bt?["Packets received",'<span class="mono">'+esc(s.packets_received!=null?s.packets_received:"–")+"</span>"]:null,
    s.last_disconnect_reason?["Last disconnect",
      esc(DISCONNECT[s.last_disconnect_reason]||("reason "+s.last_disconnect_reason)),
      s.last_disconnect_source?"source: "+esc(s.last_disconnect_source):""]:null,
    proto!=null?["Protocol version",esc(proto),
      "firmware tested with "+esc(h.casambi_protocol_min)+"–"+esc(h.casambi_protocol_max)]:null,
    ["Devices",esc(units.size),units.size?esc(online)+" online":""]
  ]);

  /* --- API side --- */
  const apiVer=(h.api_version_major!=null)?h.api_version_major+"."+h.api_version_minor:null;
  rows($("apiRows"),[
    ["Interface",httpOk?(wsUp?'<span class="pill ok">live push</span>':'<span class="pill warn">polling</span>')
                       :'<span class="pill bad">unreachable</span>',
      !httpOk?"no answer on /api/status":wsUp?"WebSocket /ws"
             :"REST only, every "+(POLL_MS/1000)+" s"],
    ["Wi-Fi",s.wifi_ssid?esc(s.wifi_ssid):"–",
      s.wifi_rssi!=null?esc(s.wifi_rssi)+" dBm":""],
    ["IP address",s.wifi_ip?mono(s.wifi_ip):"–"],
    ["Authentication",token?"API key":"open"],
    apiVer?["API version",esc(apiVer)]:null,
    ["Firmware build",esc(h.build!=null?h.build:"–")]
  ]);

  /* --- System --- */
  rows($("sysRows"),[
    ["Uptime",esc(dur(s.uptime_ms))],
    ["Free heap",esc(kb(s.free_heap)),
      s.largest_block!=null?"largest block "+esc(kb(s.largest_block)):""],
    ["Reboots",esc(s.boot_count!=null?s.boot_count:"–"),
      s.min_free_heap!=null?"min. free heap "+esc(kb(s.min_free_heap)):""],
    ["Time (UTC)",s.time_synced?mono(s.time_utc):"not synced",
      s.ntp_server?"NTP "+esc(s.ntp_server):""],
    ["Dropped pushes",esc(s.ws_drops!=null?s.ws_drops:"–"),
      (s.parse_partial||s.parse_malformed)?"parse: "+esc(s.parse_partial)+" partial / "+
        esc(s.parse_malformed)+" malformed":""]
  ]);

  /* --- devices --- */
  const list=Array.from(units.values()).sort((a,b)=>
    (a.name||"").localeCompare(b.name||"",undefined,{sensitivity:"base"})||a.id-b.id);
  $("devCount").textContent=units.size?(online+" / "+units.size+" online"):"none";
  $("devNote").textContent=!units.size?""
    :bt?"Tap on/off, drag a slider to set a value."
       :"Values are the last known state — the Bluetooth link is down, so control is unavailable.";
  $("units").innerHTML=list.length?list.map(unitCard).join("")
    :'<div class="card empty">No devices in the gateway configuration.</div>';

  const banner=$("banner");
  if(!httpOk){
    banner.textContent="The gateway is not answering. Retrying every "+(POLL_MS/1000)+" s…";
    banner.classList.remove("hide");
  }else banner.classList.add("hide");

  $("stamp").textContent="Updated "+new Date().toLocaleTimeString();
}

// One card per Casambi unit: name, reachability, the on/off toggle and one
// slider per controllable fixture control.
function unitCard(u){
  const specs=controlSpecs(u);
  const dims=specs.filter(c=>c.dimmer);
  const on=unitOn(u,dims);
  const pill='<button class="pill tgl'+(!u.online?" bad":on?" ok":"")+'" data-u="'+u.id+
      '" aria-pressed="'+(on?"true":"false")+'"'+(ctlEnabled()?"":" disabled")+'>'+
      (!u.online?"offline":on?"on":"off")+'</button>';
  const body=specs.length?specs.map(c=>ctrlHtml(u,c)).join("")
                        :'<div class="empty">No control data yet.</div>';
  return '<article class="card unit'+(u.online?"":" off")+'">'+
    '<div class="uhead"><h3>'+esc(u.name||("Unit "+u.id))+
    '<span class="id">ID '+esc(u.id)+(u.address?" · "+esc(u.address):"")+'</span></h3>'+
    pill+'</div><div class="ctrls">'+body+'</div></article>';
}

// Describe one control for both display and control:
//   name  – the generic control name the API addresses (dimmer0, temperature, …)
//   kind  – which endpoint writes it, null for read-only
//   fmt   – how the current value is rendered ("pct" | "raw" | "kelvin")
//   min/max/step/val – slider geometry in the unit the endpoint expects
// Prefers the generic `controls` array (names + raw values straight from the
// cloud fixture) and falls back to the legacy level/vertical/colorTemp fields
// for units whose fixture definition is unavailable.
function controlSpecs(u){
  const out=[];
  const push=s=>{
    // A value the user just set stays on screen until the device echoes it
    // back (or the grace period ends), so the knob does not jump around.
    const ov=override(u.id,s.name);
    if(ov!=null) s.val=ov;
    out.push(s);
  };
  if(Array.isArray(u.controls)&&u.controls.length){
    const nDim=u.controls.filter(c=>c.type==="dimmer").length;
    u.controls.forEach(c=>{
      const name=c.name||c.type||"control";
      const v=c.value!=null?c.value:0;
      if(c.type==="temperature"){
        // Kelvin bounds come from the fixture; 50 K is the on-air resolution
        // of the Casambi temperature operation (one byte, kelvin/50).
        const ok=c.kelvin!=null&&c.min!=null&&c.max>c.min;
        push({name:name,label:name,kind:ok?"temperature":null,fmt:"kelvin",
              min:c.min,max:c.max,step:50,val:ok?c.kelvin:v,
              sub:ok?esc(c.min)+"–"+esc(c.max)+" K":""});
      }else if(c.type==="dimmer"){
        // One dimmer → the plain level endpoint; several → an atomic
        // per-channel state write, which is the only way to address them.
        push({name:name,label:name,kind:nDim>1?"state":"level",fmt:"pct",
              min:0,max:255,step:1,val:v,dimmer:true});
      }else if(SCALED.has(c.type)){
        push({name:name,label:name,kind:c.type==="white"?"state":c.type,fmt:"raw",
              min:0,max:255,step:1,val:v});
      }else{
        // Unknown control type: shown, but not written — its value range is
        // not part of the interface, so a slider would be guesswork.
        push({name:name,label:name,kind:null,fmt:"plain",val:v});
      }
    });
  }else{
    push({name:"level",label:"level",kind:"level",fmt:"pct",
          min:0,max:255,step:1,val:u.level||0,dimmer:true});
    if(u.vertical!=null)
      push({name:"vertical",label:"vertical",kind:"vertical",fmt:"raw",
            min:0,max:255,step:1,val:u.vertical});
    if(u.colorTemp!=null){
      const ok=u.cctMin!=null&&u.cctMax>u.cctMin;
      push({name:"temperature",label:"temperature",kind:ok?"temperature":null,fmt:ok?"kelvin":"plain",
            min:u.cctMin,max:u.cctMax,step:50,
            val:ok?Math.round(u.cctMin+u.colorTemp*(u.cctMax-u.cctMin)/255):u.colorTemp,
            sub:ok?esc(u.cctMin)+"–"+esc(u.cctMax)+" K":""});
    }
  }
  return out;
}

// Value + hint text for a control, also used to refresh the labels live while
// a slider is being dragged.
function ctrlText(fmt,v,sub){
  if(fmt==="kelvin") return [esc(v)+" K",sub||""];
  if(fmt==="pct")    return [esc(pct(v))+" %","raw "+esc(v)];
  if(fmt==="raw")    return [esc(v),esc(pct(v))+" %"];
  return [esc(v),sub||""];
}

function ctrlHtml(u,c){
  const t=ctrlText(c.fmt,c.val,c.sub);
  let h='<div class="ctrl"><div class="row"><span class="k">'+esc(c.label)+
    '</span><span class="v">'+t[0]+(t[1]?'<small>'+t[1]+'</small>':'')+'</span></div>';
  if(c.kind){
    const span=c.max-c.min;
    const fill=span>0?Math.round((c.val-c.min)*100/span):0;
    h+='<input class="rng'+(c.kind==="temperature"?" temp":"")+'" type="range"'+
       ' min="'+c.min+'" max="'+c.max+'" step="'+c.step+'" value="'+c.val+'"'+
       ' style="--p:'+fill+'" data-u="'+u.id+'" data-c="'+esc(c.name)+'"'+
       ' data-kind="'+c.kind+'" data-fmt="'+c.fmt+'"'+
       (c.sub?' data-sub="'+esc(c.sub)+'"':'')+
       (ctlEnabled()?"":" disabled")+' aria-label="'+esc(c.label)+'">';
  }else if(c.fmt==="pct"||c.fmt==="raw"){
    h+='<div class="bar"><i style="width:'+pct(c.val)+'%"></i></div>';
  }
  return h+'</div>';
}

/* -------------------------------------------------------------------- auth */
function needAuth(){
  booted=false; httpOk=false; wsUp=false;
  if(ws){ const s=ws; ws=null; s.close(); }
  token="";
  localStorage.removeItem(TOKEN_KEY);
  $("content").classList.add("hide");
  $("auth").classList.remove("hide");
}

async function start(){
  try{
    status=await get("/api/status");
  }catch(e){
    if(e instanceof AuthError){ needAuth(); return false; }
    httpOk=false;                       // gateway unreachable, keep the UI up
  }
  httpOk=status!=null;
  booted=true;
  $("auth").classList.add("hide");
  $("content").classList.remove("hide");
  queueRender();
  await loadUnits();
  openWs();
  return true;
}

$("authForm").addEventListener("submit",async e=>{
  e.preventDefault();
  const pw=$("pw").value;
  if(!pw){ $("err").textContent="Please enter the network password."; return; }
  $("err").textContent="Checking…";
  token=sha256hex("casambi-api:"+pw);
  try{
    status=await get("/api/status");
  }catch(err){
    token="";
    $("err").textContent=(err instanceof AuthError)
      ?"Wrong password." : "Gateway not reachable ("+err.message+").";
    return;
  }
  localStorage.setItem(TOKEN_KEY,token);
  $("err").textContent="";
  $("pw").value="";
  await start();
});

$("forget").addEventListener("click",()=>{ needAuth(); });

/* Device interaction. Delegated on the container, because every render
   replaces the cards' markup — per-element listeners would be lost. */
const UNITS=$("units");
UNITS.addEventListener("pointerdown",e=>{
  const el=e.target.closest(".rng");
  if(el&&!el.disabled) dragging=el;
},{passive:true});
function endDrag(){
  if(!dragging) return;
  dragging=null;
  if(renderDeferred){ renderDeferred=false; queueRender(); }
}
addEventListener("pointerup",endDrag);
addEventListener("pointercancel",endDrag);
UNITS.addEventListener("input",e=>{
  const el=e.target.closest(".rng"); if(el) onSlide(el,false);
});
// change = the value the user settled on (drag released, or keyboard/tap)
UNITS.addEventListener("change",e=>{
  const el=e.target.closest(".rng"); if(el) onSlide(el,true);
});
UNITS.addEventListener("click",e=>{
  const b=e.target.closest("button.tgl"); if(b&&!b.disabled) onToggle(b);
});

start();
setInterval(()=>{ if(!document.hidden) tick(); },POLL_MS);
</script></body></html>)HTML";

// Page length without the terminating NUL (the response is sent with an
// explicit length so the page may contain any byte).
static const size_t DASHBOARD_HTML_LEN = sizeof(DASHBOARD_HTML) - 1;

#endif // DASHBOARD_H
