<?php
declare(strict_types=1);

if (!defined('APP_BOOTSTRAPPED') || !function_exists('authenticated')) {
  http_response_code(403);
  echo "Direct access not allowed.";
  exit;
}

require_authenticated();
require_permission('canEditConfig');

$apiKeyId = (string)(auth_info()['id'] ?? false);

$cfg = cfg();

function clean_key_for_ui(array $k): array {
  // do not expose full hashes
  $hash = (string)($k['hash'] ?? '');
  $k['hashPreview'] = $hash === '' ? '' : (substr($hash, 0, 12) . '…');
  unset($k['hash']);
  return $k;
}

function parse_sensor_list($v): array {
  if (is_array($v)) {
    $out = [];
    foreach ($v as $s) {
      if (!is_string($s)) continue;
      $s = trim($s);
      if ($s !== '' && preg_match('/^[A-Za-z0-9_-]{1,32}$/', $s)) $out[] = $s;
    }
    return array_values(array_unique($out));
  }
  if (is_string($v)) {
    $parts = preg_split('/[,\s;]+/', $v, -1, PREG_SPLIT_NO_EMPTY);
    return parse_sensor_list($parts ?: []);
  }
  return [];
}

$ajax = $_GET['ajax'] ?? '';

if ($ajax === 'list') {
  $out = $cfg;
  $out['apikeys'] = array_map('clean_key_for_ui', $cfg['apikeys'] ?? []);
  $logs = prune_log_entries(read_log_entries());
  // find newest verbose (push json errors etc.)
  $latestVerbose = '';
  for ($i = count($logs) - 1; $i >= 0; $i--) {
    if (!empty($logs[$i]['verbose']) && is_string($logs[$i]['verbose'])) { $latestVerbose = $logs[$i]['verbose']; break; }
  }
  json_response(200, ['ok' => true, 'config' => $out, 'log' => $logs, 'latestVerbose' => $latestVerbose]);
}

if ($ajax === 'hash') {
  $plain = (string)($_POST['plain'] ?? '');
  if ($plain === '') json_response(400, ['ok' => false, 'error' => 'missing_plain']);
  $hash = password_hash($plain, PASSWORD_DEFAULT);
  json_response(200, ['ok' => true, 'hash' => $hash]);
}

if ($ajax === 'save') {
  $raw = file_get_contents('php://input');
  $data = json_decode($raw ?: '', true);
  if (!is_array($data)) json_response(400, ['ok' => false, 'error' => 'invalid_json']);

  $mode = (string)($data['mode'] ?? '');
  $id = (string)($data['id'] ?? '');

  $new = [
    'id' => $id !== '' ? $id : bin2hex(random_bytes(8)),
    'allowedSensorIDs' => parse_sensor_list($data['allowedSensorIDs'] ?? []),
    'canPushData' => !empty($data['canPushData']),
    'canViewData' => !empty($data['canViewData']),
    'canEditConfig' => !empty($data['canEditConfig']),
    'maxUpdateRate' => (int)($data['maxUpdateRate'] ?? 0),
    'csvDir' => (string)($data['csvDir'] ?? ($cfg['dataDirDefault'] ?? DEFAULT_DATA_DIR)),
  ];

  // hash handling
  $plain = (string)($data['plainKey'] ?? '');
  $hash = (string)($data['hash'] ?? '');

  if ($plain !== '') {
    $new['hash'] = password_hash($plain, PASSWORD_DEFAULT);
  } else if ($hash !== '') {
    $new['hash'] = $hash;
  } else {
    // keep existing hash if editing
    $existingHash = '';
    foreach (($cfg['apikeys'] ?? []) as $k) {
      if (($k['id'] ?? '') === $new['id']) { $existingHash = (string)($k['hash'] ?? ''); break; }
    }
    if ($existingHash === '') json_response(400, ['ok' => false, 'error' => 'missing_hash_or_plainKey']);
    $new['hash'] = $existingHash;
  }

  $cfg2 = $cfg;
  $keys = $cfg2['apikeys'] ?? [];
  if (!is_array($keys)) $keys = [];

  if ($mode === 'delete') {
    $keys = array_values(array_filter($keys, fn($k) => is_array($k) && (string)($k['id'] ?? '') !== $new['id']));
  } else {
    $found = false;
    foreach ($keys as $i => $k) {
      if (!is_array($k)) continue;
      if ((string)($k['id'] ?? '') === $new['id']) {
        $keys[$i] = $new;
        $found = true;
        break;
      }
    }
    if (!$found) $keys[] = $new;
  }

  $cfg2['apikeys'] = array_values($keys);

  try {
    save_config($cfg2);
  } catch (Throwable $e) {
    add_log('error-config-write', $apiKeyId, 'save_failed: ' . $e->getMessage());
    json_response(500, ['ok' => false, 'error' => 'save_failed', 'detail' => $e->getMessage()]);
  }

  if ($mode === 'delete') {
    add_log('success-config-write', $apiKeyId, 'delete id=' . $new['id']);
    json_response(200, ['ok' => true, 'id' => $new['id'], 'deleted' => true]);
  }

  add_log('success-config-write', $apiKeyId, 'upsert id=' . $new['id']);
  json_response(200, ['ok' => true, 'id' => $new['id'], 'plainKeyEcho' => ($plain !== '' ? $plain : false)]);
}

// log page view (not the AJAX calls)
add_log('success-config-view', $apiKeyId, 'config_page');

?><!doctype html>
<html lang="de">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>ESP Logger (Config)</title>
  <style>
    body{font-family:system-ui,Segoe UI,Roboto,Helvetica,Arial,sans-serif;margin:0;background:#f6f7fb;color:#111;}
    header{background:#111;color:#fff;padding:14px 18px;}
    main{padding:18px;max-width:1200px;margin:0 auto;}
    .card{background:#fff;border-radius:10px;box-shadow:0 4px 14px rgba(0,0,0,.08);padding:14px;margin:12px 0;}
    table{width:100%;border-collapse:collapse;}
    th,td{padding:8px;border-bottom:1px solid #e7e7ef;text-align:left;vertical-align:top;}
    th{font-size:.9rem;color:#444;}
    input[type=text], input[type=number]{width:100%;box-sizing:border-box;padding:7px;border:1px solid #d7d7e5;border-radius:8px;}
    textarea{width:100%;box-sizing:border-box;padding:8px;border:1px solid #d7d7e5;border-radius:8px;min-height:140px;font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;}
    input.example-url{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;}
    input.example-url.hot{background:#ffd1d1;border-color:#ff8a8a;}
    .row{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
    .btn{background:#111;color:#fff;border:0;border-radius:10px;padding:10px 12px;cursor:pointer;}
    .btn.secondary{background:#e8e8f2;color:#111;}
    .btn.danger{background:#b42318;}
    small.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;}
    .muted{color:#666;}
    .stack{display:flex;gap:8px;flex-wrap:wrap;align-items:center;}
    .tag{display:inline-block;background:#f0f0f7;border:1px solid #e0e0ee;border-radius:999px;padding:2px 8px;font-size:.8rem;}
    .grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
    .notice{padding:10px;border-radius:10px;background:#fff7d6;border:1px solid #f0d98c;}
    .logtable td, .logtable th{text-align:left;}
  </style>
</head>
<body>
<header>
  <div style="display:flex;justify-content:space-between;gap:12px;align-items:center;">
    <div><strong>ESP Logger</strong> <span class="muted">(Config Editor)</span></div>
    <div class="stack">
      <a class="btn secondary" href="index.php?a=view">View</a>
    </div>
  </div>
</header>

<main>
  <div class="notice">
    Dieses UI speichert nur Hashes. Du kannst neue API Keys als Klartext eingeben (sie werden serverseitig gehasht).
  </div>

  <div class="card">
    <div class="stack" style="justify-content:space-between;">
      <h2 style="margin:0;">API Keys</h2>
      <button class="btn" id="btnAdd">Neuen Key hinzufügen</button>
    </div>
    <p class="muted" style="margin-top:6px;">Hinweis: Für den ESP ist der Klartext-Key in der URL nötig. Der Server speichert nur den Hash.</p>
    <div id="tblWrap"></div>
  </div>

  <div class="card">
    <div class="stack" style="justify-content:space-between;">
      <h2 style="margin:0;">Server Log</h2>
      <button class="btn secondary" id="btnRefreshLog">Refresh</button>
    </div>
    <p class="muted" style="margin-top:6px;">Limit: 200 Einträge / 30 Tage. "verbose" wird nur bei JSON-Fehlern im Push gespeichert.</p>

    <h3 style="margin:12px 0 6px 0;">Letzter "verbose" Input</h3>
    <textarea id="verboseBox" readonly></textarea>

    <h3 style="margin:12px 0 6px 0;">Letzte Einträge</h3>
    <div id="logWrap" style="max-height:360px;overflow:auto;border:1px solid #eee;border-radius:10px;"></div>
  </div>
</main>

<script>
const state = { config: null, log: [], latestVerbose: '', plainCache: {}, hotCache: {} };

async function apiList(){
  const r = await fetch('index.php?a=config&ajax=list', {credentials:'same-origin'});
  const j = await r.json();
  if(!j.ok) throw new Error(j.error||'list_failed');
  state.config = j.config;
  state.log = j.log || [];
  state.latestVerbose = j.latestVerbose || '';
  render();
}

function esc(s){ return String(s).replace(/[&<>"']/g, c=>({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;' }[c])); }

function rowKeyForm(k){
  const allowed = (k.allowedSensorIDs||[]).join(', ');
  const firstSensor = (k.allowedSensorIDs && k.allowedSensorIDs.length ? k.allowedSensorIDs[0] : '');
  return `
    <tr>
      <td><small class="mono">${esc(k.id||'')}</small><div class="muted">${esc(k.hashPreview||'')}</div></td>
      <td><input type="text" data-f="allowedSensorIDs" value="${esc(allowed)}" placeholder="A1, A2, ..." /></td>
      <td>
        <div class="stack">
          <label class="tag"><input type="checkbox" data-f="canPushData" ${k.canPushData?'checked':''}/> push</label>
          <label class="tag"><input type="checkbox" data-f="canViewData" ${k.canViewData?'checked':''}/> view</label>
          <label class="tag"><input type="checkbox" data-f="canEditConfig" ${k.canEditConfig?'checked':''}/> config</label>
        </div>
      </td>
      <td><input type="number" min="0" step="1" data-f="maxUpdateRate" value="${esc(k.maxUpdateRate??0)}" /></td>
      <td><input type="text" data-f="csvDir" value="${esc(k.csvDir||state.config.dataDirDefault||'')}" /></td>
      <td><input type="text" class="example-url" readonly data-keyid="${esc(k.id||'')}" data-firstsensor="${esc(firstSensor)}" /></td>
      <td style="min-width:240px">
        <div class="stack">
          <input type="text" data-f="plainKey" placeholder="Neuer Klartext-Key (optional)" />
          <button class="btn" data-act="save">Speichern</button>
          <button class="btn danger" data-act="del">Löschen</button>
        </div>
        <div class="muted" style="margin-top:6px">Wenn "Neuer Klartext-Key" leer ist, bleibt der bestehende Hash.</div>
      </td>
    </tr>
  `;
}

function render(){
  const keys = (state.config?.apikeys)||[];
  const html = `
    <table>
      <thead>
        <tr>
          <th>ID</th>
          <th>allowedSensorIDs</th>
          <th>Rechte</th>
          <th>maxUpdateRate (s)</th>
          <th>csvDir</th>
          <th>Beispiel-URL</th>
          <th>Aktionen</th>
        </tr>
      </thead>
      <tbody>
        ${keys.map(rowKeyForm).join('')}
      </tbody>
    </table>
  `;
  document.getElementById('tblWrap').innerHTML = html;

  refreshExampleUrls();
  renderLog();

  document.querySelectorAll('button[data-act="save"]').forEach(btn=>{
    btn.addEventListener('click', async (e)=>{
      const tr = e.target.closest('tr');
      const payload = collectRow(tr);
      const res = await apiSave(payload);
      if(payload.plainKey){
        state.plainCache[res.id] = payload.plainKey;
        state.hotCache[res.id] = true;
        const inp = tr.querySelector('[data-f="plainKey"]');
        if(inp) inp.value = '';
      }
      await apiList();
    });
  });
  document.querySelectorAll('button[data-act="del"]').forEach(btn=>{
    btn.addEventListener('click', async (e)=>{
      const tr = e.target.closest('tr');
      const payload = collectRow(tr);
      payload.mode = 'delete';
      if(!confirm('Diesen API-Key wirklich löschen?')) return;
      const res = await apiSave(payload);
      if(res?.id){ delete state.plainCache[res.id]; delete state.hotCache[res.id]; }
      await apiList();
    });
  });

  // update example URL immediately when allowedSensorIDs is edited (first ID wins)
  document.querySelectorAll('input[data-f="allowedSensorIDs"]').forEach(inp=>{
    inp.addEventListener('input', (e)=>{
      const tr = e.target.closest('tr');
      const ex = tr?.querySelector('input.example-url');
      if(!ex) return;
      const v = String(e.target.value||'');
      const first = (v.split(/[^A-Za-z0-9_-]+/).filter(Boolean)[0]) || '';
      ex.dataset.firstsensor = first;
      refreshExampleUrls();
    });
  });
}

function baseEndpointUrl(){
  // current page is index.php (router), so this is the correct endpoint
  return window.location.origin + window.location.pathname;
}

function refreshExampleUrls(){
  const base = baseEndpointUrl();
  document.querySelectorAll('input.example-url').forEach(inp=>{
    const id = inp.dataset.keyid || '';
    const sensor = inp.dataset.firstsensor || 'SENSORID';
    const key = state.plainCache[id] || 'YOUR_API_KEY';
    const url = `${base}?a=push&sensorID=${encodeURIComponent(sensor||'SENSORID')}&apikey=${encodeURIComponent(key)}`;
    inp.value = url;
    if(state.hotCache[id]) inp.classList.add('hot'); else inp.classList.remove('hot');
  });
}

function renderLog(){
  const box = document.getElementById('verboseBox');
  if(box) box.value = state.latestVerbose || '';

  const logs = Array.isArray(state.log) ? state.log.slice().reverse().slice(0, 80) : [];
  const rows = logs.map(e=>{
    const dt = esc(e.date||'');
    const ty = esc(e.type||'');
    const kid = (e.apiKeyId===false ? 'false' : esc(e.apiKeyId||''));
    const det = esc(e.details||'');
    return `<tr><td><small class="mono">${dt}</small></td><td>${ty}</td><td><small class="mono">${kid}</small></td><td>${det}</td></tr>`;
  }).join('');
  const html = `
    <table class="logtable" style="width:100%;border-collapse:collapse;">
      <thead>
        <tr>
          <th class="left">date</th>
          <th class="left">type</th>
          <th class="left">apiKeyId</th>
          <th class="left">details</th>
        </tr>
      </thead>
      <tbody>${rows || '<tr><td colspan="4" class="muted">(keine Einträge)</td></tr>'}</tbody>
    </table>
  `;
  const wrap = document.getElementById('logWrap');
  if(wrap) wrap.innerHTML = html;
}

function collectRow(tr){
  const id = tr.querySelector('small.mono')?.textContent?.trim() || '';
  const getVal = (f)=> tr.querySelector(`[data-f="${f}"]`);
  return {
    id,
    allowedSensorIDs: getVal('allowedSensorIDs')?.value || '',
    canPushData: !!getVal('canPushData')?.checked,
    canViewData: !!getVal('canViewData')?.checked,
    canEditConfig: !!getVal('canEditConfig')?.checked,
    maxUpdateRate: parseInt(getVal('maxUpdateRate')?.value||'0',10),
    csvDir: getVal('csvDir')?.value || '',
    plainKey: getVal('plainKey')?.value || ''
  };
}

async function apiSave(payload){
  const r = await fetch('index.php?a=config&ajax=save', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify(payload),
    credentials:'same-origin'
  });
  const j = await r.json();
  if(!j.ok) throw new Error(j.error||'save_failed');
  return j;
}

document.getElementById('btnAdd').addEventListener('click', async ()=>{
  const plain = prompt('Klartext-API-Key (wird gehasht gespeichert):');
  if(!plain) return;
  const sensors = prompt('allowedSensorIDs (z.B. A1,A2):', 'A1');
  const csvDir = prompt('csvDir (leer für Default):', state.config?.dataDirDefault||'');
  const payload = {
    id: '',
    plainKey: plain,
    allowedSensorIDs: sensors||'',
    canPushData: true,
    canViewData: false,
    canEditConfig: false,
    maxUpdateRate: 290,
    csvDir: csvDir||''
  };
  const res = await apiSave(payload);
  if(res?.id){
    state.plainCache[res.id] = plain;
    state.hotCache[res.id] = true;
  }
  await apiList();
});

document.getElementById('btnRefreshLog').addEventListener('click', ()=>apiList());

apiList().catch(err=>{
  document.getElementById('tblWrap').innerHTML = '<div class="muted">Fehler: '+esc(err.message)+'</div>';
});
</script>
</body>
</html>
