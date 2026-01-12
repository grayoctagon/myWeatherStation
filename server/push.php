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


// map ds array by serial number (sn)
$dsBySn = [];
$dsSns = [];
$ds = $doc['ds'] ?? [];
if (is_array($ds)) {
  foreach ($ds as $r) {
    if (!is_array($r)) continue;
    $sn = $r['sn'] ?? '';
    if (!is_string($sn) || $sn === '') continue;
    if (!isset($dsBySn[$sn])) $dsSns[] = $sn;
    $dsBySn[$sn] = $r;
  }
}

[$tMin, $tMax, $tAvg] = nested_minmaxavg($doc, 'temp');
[$hMin, $hMax, $hAvg] = nested_minmaxavg($doc, 'hum');
[$pMin, $pMax, $pAvg] = nested_minmaxavg($doc, 'pres');

$tsStr = val_or_empty($doc['ts_str'] ?? $dt->format(DateTimeInterface::ATOM));
$headerBase = [
  'ts','ts_str',
  'temp_min','temp_max','temp_avg',
  'hum_min','hum_max','hum_avg',
  'pres_min','pres_max','pres_avg'
];

$payloadCols = $headerBase;
foreach ($dsSns as $sn) {
  // columns are derived from DS18B20 serial number
  $payloadCols[] = "ds_{$sn}_min";
  $payloadCols[] = "ds_{$sn}_max";
  $payloadCols[] = "ds_{$sn}_avg";
}

function rewrite_csv_expand_and_dedupe(string $path, array $existingHeader, array $missingCols): bool {
  $newHeader = array_values(array_merge($existingHeader, $missingCols));
  $tmp = $path . '.tmp.' . bin2hex(random_bytes(4));

  $in = fopen($path, 'rb');
  if ($in === false) return false;

  $out = fopen($tmp, 'wb');
  if ($out === false) { fclose($in); return false; }

  // discard original header
  fgetcsv($in, 0, ';');

  fputcsv($out, $newHeader, ';');

  $existingCount = count($existingHeader);
  $pad = count($missingCols);

  while (($row = fgetcsv($in, 0, ';')) !== false) {
    if (!is_array($row)) continue;

    // remove duplicated header lines in the middle of the file
    if ($row === $existingHeader || $row === $newHeader) continue;

    // normalize row length to existing header columns
    $row = array_slice($row, 0, $existingCount);
    while (count($row) < $existingCount) $row[] = '';

    // append new empty columns
    for ($i = 0; $i < $pad; $i++) $row[] = '';

    fputcsv($out, $row, ';');
  }

  fclose($in);
  fclose($out);

  if (!rename($tmp, $path)) {
    @unlink($tmp);
    return false;
  }

  return true;
}

$lockPath = $path . '.lock';
$lh = fopen($lockPath, 'c');
if ($lh === false) {
  add_log('error-push-write', $apiKeyId, 'cannot_open_lock: ' . $lockPath);
  json_response(500, ['ok' => false, 'error' => 'cannot_open_lock']);
}
if (!flock($lh, LOCK_EX)) {
  fclose($lh);
  add_log('error-push-write', $apiKeyId, 'cannot_lock: ' . $lockPath);
  json_response(500, ['ok' => false, 'error' => 'cannot_lock']);
}

clearstatcache(true, $path);

$header = [];
if (is_file($path) && filesize($path) > 0) {
  $in = fopen($path, 'rb');
  if ($in === false) {
    flock($lh, LOCK_UN); fclose($lh);
    add_log('error-push-write', $apiKeyId, 'cannot_open_csv_read: ' . $path);
    json_response(500, ['ok' => false, 'error' => 'cannot_open_csv']);
  }
  $existingHeader = fgetcsv($in, 0, ';');
  fclose($in);

  if (!is_array($existingHeader) || count($existingHeader) === 0) {
    flock($lh, LOCK_UN); fclose($lh);
    add_log('error-push-write', $apiKeyId, 'bad_header: ' . $path);
    json_response(500, ['ok' => false, 'error' => 'bad_header']);
  }
  $existingHeader = array_map('strval', $existingHeader);

  $missing = array_values(array_diff($payloadCols, $existingHeader));

  // detect duplicated header lines (legacy bug cleanup)
  $dup = 0;
  $scan = fopen($path, 'rb');
  if ($scan !== false) {
    while (($r = fgetcsv($scan, 0, ';')) !== false) {
      if (!is_array($r)) continue;
      if ($r === $existingHeader) $dup++;
      if ($dup >= 2) break;
    }
    fclose($scan);
  }

  if (count($missing) > 0 || $dup >= 2) {
    if (!rewrite_csv_expand_and_dedupe($path, $existingHeader, $missing)) {
      flock($lh, LOCK_UN); fclose($lh);
      add_log('error-push-write', $apiKeyId, 'csv_rewrite_failed: ' . $path);
      json_response(500, ['ok' => false, 'error' => 'csv_rewrite_failed']);
    }
    $header = array_values(array_merge($existingHeader, $missing));
  } else {
    $header = $existingHeader;
  }
} else {
  // new (or empty) file: write header once
  $header = $payloadCols;
  $out = fopen($path, 'wb');
  if ($out === false) {
    flock($lh, LOCK_UN); fclose($lh);
    add_log('error-push-write', $apiKeyId, 'cannot_open_csv_create: ' . $path);
    json_response(500, ['ok' => false, 'error' => 'cannot_open_csv']);
  }
  fputcsv($out, $header, ';');
  fclose($out);
}

$values = [
  'ts' => (string)$ts,
  'ts_str' => $tsStr,
  'temp_min' => val_or_empty($tMin),
  'temp_max' => val_or_empty($tMax),
  'temp_avg' => val_or_empty($tAvg),
  'hum_min' => val_or_empty($hMin),
  'hum_max' => val_or_empty($hMax),
  'hum_avg' => val_or_empty($hAvg),
  'pres_min' => val_or_empty($pMin),
  'pres_max' => val_or_empty($pMax),
  'pres_avg' => val_or_empty($pAvg),
];

foreach ($dsBySn as $sn => $r) {
  $values["ds_{$sn}_min"] = val_or_empty($r['min'] ?? '');
  $values["ds_{$sn}_max"] = val_or_empty($r['max'] ?? '');
  $values["ds_{$sn}_avg"] = val_or_empty($r['avg'] ?? '');
}

$row = [];
foreach ($header as $col) {
  $row[] = val_or_empty($values[$col] ?? '');
}

$fh = fopen($path, 'ab');
if ($fh === false) {
  flock($lh, LOCK_UN); fclose($lh);
  add_log('error-push-write', $apiKeyId, 'cannot_open_csv_append: ' . $path);
  json_response(500, ['ok' => false, 'error' => 'cannot_open_csv']);
}
fputcsv($fh, $row, ';');
fclose($fh);

flock($lh, LOCK_UN);
fclose($lh);

add_log('success-push', $apiKeyId, 'sensorID=' . $sensorID . ' file=' . basename($path));

json_response(200, ['ok' => true, 'sensorID' => $sensorID, 'file' => basename($path)]);
