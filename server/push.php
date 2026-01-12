<?php
declare(strict_types=1);

if (!defined('APP_BOOTSTRAPPED') || !function_exists('authenticated')) {
  http_response_code(403);
  echo "Direct access not allowed.";
  exit;
}

require_authenticated();
require_permission('canPushData');

$apiKeyId = (string)(auth_info()['id'] ?? false);

$sensorID = sanitize_sensor_id((string)($_GET['sensorID'] ?? ''));
if ($sensorID === null) {
  add_log('error-push-invalid-sensorID', $apiKeyId, 'missing_or_invalid_sensorID');
  json_response(400, ['ok' => false, 'error' => 'missing_or_invalid_sensorID']);
}

require_sensor_allowed($sensorID);

$cfg = cfg();
$a = auth_info();

// rate limit (per sensorID, based on month-file mtime)
$maxRate = (int)($a['maxUpdateRate'] ?? 0);
if ($maxRate < 0) $maxRate = 0;

// read JSON payload
$raw = file_get_contents('php://input');
$doc = json_decode($raw ?: '', true);
if (!is_array($doc)) {
  add_log('error-push-invalid-json', $apiKeyId, 'invalid_json', $raw ?? '');
  json_response(400, ['ok' => false, 'error' => 'invalid_json']);
}

$ts = $doc['ts'] ?? null;
if (!is_int($ts)) {
  if (is_string($ts) && ctype_digit($ts)) $ts = (int)$ts;
}
if (!is_int($ts) || $ts <= 0) {
  // fallback to server time
  $ts = time();
}

$dt = new DateTimeImmutable('@' . $ts);
$dt = $dt->setTimezone(new DateTimeZone(APP_TZ));
$ym = $dt->format('Y-m');
$fname = $ym . '_' . $sensorID . '.csv';

$dataDir = $a['csvDir'] ?? ($cfg['dataDirDefault'] ?? DEFAULT_DATA_DIR);
if (!is_string($dataDir) || $dataDir === '') $dataDir = DEFAULT_DATA_DIR;

if (!is_dir($dataDir)) {
  if (!mkdir($dataDir, 0775, true) && !is_dir($dataDir)) {
    add_log('error-push-storage', $apiKeyId, 'cannot_create_data_dir: ' . $dataDir);
    json_response(500, ['ok' => false, 'error' => 'cannot_create_data_dir']);
  }
}

$path = rtrim($dataDir, "/\\") . DIRECTORY_SEPARATOR . $fname;

if ($maxRate > 0 && is_file($path)) {
  $last = @filemtime($path);
  if (is_int($last)) {
    $delta = time() - $last;
    if ($delta < $maxRate) {
      add_log('error-push-rate-limited', $apiKeyId, 'rate_limited sensorID=' . $sensorID . ' retry_after=' . ($maxRate - $delta));
      json_response(429, ['ok' => false, 'error' => 'rate_limited', 'retry_after' => ($maxRate - $delta)]);
    }
  }
}

$dsMax = (int)($cfg['ds18Max'] ?? DS18_MAX_DEFAULT);
if ($dsMax <= 0 || $dsMax > 64) $dsMax = DS18_MAX_DEFAULT;

function val_or_empty($v): string {
  if ($v === null) return '';
  if (is_bool($v)) return $v ? '1' : '0';
  if (is_int($v) || is_float($v)) return (string)$v;
  if (is_string($v)) return trim($v);
  return '';
}

function nested_minmaxavg(array $doc, string $key): array {
  $o = $doc[$key] ?? null;
  if (!is_array($o)) return ['', '', ''];
  return [
    val_or_empty($o['min'] ?? ''),
    val_or_empty($o['max'] ?? ''),
    val_or_empty($o['avg'] ?? ''),
  ];
}

// map ds array by idx
$dsByIdx = [];
$ds = $doc['ds'] ?? [];
if (is_array($ds)) {
  foreach ($ds as $row) {
    if (!is_array($row)) continue;
    $idx = $row['idx'] ?? null;
    if (!is_int($idx)) {
      if (is_string($idx) && ctype_digit($idx)) $idx = (int)$idx;
    }
    if (!is_int($idx) || $idx < 1) continue;
    $dsByIdx[$idx] = $row;
  }
}

[$tMin, $tMax, $tAvg] = nested_minmaxavg($doc, 'temp');
[$hMin, $hMax, $hAvg] = nested_minmaxavg($doc, 'hum');
[$pMin, $pMax, $pAvg] = nested_minmaxavg($doc, 'pres');

$tsStr = val_or_empty($doc['ts_str'] ?? $dt->format(DateTimeInterface::ATOM));
$remoteIp = $_SERVER['REMOTE_ADDR'] ?? '';

$header = [
  'ts','ts_str',
  'temp_min','temp_max','temp_avg',
  'hum_min','hum_max','hum_avg',
  'pres_min','pres_max','pres_avg',
  'remote_ip'
];

for ($i = 1; $i <= $dsMax; $i++) {
  $header[] = "ds{$i}_sn";
  $header[] = "ds{$i}_t";
  $header[] = "ds{$i}_min";
  $header[] = "ds{$i}_max";
  $header[] = "ds{$i}_avg";
}

$row = [
  (string)$ts,
  $tsStr,
  $tMin, $tMax, $tAvg,
  $hMin, $hMax, $hAvg,
  $pMin, $pMax, $pAvg,
  $remoteIp
];

for ($i = 1; $i <= $dsMax; $i++) {
  $r = $dsByIdx[$i] ?? null;
  if (!is_array($r)) {
    $row = array_merge($row, ['', '', '', '', '']);
    continue;
  }
  $row[] = val_or_empty($r['sn'] ?? '');
  $row[] = val_or_empty($r['t'] ?? '');
  $row[] = val_or_empty($r['min'] ?? '');
  $row[] = val_or_empty($r['max'] ?? '');
  $row[] = val_or_empty($r['avg'] ?? '');
}

$fh = fopen($path, 'ab');
if ($fh === false) {
  add_log('error-push-write', $apiKeyId, 'cannot_open_csv: ' . $path);
  json_response(500, ['ok' => false, 'error' => 'cannot_open_csv']);
}

if (!flock($fh, LOCK_EX)) {
  fclose($fh);
  add_log('error-push-write', $apiKeyId, 'cannot_lock_csv: ' . $path);
  json_response(500, ['ok' => false, 'error' => 'cannot_lock_csv']);
}

$needsHeader = (ftell($fh) === 0) || (filesize($path) === 0);
if ($needsHeader) {
  fputcsv($fh, $header, ';');
}

fputcsv($fh, $row, ';');

fflush($fh);
flock($fh, LOCK_UN);
fclose($fh);

add_log('success-push', $apiKeyId, 'sensorID=' . $sensorID . ' file=' . basename($path));

json_response(200, ['ok' => true, 'sensorID' => $sensorID, 'file' => basename($path)]);
