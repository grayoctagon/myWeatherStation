#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BME280.h>

// ---------------- Display ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// buttons
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
// Falls du andere Pins nutzt, hier anpassen:
#ifndef SDA_PIN
  #define SDA_PIN 8
#endif
#ifndef SCL_PIN
  #define SCL_PIN 9
#endif

// ---------------- Graph Settings ----------------
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

// Timing
const uint32_t SAMPLE_MS = 300;
const uint32_t SCREEN_MS = 4000;
bool autoswitchScreens=false;
uint32_t lastSample = 0;
uint32_t lastScreen = 0;
uint8_t screenIdx = 0; // 0 Temp, 1 Hum, 2 Pres

static void fmtFloat(char* out, size_t outSize, float v, uint8_t decimals) {
  if (!isfinite(v)) {
    strncpy(out, "nan", outSize);
    out[outSize - 1] = '\0';
    return;
  }
  // dtostrf(width, precision)
  char buf[16];
  dtostrf(v, 0, decimals, buf);
  // dtostrf kann führende Leerzeichen produzieren
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
  // minimale Range, damit der Graph nicht "zusammenfällt"
  float minRange = 1.0f;
  if (var == VAR_TEMP) minRange = 0.8f;
  if (var == VAR_HUM)  minRange = 3.0f;
  if (var == VAR_PRES) minRange = 2.0f;

  if (range < minRange) {
    float mid = (mx + mn) * 0.5f;
    mn = mid - minRange * 0.5f;
    mx = mid + minRange * 0.5f;
    range = mx - mn;
  }

  // etwas Luft oben/unten
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
    prevX = px; prevY = py;
  }

  // Min/Max rechts
  char bufMax[12], bufMin[12];
  uint8_t dec = (var == VAR_TEMP) ? 1 : ((var == VAR_PRES) ? 0 : 0);
  fmtFloat(bufMax, sizeof(bufMax), mx, dec);
  fmtFloat(bufMin, sizeof(bufMin), mn, dec);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const uint8_t TXT_X = 106;
  display.setCursor(TXT_X, GRAPH_Y);
  display.print(bufMax);

  display.setCursor(TXT_X, GRAPH_Y + GRAPH_H - 8);
  display.print(bufMin);
}

static void drawHeader(uint8_t var) {
  display.setTextColor(SSD1306_WHITE);

  // Titel
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (var == VAR_TEMP) display.print("TEMPERATUR");
  if (var == VAR_HUM)  display.print("LUFTFEUCHTE");
  if (var == VAR_PRES) display.print("LUFTDRUCK");

  // Wert groß
  display.setTextSize(2);
  display.setCursor(0, 10);

  char valBuf[16];
  if (var == VAR_TEMP) {
    fmtFloat(valBuf, sizeof(valBuf), curTemp, 2);
    display.print(valBuf);
    display.cp437(true);
    display.write(248); // Gradzeichen in CP437
    display.print("C");
  } else if (var == VAR_HUM) {
    fmtFloat(valBuf, sizeof(valBuf), curHum, 1);
    display.print(valBuf);
    display.print("%");
  } else {
    fmtFloat(valBuf, sizeof(valBuf), curPres, 2);
    display.print(valBuf);
    display.setTextSize(1);
    display.setCursor(0, 10 + 16);
    display.print("hPa");
  }
}

static void redrawScreen() {
  uint8_t var = screenIdx;

  display.clearDisplay();
  drawHeader(var);
  drawGraph(var);
  display.display();
}

static void sampleSensors() {
  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float p = bme.readPressure() / 100.0f; // Pa -> hPa

  curTemp = t;
  curHum  = h;
  curPres = p;

  hist[VAR_TEMP][histPos] = curTemp;
  hist[VAR_HUM][histPos]  = curHum;
  hist[VAR_PRES][histPos] = curPres;

  histPos++;
  if (histPos >= GRAPH_W) {
    histPos = 0;
    histFilled = true;
  }
}

void checkButtons(){
  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    bool dobtn = false;

    noInterrupts();
    if (btnFlag[i]) {
      btnFlag[i] = false;
      dobtn = true;
    }
    interrupts();

    if (dobtn) {
      Serial.print("btn pushed ");
      Serial.println(i); // 0..3
      if(i==0){
        screenIdx = (screenIdx + 1) % 3;
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

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

  // Startwerte füllen
  for (uint8_t i = 0; i < GRAPH_W; i++) {
    hist[VAR_TEMP][i] = NAN;
    hist[VAR_HUM][i]  = NAN;
    hist[VAR_PRES][i] = NAN;
  }

  lastSample = millis();
  lastScreen = millis();
  sampleSensors();
  redrawScreen();

  // buttons:
  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    pinMode(btnPins[i], INPUT_PULLUP);
    attachInterruptArg(btnPins[i], onBtn, (void*)i, FALLING);
  }
}

void loop() {
  uint32_t now = millis();
  bool needRedraw = false;

  checkButtons();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample += SAMPLE_MS;
    sampleSensors();
    needRedraw = true;
  }
  
  if(autoswitchScreens)
  if (now - lastScreen >= SCREEN_MS) {
    lastScreen += SCREEN_MS;
    screenIdx = (screenIdx + 1) % 3;
    needRedraw = true;
  }

  if (needRedraw) redrawScreen();

  delay(10);
}
