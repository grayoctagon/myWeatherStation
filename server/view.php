<?php
declare(strict_types=1);

if (!defined('APP_BOOTSTRAPPED') || !function_exists('authenticated')) {
  http_response_code(403);
  echo "Direct access not allowed.";
  exit;
}

require_authenticated();
require_permission('canViewData');

$apiKeyId = (string)(auth_info()['id'] ?? false);

$cfg = cfg();
$a = auth_info();

$sensorIDs = $a['allowedSensorIDs'] ?? [];
if (!is_array($sensorIDs)) $sensorIDs = [];
$sensorIDs = array_values(array_filter($sensorIDs, fn($s) => is_string($s) && $s !== ''));

$dataDir = $a['csvDir'] ?? ($cfg['dataDirDefault'] ?? DEFAULT_DATA_DIR);
if (!is_string($dataDir) || $dataDir === '') $dataDir = DEFAULT_DATA_DIR;

$dsMax = (int)($cfg['ds18Max'] ?? DS18_MAX_DEFAULT);
if ($dsMax <= 0 || $dsMax > 64) $dsMax = DS18_MAX_DEFAULT;

function list_months_for_sensor(string $dataDir, string $sensorID): array {
  $months = [];
  if (!is_dir($dataDir)) return [];
  $pat = '/^(\d{4}-\d{2})_' . preg_quote($sensorID, '/') . '\.csv$/';
  foreach (scandir($dataDir) ?: [] as $f) {
    if (!is_string($f)) continue;
    if (preg_match($pat, $f, $m)) $months[$m[1]] = true;
  }
  $out = array_keys($months);
  sort($out);
  return $out;
}

function read_csv_series(string $path, int $dsMax): array {
  if (!is_file($path)) return ['ok' => false, 'error' => 'not_found'];
  $fh = fopen($path, 'rb');
  if ($fh === false) return ['ok' => false, 'error' => 'cannot_open'];

  $header = fgetcsv($fh, 0, ';');
  if (!is_array($header)) { fclose($fh); return ['ok' => false, 'error' => 'bad_header']; }
  $idx = [];
  foreach ($header as $i => $h) $idx[(string)$h] = (int)$i;

  $metrics = [
    'temp' => ['avg' => 'temp_avg', 'min' => 'temp_min', 'max' => 'temp_max', 'unit' => '°C'],
    'hum'  => ['avg' => 'hum_avg',  'min' => 'hum_min',  'max' => 'hum_max',  'unit' => '%'],
    'pres' => ['avg' => 'pres_avg', 'min' => 'pres_min', 'max' => 'pres_max', 'unit' => 'hPa'],
  ];

  for ($i = 1; $i <= $dsMax; $i++) {
    $metrics["ds{$i}"] = ['avg' => "ds{$i}_avg", 'min' => "ds{$i}_min", 'max' => "ds{$i}_max", 'sn' => "ds{$i}_sn", 'unit' => '°C'];
  }

  $out = [];
  foreach ($metrics as $k => $_) $out[$k] = ['t' => [], 'avg' => [], 'min' => [], 'max' => [], 'sn' => ''];

  while (($row = fgetcsv($fh, 0, ';')) !== false) {
    if (!is_array($row) || count($row) < 3) continue;

    $ts = $row[$idx['ts'] ?? 0] ?? '';
    if (!is_string($ts) || $ts === '' || !ctype_digit($ts)) continue;
    $t = (int)$ts;

    foreach ($metrics as $k => $m) {
      $avg = $row[$idx[$m['avg']] ?? -1] ?? '';
      $min = $row[$idx[$m['min']] ?? -1] ?? '';
      $max = $row[$idx[$m['max']] ?? -1] ?? '';

      if ($avg === '' && $min === '' && $max === '') continue;

      $out[$k]['t'][] = $t;
      $out[$k]['avg'][] = ($avg === '' ? null : (float)str_replace(',', '.', $avg));
      $out[$k]['min'][] = ($min === '' ? null : (float)str_replace(',', '.', $min));
      $out[$k]['max'][] = ($max === '' ? null : (float)str_replace(',', '.', $max));

      if (!empty($m['sn']) && $out[$k]['sn'] === '') {
        $sn = $row[$idx[$m['sn']] ?? -1] ?? '';
        if (is_string($sn) && $sn !== '') $out[$k]['sn'] = $sn;
      }
    }
  }
  fclose($fh);

  // prune empty ds metrics
  foreach (array_keys($out) as $k) {
    if ($k === 'temp' || $k === 'hum' || $k === 'pres') continue;
    if (empty($out[$k]['t'])) unset($out[$k]);
  }

  return ['ok' => true, 'series' => $out];
}

$ajax = $_GET['ajax'] ?? '';
if ($ajax === 'load') {
  $sensorID = sanitize_sensor_id((string)($_GET['sensorID'] ?? ''));
  $month = (string)($_GET['month'] ?? '');
  if ($sensorID === null) json_response(400, ['ok' => false, 'error' => 'invalid_sensorID']);
  if (!preg_match('/^\d{4}-\d{2}$/', $month)) json_response(400, ['ok' => false, 'error' => 'invalid_month']);
  require_sensor_allowed($sensorID);

  $path = rtrim($dataDir, "/\\") . DIRECTORY_SEPARATOR . $month . '_' . $sensorID . '.csv';
  $r = read_csv_series($path, $dsMax);
  if (!$r['ok']) {
    add_log('error-view-csv-not-found', $apiKeyId, 'csv_not_found sensorID=' . $sensorID . ' month=' . $month);
    json_response(404, ['ok' => false, 'error' => 'csv_not_found']);
  }
  json_response(200, ['ok' => true, 'sensorID' => $sensorID, 'month' => $month, 'dsMax' => $dsMax, 'series' => $r['series']]);
}

// log page view (not the AJAX calls)
add_log('success-view', $apiKeyId, 'view_page');

$monthsSet = [];
foreach ($sensorIDs as $sid) {
  foreach (list_months_for_sensor($dataDir, $sid) as $m) $monthsSet[$m] = true;
}
$months = array_keys($monthsSet);
sort($months);

?><!doctype html>
<html lang="de">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>ESP Logger (View)</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
  <style>
    body{font-family:system-ui,Segoe UI,Roboto,Helvetica,Arial,sans-serif;margin:0;background:#f6f7fb;color:#111;}
    header{background:#111;color:#fff;padding:14px 18px;}
    main{padding:18px;max-width:1400px;margin:0 auto;}
    .layout{display:grid;grid-template-columns:420px 1fr;gap:14px;align-items:start;}
    .card{background:#fff;border-radius:10px;box-shadow:0 4px 14px rgba(0,0,0,.08);padding:14px;}
    table{width:100%;border-collapse:collapse;}
    th,td{padding:6px;border-bottom:1px solid #e7e7ef;text-align:center;}
    th.sticky{position:sticky;top:0;background:#fff;z-index:2;}
    td.left, th.left{text-align:left;}
    .btn{background:#111;color:#fff;border:0;border-radius:10px;padding:10px 12px;cursor:pointer;}
    .btn.secondary{background:#e8e8f2;color:#111;}
    .muted{color:#666;}
    .stack{display:flex;gap:8px;flex-wrap:wrap;align-items:center;}
    .grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
    .metrics{display:grid;grid-template-columns:1fr 1fr;gap:6px;}
    .metrics label{display:flex;gap:6px;align-items:center;background:#f0f0f7;border:1px solid #e0e0ee;border-radius:10px;padding:6px 8px;}
    canvas{width:100%;height:560px;}
  </style>
</head>
<body>
<header>
  <div style="display:flex;justify-content:space-between;gap:12px;align-items:center;">
    <div><strong>ESP Logger</strong> <span class="muted">(View)</span></div>
    <div class="stack">
      <?php if (!empty($a['canEditConfig'])): ?>
        <a class="btn secondary" href="index.php?a=config">Config</a>
      <?php endif; ?>
    </div>
  </div>
</header>

<main>
  <div class="layout">
    <div class="card">
      <h2 style="margin:0 0 8px 0;">CSV-Auswahl</h2>
      <p class="muted" style="margin:0 0 12px 0;">
        Matrix: Spalten sind SensorIDs, Zeilen sind Monate (YYYY-MM).
      </p>

      <div style="max-height:360px;overflow:auto;border:1px solid #eee;border-radius:10px;">
        <table id="matrix">
          <thead>
            <tr>
              <th class="sticky left">Monat</th>
              <?php foreach ($sensorIDs as $sid): ?>
                <th class="sticky"><?php echo htmlspecialchars($sid, ENT_QUOTES); ?></th>
              <?php endforeach; ?>
            </tr>
          </thead>
          <tbody>
            <?php foreach ($months as $m): ?>
              <tr>
                <td class="left"><?php echo htmlspecialchars($m, ENT_QUOTES); ?></td>
                <?php foreach ($sensorIDs as $sid): ?>
                  <?php
                    $file = rtrim($dataDir, "/\\") . DIRECTORY_SEPARATOR . $m . '_' . $sid . '.csv';
                    $exists = is_file($file);
                  ?>
                  <td>
                    <?php if ($exists): ?>
                      <input type="checkbox" data-month="<?php echo htmlspecialchars($m, ENT_QUOTES); ?>" data-sensor="<?php echo htmlspecialchars($sid, ENT_QUOTES); ?>" />
                    <?php else: ?>
                      <span class="muted">-</span>
                    <?php endif; ?>
                  </td>
                <?php endforeach; ?>
              </tr>
            <?php endforeach; ?>
          </tbody>
        </table>
      </div>

      <div style="margin-top:14px;">
        <h3 style="margin:0 0 8px 0;">Metriken</h3>
        <div class="metrics" id="metrics">
          <label><input type="checkbox" data-m="temp" checked /> Temp (°C)</label>
          <label><input type="checkbox" data-m="hum" checked /> Hum (%)</label>
          <label><input type="checkbox" data-m="pres" /> Pres (hPa)</label>
          <?php for ($i=1; $i <= $dsMax; $i++): ?>
            <label><input type="checkbox" data-m="<?php echo "ds{$i}"; ?>" /> <?php echo "DS{$i} (°C)"; ?></label>
          <?php endfor; ?>
        </div>
        <div class="stack" style="margin-top:12px;">
          <button class="btn" id="btnDraw">Graph aktualisieren</button>
          <button class="btn secondary" id="btnClear">Auswahl löschen</button>
        </div>
        <p class="muted" id="status" style="margin-top:10px;"></p>
      </div>
    </div>

    <div class="card">
      <h2 style="margin:0 0 8px 0;">Graph</h2>
      <p class="muted" style="margin:0 0 12px 0;">Linie zeigt Avg, Bereich zeigt Min/Max (20% transparent).</p>
      <canvas id="c"></canvas>
    </div>
  </div>
</main>

<script>
let chart = null;

function setStatus(s){ document.getElementById('status').textContent = s; }

function hashStr(s){
  let h = 2166136261;
  for(let i=0;i<s.length;i++){
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return (h>>>0);
}
function colorFor(label, alpha=1){
  const h = hashStr(label) % 360;
  return `hsla(${h}, 70%, 45%, ${alpha})`;
}

function metricMeta(m){
  if(m==='temp') return {name:'Temp (°C)', axis:'y'};
  if(m==='hum') return {name:'Hum (%)', axis:'y'};
  if(m==='pres') return {name:'Pres (hPa)', axis:'y2'};
  if(m.startsWith('ds')) return {name: m.toUpperCase()+' (°C)', axis:'y'};
  return {name:m, axis:'y'};
}

function selectedMatrix(){
  const sel = [];
  document.querySelectorAll('#matrix input[type=checkbox]:checked').forEach(cb=>{
    sel.push({month: cb.dataset.month, sensorID: cb.dataset.sensor});
  });
  return sel;
}

function selectedMetrics(){
  const ms = [];
  document.querySelectorAll('#metrics input[type=checkbox]:checked').forEach(cb=> ms.push(cb.dataset.m));
  return ms;
}

async function loadOne(sensorID, month){
  const u = new URL(location.href);
  u.searchParams.set('a','view');
  u.searchParams.set('ajax','load');
  u.searchParams.set('sensorID', sensorID);
  u.searchParams.set('month', month);
  const r = await fetch(u.toString(), {credentials:'same-origin'});
  const j = await r.json();
  if(!j.ok) throw new Error(j.error||'load_failed');
  return j;
}

function addPoints(dst, tArr, yArr){
  for(let i=0;i<tArr.length;i++){
    const x = tArr[i]*1000;
    const y = yArr[i];
    if(y === null || y === undefined) continue;
    dst.push({x, y});
  }
}

function sortPoints(arr){
  arr.sort((a,b)=>a.x-b.x);
}

function makeEnvelopeDatasets(labelBase, pointsMin, pointsMax, color){
  // min dataset first (invisible line)
  const dsMin = {
    label: labelBase + ' (min)',
    data: pointsMin,
    parsing: false,
    pointRadius: 0,
    borderWidth: 0,
    borderColor: 'transparent',
    backgroundColor: 'transparent',
    fill: false,
  };
  const dsMax = {
    label: labelBase + ' (max)',
    data: pointsMax,
    parsing: false,
    pointRadius: 0,
    borderWidth: 0,
    borderColor: 'transparent',
    backgroundColor: colorFor(labelBase, 0.2),
    fill: '-1',
  };
  return [dsMin, dsMax];
}

function makeAvgDataset(labelBase, pointsAvg, axis){
  return {
    label: labelBase,
    data: pointsAvg,
    parsing: false,
    pointRadius: 0,
    borderWidth: 2,
    borderColor: colorFor(labelBase, 1),
    backgroundColor: 'transparent',
    fill: false,
    yAxisID: axis
  };
}

async function draw(){
  const files = selectedMatrix();
  const metrics = selectedMetrics();
  if(files.length === 0){ setStatus('Bitte mindestens eine CSV auswählen.'); return; }
  if(metrics.length === 0){ setStatus('Bitte mindestens eine Metrik auswählen.'); return; }

  setStatus('Lade Daten…');

  // buckets per (sensorID, metric)
  const bucket = {}; // key -> {min:[],max:[],avg:[], axis, labelBase}
  const tasks = files.map(async f=>{
    const j = await loadOne(f.sensorID, f.month);
    for(const m of metrics){
      const s = j.series[m];
      if(!s) continue;
      const meta = metricMeta(m);
      const labelBase = `${f.sensorID}:${meta.name}` + (s.sn ? ` (${s.sn})` : '');
      const key = `${f.sensorID}||${m}`;
      if(!bucket[key]){
        bucket[key] = {labelBase, axis: meta.axis, min:[], max:[], avg:[]};
      }
      addPoints(bucket[key].avg, s.t, s.avg);
      addPoints(bucket[key].min, s.t, s.min);
      addPoints(bucket[key].max, s.t, s.max);
    }
  });

  try{
    await Promise.all(tasks);
  }catch(e){
    setStatus('Fehler beim Laden: ' + e.message);
    return;
  }

  const datasets = [];
  for(const k of Object.keys(bucket)){
    const b = bucket[k];
    sortPoints(b.avg); sortPoints(b.min); sortPoints(b.max);

    // envelope behind avg
    datasets.push(...makeEnvelopeDatasets(b.labelBase, b.min, b.max));
    datasets.push(makeAvgDataset(b.labelBase, b.avg, b.axis));
  }

  const ctx = document.getElementById('c');

  if(chart) chart.destroy();
  chart = new Chart(ctx, {
    type:'line',
    data:{datasets},
    options:{
      responsive:true,
      maintainAspectRatio:false,
      interaction:{mode:'nearest', intersect:false},
      plugins:{
        legend:{display:true},
        tooltip:{
          callbacks:{
            title:(items)=>{
              if(!items?.length) return '';
              const x = items[0].parsed.x;
              const d = new Date(x);
              return d.toLocaleString('de-AT');
            }
          }
        }
      },
      scales:{
        x:{
          type:'linear',
          ticks:{
            callback:(v)=>{
              const d = new Date(v);
              return d.toLocaleString('de-AT', {month:'2-digit', day:'2-digit', hour:'2-digit', minute:'2-digit'});
            }
          }
        },
        y:{type:'linear', position:'left'},
        y2:{type:'linear', position:'right', grid:{drawOnChartArea:false}}
      }
    }
  });

  setStatus(`Fertig. Datasets: ${Math.floor(datasets.length/3)} (je Metrik 3 Layers).`);
}

document.getElementById('btnDraw').addEventListener('click', draw);
document.getElementById('btnClear').addEventListener('click', ()=>{
  document.querySelectorAll('#matrix input[type=checkbox]').forEach(cb=>cb.checked=false);
  setStatus('Auswahl gelöscht.');
});
</script>

</body>
</html>
