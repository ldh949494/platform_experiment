#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

namespace {
constexpr uint8_t TEMP_SENSOR_PIN = 13;
constexpr uint8_t I2C_SCL_PIN = 21;
constexpr uint8_t I2C_SDA_PIN = 47;

constexpr int SCREEN_WIDTH = 128;
constexpr int SCREEN_HEIGHT = 64;
constexpr int OLED_RESET_PIN = -1;
constexpr uint8_t OLED_I2C_ADDRESS = 0x3C;

constexpr unsigned long SAMPLE_INTERVAL_MS = 2000;

OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature tempSensor(&oneWire);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN);

bool oledReady = false;
unsigned long lastSampleMs = 0;

int scanI2CBus() {
  int foundDevices = 0;

  Serial.println("I2C scan started...");
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    const uint8_t error = Wire.endTransmission();

    if (error == 0) {
      ++foundDevices;
      Serial.printf("I2C device found at 0x%02X\n", address);
    } else if (error == 4) {
      Serial.printf("I2C unknown error at 0x%02X\n", address);
    }
  }

  if (foundDevices == 0) {
    Serial.println("I2C scan complete: no devices found.");
  } else {
    Serial.printf("I2C scan complete: %d device(s) found.\n", foundDevices);
  }

  return foundDevices;
}

void drawStatusScreen(const char *line1, const char *line2) {
  if (!oledReady) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(line1);
  display.setCursor(0, 16);
  display.println(line2);
  display.display();
}

void initOled() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    oledReady = false;
    Serial.printf("ERROR: OLED init failed at I2C address 0x%02X.\n", OLED_I2C_ADDRESS);
    return;
  }

  oledReady = true;
  Serial.printf("OLED initialized at I2C address 0x%02X.\n", OLED_I2C_ADDRESS);
  drawStatusScreen("System starting", "OLED ready");
}

void initTemperatureSensor() {
  tempSensor.begin();
  const int sensorCount = tempSensor.getDeviceCount();
  Serial.printf("DS18B20 device count: %d\n", sensorCount);

  if (sensorCount <= 0) {
    Serial.println("WARN: DS18B20 not found. Check GPIO13 wiring and sensor power.");
  }
}

void updateDisplay(float temperatureC, bool valid) {
  if (!oledReady) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Temp: ");

  display.setTextSize(2);
  display.setCursor(0, 12);
  if (valid) {
    char tempBuffer[16] = {0};
    snprintf(tempBuffer, sizeof(tempBuffer), "%.1f C", temperatureC);
    display.println(tempBuffer);
  } else {
    display.println("ERR");
  }

  display.setTextSize(1);
  display.setCursor(0, 44);
  display.println("Humi: -- %");
  display.display();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("ESP32-S3 DS18B20 + OLED debug start");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Serial.printf("I2C initialized. SDA=%u, SCL=%u\n", I2C_SDA_PIN, I2C_SCL_PIN);

  const int i2cDeviceCount = scanI2CBus();
  if (i2cDeviceCount == 0) {
    Serial.println("WARN: Check SDA/SCL wiring, OLED power, and OLED I2C address.");
  }

  initOled();
  drawStatusScreen("System starting", "Init DS18B20...");

  initTemperatureSensor();
  drawStatusScreen("System ready", "Sampling every 2s");

  lastSampleMs = millis() - SAMPLE_INTERVAL_MS;
}

void loop() {
  const unsigned long now = millis();
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) {
    return;
  }
  lastSampleMs = now;

  tempSensor.requestTemperatures();
  const float temperatureC = tempSensor.getTempCByIndex(0);
  const bool valid = (temperatureC != DEVICE_DISCONNECTED_C);

  if (valid) {
    Serial.printf("Temperature: %.1f C\n", temperatureC);
  } else {
    Serial.println("ERROR: DS18B20 read failed (DEVICE_DISCONNECTED_C).");
  }

  if (oledReady) {
    updateDisplay(temperatureC, valid);
  } else {
    Serial.println("ERROR: OLED not initialized; skipping display update.");
  }
}

