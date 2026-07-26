/**
 * Status Dashboard - the browser page served at GET /
 *
 * A single self-contained page (HTML + CSS + JS, no external resources — the
 * gateway has no internet access and must work on a plain LAN): it shows the
 * two link states of the gateway (Bluetooth side = Casambi mesh, API side =
 * Wi-Fi/REST/WebSocket) plus every unit with its generic, cloud-derived
 * control names and current values.
 *
 * The page itself is static and served unauthenticated; ALL data it displays
 * comes from the authenticated endpoints (GET /api/status, GET /api/units and
 * the /ws push channel), so it exposes nothing that a client without the API
 * token could not already see. When a Casambi password is stored, the page
 * asks for it once and derives the API token the same way the FHEM module
 * does — SHA-256("casambi-api:" + password), computed in the browser (a small
 * JS implementation, because window.crypto.subtle is unavailable over plain
 * http://) and kept in localStorage. The password never goes on the wire.
 *
 * The page consumes the /api/* + /ws interface exactly as documented; it adds
 * no endpoint, message type or field of its own, so it is NOT covered by the
 * VERSIONING CONTRACT at FHEM_API_VERSION_MAJOR in config.h. It does have to
 * FOLLOW that interface though: when a field it reads is renamed or removed,
 * update the JavaScript below in the same commit.
 *
 * Served with the (const uint8_t*, len) response overload, which streams
 * straight from flash (AsyncProgmemResponse) — the plain const char* overload
 * would copy the whole page into a String and need a ~23 kB contiguous heap
 * block on every request.
 *
 * To work on the page: copy the raw string into a .html file and open it
 * against a gateway (or a mock serving /api/status, /api/units and /ws).
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
/* ---------- auth / banner ---------- */
#auth{max-width:420px;margin:8vh auto 0}
#auth p{margin:0 0 12px;color:var(--mut);font-size:.9rem}
label{display:block;font-size:.85rem;color:var(--mut);margin-bottom:4px}
input{width:100%;padding:10px 12px;font-size:1rem;color:var(--fg);background:var(--bg);
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
    :(bt?"":"Values are the last known state — the Bluetooth link is down.");
  $("units").innerHTML=list.length?list.map(unitCard).join("")
    :'<div class="card empty">No devices in the gateway configuration.</div>';

  const banner=$("banner");
  if(!httpOk){
    banner.textContent="The gateway is not answering. Retrying every "+(POLL_MS/1000)+" s…";
    banner.classList.remove("hide");
  }else banner.classList.add("hide");

  $("stamp").textContent="Updated "+new Date().toLocaleTimeString();
}

// One card per Casambi unit: name, reachability, and every fixture control
// with its generic (cloud-derived) name and current value.
function unitCard(u){
  const on=!!u.on||u.level>0;
  const pill=!u.online?'<span class="pill bad">offline</span>'
            :on?'<span class="pill ok">on</span>':'<span class="pill">off</span>';
  let body=controlRows(u);
  if(!body) body='<div class="empty">No control data yet.</div>';
  return '<article class="card unit'+(u.online?"":" off")+'">'+
    '<div class="uhead"><h3>'+esc(u.name||("Unit "+u.id))+
    '<span class="id">ID '+esc(u.id)+(u.address?" · "+esc(u.address):"")+'</span></h3>'+
    pill+'</div><div class="ctrls">'+body+'</div></article>';
}

// Prefer the generic `controls` array (name + raw value straight from the
// cloud fixture); fall back to the legacy level/vertical/colorTemp fields for
// units whose fixture definition is unavailable.
function controlRows(u){
  const out=[];
  if(Array.isArray(u.controls)&&u.controls.length){
    u.controls.forEach(c=>{
      const name=c.name||c.type||"control";
      const v=c.value!=null?c.value:0;
      if(c.type==="temperature"&&c.kelvin!=null){
        out.push([name,esc(c.kelvin)+" K","raw "+esc(v)+
          (c.min!=null?" · "+esc(c.min)+"–"+esc(c.max)+" K":"")]);
      }else if(c.type==="dimmer"){
        out.push([name,esc(pct(v))+" %","raw "+esc(v),v]);
      }else if(SCALED.has(c.type)){
        out.push([name,esc(v),esc(pct(v))+" %",v]);
      }else{
        out.push([name,esc(v)]);
      }
    });
  }else{
    out.push(["level",esc(pct(u.level||0))+" %","raw "+esc(u.level||0),u.level||0]);
    if(u.vertical!=null) out.push(["vertical",esc(u.vertical),esc(pct(u.vertical))+" %",u.vertical]);
    if(u.colorTemp!=null){
      const k=(u.cctMin!=null&&u.cctMax>u.cctMin)
        ?Math.round(u.cctMin+u.colorTemp*(u.cctMax-u.cctMin)/255):null;
      out.push(["temperature",k!=null?esc(k)+" K":esc(u.colorTemp),"raw "+esc(u.colorTemp)]);
    }
  }
  return out.map(r=>
    '<div class="row"><span class="k">'+esc(r[0])+'</span><span class="v">'+r[1]+
    (r[2]?'<small>'+r[2]+'</small>':'')+'</span></div>'+
    (r[3]!=null?'<div class="bar"><i style="width:'+pct(r[3])+'%"></i></div>':'')).join("");
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

start();
setInterval(()=>{ if(!document.hidden) tick(); },POLL_MS);
</script></body></html>)HTML";

// Page length without the terminating NUL (the response is sent with an
// explicit length so the page may contain any byte).
static const size_t DASHBOARD_HTML_LEN = sizeof(DASHBOARD_HTML) - 1;

#endif // DASHBOARD_H
