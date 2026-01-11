#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BME280.h>

#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <esp_sntp.h>

#include "web_pages.h"

// ---------------- Display ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------------- Buttons ----------------
constexpr uint8_t BTN_COUNT = 4;
const uint8_t btnPins[BTN_COUNT] = {0, 1, 3, 10};
volatile bool btnFlag[BTN_COUNT] = {false, false, false, false};

void ARDUINO_ISR_ATTR onBtn(void *arg) {
  uint32_t idx = (uint32_t)arg;   // Index 0..3
  btnFlag[idx] = true;
}

// ---------------- Sensor ----------------
#define BME_ADDR 0x76
Adafruit_BME280 bme;

// ---------------- ESP32-C3 Super Mini I2C Pins ----------------
#ifndef SDA_PIN
  #define SDA_PIN 8
#endif
#ifndef SCL_PIN
  #define SCL_PIN 9
#endif

// ---------------- Graph Settings (OLED) ----------------
enum VarKind { VAR_TEMP = 0, VAR_HUM = 1, VAR_PRES = 2 };
const uint8_t GRAPH_X = 0;
const uint8_t GRAPH_Y = 28;
const uint8_t GRAPH_W = 104;   // rechts Platz für Min/Max Text
const uint8_t GRAPH_H = 36;

float hist[3][GRAPH_W];
uint8_t histPos = 0;
bool histFilled = false;

float curTemp = NAN;
float curHum  = NAN;
float curPres = NAN; // hPa

// ---------------- Web History (RAM) ----------------
constexpr uint16_t WEB_MAX_POINTS = 900;  // ca. 75 Minuten bei 5 s
struct WebPoint {
  uint32_t ts;   // Unix epoch seconds (wenn NTP synced), sonst Sekunden seit Boot
  float t;
  float h;
  float p;
};
WebPoint webHist[WEB_MAX_POINTS];
uint16_t webPos = 0;
bool webFilled = false;

// ---------------- Config / Settings ----------------
constexpr uint8_t MAX_WIFI = 8;
constexpr size_t WIFI_SSID_LEN = 32;
constexpr size_t WIFI_PASS_LEN = 64;
constexpr size_t MAX_URL_LEN  = 196;

struct WifiNet {
  char ssid[WIFI_SSID_LEN + 1];
  char pass[WIFI_PASS_LEN + 1];
};

struct ButtonCfg {
  bool switchScreen;
  char url[MAX_URL_LEN + 1];
};

struct Config {
  // UI / Sampling
  uint32_t sampleMs;          // OLED history sampling
  uint32_t screenMs;          // auto switch
  bool autoswitchScreens;
  uint8_t screenIdx;          // 0..2 (Startscreen)

  // WiFi
  uint8_t wifiCount;
  WifiNet wifi[MAX_WIFI];

  // Time
  uint32_t ntpResyncMs;       // default 30 min

  // Logging
  uint32_t logShortMs;        // read interval into RAM + agg
  uint32_t logLongMs;         // flush min/max/avg
  char logPostUrl[MAX_URL_LEN + 1];
  bool logPostInsecureTls;

  // Buttons
  ButtonCfg btn[BTN_COUNT];
};

Config cfg;

// runtime variables (werden aus cfg übernommen)
uint32_t SAMPLE_MS = 300;
uint32_t SCREEN_MS = 4000;
bool autoswitchScreens = false;
uint8_t screenIdx = 0;

// ---------------- Web server ----------------
WebServer server(80);

// ---------------- Timing ----------------
uint32_t lastOledSampleMs = 0;
uint32_t lastScreenMs = 0;
uint32_t lastLogSampleMs = 0;
uint32_t lastLogFlushMs  = 0;

// ---------------- Time keeping ----------------
bool timeSynced = false;
uint32_t lastSyncMillis = 0;
uint32_t lastNtpSyncMs = 0;
time_t lastSyncEpoch = 0;

// ---------------- Logging aggregate ----------------
struct Agg {
  bool has;
  float tMin, tMax, tSum;
  float hMin, hMax, hSum;
  float pMin, pMax, pSum;
  uint32_t n;
};
Agg agg;

// ---------------- Paths ----------------
const char* CONFIG_PATH  = "/config.json";
const char* LOG_PATH     = "/log.csv";
const char* LOG_TMP_PATH = "/log.tmp";

// ---------------- Helpers ----------------
static void safeStrCopy(char* dst, size_t dstLen, const String& src) {
  if (dstLen == 0) return;
  size_t n = src.length();
  if (n >= dstLen) n = dstLen - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

static uint32_t nowEpochOrUptime() {
  uint32_t nowMs = millis();
  if (!timeSynced) return nowMs / 1000UL;

  uint32_t deltaMs = (uint32_t)(nowMs - lastSyncMillis); // overflow-safe
  return (uint32_t)lastSyncEpoch + (deltaMs / 1000UL);
}

static String formatTs(uint32_t epochOrUptime) {
  if (!timeSynced) {
    char buf[24];
    snprintf(buf, sizeof(buf), "uptime:%lus", (unsigned long)epochOrUptime);
    return String(buf);
  }

  time_t tt = (time_t)epochOrUptime;
  struct tm tmv;
  localtime_r(&tt, &tmv);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
  return String(buf);
}

static String formatTimeLabel(uint32_t epochOrUptime) {
  if (!timeSynced) {
    char buf[16];
    snprintf(buf, sizeof(buf), "+%lus", (unsigned long)epochOrUptime);
    return String(buf);
  }

  time_t tt = (time_t)epochOrUptime;
  struct tm tmv;
  localtime_r(&tt, &tmv);
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
  return String(buf);
}

// ---------------- OLED helpers ----------------
static void fmtFloat(char* out, size_t outSize, float v, uint8_t decimals) {
  if (!isfinite(v)) {
    strncpy(out, "nan", outSize);
    out[outSize - 1] = '\0';
    return;
  }
  char buf[16];
  dtostrf(v, 0, decimals, buf);
  while (*buf == ' ') memmove(buf, buf + 1, strlen(buf));
  strncpy(out, buf, outSize);
  out[outSize - 1] = '\0';
}

static void getMinMax(uint8_t var, float &mn, float &mx) {
  uint8_t len = histFilled ? GRAPH_W : histPos;
  mn =  1e9f;
  mx = -1e9f;

  if (len == 0) {
    mn = 0; mx = 1;
    return;
  }

  for (uint8_t i = 0; i < len; i++) {
    float v = hist[var][i];
    if (!isfinite(v)) continue;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }

  if (!(isfinite(mn) && isfinite(mx))) {
    mn = 0; mx = 1;
    return;
  }

  float range = mx - mn;
  float minRange = 1.0f;
  if (var == VAR_TEMP) minRange = 0.8f;
  if (var == VAR_HUM)  minRange = 2.0f;
  if (var == VAR_PRES) minRange = 2.0f;

  if (range < minRange) {
    float mid = (mn + mx) * 0.5f;
    mn = mid - minRange * 0.5f;
    mx = mid + minRange * 0.5f;
    range = mx - mn;
  }

  float pad = range * 0.05f;
  mn -= pad;
  mx += pad;
}

static void drawGraph(uint8_t var) {
  display.drawRect(GRAPH_X, GRAPH_Y, GRAPH_W, GRAPH_H, SSD1306_WHITE);

  uint8_t len = histFilled ? GRAPH_W : histPos;
  if (len < 2) return;

  float mn, mx;
  getMinMax(var, mn, mx);

  int16_t prevX = -1, prevY = -1;

  for (uint8_t x = 0; x < len; x++) {
    uint8_t idx = histFilled ? (uint8_t)((histPos + x) % GRAPH_W) : x;
    float v = hist[var][idx];

    float t = (v - mn) / (mx - mn);
    if (t < 0) t = 0;
    if (t > 1) t = 1;

    int16_t px = GRAPH_X + x;
    int16_t py = GRAPH_Y + (GRAPH_H - 1) - (int16_t)lroundf(t * (GRAPH_H - 3)) - 1;

    if (prevX >= 0) display.drawLine(prevX, prevY, px, py, SSD1306_WHITE);
    prevX = px;
    prevY = py;
  }

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  char buf[16];

  fmtFloat(buf, sizeof(buf), mx, (var == VAR_PRES) ? 0 : 1);
  display.setCursor(GRAPH_W + 2, GRAPH_Y);
  display.print(buf);

  fmtFloat(buf, sizeof(buf), mn, (var == VAR_PRES) ? 0 : 1);
  display.setCursor(GRAPH_W + 2, GRAPH_Y + GRAPH_H - 10);
  display.print(buf);
}

static void drawHeader(uint8_t var) {
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);

  char buf[16];

  if (var == VAR_TEMP) {
    fmtFloat(buf, sizeof(buf), curTemp, 1);
    display.print(buf);
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print("Temp (C)");
  } else if (var == VAR_HUM) {
    fmtFloat(buf, sizeof(buf), curHum, 1);
    display.print(buf);
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print("Hum (%)");
  } else {
    fmtFloat(buf, sizeof(buf), curPres, 0);
    display.print(buf);
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print("Pres (hPa)");
  }
}

static void redrawScreen() {
  uint8_t var = screenIdx;
  display.clearDisplay();
  drawHeader(var);
  drawGraph(var);
  display.display();
}

// ---------------- Sensor reading + history ----------------
static void pushOledHistory(float t, float h, float p) {
  hist[VAR_TEMP][histPos] = t;
  hist[VAR_HUM][histPos]  = h;
  hist[VAR_PRES][histPos] = p;

  histPos++;
  if (histPos >= GRAPH_W) {
    histPos = 0;
    histFilled = true;
  }
}

static void pushWebHistory(uint32_t ts, float t, float h, float p) {
  webHist[webPos] = {ts, t, h, p};
  webPos++;
  if (webPos >= WEB_MAX_POINTS) {
    webPos = 0;
    webFilled = true;
  }
}

static void aggReset() {
  agg.has = false;
  agg.n = 0;
  agg.tSum = agg.hSum = agg.pSum = 0.0f;
  agg.tMin = agg.hMin = agg.pMin = NAN;
  agg.tMax = agg.hMax = agg.pMax = NAN;
}

static void aggUpdate(float t, float h, float p) {
  if (!(isfinite(t) && isfinite(h) && isfinite(p))) return;

  if (!agg.has) {
    agg.has = true;
    agg.tMin = agg.tMax = t;
    agg.hMin = agg.hMax = h;
    agg.pMin = agg.pMax = p;
    agg.tSum = t;
    agg.hSum = h;
    agg.pSum = p;
    agg.n = 1;
    return;
  }

  if (t < agg.tMin) agg.tMin = t;
  if (t > agg.tMax) agg.tMax = t;
  if (h < agg.hMin) agg.hMin = h;
  if (h > agg.hMax) agg.hMax = h;
  if (p < agg.pMin) agg.pMin = p;
  if (p > agg.pMax) agg.pMax = p;

  agg.tSum += t;
  agg.hSum += h;
  agg.pSum += p;
  agg.n++;
}

static bool readSensorOnce(float &t, float &h, float &p) {
  float tt = bme.readTemperature();
  float hh = bme.readHumidity();
  float pp = bme.readPressure() / 100.0f;

  if (!isfinite(tt) || !isfinite(hh) || !isfinite(pp)) return false;
  t = tt; h = hh; p = pp;
  return true;
}

// ---------------- WiFi ----------------
static void printWifiStatus() {
  if (WiFi.getMode() & WIFI_MODE_STA) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("WiFi connected. SSID: ");
      Serial.print(WiFi.SSID());
      Serial.print(" IP: ");
      Serial.println(WiFi.localIP());
      return;
    }
  }
  if (WiFi.getMode() & WIFI_MODE_AP) {
    Serial.print("AP running. SSID: ");
    Serial.print(WiFi.softAPSSID());
    Serial.print(" IP: ");
    Serial.println(WiFi.softAPIP());
    return;
  }
  Serial.println("WiFi not connected.");
}

static bool connectKnownWifi(uint32_t perNetTimeoutMs = 12000) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);

  for (uint8_t i = 0; i < cfg.wifiCount; i++) {
    const char* ssid = cfg.wifi[i].ssid;
    const char* pass = cfg.wifi[i].pass;
    if (ssid[0] == '\0') continue;

    Serial.print("Trying WiFi: ");
    Serial.println(ssid);

    WiFi.begin(ssid, pass);
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < perNetTimeoutMs) {
      if (WiFi.status() == WL_CONNECTED) {
        printWifiStatus();
        return true;
      }
      delay(250);
    }

    WiFi.disconnect(true, true);
    delay(150);
  }
  return false;
}

static void startFallbackAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("BME280-Setup");  // offen, damit Settings erreichbar sind
  printWifiStatus();
}

// ---------------- NTP ----------------
static void setupNtp() {
  configTzTime("CET-1CEST,M3.5.0/02,M10.5.0/03", "pool.ntp.org", "time.google.com", "time.nist.gov");
  sntp_set_sync_interval(cfg.ntpResyncMs);
}

static bool syncTimeNow(uint32_t waitMs = 8000) {
  if (WiFi.status() != WL_CONNECTED) return false;

  setupNtp();
  struct tm tmv;
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < waitMs) {
    if (getLocalTime(&tmv, 500)) {
      time_t nowEpoch = time(nullptr);
      timeSynced = true;
      lastSyncEpoch = nowEpoch;
      lastSyncMillis = millis();
      lastNtpSyncMs = millis();
      Serial.print("Time synced: ");
      Serial.println(formatTs((uint32_t)nowEpoch));
      return true;
    }
    delay(100);
  }
  return false;
}

// ---------------- HTTP helpers ----------------
static bool httpGetUrl(const char* url) {
  if (!url || url[0] == '\0') return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setTimeout(4000);

  String surl(url);

  if (surl.startsWith("https://")) {
    WiFiClientSecure client;
    client.setInsecure(); // default insecure (Buttons), sonst Zertifikat-Pinning nötig
    if (!http.begin(client, surl)) return false;
  } else {
    WiFiClient client;
    if (!http.begin(client, surl)) return false;
  }

  int code = http.GET();
  http.end();
  return (code > 0 && code < 400);
}

static bool httpPostJson(const char* url, const String& payload, bool insecureTls) {
  if (!url || url[0] == '\0') return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setTimeout(6000);
  String surl(url);

  if (surl.startsWith("https://")) {
    WiFiClientSecure client;
    if (insecureTls) client.setInsecure();
    if (!http.begin(client, surl)) return false;
  } else {
    WiFiClient client;
    if (!http.begin(client, surl)) return false;
  }

  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t*)payload.c_str(), payload.length());
  http.end();
  return (code > 0 && code < 300);
}

// ---------------- Logging ----------------
static bool ensureLogHeader() {
  if (SPIFFS.exists(LOG_PATH)) return true;
  File f = SPIFFS.open(LOG_PATH, FILE_WRITE);
  if (!f) return false;
  f.println("epoch,ts,temp_min,temp_max,temp_avg,hum_min,hum_max,hum_avg,pres_min,pres_max,pres_avg");
  f.close();
  return true;
}

static bool trimCsvIfNeededOnce() {
  size_t total = SPIFFS.totalBytes();
  size_t used  = SPIFFS.usedBytes();
  if (total == 0) return true;

  float frac = (float)used / (float)total;
  if (frac <= 0.40f) return true;
  if (!SPIFFS.exists(LOG_PATH)) return true;

  File in = SPIFFS.open(LOG_PATH, FILE_READ);
  if (!in) return false;

  File out = SPIFFS.open(LOG_TMP_PATH, FILE_WRITE);
  if (!out) {
    in.close();
    return false;
  }

  String header = in.readStringUntil('\n');
  header.trim();
  if (header.length() > 0) out.println(header);

  for (int i = 0; i < 2; i++) {
    if (!in.available()) break;
    in.readStringUntil('\n');
  }

  while (in.available()) {
    String line = in.readStringUntil('\n');
    if (line.length() == 0 && !in.available()) break;
    out.print(line);
    if (!line.endsWith("\n")) out.print("\n");
  }

  in.close();
  out.close();

  SPIFFS.remove(LOG_PATH);
  SPIFFS.rename(LOG_TMP_PATH, LOG_PATH);

  Serial.println("CSV trimmed (removed two oldest rows).");
  return true;
}

static void flushAggToLogAndPost() {
  if (!agg.has || agg.n == 0) {
    aggReset();
    return;
  }

  ensureLogHeader();

  float tAvg = agg.tSum / (float)agg.n;
  float hAvg = agg.hSum / (float)agg.n;
  float pAvg = agg.pSum / (float)agg.n;

  uint32_t ts = nowEpochOrUptime();
  String tsStr = formatTs(ts);

  File f = SPIFFS.open(LOG_PATH, FILE_APPEND);
  if (f) {
    char buf[256];
    snprintf(buf, sizeof(buf),
      "%lu,%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
      (unsigned long)ts,
      tsStr.c_str(),
      agg.tMin, agg.tMax, tAvg,
      agg.hMin, agg.hMax, hAvg,
      agg.pMin, agg.pMax, pAvg
    );
    f.println(buf);
    f.close();
  }

  trimCsvIfNeededOnce();

  if (cfg.logPostUrl[0] != '\0') {
    StaticJsonDocument<384> doc;
    doc["ts"] = ts;
    doc["ts_str"] = tsStr;

    JsonObject temp = doc.createNestedObject("temp");
    temp["min"] = agg.tMin;
    temp["max"] = agg.tMax;
    temp["avg"] = tAvg;

    JsonObject hum = doc.createNestedObject("hum");
    hum["min"] = agg.hMin;
    hum["max"] = agg.hMax;
    hum["avg"] = hAvg;

    JsonObject pres = doc.createNestedObject("pres");
    pres["min"] = agg.pMin;
    pres["max"] = agg.pMax;
    pres["avg"] = pAvg;

    String payload;
    serializeJson(doc, payload);

    bool ok = httpPostJson(cfg.logPostUrl, payload, cfg.logPostInsecureTls);
    Serial.print("POST log: ");
    Serial.println(ok ? "OK" : "FAIL");
  }

  aggReset();
}

// ---------------- Config load/save/apply ----------------
static void setDefaultConfig() {
  memset(&cfg, 0, sizeof(cfg));

  cfg.sampleMs = 300;
  cfg.screenMs = 4000;
  cfg.autoswitchScreens = false;
  cfg.screenIdx = 0;

  cfg.wifiCount = 1;
  safeStrCopy(cfg.wifi[0].ssid, sizeof(cfg.wifi[0].ssid), "findme2");
  safeStrCopy(cfg.wifi[0].pass, sizeof(cfg.wifi[0].pass), "jsps6716");

  cfg.ntpResyncMs = 30UL * 60UL * 1000UL;

  cfg.logShortMs = 5000;
  cfg.logLongMs  = 5UL * 60UL * 1000UL;
  cfg.logPostUrl[0] = '\0';
  cfg.logPostInsecureTls = true;

  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    cfg.btn[i].switchScreen = false;
    cfg.btn[i].url[0] = '\0';
  }
  cfg.btn[0].switchScreen = true;
}

static void applyConfigRuntime() {
  SAMPLE_MS = (cfg.sampleMs < 100) ? 100 : cfg.sampleMs;
  SCREEN_MS = (cfg.screenMs < 500) ? 500 : cfg.screenMs;
  autoswitchScreens = cfg.autoswitchScreens;
  screenIdx = cfg.screenIdx % 3;

  cfg.logShortMs = (cfg.logShortMs < 250) ? 250 : cfg.logShortMs;
  cfg.logLongMs  = (cfg.logLongMs < 1000) ? 1000 : cfg.logLongMs;
  cfg.ntpResyncMs = (cfg.ntpResyncMs < 5UL * 60UL * 1000UL) ? (5UL * 60UL * 1000UL) : cfg.ntpResyncMs;
}

static bool saveConfig() {
  StaticJsonDocument<4096> doc;

  doc["sampleMs"] = cfg.sampleMs;
  doc["screenMs"] = cfg.screenMs;
  doc["autoswitchScreens"] = cfg.autoswitchScreens;
  doc["screenIdx"] = cfg.screenIdx;
  doc["ntpResyncMs"] = cfg.ntpResyncMs;

  JsonArray w = doc.createNestedArray("wifi");
  for (uint8_t i = 0; i < cfg.wifiCount; i++) {
    JsonObject it = w.createNestedObject();
    it["ssid"] = cfg.wifi[i].ssid;
    it["pass"] = cfg.wifi[i].pass;
  }

  JsonObject log = doc.createNestedObject("log");
  log["shortMs"] = cfg.logShortMs;
  log["longMs"] = cfg.logLongMs;
  log["postUrl"] = cfg.logPostUrl;
  log["insecureTls"] = cfg.logPostInsecureTls;

  JsonArray btn = doc.createNestedArray("buttons");
  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    JsonObject b = btn.createNestedObject();
    b["switchScreen"] = cfg.btn[i].switchScreen;
    b["url"] = cfg.btn[i].url;
    b["pin"] = btnPins[i];
  }

  File f = SPIFFS.open(CONFIG_PATH, FILE_WRITE);
  if (!f) return false;
  if (serializeJsonPretty(doc, f) == 0) {
    f.close();
    return false;
  }
  f.close();
  return true;
}

static bool loadConfig() {
  if (!SPIFFS.exists(CONFIG_PATH)) {
    setDefaultConfig();
    saveConfig();
    applyConfigRuntime();
    return true;
  }

  File f = SPIFFS.open(CONFIG_PATH, FILE_READ);
  if (!f) {
    setDefaultConfig();
    applyConfigRuntime();
    return false;
  }

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    setDefaultConfig();
    applyConfigRuntime();
    return false;
  }

  setDefaultConfig();

  cfg.sampleMs = doc["sampleMs"] | cfg.sampleMs;
  cfg.screenMs = doc["screenMs"] | cfg.screenMs;
  cfg.autoswitchScreens = doc["autoswitchScreens"] | cfg.autoswitchScreens;
  cfg.screenIdx = doc["screenIdx"] | cfg.screenIdx;

  cfg.ntpResyncMs = doc["ntpResyncMs"] | cfg.ntpResyncMs;

  if (doc["wifi"].is<JsonArray>()) {
    JsonArray w = doc["wifi"].as<JsonArray>();
    cfg.wifiCount = 0;
    for (JsonObject it : w) {
      if (cfg.wifiCount >= MAX_WIFI) break;
      String ssid = it["ssid"] | "";
      String pass = it["pass"] | "";
      safeStrCopy(cfg.wifi[cfg.wifiCount].ssid, sizeof(cfg.wifi[cfg.wifiCount].ssid), ssid);
      safeStrCopy(cfg.wifi[cfg.wifiCount].pass, sizeof(cfg.wifi[cfg.wifiCount].pass), pass);
      cfg.wifiCount++;
    }
  }

  if (doc["log"].is<JsonObject>()) {
    JsonObject log = doc["log"].as<JsonObject>();
    cfg.logShortMs = log["shortMs"] | cfg.logShortMs;
    cfg.logLongMs  = log["longMs"] | cfg.logLongMs;
    safeStrCopy(cfg.logPostUrl, sizeof(cfg.logPostUrl), String((const char*)(log["postUrl"] | "")));
    cfg.logPostInsecureTls = log["insecureTls"] | cfg.logPostInsecureTls;
  }

  if (doc["buttons"].is<JsonArray>()) {
    JsonArray btn = doc["buttons"].as<JsonArray>();
    uint8_t i = 0;
    for (JsonObject b : btn) {
      if (i >= BTN_COUNT) break;
      cfg.btn[i].switchScreen = b["switchScreen"] | cfg.btn[i].switchScreen;
      safeStrCopy(cfg.btn[i].url, sizeof(cfg.btn[i].url), String((const char*)(b["url"] | "")));
      i++;
    }
  }

  applyConfigRuntime();
  return true;
}

// ---------------- Web UI ----------------
/*static const char INDEX_HTML[] PROGMEM = R"rawliteral(

)rawliteral";

static const char SETTINGS_HTML[] PROGMEM = R"rawliteral(

)rawliteral";
*/
// ---------------- Web handlers ----------------
static void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }
static void handleSettings() { server.send_P(200, "text/html", SETTINGS_HTML); }

static void handleApiState() {
  StaticJsonDocument<384> doc;
  doc["temp"] = curTemp;
  doc["hum"]  = curHum;
  doc["pres"] = curPres;

  doc["time_synced"] = timeSynced;
  doc["time"] = nowEpochOrUptime();
  doc["time_str"] = formatTs(nowEpochOrUptime());

  if (WiFi.status() == WL_CONNECTED) {
    doc["ssid"] = WiFi.SSID();
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
  } else if (WiFi.getMode() & WIFI_MODE_AP) {
    doc["ssid"] = String("AP: ") + WiFi.softAPSSID();
    doc["ip"] = WiFi.softAPIP().toString();
  } else {
    doc["ssid"] = "";
    doc["ip"] = "";
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleApiHistory() {
  uint16_t len = webFilled ? WEB_MAX_POINTS : webPos;
  if (len == 0) {
    server.send(200, "application/json", "{\"labels\":[],\"temp\":[],\"hum\":[],\"pres\":[]}");
    return;
  }

  uint16_t want = len;
  if (server.hasArg("n")) {
    int n = server.arg("n").toInt();
    if (n > 0 && n < (int)len) want = (uint16_t)n;
  }

  uint16_t startIdx;
  if (!webFilled) {
    startIdx = (len > want) ? (len - want) : 0;
  } else {
    uint16_t newest = (webPos == 0) ? (WEB_MAX_POINTS - 1) : (webPos - 1);
    uint16_t oldestWantedOffset = want - 1;
    startIdx = (uint16_t)((newest + WEB_MAX_POINTS - oldestWantedOffset) % WEB_MAX_POINTS);
  }

  String labels = "[";
  String temp   = "[";
  String hum    = "[";
  String pres   = "[";

  for (uint16_t i = 0; i < want; i++) {
    uint16_t idx = (uint16_t)((startIdx + i) % WEB_MAX_POINTS);
    WebPoint &pt = webHist[idx];

    String lab = formatTimeLabel(pt.ts);
    labels += "\""; labels += lab; labels += "\"";
    temp += String(pt.t, 3);
    hum  += String(pt.h, 3);
    pres += String(pt.p, 3);

    if (i + 1 < want) { labels += ","; temp += ","; hum += ","; pres += ","; }
  }

  labels += "]";
  temp   += "]";
  hum    += "]";
  pres   += "]";

  String out = "{\"labels\":" + labels + ",\"temp\":" + temp + ",\"hum\":" + hum + ",\"pres\":" + pres + "}";
  server.send(200, "application/json", out);
}

static void configToJson(String &out) {
  StaticJsonDocument<4096> doc;

  doc["sampleMs"] = cfg.sampleMs;
  doc["screenMs"] = cfg.screenMs;
  doc["autoswitchScreens"] = cfg.autoswitchScreens;
  doc["screenIdx"] = cfg.screenIdx;

  JsonArray w = doc.createNestedArray("wifi");
  for (uint8_t i = 0; i < cfg.wifiCount; i++) {
    JsonObject it = w.createNestedObject();
    it["ssid"] = cfg.wifi[i].ssid;
    it["pass"] = cfg.wifi[i].pass;
  }

  JsonObject log = doc.createNestedObject("log");
  log["shortMs"] = cfg.logShortMs;
  log["longMs"] = cfg.logLongMs;
  log["postUrl"] = cfg.logPostUrl;
  log["insecureTls"] = cfg.logPostInsecureTls;

  JsonArray btn = doc.createNestedArray("buttons");
  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    JsonObject b = btn.createNestedObject();
    b["switchScreen"] = cfg.btn[i].switchScreen;
    b["url"] = cfg.btn[i].url;
    b["pin"] = btnPins[i];
  }

  serializeJson(doc, out);
}

static void handleApiConfigGet() {
  String out;
  configToJson(out);
  server.send(200, "application/json", out);
}

static void handleApiConfigPost() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "missing body"); return; }

  String body = server.arg("plain");
  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) { server.send(400, "text/plain", "bad json"); return; }

  cfg.sampleMs = doc["sampleMs"] | cfg.sampleMs;
  cfg.screenMs = doc["screenMs"] | cfg.screenMs;
  cfg.autoswitchScreens = doc["autoswitchScreens"] | cfg.autoswitchScreens;
  cfg.screenIdx = (uint8_t)(doc["screenIdx"] | cfg.screenIdx);

  if (doc["wifi"].is<JsonArray>()) {
    JsonArray w = doc["wifi"].as<JsonArray>();
    cfg.wifiCount = 0;
    for (JsonObject it : w) {
      if (cfg.wifiCount >= MAX_WIFI) break;
      safeStrCopy(cfg.wifi[cfg.wifiCount].ssid, sizeof(cfg.wifi[cfg.wifiCount].ssid), String((const char*)(it["ssid"] | "")));
      safeStrCopy(cfg.wifi[cfg.wifiCount].pass, sizeof(cfg.wifi[cfg.wifiCount].pass), String((const char*)(it["pass"] | "")));
      cfg.wifiCount++;
    }
  }

  if (doc["log"].is<JsonObject>()) {
    JsonObject log = doc["log"].as<JsonObject>();
    cfg.logShortMs = log["shortMs"] | cfg.logShortMs;
    cfg.logLongMs  = log["longMs"] | cfg.logLongMs;
    safeStrCopy(cfg.logPostUrl, sizeof(cfg.logPostUrl), String((const char*)(log["postUrl"] | "")));
    cfg.logPostInsecureTls = log["insecureTls"] | cfg.logPostInsecureTls;
  }

  if (doc["buttons"].is<JsonArray>()) {
    JsonArray btn = doc["buttons"].as<JsonArray>();
    uint8_t i = 0;
    for (JsonObject b : btn) {
      if (i >= BTN_COUNT) break;
      cfg.btn[i].switchScreen = b["switchScreen"] | cfg.btn[i].switchScreen;
      safeStrCopy(cfg.btn[i].url, sizeof(cfg.btn[i].url), String((const char*)(b["url"] | "")));
      i++;
    }
  }

  applyConfigRuntime();
  saveConfig();

  String out;
  configToJson(out);
  server.send(200, "application/json", out);
}

static void handleWifiReconnect() {
  bool ok = connectKnownWifi();
  if (!ok) startFallbackAP();
  if (WiFi.status() == WL_CONNECTED) syncTimeNow();

  StaticJsonDocument<128> doc;
  doc["ok"] = (WiFi.status() == WL_CONNECTED);
  doc["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/history", HTTP_GET, handleApiHistory);
  server.on("/api/config", HTTP_GET, handleApiConfigGet);
  server.on("/api/config", HTTP_POST, handleApiConfigPost);
  server.on("/api/wifi/reconnect", HTTP_POST, handleWifiReconnect);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  Serial.println("Web server started.");
}

// ---------------- Button handling ----------------
static void handleButton(uint8_t i) {
  Serial.print("btn pushed ");
  Serial.println(i);

  if (cfg.btn[i].switchScreen) {
    screenIdx = (screenIdx + 1) % 3;
    return;
  }
  if (cfg.btn[i].url[0] != '\0') {
    bool ok = httpGetUrl(cfg.btn[i].url);
    Serial.print("btn url ");
    Serial.println(ok ? "OK" : "FAIL");
  }
}

static void checkButtons() {
  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    bool dobtn = false;

    noInterrupts();
    if (btnFlag[i]) { btnFlag[i] = false; dobtn = true; }
    interrupts();

    if (dobtn) handleButton(i);
  }
}

// ---------------- Setup/Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(50);

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
  }

  loadConfig();
  applyConfigRuntime();

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    while (true) { delay(1000); }
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  bool ok = bme.begin(BME_ADDR);
  if (!ok) {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("BME280 nicht gefunden");
    display.setCursor(0, 10);
    display.print("Adresse 0x76?");
    display.display();
    while (true) { delay(1000); }
  }

  for (uint8_t i = 0; i < GRAPH_W; i++) {
    hist[VAR_TEMP][i] = NAN;
    hist[VAR_HUM][i]  = NAN;
    hist[VAR_PRES][i] = NAN;
  }
  aggReset();

  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    pinMode(btnPins[i], INPUT_PULLUP);
    attachInterruptArg(btnPins[i], onBtn, (void*)i, FALLING);
  }

  bool staOk = connectKnownWifi();
  if (!staOk) startFallbackAP();
  printWifiStatus();

  syncTimeNow();

  setupWeb();

  float t, h, p;
  if (readSensorOnce(t, h, p)) {
    curTemp = t; curHum = h; curPres = p;
    pushOledHistory(t, h, p);
    pushWebHistory(nowEpochOrUptime(), t, h, p);
    aggUpdate(t, h, p);
  }

  uint32_t now = millis();
  lastOledSampleMs = now;
  lastScreenMs = now;
  lastLogSampleMs = now;
  lastLogFlushMs  = now;

  redrawScreen();
}

void loop() {
  server.handleClient();

  uint32_t now = millis();
  bool needRedraw = false;

  checkButtons();

  if (WiFi.status() == WL_CONNECTED && timeSynced) {
    if ((uint32_t)(now - lastNtpSyncMs) >= cfg.ntpResyncMs) {
      syncTimeNow();
    }
  } else if (WiFi.status() == WL_CONNECTED && !timeSynced) {
    if ((uint32_t)(now - lastNtpSyncMs) >= 60000UL) {
      syncTimeNow();
    }
  }

  bool dueOled = ((uint32_t)(now - lastOledSampleMs) >= SAMPLE_MS);
  bool dueLog  = ((uint32_t)(now - lastLogSampleMs) >= cfg.logShortMs);

  if (dueOled || dueLog) {
    float t, h, p;
    if (readSensorOnce(t, h, p)) {
      curTemp = t; curHum = h; curPres = p;

      if (dueOled) {
        lastOledSampleMs += SAMPLE_MS;
        pushOledHistory(t, h, p);
        needRedraw = true;
      }

      if (dueLog) {
        lastLogSampleMs += cfg.logShortMs;
        uint32_t ts = nowEpochOrUptime();
        pushWebHistory(ts, t, h, p);
        aggUpdate(t, h, p);
      }
    }
  }

  if (autoswitchScreens) {
    if ((uint32_t)(now - lastScreenMs) >= SCREEN_MS) {
      lastScreenMs += SCREEN_MS;
      screenIdx = (screenIdx + 1) % 3;
      needRedraw = true;
    }
  }

  if ((uint32_t)(now - lastLogFlushMs) >= cfg.logLongMs) {
    lastLogFlushMs += cfg.logLongMs;
    flushAggToLogAndPost();
  }

  if (needRedraw) redrawScreen();
  delay(5);
}
