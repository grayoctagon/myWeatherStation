<?php
declare(strict_types=1);

/*
  ESP Logger Endpoint
  Front Controller: index.php
  Routes:
    ?a=push   (ESP posts JSON payload)
    ?a=view   (Chart.js GUI + AJAX data loader)
    ?a=config (GUI to manage API keys, only if canEditConfig=true)
*/

session_start();

define('APP_BOOTSTRAPPED', true);
define('APP_TZ', 'Europe/Vienna');
date_default_timezone_set(APP_TZ);

define('CONFIG_PATH', __DIR__ . '/config.json');
define('DEFAULT_DATA_DIR', __DIR__ . '/data');
define('DS18_MAX_DEFAULT', 16);

// log defaults
define('LOG_MAX_ENTRIES', 200);
define('LOG_MAX_DAYS', 30);
define('LOG_VERBOSE_MAX_BYTES', 100000); // safety cap

function json_response(int $status, array $data): void {
  http_response_code($status);
  header('Content-Type: application/json; charset=utf-8');
  echo json_encode($data, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
  exit;
}

function load_config(): array {
  if (!is_file(CONFIG_PATH)) {
    return [
      'version' => 1,
      'dataDirDefault' => DEFAULT_DATA_DIR,
      'logPath' => rtrim(DEFAULT_DATA_DIR, "/\\") . DIRECTORY_SEPARATOR . 'log.json',
      'ds18Max' => DS18_MAX_DEFAULT,
      'apikeys' => []
    ];
  }
  $raw = file_get_contents(CONFIG_PATH);
  $cfg = json_decode($raw ?: '[]', true);
  if (!is_array($cfg)) $cfg = [];
  $cfg += [
    'version' => 1,
    'dataDirDefault' => DEFAULT_DATA_DIR,
    'logPath' => rtrim(DEFAULT_DATA_DIR, "/\\") . DIRECTORY_SEPARATOR . 'log.json',
    'ds18Max' => DS18_MAX_DEFAULT,
    'apikeys' => []
  ];
  if (!is_array($cfg['apikeys'])) $cfg['apikeys'] = [];

  // ensure ids
  foreach ($cfg['apikeys'] as $i => $k) {
    if (!is_array($k)) { unset($cfg['apikeys'][$i]); continue; }
    if (empty($k['id'])) $cfg['apikeys'][$i]['id'] = bin2hex(random_bytes(8));
    $cfg['apikeys'][$i] += [
      'csvDir' => $cfg['dataDirDefault'],
      'allowedSensorIDs' => [],
      'canPushData' => false,
      'canViewData' => false,
      'canEditConfig' => false,
      'maxUpdateRate' => 0
    ];
    if (!is_array($cfg['apikeys'][$i]['allowedSensorIDs'])) $cfg['apikeys'][$i]['allowedSensorIDs'] = [];
  }
  $cfg['apikeys'] = array_values($cfg['apikeys']);

  return $cfg;
}

function save_config(array $cfg): void {
  $tmp = CONFIG_PATH . '.tmp';
  $json = json_encode($cfg, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
  if ($json === false) throw new RuntimeException('Could not encode config as JSON.');
  if (file_put_contents($tmp, $json . "\n", LOCK_EX) === false) {
    throw new RuntimeException('Could not write temp config file.');
  }
  if (!rename($tmp, CONFIG_PATH)) {
    @unlink($tmp);
    throw new RuntimeException('Could not replace config file.');
  }
}

function current_action(): string {
  $a = $GLOBALS['ACTION'] ?? '';
  return is_string($a) && $a !== '' ? $a : 'unknown';
}

function log_path(): string {
  $cfg = $GLOBALS['CONFIG'] ?? null;
  if (is_array($cfg) && !empty($cfg['logPath']) && is_string($cfg['logPath'])) {
    return $cfg['logPath'];
  }
  return rtrim(DEFAULT_DATA_DIR, "/\\") . DIRECTORY_SEPARATOR . 'log.json';
}

function read_log_entries(): array {
  $path = log_path();
  if (!is_file($path)) return [];
  $raw = @file_get_contents($path);
  $j = json_decode($raw ?: '[]', true);
  return is_array($j) ? $j : [];
}

function write_log_entries(array $entries): void {
  $path = log_path();
  $dir = dirname($path);
  if (!is_dir($dir)) {
    @mkdir($dir, 0775, true);
  }
  $tmp = $path . '.tmp';
  $json = json_encode($entries, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
  if ($json === false) return;
  if (file_put_contents($tmp, $json . "\n", LOCK_EX) === false) return;
  @rename($tmp, $path);
}

function prune_log_entries(array $entries): array {
  $cut = time() - (LOG_MAX_DAYS * 86400);
  $out = [];
  foreach ($entries as $e) {
    if (!is_array($e)) continue;
    $dt = (string)($e['date'] ?? '');
    if (!preg_match('/^(\d{4})-(\d{2})-(\d{2})_(\d{2})-(\d{2})-(\d{2})$/', $dt, $m)) continue;
    $ts = @mktime((int)$m[4], (int)$m[5], (int)$m[6], (int)$m[2], (int)$m[3], (int)$m[1]);
    if (!is_int($ts) || $ts < $cut) continue;
    $out[] = $e;
  }
  // keep last N
  if (count($out) > LOG_MAX_ENTRIES) {
    $out = array_slice($out, -LOG_MAX_ENTRIES);
  }
  return $out;
}

function add_log(string $type, $apiKeyId, string $details, ?string $verbose = null): void {
  $dt = (new DateTimeImmutable('now', new DateTimeZone(APP_TZ)))->format('Y-m-d_H-i-s');
  $entry = [
    'date' => $dt,
    'type' => $type,
    'apiKeyId' => ($apiKeyId === false || $apiKeyId === null || $apiKeyId === '') ? false : (string)$apiKeyId,
    'details' => $details,
  ];
  if ($verbose !== null) {
    if (strlen($verbose) > LOG_VERBOSE_MAX_BYTES) {
      $entry['details'] .= ' (verbose_truncated)';
      $verbose = substr($verbose, 0, LOG_VERBOSE_MAX_BYTES);
    }
    $entry['verbose'] = $verbose;
  }
  $entries = read_log_entries();
  $entries[] = $entry;
  $entries = prune_log_entries($entries);
  write_log_entries($entries);
}

function request_param(string $name, ?string $default = null): ?string {
  if (isset($_GET[$name])) return is_string($_GET[$name]) ? trim($_GET[$name]) : $default;
  if (isset($_POST[$name])) return is_string($_POST[$name]) ? trim($_POST[$name]) : $default;
  return $default;
}

function sanitize_sensor_id(string $sensorID): ?string {
  $sensorID = trim($sensorID);
  if ($sensorID === '') return null;
  if (!preg_match('/^[A-Za-z0-9_-]{1,32}$/', $sensorID)) return null;
  return $sensorID;
}

function authenticate(array $cfg): ?array {
  // session reuse for browser (push calls usually do not keep cookies)
  if (!empty($_SESSION['auth_key_id'])) {
    foreach ($cfg['apikeys'] as $k) {
      if (($k['id'] ?? null) === $_SESSION['auth_key_id']) return $k;
    }
  }

  $plain = request_param('apikey');
  if ($plain === null || $plain === '') return null;

  foreach ($cfg['apikeys'] as $k) {
    $hash = $k['hash'] ?? '';
    if (is_string($hash) && $hash !== '' && password_verify($plain, $hash)) {
      // store session for browser usage
      $_SESSION['auth_key_id'] = $k['id'];
      return $k;
    }
  }
  return null;
}

function authenticated(): bool {
  return is_array($GLOBALS['AUTH'] ?? null);
}

function auth_info(): array {
  return $GLOBALS['AUTH'] ?? [];
}

function cfg(): array {
  return $GLOBALS['CONFIG'] ?? [];
}

function require_authenticated(): void {
  if (!authenticated()) {
    add_log('error-unauthorized', false, 'Unauthorized request for action=' . current_action());
    json_response(401, ['ok' => false, 'error' => 'unauthorized']);
  }
}

function require_permission(string $perm): void {
  require_authenticated();
  $a = auth_info();
  if (empty($a[$perm])) {
    add_log('error-forbidden', (string)($a['id'] ?? false), 'Missing permission ' . $perm . ' for action=' . current_action());
    json_response(403, ['ok' => false, 'error' => 'forbidden', 'missing' => $perm]);
  }
}

function require_sensor_allowed(string $sensorID): void {
  require_authenticated();
  $a = auth_info();
  $allowed = $a['allowedSensorIDs'] ?? [];
  if (!is_array($allowed) || !in_array($sensorID, $allowed, true)) {
    add_log('error-sensor-not-allowed', (string)($a['id'] ?? false), 'sensorID not allowed: ' . $sensorID . ' action=' . current_action());
    json_response(403, ['ok' => false, 'error' => 'sensor_not_allowed', 'sensorID' => $sensorID]);
  }
}

// bootstrap globals
$CONFIG = load_config();
$AUTH = authenticate($CONFIG);

$GLOBALS['CONFIG'] = $CONFIG;
$GLOBALS['AUTH'] = $AUTH;

// router
$action = request_param('a', 'view');
$GLOBALS['ACTION'] = $action;
if ($action === 'push') {
  require __DIR__ . '/push.php';
  exit;
}
if ($action === 'config') {
  require __DIR__ . '/config.php';
  exit;
}

// default: view
require __DIR__ . '/view.php';
exit;
