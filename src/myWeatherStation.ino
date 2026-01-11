#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BME280.h>

#include <OneWire.h>
#include <DallasTemperature.h>

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

// ---------------- DS18B20 (1-Wire) ----------------
static const uint8_t ONE_WIRE_PIN = 4;          // GPIO 4 (DS18B20 Data)
static const uint8_t DS18_MAX     = 16;         // max. Anzahl (bei Bedarf erhöhen)
static const uint8_t DS18_RES_BITS = 12;        // 9..12 (12 = max Genauigkeit, max Zeit)

OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature ds18(&oneWire);

DeviceAddress dsAddr[DS18_MAX];
char dsSn[DS18_MAX][17];                        // 16 hex chars + null
float dsTempC[DS18_MAX];                        // zuletzt gelesene Werte
uint8_t dsCount = 0;

struct DsAgg {
  bool has;
  float vMin, vMax, vSum;
  uint32_t n;
};
DsAgg dsAgg[DS18_MAX];

bool dsConversionInProgress = false;
uint32_t dsRequestMs = 0;
uint16_t dsWaitMs = 750;


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
uint32_t lastNtpSyncTry = 0;
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


// ---------------- Log info (for UI) ----------------
static uint32_t logInfoLines = 0;
static uint32_t logInfoBytes = 0;
static uint32_t logInfoAtMs  = 0;

static void refreshLogInfo(bool force = false) {
  uint32_t now = millis();
  if (!force && (uint32_t)(now - logInfoAtMs) < 15000UL) return; // max. alle 15s scannen
  logInfoAtMs = now;

  logInfoLines = 0;
  logInfoBytes = 0;

  if (!SPIFFS.exists(LOG_PATH)) return;
  File f = SPIFFS.open(LOG_PATH, FILE_READ);
  if (!f) return;

  logInfoBytes = f.size();

  const size_t BUFSZ = 512;
  uint8_t buf[BUFSZ];
  int lastChar = -1;

  while (f.available()) {
    size_t n = f.read(buf, BUFSZ);
    if (n == 0) break;
    lastChar = buf[n - 1];
    for (size_t i = 0; i < n; i++) {
      if (buf[i] == '\n') logInfoLines++;
    }
  }
  f.close();

  // falls die letzte Zeile nicht mit \n endet
  if (logInfoBytes > 0 && lastChar != '\n') logInfoLines++;
}

static String makeDownloadFilename() {
  if (timeSynced) {
    time_t tt = (time_t)nowEpochOrUptime();
    struct tm tmv;
    localtime_r(&tt, &tmv);
    char buf[40];
    strftime(buf, sizeof(buf), "temp%Y-%m-%d_%H-%M-%S.csv", &tmv);
    return String(buf);
  }

  char buf[40];
  snprintf(buf, sizeof(buf), "temp_uptime%lus.log", (unsigned long)(millis() / 1000UL));
  return String(buf);
}

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

static String formatDateLabel(uint32_t epochOrUptime) {
  if (!timeSynced) {
    char buf[16];
    snprintf(buf, sizeof(buf), "+%lus", (unsigned long)epochOrUptime);
    return String(buf);
  }

  time_t tt = (time_t)epochOrUptime;
  struct tm tmv;
  localtime_r(&tt, &tmv);
  char buf[16];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
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
  display.setTextWrap(false);
  fmtFloat(buf, sizeof(buf), mx, (var == VAR_PRES) ? 0 : 1);
  display.setCursor(GRAPH_W + 1, GRAPH_Y);
  display.print(buf);

  fmtFloat(buf, sizeof(buf), mn, (var == VAR_PRES) ? 0 : 1);
  display.setCursor(GRAPH_W + 1, GRAPH_Y + GRAPH_H - 10);
  display.print(buf);
}

static void drawHeader(uint8_t var) {
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);

  char buf[16];

  if (var == VAR_TEMP) {
    display.print("Temp (C)");
    display.setTextSize(2);
    display.setCursor(0, 11);
    fmtFloat(buf, sizeof(buf), curTemp, 2);
    display.print(buf);
  } else if (var == VAR_HUM) {
    display.print("Hum (%)");
    display.setTextSize(2);
    display.setCursor(0, 11);
    fmtFloat(buf, sizeof(buf), curHum, 1);
    display.print(buf);
  } else {
    display.print("Pres (hPa)");
    display.setTextSize(2);
    display.setCursor(0, 11);
    fmtFloat(buf, sizeof(buf), curPres, 1);
    display.print(buf);
  }
  display.setTextSize(1);
  String date=formatDateLabel(nowEpochOrUptime());
  String time=formatTimeLabel(nowEpochOrUptime());
  display.setCursor(128-6*date.length(), 0);
  display.print(date);
  display.setCursor(128-6*time.length(), 11);
  display.print(time);
}

static String ellipsize(const String &s, uint8_t maxChars) {
  if (maxChars < 4) return s.substring(0, maxChars);
  if (s.length() <= maxChars) return s;
  return s.substring(0, maxChars - 3) + "...";
}

void drawWifiStatus() {
  const bool staConnected = ((WiFi.getMode() & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED));
  const bool apRunning    = (WiFi.getMode() & WIFI_MODE_AP);

  const uint32_t durationMs = staConnected ? 5000UL : 15000UL;
  const uint32_t startMs = millis();

  // Layout
  const int lineH = 8;                    // Textsize(1) Zeilenhöhe
  const int bodyY = 18;                   // unter Datum/Uhrzeit
  const int barH  = 6;
  const int barY  = SCREEN_HEIGHT - barH; // unten

  while ((uint32_t)(millis() - startMs) < durationMs) {
    const uint32_t nowMs = millis();
    const uint32_t elapsed = (uint32_t)(nowMs - startMs);
    const uint32_t remaining = (elapsed < durationMs) ? (durationMs - elapsed) : 0;

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    // Kopfzeile links
    display.setCursor(0, 0);
    display.print("WiFi");

    // Datum + Uhrzeit rechts oben
    const uint32_t ts = nowEpochOrUptime();
    String date = formatDateLabel(ts);
    String tim  = formatTimeLabel(ts);

    int16_t dateX = (int16_t)SCREEN_WIDTH - (int16_t)(6 * date.length());
    int16_t timeX = (int16_t)SCREEN_WIDTH - (int16_t)(6 * tim.length());
    if (dateX < 0) dateX = 0;
    if (timeX < 0) timeX = 0;

    display.setCursor(dateX, 0);
    display.print(date);
    display.setCursor(timeX, 10);
    display.print(tim);

    // Body
    if (staConnected) {
      String ssid = WiFi.SSID();
      String ip   = WiFi.localIP().toString();

      display.setCursor(0, bodyY + 0 * lineH);
      display.print("Verbunden mit:");

      // SSID ggf. in 2x, wenn sie kurz genug ist
      const uint8_t maxBigChars = (uint8_t)(SCREEN_WIDTH / (6 * 2)); // bei TextSize(2)
      if (ssid.length() <= maxBigChars) {
        display.setTextSize(2);
        int16_t x = (int16_t)((SCREEN_WIDTH - (int)(ssid.length() * 12)) / 2);
        if (x < 0) x = 0;
        display.setCursor(x, bodyY + 1 * lineH);
        display.print(ssid);
        display.setTextSize(1);
        display.setCursor(0, bodyY + 4 * lineH);
      } else {
        //ssid = ellipsize(ssid, (uint8_t)(SCREEN_WIDTH / 6)); // TextSize(1)
        display.setCursor(0, bodyY + 1 * lineH);
        display.print(ssid);
        display.setCursor(0, bodyY + 3 * lineH);
      }

      display.print("IP: ");
      display.print(ip);
    } else {
      // Fallback / kein WLAN
      String apSsid = apRunning ? WiFi.softAPSSID() : String("BME280-Setup");
      String apIp   = apRunning ? WiFi.softAPIP().toString() : String("192.168.4.1");

      //apSsid = ellipsize(apSsid, (uint8_t)(SCREEN_WIDTH / 6));

      display.setCursor(0, bodyY + 0 * lineH);
      display.print("WLAN geht nicht.");

      display.setCursor(0, bodyY + 1 * lineH);
      display.print("Hotspot: ");
      display.print(apSsid);

      display.setCursor(0, bodyY + 2 * lineH);
      display.print("IP: ");
      display.print(apIp);

      display.setCursor(0, bodyY + 3 * lineH);
      display.print("URL: /settings");
    }

    // Restzeit-Balken (voll = gerade gestartet, leer = abgelaufen)
    display.drawRect(0, barY, SCREEN_WIDTH, barH, SSD1306_WHITE);
    const uint16_t fillMax = (uint16_t)(SCREEN_WIDTH - 2);
    const uint16_t fillW = (uint16_t)(((uint64_t)remaining * (uint64_t)fillMax) / (uint64_t)durationMs);
    if (fillW > 0) display.fillRect(1, barY + 1, fillW, barH - 2, SSD1306_WHITE);

    display.display();
    delay(40);
    yield();
  }

  // Zurück auf normalen Screen
  redrawScreen();
}

static void drawDsListScreen() {
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // Header links
  display.setCursor(0, 0);
  display.print("DS18B20");

  // Datum + Uhrzeit rechts
  const uint32_t ts = nowEpochOrUptime();
  String date = formatDateLabel(ts);
  String tim  = formatTimeLabel(ts);

  int16_t dateX = (int16_t)SCREEN_WIDTH - (int16_t)(6 * date.length());
  int16_t timeX = (int16_t)SCREEN_WIDTH - (int16_t)(6 * tim.length());
  if (dateX < 0) dateX = 0;
  if (timeX < 0) timeX = 0;

  display.setCursor(dateX, 0);
  display.print(date);
  display.setCursor(timeX, 10);
  display.print(tim);

  const int lineH = 9;
  int y = 20;

  if (dsCount == 0) {
    display.setCursor(0, y);
    display.print("keine Sensoren");
    return;
  }

  const uint8_t maxLines = (uint8_t)((SCREEN_HEIGHT - y) / lineH);
  uint8_t shown = 0;

  for (uint8_t i = 0; i < dsCount && shown < maxLines; i++) {
    display.setCursor(0, y);
    display.print("DS18B20 #");
    display.print(i + 1);
    display.print(": ");

    char buf[16];
    if (isfinite(dsTempC[i])) {
      fmtFloat(buf, sizeof(buf), dsTempC[i], 2);
      display.print(buf);
      display.print(" C");
    } else {
      display.print("-");
    }

    y += lineH;
    shown++;
  }

  if (dsCount > shown && shown > 0) {
    display.setCursor(0, SCREEN_HEIGHT - lineH);
    display.print("... (");
    display.print(dsCount);
    display.print(")");
  }
}

static void redrawScreen() {
  display.clearDisplay();
  if (screenIdx <= 2) {
    uint8_t var = screenIdx;
    drawHeader(var);
    drawGraph(var);
  } else {
    drawDsListScreen();
  }
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
  dsAggResetAll();
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

// ---------------- DS18B20 helpers ----------------
static uint16_t dsResolutionToWaitMs(uint8_t bits) {
  switch (bits) {
    case 9:  return 94;
    case 10: return 188;
    case 11: return 375;
    default: return 750;
  }
}

static void dsPrintAddress(const DeviceAddress addr) {
  for (uint8_t i = 0; i < 8; i++) {
    if (addr[i] < 16) Serial.print('0');
    Serial.print(addr[i], HEX);
  }
}

static void dsAddrToHexString(const DeviceAddress addr, char *out, size_t outLen) {
  if (!out || outLen < 17) return;
  for (uint8_t i = 0; i < 8; i++) {
    snprintf(out + (i * 2), outLen - (i * 2), "%02X", (unsigned int)addr[i]);
  }
  out[16] = '\0';
}

static void dsAggResetAll() {
  for (uint8_t i = 0; i < DS18_MAX; i++) {
    dsAgg[i].has = false;
    dsAgg[i].n = 0;
    dsAgg[i].vSum = 0.0f;
    dsAgg[i].vMin = NAN;
    dsAgg[i].vMax = NAN;
  }
}

static void dsAggUpdateOne(uint8_t idx, float v) {
  if (idx >= dsCount) return;
  if (!isfinite(v)) return;

  DsAgg &a = dsAgg[idx];
  if (!a.has || a.n == 0) {
    a.has = true;
    a.vMin = a.vMax = v;
    a.vSum = v;
    a.n = 1;
    return;
  }
  if (v < a.vMin) a.vMin = v;
  if (v > a.vMax) a.vMax = v;
  a.vSum += v;
  a.n++;
}

static void dsScanAndCacheDevices() {
  dsCount = ds18.getDeviceCount();
  Serial.print("Gefundene 1-Wire Devices: ");
  Serial.println(dsCount);

  if (dsCount > DS18_MAX) {
    Serial.print("Warnung: Mehr Devices als DS18_MAX (");
    Serial.print(DS18_MAX);
    Serial.println("). Es werden nur die ersten gespeichert.");
    dsCount = DS18_MAX;
  }

  Serial.print("Parasite Power Mode: ");
  Serial.println(ds18.isParasitePowerMode() ? "JA" : "NEIN");

  for (uint8_t i = 0; i < dsCount; i++) {
    DeviceAddress addr;
    if (ds18.getAddress(addr, i)) {
      memcpy(dsAddr[i], addr, 8);
      dsAddrToHexString(dsAddr[i], dsSn[i], sizeof(dsSn[i]));

      Serial.print("DS18B20 #");
      Serial.print(i + 1);
      Serial.print(" ROM: ");
      dsPrintAddress(dsAddr[i]);
      Serial.println();
    } else {
      dsSn[i][0] = '\0';
      Serial.print("Sensor ");
      Serial.print(i);
      Serial.println(" Adresse konnte nicht gelesen werden.");
    }
    dsTempC[i] = NAN;
  }
}

static void dsStartConversion(uint32_t nowMs) {
  if (dsCount == 0) return;
  if (dsConversionInProgress) return;
  ds18.requestTemperatures();   // non-blocking (setWaitForConversion(false))
  dsRequestMs = nowMs;
  dsConversionInProgress = true;
}

static void dsPollConversion(uint32_t nowMs) {
  if (!dsConversionInProgress) return;
  if ((uint32_t)(nowMs - dsRequestMs) < dsWaitMs) return;

  // Werte abholen
  for (uint8_t i = 0; i < dsCount; i++) {
    float c = ds18.getTempC(dsAddr[i]);
    if (c == DEVICE_DISCONNECTED_C || !isfinite(c)) {
      dsTempC[i] = NAN;
    } else {
      dsTempC[i] = c;
      dsAggUpdateOne(i, c);
    }
  }

  dsConversionInProgress = false;
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

  display.clearDisplay();
  display.setTextWrap(false);
  display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
  display.setCursor(3, 3);
  display.println("   connecting WiFi");
  display.display();
  for (uint8_t i = 0; i < cfg.wifiCount; i++) {
    const char* ssid = cfg.wifi[i].ssid;
    const char* pass = cfg.wifi[i].pass;
    if (ssid[0] == '\0') continue;

    Serial.print("Trying WiFi: ");
    Serial.println(ssid);

    display.print("#");
    display.print(i);
    display.print(": ");
    display.println(ssid);
    display.display();

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
  lastNtpSyncTry = millis();
  Serial.print("going to sync Time...");
  setupNtp();
  struct tm tmv;
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < waitMs) {
    if (getLocalTime(&tmv, 500)) {
      time_t nowEpoch = time(nullptr);
      timeSynced = true;
      lastSyncEpoch = nowEpoch;
      lastSyncMillis = millis();
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

  String header = "epoch,ts,temp_min,temp_max,temp_avg,hum_min,hum_max,hum_avg,pres_min,pres_max,pres_avg";
  for (uint8_t i = 0; i < dsCount; i++) {
    String name = (dsSn[i][0] != '\0') ? String(dsSn[i]) : (String("DS18B20#") + String(i + 1));
    header += ",";
    header += name + "_min";
    header += ",";
    header += name + "_max";
    header += ",";
    header += name + "_avg";
  }
  f.println(header);
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
  bool dsHas = false;
  for (uint8_t i = 0; i < dsCount; i++) {
    if (dsAgg[i].has && dsAgg[i].n > 0) { dsHas = true; break; }
  }
  bool bmeHas = (agg.has && agg.n > 0);

  if (!bmeHas && !dsHas) {
    aggReset();
    return;
  }

  ensureLogHeader();

  float tAvg = NAN, hAvg = NAN, pAvg = NAN;
  if (bmeHas) {
    tAvg = agg.tSum / (float)agg.n;
    hAvg = agg.hSum / (float)agg.n;
    pAvg = agg.pSum / (float)agg.n;
  }

  uint32_t ts = nowEpochOrUptime();
  String tsStr = formatTs(ts);

  auto csvAppendFloat = [](String &line, float v, uint8_t decimals) {
    line += ",";
    if (isfinite(v)) line += String((double)v, (unsigned int)decimals);
  };

  File f = SPIFFS.open(LOG_PATH, FILE_APPEND);
  if (f) {
    String line;
    line.reserve(160 + (dsCount * 40));

    line += String((unsigned long)ts);
    line += ",";
    line += tsStr;

    csvAppendFloat(line, bmeHas ? agg.tMin : NAN, 3);
    csvAppendFloat(line, bmeHas ? agg.tMax : NAN, 3);
    csvAppendFloat(line, bmeHas ? tAvg    : NAN, 3);

    csvAppendFloat(line, bmeHas ? agg.hMin : NAN, 3);
    csvAppendFloat(line, bmeHas ? agg.hMax : NAN, 3);
    csvAppendFloat(line, bmeHas ? hAvg    : NAN, 3);

    csvAppendFloat(line, bmeHas ? agg.pMin : NAN, 3);
    csvAppendFloat(line, bmeHas ? agg.pMax : NAN, 3);
    csvAppendFloat(line, bmeHas ? pAvg    : NAN, 3);

    for (uint8_t i = 0; i < dsCount; i++) {
      float dMin = (dsAgg[i].has && dsAgg[i].n > 0) ? dsAgg[i].vMin : NAN;
      float dMax = (dsAgg[i].has && dsAgg[i].n > 0) ? dsAgg[i].vMax : NAN;
      float dAvg = (dsAgg[i].has && dsAgg[i].n > 0) ? (dsAgg[i].vSum / (float)dsAgg[i].n) : NAN;
      csvAppendFloat(line, dMin, 3);
      csvAppendFloat(line, dMax, 3);
      csvAppendFloat(line, dAvg, 3);
    }

    f.println(line);
    f.close();
  }

  trimCsvIfNeededOnce();

  if (cfg.logPostUrl[0] != '\0') {
    DynamicJsonDocument doc(512 + (size_t)dsCount * 180);

    doc["ts"] = ts;
    doc["ts_str"] = tsStr;

    JsonObject temp = doc.createNestedObject("temp");
    JsonObject hum  = doc.createNestedObject("hum");
    JsonObject pres = doc.createNestedObject("pres");

    if (bmeHas) {
      temp["min"] = agg.tMin; temp["max"] = agg.tMax; temp["avg"] = tAvg;
      hum["min"]  = agg.hMin; hum["max"]  = agg.hMax; hum["avg"]  = hAvg;
      pres["min"] = agg.pMin; pres["max"] = agg.pMax; pres["avg"] = pAvg;
    } else {
      temp["min"] = nullptr; temp["max"] = nullptr; temp["avg"] = nullptr;
      hum["min"]  = nullptr; hum["max"]  = nullptr; hum["avg"]  = nullptr;
      pres["min"] = nullptr; pres["max"] = nullptr; pres["avg"] = nullptr;
    }

    JsonArray dsArr = doc.createNestedArray("ds");
    for (uint8_t i = 0; i < dsCount; i++) {
      JsonObject o = dsArr.createNestedObject();
      o["idx"] = i + 1;
      o["sn"]  = dsSn[i];

      if (isfinite(dsTempC[i])) o["t"] = dsTempC[i];
      else o["t"] = nullptr;

      if (dsAgg[i].has && dsAgg[i].n > 0) {
        o["min"] = dsAgg[i].vMin;
        o["max"] = dsAgg[i].vMax;
        o["avg"] = dsAgg[i].vSum / (float)dsAgg[i].n;
      } else {
        o["min"] = nullptr;
        o["max"] = nullptr;
        o["avg"] = nullptr;
      }
    }

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
  screenIdx = cfg.screenIdx % 4;

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
  StaticJsonDocument<2048> doc;
  doc["temp"] = curTemp;
  doc["hum"]  = curHum;
  doc["pres"] = curPres;

  doc["dsCount"] = dsCount;
  JsonArray dsArr = doc.createNestedArray("ds");
  for (uint8_t i = 0; i < dsCount; i++) {
    JsonObject o = dsArr.createNestedObject();
    o["idx"] = i + 1;
    o["sn"]  = dsSn[i];
    if (isfinite(dsTempC[i])) o["t"] = dsTempC[i];
    else o["t"] = nullptr;
  }


  doc["screenIdx"] = screenIdx;
  doc["autoswitchScreens"] = autoswitchScreens;

  refreshLogInfo(false);
  doc["log_exists"] = SPIFFS.exists(LOG_PATH);
  doc["log_bytes"] = logInfoBytes;
  doc["log_lines"] = logInfoLines;

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

static void handleDownloadLog() {
  if (!SPIFFS.exists(LOG_PATH)) {
    server.send(404, "text/plain", "log.csv not found");
    return;
  }

  File f = SPIFFS.open(LOG_PATH, FILE_READ);
  if (!f) {
    server.send(500, "text/plain", "open failed");
    return;
  }

  size_t len = f.size();
  String fname = makeDownloadFilename();

  // wichtig fuer Download + Fortschritt (Content-Length)
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Content-Length", String(len));

  server.streamFile(f, "text/csv");
  f.close();
}

static void handleApiScreenSet() {
  int idx = -1;

  if (server.hasArg("idx")) {
    idx = server.arg("idx").toInt();
  } else if (server.hasArg("plain")) {
    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (!err) {
      idx = doc["screenIdx"] | doc["idx"] | -1;
    }
  }

  if (idx < 0 || idx > 3) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"bad idx\"}");
    return;
  }

  screenIdx = (uint8_t)idx;     // sofort auf OLED anzeigen
  cfg.screenIdx = (uint8_t)idx; // nur RAM (kein Flash write)
  lastScreenMs = millis();      // Timer neu starten
  redrawScreen();

  StaticJsonDocument<192> outDoc;
  outDoc["ok"] = true;
  outDoc["screenIdx"] = screenIdx;
  outDoc["autoswitchScreens"] = autoswitchScreens;

  String out;
  serializeJson(outDoc, out);
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
  server.on("/api/screen", HTTP_POST, handleApiScreenSet);
  server.on("/download", HTTP_GET, handleDownloadLog);
  server.on("/api/log/clear", HTTP_POST, handleApiLogClear);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  Serial.println("Web server started.");
}

static void handleApiLogClear() {
  bool existed = SPIFFS.exists(LOG_PATH);
  if (existed) {
    SPIFFS.remove(LOG_PATH);
  }

  // optional: Datei sofort wieder anlegen (damit spätere Append-Logik sicher ist)
  // und Header schreiben (bitte Header an deinen bestehenden CSV-Header anpassen)
  /*File f = SPIFFS.open(LOG_PATH, FILE_WRITE);
  if (f) {
    //f.println("epoch,ts,temp_min,temp_max,temp_avg,hum_min,hum_max,hum_avg,pres_min,pres_max,pres_avg");  // (hier ggf. deinen echten Header einsetzen)
    f.close();
  }*/ensureLogHeader();

  refreshLogInfo(true);

  StaticJsonDocument<192> doc;
  doc["ok"] = true;
  doc["cleared"] = existed;
  doc["log_exists"] = SPIFFS.exists(LOG_PATH);
  doc["log_bytes"] = logInfoBytes;
  doc["log_lines"] = logInfoLines;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------- Button handling ----------------
static void handleButton(uint8_t i) {
  Serial.print("btn pushed ");
  Serial.println(i);

  if (cfg.btn[i].switchScreen) {
    screenIdx = (screenIdx + 1) % 4;
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
  delay(100);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(3, 3);
  display.print("starting...");
  display.print(millis());
  display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
  display.display();
  delay(1000);

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


  // DS18B20 init (1-Wire)
  ds18.begin();
  ds18.setWaitForConversion(false);
  dsWaitMs = dsResolutionToWaitMs(DS18_RES_BITS);
  ds18.setResolution(DS18_RES_BITS);

  dsScanAndCacheDevices();


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

  drawWifiStatus();

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
    if ((uint32_t)(now - lastNtpSyncTry) >= cfg.ntpResyncMs) {
      syncTimeNow();
    }
  } else if (WiFi.status() == WL_CONNECTED && !timeSynced) {
    if ((uint32_t)(now - lastNtpSyncTry) >= 60000UL) {
      syncTimeNow();
    }
  }

  // DS18B20 conversion completion poll (non-blocking)
  dsPollConversion(now);

  bool dueOled = ((uint32_t)(now - lastOledSampleMs) >= SAMPLE_MS);
  bool dueLog  = ((uint32_t)(now - lastLogSampleMs) >= cfg.logShortMs);

  if (dueOled || dueLog) {
    float t, h, p;
    bool bmeOk = readSensorOnce(t, h, p);
    if (bmeOk) {
      curTemp = t; curHum = h; curPres = p;
    }

    if (dueOled && bmeOk) {
      lastOledSampleMs += SAMPLE_MS;
      pushOledHistory(t, h, p);
      needRedraw = true;
    }

    if (dueLog) {
      lastLogSampleMs += cfg.logShortMs;

      // DS18B20 Werte nur im Logging-Zyklus anstoßen
      dsStartConversion(now);

      if (bmeOk) {
        uint32_t ts = nowEpochOrUptime();
        pushWebHistory(ts, t, h, p);
        aggUpdate(t, h, p);
      }
    }
  }

  // evtl. sofort fertig (z.B. bei niedriger Auflösung)
  dsPollConversion(now);


  if (autoswitchScreens) {
    if ((uint32_t)(now - lastScreenMs) >= SCREEN_MS) {
      //Serial.print("SCREEN_MS is:");
      //Serial.println(SCREEN_MS);
      lastScreenMs = now;
      screenIdx = (screenIdx + 1) % 4;
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
