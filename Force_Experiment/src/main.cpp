#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <HX711.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <XPT2046_Touchscreen.h>

namespace {
constexpr int kTftSckPin = 21;
constexpr int kTftMosiPin = 47;
constexpr int kTftMisoPin = 48;
constexpr int kTftCsPin = 42;
constexpr int kTftDcPin = 41;
constexpr int kTftBacklightPin = 14;

constexpr int kTouchCsPin = 46;
constexpr int kTouchIrqPin = 2;

constexpr int kHxDtPin = 4;
constexpr int kHxSckPin = 5;

constexpr int kScreenWidth = 240;
constexpr int kScreenHeight = 320;
constexpr uint8_t kScreenRotation = 0;

constexpr int kTouchMinX = 240;
constexpr int kTouchMaxX = 3880;
constexpr int kTouchMinY = 240;
constexpr int kTouchMaxY = 3880;

constexpr uint32_t kSampleIntervalMs = 60;
constexpr uint32_t kDisplayIntervalMs = 250;
constexpr uint32_t kStatusIntervalMs = 1000;
constexpr uint32_t kTouchDebounceMs = 220;
constexpr uint32_t kSensorTimeoutMs = 1200;
constexpr uint32_t kHxRecoverIntervalMs = 2000;
constexpr uint8_t kAverageWindow = 10;

constexpr float kDisplayFilterAlpha = 0.20f;
constexpr float kScaleAdjustStep = 0.01f;
constexpr float kMinimumScaleFactor = 0.001f;
constexpr float kMinimumCalDeltaCounts = 100.0f;

constexpr uint16_t kColorBg = ST77XX_BLACK;
constexpr uint16_t kColorPanel = ST77XX_BLUE;
constexpr uint16_t kColorValueBg = ST77XX_BLACK;
constexpr uint16_t kColorText = ST77XX_WHITE;
constexpr uint16_t kColorAccent = ST77XX_CYAN;
constexpr uint16_t kColorButton = ST77XX_BLUE;
constexpr uint16_t kColorWarning = ST77XX_YELLOW;
constexpr uint16_t kColorError = ST77XX_RED;
}

struct Button {
  const char *label;
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  uint16_t fill;
};

SPIClass g_spi(FSPI);
Adafruit_ST7789 g_tft(&g_spi, kTftCsPin, kTftDcPin, -1);
XPT2046_Touchscreen g_touch(kTouchCsPin, kTouchIrqPin);
Preferences g_preferences;
HX711 g_scale;

Button g_buttons[3];
long g_rawWindow[kAverageWindow] = {0};
uint8_t g_windowCount = 0;
uint8_t g_windowIndex = 0;

long g_latestRaw = 0;
long g_averageRaw = 0;
float g_filteredRaw = 0.0f;
float g_displayValue = 0.0f;
long g_tareOffset = 0;
float g_scaleFactor = 1.0f;

bool g_hasSample = false;
bool g_hasFiltered = false;
bool g_hxReady = false;
bool g_touchReady = false;
bool g_touchWasDown = false;
bool g_timeoutPrinted = false;
bool g_isCalibrated = false;

unsigned long g_lastSampleMs = 0;
unsigned long g_lastDisplayMs = 0;
unsigned long g_lastStatusMs = 0;
unsigned long g_lastTouchMs = 0;
unsigned long g_lastReadyMs = 0;
unsigned long g_lastHxRecoverMs = 0;

String g_cmdBuffer;

void resetRawWindow() {
  g_windowCount = 0;
  g_windowIndex = 0;
  g_averageRaw = 0;
}

void pushRawSample(long raw) {
  g_rawWindow[g_windowIndex] = raw;
  g_windowIndex = (g_windowIndex + 1) % kAverageWindow;
  if (g_windowCount < kAverageWindow) {
    ++g_windowCount;
  }

  int64_t sum = 0;
  for (uint8_t i = 0; i < g_windowCount; ++i) {
    sum += g_rawWindow[i];
  }
  g_averageRaw = static_cast<long>(sum / g_windowCount);
}

void saveCalibration() {
  g_preferences.putLong("tare", g_tareOffset);
  g_preferences.putFloat("scale", g_scaleFactor);
  g_preferences.putBool("cal_ok", g_isCalibrated);
}

void loadCalibration() {
  g_tareOffset = g_preferences.isKey("tare") ? g_preferences.getLong("tare", 0) : 0;
  g_isCalibrated = g_preferences.isKey("cal_ok") ? g_preferences.getBool("cal_ok", false) : false;
  g_scaleFactor = g_preferences.isKey("scale") ? g_preferences.getFloat("scale", 0.0f) : 0.0f;
  if (g_isCalibrated && fabsf(g_scaleFactor) < kMinimumScaleFactor) {
    g_isCalibrated = false;
    g_scaleFactor = 0.0f;
  }
}

const char *statusText() {
  if (!g_hxReady) {
    return "HX711 ERROR";
  }
  if (!g_hasSample) {
    return "WAITING";
  }
  if (millis() - g_lastReadyMs > kSensorTimeoutMs) {
    return "TIMEOUT";
  }
  if (!g_isCalibrated) {
    return "CAL REQD";
  }
  return "RUNNING";
}

void printStatus(bool force) {
  const unsigned long now = millis();
  if (!force && now - g_lastStatusMs < kStatusIntervalMs) {
    return;
  }
  g_lastStatusMs = now;
  Serial.printf("[STATUS] raw=%ld avg=%ld tare=%ld scale=%.8f calibrated=%d value=%.2fg status=%s\r\n",
                g_latestRaw,
                g_averageRaw,
                g_tareOffset,
                g_scaleFactor,
                g_isCalibrated ? 1 : 0,
                g_displayValue,
                statusText());
}

void setupButtons() {
  const int16_t margin = 10;
  const int16_t gap = 8;
  const int16_t buttonHeight = 56;
  const int16_t totalWidth = kScreenWidth - (margin * 2);
  const int16_t buttonWidth = (totalWidth - (gap * 2)) / 3;
  const int16_t top = kScreenHeight - buttonHeight - margin;

  g_buttons[0] = {"TARE", margin, top, buttonWidth, buttonHeight, kColorButton};
  g_buttons[1] = {"CAL+", margin + buttonWidth + gap, top, buttonWidth, buttonHeight, kColorButton};
  g_buttons[2] = {"CAL-", margin + ((buttonWidth + gap) * 2), top, buttonWidth, buttonHeight, kColorButton};
}

void drawButton(const Button &button) {
  g_tft.fillRoundRect(button.x, button.y, button.w, button.h, 8, button.fill);
  g_tft.drawRoundRect(button.x, button.y, button.w, button.h, 8, ST77XX_WHITE);

  int16_t x1;
  int16_t y1;
  uint16_t w;
  uint16_t h;
  g_tft.setTextSize(2);
  g_tft.getTextBounds(button.label, 0, 0, &x1, &y1, &w, &h);
  g_tft.setCursor(button.x + (button.w - static_cast<int16_t>(w)) / 2,
                  button.y + (button.h - static_cast<int16_t>(h)) / 2);
  g_tft.setTextColor(ST77XX_WHITE, button.fill);
  g_tft.print(button.label);
}

void drawStaticUi() {
  g_tft.fillScreen(kColorBg);
  g_tft.fillRect(0, 0, kScreenWidth, 30, kColorPanel);
  g_tft.setTextSize(2);
  g_tft.setTextColor(kColorText, kColorPanel);
  g_tft.setCursor(10, 8);
  g_tft.print("Force Monitor");

  g_tft.drawRoundRect(8, 42, kScreenWidth - 16, 112, 10, kColorAccent);
  g_tft.drawRoundRect(8, 162, kScreenWidth - 16, 70, 10, ST77XX_WHITE);

  for (const Button &button : g_buttons) {
    drawButton(button);
  }
}

void renderDynamicUi() {
  char valueBuffer[24];
  char statusBuffer[32];
  char rawBuffer[32];
  char tareBuffer[32];
  char scaleBuffer[32];

  snprintf(valueBuffer, sizeof(valueBuffer), "%.2f g", g_displayValue);
  snprintf(statusBuffer, sizeof(statusBuffer), "STATUS: %s", statusText());
  snprintf(rawBuffer, sizeof(rawBuffer), "RAW: %ld", g_latestRaw);
  snprintf(tareBuffer, sizeof(tareBuffer), "TARE: %ld", g_tareOffset);
  if (g_isCalibrated) {
    snprintf(scaleBuffer, sizeof(scaleBuffer), "K: %.8f g/cnt", g_scaleFactor);
  } else {
    snprintf(scaleBuffer, sizeof(scaleBuffer), "Use: tare -> c <grams>");
  }

  g_tft.fillRect(14, 48, kScreenWidth - 28, 100, kColorValueBg);
  g_tft.setTextColor(kColorText, kColorValueBg);
  g_tft.setTextSize(1);
  g_tft.setCursor(18, 54);
  g_tft.print("Value");

  g_tft.fillRect(18, 76, kScreenWidth - 36, 48, kColorValueBg);
  g_tft.setTextSize(3);
  int16_t x1;
  int16_t y1;
  uint16_t w;
  uint16_t h;
  g_tft.getTextBounds(valueBuffer, 0, 0, &x1, &y1, &w, &h);
  g_tft.setCursor((kScreenWidth - static_cast<int16_t>(w)) / 2, 92);
  g_tft.setTextColor(kColorAccent, kColorValueBg);
  g_tft.print(valueBuffer);

  g_tft.fillRect(14, 168, kScreenWidth - 28, 58, kColorBg);
  uint16_t statusColor = kColorAccent;
  if (strcmp(statusText(), "TIMEOUT") == 0 || strcmp(statusText(), "WAITING") == 0) {
    statusColor = kColorWarning;
  } else if (strcmp(statusText(), "HX711 ERROR") == 0) {
    statusColor = kColorError;
  }

  g_tft.setTextSize(1);
  g_tft.setCursor(16, 170);
  g_tft.setTextColor(statusColor, kColorBg);
  g_tft.print(statusBuffer);

  g_tft.setTextColor(kColorText, kColorBg);
  g_tft.setCursor(16, 188);
  g_tft.print(rawBuffer);
  g_tft.setCursor(16, 204);
  g_tft.print(tareBuffer);
  g_tft.setCursor(16, 220);
  g_tft.print(scaleBuffer);
}

bool pointInButton(const Button &button, int16_t x, int16_t y) {
  return x >= button.x && x < button.x + button.w && y >= button.y && y < button.y + button.h;
}

void applyTare() {
  if (!g_hasSample) {
    return;
  }
  g_tareOffset = g_averageRaw;
  saveCalibration();
  Serial.printf("[CAL] Tare set to %ld\r\n", g_tareOffset);
}

void applyKnownWeightCalibration(float knownGrams) {
  if (!g_hasSample) {
    Serial.println("[CAL] Failed: no HX711 sample yet.");
    return;
  }
  if (knownGrams <= 0.0f) {
    Serial.println("[CAL] Failed: known weight must be > 0 grams.");
    return;
  }

  const float deltaCounts = g_filteredRaw - static_cast<float>(g_tareOffset);
  if (fabsf(deltaCounts) < kMinimumCalDeltaCounts) {
    Serial.println("[CAL] Failed: delta too small. Tare first, then place known weight.");
    return;
  }

  g_scaleFactor = knownGrams / deltaCounts;
  g_isCalibrated = true;
  saveCalibration();
  Serial.printf("[CAL] Success: known=%.2fg delta=%.2f scale=%.8f g/count\r\n",
                knownGrams,
                deltaCounts,
                g_scaleFactor);
}

void adjustScale(bool increase) {
  if (!g_isCalibrated) {
    Serial.println("[CAL] Ignored: calibrate first with command c <grams>.");
    return;
  }
  const float factor = increase ? (1.0f + kScaleAdjustStep) : (1.0f - kScaleAdjustStep);
  g_scaleFactor *= factor;
  if (fabsf(g_scaleFactor) < kMinimumScaleFactor) {
    g_scaleFactor = g_scaleFactor < 0.0f ? -kMinimumScaleFactor : kMinimumScaleFactor;
  }
  saveCalibration();
  Serial.printf("[CAL] Scale updated to %.8f g/count\r\n", g_scaleFactor);
}

bool mapTouchPoint(int16_t &screenX, int16_t &screenY) {
  if (!g_touchReady || !g_touch.touched()) {
    return false;
  }

  TS_Point point = g_touch.getPoint();
  point.x = constrain(point.x, kTouchMinX, kTouchMaxX);
  point.y = constrain(point.y, kTouchMinY, kTouchMaxY);

  screenX = map(point.y, kTouchMaxY, kTouchMinY, 0, kScreenWidth - 1);
  screenY = map(point.x, kTouchMinX, kTouchMaxX, 0, kScreenHeight - 1);
  screenX = constrain(screenX, 0, kScreenWidth - 1);
  screenY = constrain(screenY, 0, kScreenHeight - 1);
  return true;
}

void handleTouch() {
  int16_t x = 0;
  int16_t y = 0;
  const bool touchDown = mapTouchPoint(x, y);
  const unsigned long now = millis();

  if (!touchDown) {
    g_touchWasDown = false;
    return;
  }

  if (g_touchWasDown) {
    return;
  }
  g_touchWasDown = true;

  if (now - g_lastTouchMs < kTouchDebounceMs) {
    return;
  }
  g_lastTouchMs = now;

  Serial.printf("[TOUCH] screen x=%d y=%d\r\n", x, y);

  if (pointInButton(g_buttons[0], x, y)) {
    applyTare();
    renderDynamicUi();
    return;
  }
  if (pointInButton(g_buttons[1], x, y)) {
    adjustScale(true);
    renderDynamicUi();
    return;
  }
  if (pointInButton(g_buttons[2], x, y)) {
    adjustScale(false);
    renderDynamicUi();
  }
}

void recoverHx711IfNeeded(unsigned long now) {
  if (now - g_lastHxRecoverMs < kHxRecoverIntervalMs) {
    return;
  }

  g_lastHxRecoverMs = now;
  Serial.println("[HX711] Recovery: power cycle via SCK.");
  g_scale.power_down();
  delay(1);
  g_scale.power_up();
  delay(1);
  digitalWrite(kHxSckPin, LOW);
}

void updateSamples() {
  const unsigned long now = millis();
  if (now - g_lastSampleMs < kSampleIntervalMs) {
    return;
  }
  g_lastSampleMs = now;

  if (!g_hxReady) {
    return;
  }

  if (!g_scale.is_ready()) {
    if (!g_timeoutPrinted && (now - g_lastReadyMs > kSensorTimeoutMs)) {
      Serial.println("[WARN] HX711 timeout.");
      g_timeoutPrinted = true;
    }
    recoverHx711IfNeeded(now);
    return;
  }

  g_timeoutPrinted = false;
  g_lastReadyMs = now;

  g_latestRaw = g_scale.read();
  g_hasSample = true;
  pushRawSample(g_latestRaw);

  if (!g_hasFiltered) {
    g_filteredRaw = static_cast<float>(g_averageRaw);
    g_hasFiltered = true;
  } else {
    g_filteredRaw = (kDisplayFilterAlpha * static_cast<float>(g_averageRaw)) +
                    ((1.0f - kDisplayFilterAlpha) * g_filteredRaw);
  }

  if (g_isCalibrated) {
    g_displayValue = (g_filteredRaw - static_cast<float>(g_tareOffset)) * g_scaleFactor;
  } else {
    g_displayValue = 0.0f;
  }
}

void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  h            Show this help");
  Serial.println("  r            Print current state");
  Serial.println("  t            Apply tare");
  Serial.println("  c <grams>    Calibrate using a known weight in grams");
  Serial.println("  +            Increase scale factor by 1%");
  Serial.println("  -            Decrease scale factor by 1%");
  Serial.println();
}

void handleCommand(const String &line) {
  String cmd = line;
  cmd.trim();
  if (cmd.isEmpty()) {
    return;
  }

  if (cmd == "h" || cmd == "?") {
    printHelp();
    return;
  }
  if (cmd == "r") {
    printStatus(true);
    return;
  }
  if (cmd == "t") {
    applyTare();
    renderDynamicUi();
    return;
  }
  if (cmd.startsWith("c ")) {
    const float knownGrams = cmd.substring(2).toFloat();
    applyKnownWeightCalibration(knownGrams);
    renderDynamicUi();
    return;
  }
  if (cmd == "+") {
    adjustScale(true);
    renderDynamicUi();
    return;
  }
  if (cmd == "-") {
    adjustScale(false);
    renderDynamicUi();
    return;
  }
}

void readSerialCommands() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      handleCommand(g_cmdBuffer);
      g_cmdBuffer = "";
      continue;
    }
    if (g_cmdBuffer.length() < 64) {
      g_cmdBuffer += c;
    }
  }
}

void setupDisplay() {
  pinMode(kTftBacklightPin, OUTPUT);
  digitalWrite(kTftBacklightPin, HIGH);

  g_spi.begin(kTftSckPin, kTftMisoPin, kTftMosiPin, -1);

  g_tft.init(240, 320);
  g_tft.setRotation(kScreenRotation);
  g_tft.invertDisplay(false);
  g_tft.fillScreen(kColorBg);

  g_touch.begin(g_spi);
  g_touch.setRotation(kScreenRotation);
  g_touchReady = true;

  setupButtons();
  drawStaticUi();
  renderDynamicUi();
}

void setupScale() {
  pinMode(kHxDtPin, INPUT_PULLUP);
  pinMode(kHxSckPin, OUTPUT);
  digitalWrite(kHxSckPin, LOW);

  g_scale.begin(kHxDtPin, kHxSckPin);
  g_scale.set_scale(1.0f);
  g_scale.set_offset(0);
  g_scale.power_up();
  digitalWrite(kHxSckPin, LOW);

  g_hxReady = true;
  g_lastReadyMs = millis();

  if (g_scale.is_ready()) {
    g_latestRaw = g_scale.read();
    resetRawWindow();
    pushRawSample(g_latestRaw);
    g_filteredRaw = static_cast<float>(g_averageRaw);
    g_displayValue = g_isCalibrated
                       ? (g_filteredRaw - static_cast<float>(g_tareOffset)) * g_scaleFactor
                       : 0.0f;
    g_hasSample = true;
    g_hasFiltered = true;
    g_lastReadyMs = millis();
  } else {
    Serial.println("[WARN] HX711 not ready at boot.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=== ESP32-S3 Touch Scale UI ===");
  Serial.println("[DISPLAY] Adafruit_ST7789 240x320");
  Serial.println("[TOUCH] XPT2046 raw mapping, TP_CS=46, TP_PEN=2");
  Serial.printf("[PINS] TFT SCK=%d MOSI=%d MISO=%d CS=%d DC=%d BL=%d\r\n",
                kTftSckPin, kTftMosiPin, kTftMisoPin, kTftCsPin, kTftDcPin, kTftBacklightPin);
  Serial.printf("[PINS] HX711 DT=%d SCK=%d\r\n", kHxDtPin, kHxSckPin);

  g_preferences.begin("scale-ui", false);
  loadCalibration();
  Serial.printf("[NVS] tare=%ld scale=%.8f calibrated=%d\r\n",
                g_tareOffset,
                g_scaleFactor,
                g_isCalibrated ? 1 : 0);

  setupDisplay();
  setupScale();
  printHelp();
}

void loop() {
  readSerialCommands();
  updateSamples();
  handleTouch();
  printStatus(false);

  if (millis() - g_lastDisplayMs >= kDisplayIntervalMs) {
    g_lastDisplayMs = millis();
    renderDynamicUi();
  }

  delay(1);
}
