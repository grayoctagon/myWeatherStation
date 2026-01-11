#pragma once
#include <Arduino.h>
#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"HTML_INDEX(
<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>BME280 Monitor</title>
  <style>
    :root { --bg1:#f7f7f7; --bg2:#e3e3e3; --card:#ffffffcc; --border:#c9c9c9; --text:#000; }
    body{ margin:0; font-family:system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif; color:var(--text); background:linear-gradient(180deg,var(--bg1),var(--bg2)); }
    header{ display:flex; align-items:center; justify-content:space-between; padding:14px 16px; border-bottom:1px solid var(--border); }
    header h1{ margin:0; font-size:18px; letter-spacing:0.2px; }
    a.icon{ text-decoration:none; font-size:22px; line-height:1; padding:6px 10px; border-radius:10px; border:1px solid var(--border); background:#fff; color:#000; }
    main{ padding:12px 12px 20px; max-width:1100px; margin:0 auto; }
    .grid{ display:grid; grid-template-columns:1fr; gap:12px; }
    .card{ background:var(--card); border:1px solid var(--border); border-radius:14px; padding:12px; backdrop-filter:blur(6px); }
    .stats{ display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:10px; margin-top:10px; }
    .stat{ background:#fff; border:1px solid var(--border); border-radius:12px; padding:10px; }
    .stat .k{ font-size:12px; opacity:0.8; }
    .stat .v{ font-size:22px; font-weight:650; margin-top:2px; }
    .sub{ margin-top:6px; font-size:12px; opacity:0.85; display:flex; gap:10px; flex-wrap:wrap; }
    canvas{ width:100% !important; height:54vh !important; }
    @media (min-width:900px){ .grid{ grid-template-columns:2fr 1fr; } canvas{ height:62vh !important; } }
  </style>
</head>
<body>
  <header>
    <h1>BME280 Monitor</h1>
    <a class="icon" href="/settings" title="Einstellungen">⚙️</a>
  </header>
  <main>
    <div class="grid">
      <div class="card"><canvas id="chart"></canvas></div>
      <div class="card">
        <div style="font-weight:650;margin-bottom:8px;">Aktuell</div>
        <div class="stats">
          <div class="stat"><div class="k">Temp</div><div class="v" id="t">-</div></div>
          <div class="stat"><div class="k">Hum</div><div class="v" id="h">-</div></div>
          <div class="stat"><div class="k">Pres</div><div class="v" id="p">-</div></div>
        </div>
        <div class="sub">
          <div id="time">Zeit: -</div>
          <div id="wifi">WiFi: -</div>
          <div id="ip">IP: -</div>
        </div>
      </div>
    </div>
  </main>

  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
  <script>
    let chart;
    function fmt(v,d=2){ if(v===null||v===undefined||Number.isNaN(v)) return "-"; return Number(v).toFixed(d); }
    async function getJSON(url){ const r=await fetch(url,{cache:"no-store"}); if(!r.ok) throw new Error(await r.text()); return await r.json(); }

    function buildChart(labels,temp,hum,pres){
      const ctx=document.getElementById("chart");
      chart=new Chart(ctx,{
        type:"line",
        data:{ labels,
          datasets:[
            { label:"Temp (C)", data:temp, yAxisID:"yT", borderColor:"red", tension:0.25, pointRadius:0 },
            { label:"Hum (%)", data:hum, yAxisID:"yH", borderColor:"blue", tension:0.25, pointRadius:0 },
            { label:"Pres (hPa)", data:pres, yAxisID:"yP", borderColor:"#b0b0b0", tension:0.25, pointRadius:0 },
          ]},
        options:{
          responsive:true, maintainAspectRatio:false,
          interaction:{ mode:"index", intersect:false },
          plugins:{ legend:{display:true}, tooltip:{enabled:true} },
          scales:{
            x:{ ticks:{ maxRotation:0, autoSkip:true, maxTicksLimit:12 } },
            yT:{ position:"left",  min:-10, max:40,  grid:{ drawOnChartArea:true } },
            yH:{ position:"right", min:0,   max:100, grid:{ drawOnChartArea:false } },
            yP:{ position:"right", min:980, max:1040, grid:{ drawOnChartArea:false } }
          }
        }
      });
    }

    async function refreshHistory(){
      const hist=await getJSON("/api/history?n=600");
      if(!chart) buildChart(hist.labels,hist.temp,hist.hum,hist.pres);
      else{
        chart.data.labels=hist.labels;
        chart.data.datasets[0].data=hist.temp;
        chart.data.datasets[1].data=hist.hum;
        chart.data.datasets[2].data=hist.pres;
        chart.update("none");
      }
    }

    async function refreshState(){
      const st=await getJSON("/api/state");
      document.getElementById("t").textContent=fmt(st.temp,2)+" C";
      document.getElementById("h").textContent=fmt(st.hum,2)+" %";
      document.getElementById("p").textContent=fmt(st.pres,2)+" hPa";
      document.getElementById("time").textContent="Zeit: "+(st.time_str||"-");
      document.getElementById("wifi").textContent="WiFi: "+(st.ssid||"-");
      document.getElementById("ip").textContent="IP: "+(st.ip||"-");
    }

    async function loop(){
      try{ await refreshState(); }catch(e){}
      try{ await refreshHistory(); }catch(e){}
      setTimeout(loop,2500);
    }
    loop();
  </script>
</body>
</html>
)HTML_INDEX";

static const char SETTINGS_HTML[] PROGMEM = R"HTML_SETTINGS(
<!doctype html>
<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>Einstellungen</title>
  <style>
    :root { --bg1:#f7f7f7; --bg2:#e3e3e3; --card:#ffffffcc; --border:#c9c9c9; --text:#000; --btn:#fff; }
    body{ margin:0; font-family:system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif; color:var(--text); background:linear-gradient(180deg,var(--bg1),var(--bg2)); }
    header{ display:flex; align-items:center; justify-content:space-between; padding:14px 16px; border-bottom:1px solid var(--border); }
    header a{ text-decoration:none; color:#000; }
    main{ padding:12px; max-width:1100px; margin:0 auto; }
    .card{ background:var(--card); border:1px solid var(--border); border-radius:14px; padding:12px; margin-bottom:12px; backdrop-filter:blur(6px); }
    h2{ margin:0 0 10px; font-size:15px; }
    label{ font-size:12px; opacity:0.9; display:block; margin:6px 0 4px; }
    input[type="text"], input[type="number"]{ width:100%; padding:10px; border-radius:10px; border:1px solid var(--border); background:#fff; box-sizing:border-box; }
    table{ width:100%; border-collapse:collapse; }
    td,th{ border-bottom:1px solid var(--border); padding:8px 6px; font-size:13px; }
    th{ text-align:left; font-size:12px; opacity:0.8; }
    button{ padding:8px 10px; border-radius:10px; border:1px solid var(--border); background:var(--btn); cursor:pointer; }
    .row{ display:grid; grid-template-columns:1fr 1fr; gap:10px; }
    .chips button{ margin-right:6px; }
    .muted{ opacity:0.75; font-size:12px; }
    .actions{ display:flex; gap:10px; flex-wrap:wrap; }
  </style>
</head>
<body>
  <header>
    <div style="font-weight:650;">Einstellungen</div>
    <a href="/" title="Zurück">Zurück</a>
  </header>

  <main>
    <div class="card">
      <h2>Sampling und Screen</h2>
      <div class="row">
        <div><label>SAMPLE_MS (ms)</label><input id="sampleMs" type="number" min="100" step="50" /></div>
        <div><label>SCREEN_MS (ms)</label><input id="screenMs" type="number" min="500" step="100" /></div>
      </div>
      <label><input id="autoswitch" type="checkbox" /> autoswitchScreens</label>

      <div style="margin-top:10px;">
        <div class="muted">screenIdx (Startscreen)</div>
        <div class="chips" style="margin-top:6px;">
          <button onclick="setScreen(0)">Temp</button>
          <button onclick="setScreen(1)">Hum</button>
          <button onclick="setScreen(2)">Pres</button>
          <span id="screenLbl" class="muted"></span>
        </div>
      </div>
    </div>

    <div class="card">
      <h2>WiFi Netzwerke (Reihenfolge = Priorität)</h2>
      <div class="muted">Beim Boot wird nacheinander probiert, was verfügbar ist.</div>
      <table id="wifiTbl">
        <thead><tr><th>#</th><th>SSID</th><th>Passwort</th><th>Sort</th><th>Del</th></tr></thead>
        <tbody></tbody>
      </table>
      <div style="margin-top:10px;" class="actions"><button onclick="addWifi()">+ hinzufügen</button></div>
    </div>

    <div class="card">
      <h2>Logging</h2>
      <div class="row">
        <div><label>Short Interval (ms)</label><input id="logShortMs" type="number" min="250" step="250" /></div>
        <div><label>Long Interval (ms)</label><input id="logLongMs" type="number" min="1000" step="1000" /></div>
      </div>

      <label>POST URL (optional, http oder https)</label>
      <input id="postUrl" type="text" placeholder="https://example.com/endpoint" />
      <label><input id="insecureTls" type="checkbox" /> HTTPS ohne Zertifikatsprüfung (insecure)</label>
      <div class="muted">Wenn dein Netz kein Internet hat, lädt Chart.js auf der Hauptseite nicht. Dann sag Bescheid, dann betten wir es ein.</div>
    </div>

    <div class="card">
      <h2>Buttons</h2>
      <div id="btnCfg"></div>
      <div class="muted">Entweder URL (GET) oder switch screenIdx.</div>
    </div>

    <div class="card">
      <div class="actions">
        <button onclick="save()">Speichern</button>
        <button onclick="reconnect()">WiFi neu verbinden</button>
      </div>
      <div id="msg" class="muted" style="margin-top:8px;"></div>
    </div>
  </main>

  <script>
    let cfg=null;
    function $(id){ return document.getElementById(id); }
    function setMsg(s){ $("msg").textContent=s; }
    function setScreen(i){ cfg.screenIdx=i; $("screenLbl").textContent=" aktuell: "+["Temp","Hum","Pres"][i]; }

    function wifiRowHtml(i,w){
      return `
        <tr>
          <td>${i+1}</td>
          <td><input type="text" value="${(w.ssid||"").replaceAll('"','&quot;')}" onchange="cfg.wifi[${i}].ssid=this.value" /></td>
          <td><input type="text" value="${(w.pass||"").replaceAll('"','&quot;')}" onchange="cfg.wifi[${i}].pass=this.value" /></td>
          <td>
            <button onclick="moveWifi(${i},-1)">⬆️</button>
            <button onclick="moveWifi(${i}, 1)">⬇️</button>
          </td>
          <td><button onclick="delWifi(${i})">🗑️</button></td>
        </tr>`;
    }
    function renderWifi(){
      const tb=$("wifiTbl").querySelector("tbody");
      tb.innerHTML=cfg.wifi.map((w,i)=>wifiRowHtml(i,w)).join("");
    }
    function addWifi(){ cfg.wifi.push({ssid:"",pass:""}); renderWifi(); }
    function delWifi(i){ cfg.wifi.splice(i,1); renderWifi(); }
    function moveWifi(i,dir){
      const j=i+dir; if(j<0||j>=cfg.wifi.length) return;
      const tmp=cfg.wifi[i]; cfg.wifi[i]=cfg.wifi[j]; cfg.wifi[j]=tmp; renderWifi();
    }

    function renderButtons(){
      const wrap=$("btnCfg"); wrap.innerHTML="";
      cfg.buttons.forEach((b,i)=>{
        const div=document.createElement("div");
        div.style.border="1px solid #c9c9c9";
        div.style.borderRadius="12px";
        div.style.padding="10px";
        div.style.marginBottom="10px";
        div.style.background="#fff";
        div.innerHTML=`
          <div style="font-weight:650;margin-bottom:6px;">Button ${i} (Pin ${b.pin})</div>
          <label><input type="checkbox" ${b.switchScreen?"checked":""} onchange="cfg.buttons[${i}].switchScreen=this.checked"> switch screenIdx</label>
          <label>URL (optional)</label>
          <input type="text" value="${(b.url||"").replaceAll('"','&quot;')}" onchange="cfg.buttons[${i}].url=this.value" placeholder="http(s)://..." />
        `;
        wrap.appendChild(div);
      });
    }

    async function getJSON(url){
      const r=await fetch(url,{cache:"no-store"});
      if(!r.ok) throw new Error(await r.text());
      return await r.json();
    }
    async function postJSON(url,obj){
      const r=await fetch(url,{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(obj)});
      if(!r.ok) throw new Error(await r.text());
      return await r.json();
    }

    async function load(){
      cfg=await getJSON("/api/config");
      $("sampleMs").value=cfg.sampleMs;
      $("screenMs").value=cfg.screenMs;
      $("autoswitch").checked=cfg.autoswitchScreens;
      setScreen(cfg.screenIdx);

      $("logShortMs").value=cfg.log.shortMs;
      $("logLongMs").value=cfg.log.longMs;
      $("postUrl").value=cfg.log.postUrl||"";
      $("insecureTls").checked=!!cfg.log.insecureTls;

      renderWifi();
      renderButtons();
    }

    async function save(){
      cfg.sampleMs=Number($("sampleMs").value||300);
      cfg.screenMs=Number($("screenMs").value||4000);
      cfg.autoswitchScreens=$("autoswitch").checked;

      cfg.log.shortMs=Number($("logShortMs").value||5000);
      cfg.log.longMs=Number($("logLongMs").value||300000);
      cfg.log.postUrl=$("postUrl").value||"";
      cfg.log.insecureTls=$("insecureTls").checked;

      try{ await postJSON("/api/config",cfg); setMsg("Gespeichert. Runtime ist aktualisiert."); }
      catch(e){ setMsg("Fehler: "+e); }
    }

    async function reconnect(){
      setMsg("Verbinde...");
      try{
        const res=await postJSON("/api/wifi/reconnect",{});
        setMsg(res.ok ? ("OK: "+(res.ip||"-")) : "FAIL");
      }catch(e){ setMsg("Fehler: "+e); }
    }

    load();
  </script>
</body>
</html>
)HTML_SETTINGS";


