#include <Wire.h>
#include <Arduino_RouterBridge.h>
#include <Adafruit_Sensor.h>
#include "Adafruit_BME680.h"

#define BME_ADDR   0x77
#define BUTTON_PIN 2                       // button between D2 and GND

Adafruit_BME680 bme(&Wire);

const unsigned long READ_INTERVAL_MS = 1000;   // 1 Hz is better for ML than 3 s
//const unsigned long DEBOUNCE_MS = 50;

bool recording = false;
unsigned long startTime = 0, lastRead = 0, durationMs = 0;
//bool lastBtn = HIGH;
//unsigned long lastBtnChange = 0;

void setup() {
  Monitor.begin();
  Bridge.begin();
  //pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  Wire.begin();

  if (!bme.begin(BME_ADDR)) {
    Monitor.println("BME688 not found.");
    while (true) delay(1000);
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);
  Monitor.println("Ready. Press button to start/stop recording.");
}

void setRecording(bool on) {
  if (on == recording) return;
  recording = on;
  digitalWrite(LED_BUILTIN, recording ? HIGH : LOW);
  if (recording) {
    startTime = millis();
    lastRead = 0;
  }
  Bridge.notify("recording", recording ? 1 : 0);
  Monitor.println(recording ? "REC start" : "REC stop");
}

void handleCommand(String cmd) {
  cmd.trim();
  if (cmd == "start" || cmd.startsWith("start ")) {
    if (recording) {
      Monitor.println("Already recording");
    } else {
      long secs = cmd.length() > 5 ? cmd.substring(6).toInt() : 0;
      durationMs = secs > 0 ? (unsigned long)secs * 1000UL : 0;
      setRecording(true);
      if (durationMs > 0) {
        Monitor.println("Will stop after " + String(secs) + " s");
      }
    }
  } else if (cmd == "stop") {
    setRecording(false);
  } else if (cmd.startsWith("label ")) {
    if (recording) {
      Monitor.println("Stop recording before changing the label");
    } else {
      String label = cmd.substring(6);
      label.trim();
      Bridge.notify("label", label);
      Monitor.println("Label set: " + label);
    }
  } else if (cmd.length() > 0) {
    Monitor.println("Commands: start | stop | label <name>");
  }
}



void loop() {
  if (Monitor.available()) {
    handleCommand(Monitor.readStringUntil('\n'));
  }

  if (recording && durationMs > 0 && millis() - startTime >= durationMs) {
    setRecording(false);
  }

  if (!recording || millis() - lastRead < READ_INTERVAL_MS) return;
  lastRead = millis();

  if (!bme.performReading()) {
    Monitor.println("Reading failed");
    return;
  }

  Bridge.notify("sample",
                (float)(millis() - startTime),      // timestamp ms since start
                bme.temperature,
                bme.humidity,
                bme.pressure / 100.0f,              // hPa
                bme.gas_resistance / 1000.0f);      // kOhm
}