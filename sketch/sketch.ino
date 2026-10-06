#include <Wire.h>
#include <Arduino_RouterBridge.h>
#include <Adafruit_Sensor.h>
#include "Adafruit_BME680.h"

#define BME_ADDR   0x77

Adafruit_BME680 bme(&Wire);

const unsigned long READ_INTERVAL_MS = 1000;   // 1 Hz: same rate used for training data

// ---------------- Operating mode ----------------
enum Mode { MODE_SELECT, MODE_RECORD, MODE_INFER };
Mode mode = MODE_SELECT;
bool handDetected = false;                 // last state decided by the model
void setHand(bool on);                     // defined in the INFERENCE section

bool recording = false;
unsigned long startTime = 0, lastRead = 0, durationMs = 0;

void printMenu() {
  Monitor.println("Select mode: recording | inference");
}

void setup() {
  Monitor.begin();
  Bridge.begin();
  pinMode(LED_BUILTIN, OUTPUT);
  Wire.begin();

  Bridge.provide_safe("set_hand", setHand);   // result from Python

  if (!bme.begin(BME_ADDR)) {
    Monitor.println("BME688 not found.");
    while (true) delay(1000);
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);
  Monitor.println("Ready.");
  printMenu();
}

// =====================================================================
//  RECORDING (unchanged)
// =====================================================================
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
  } else if (cmd == "mode") {                          // NEW: back to mode selection
    if (recording) {
      Monitor.println("Stop recording before changing mode");
    } else {
      mode = MODE_SELECT;
      printMenu();
    }
  } else if (cmd.length() > 0) {
    Monitor.println("Commands: start | stop | label <n> | mode");
  }
}

void recordingLoop() {
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

// =====================================================================
//  INFERENCE (NEW)
// =====================================================================
// Called from Python -> Bridge.call("set_hand", True/False)
void setHand(bool on) {
  if (mode != MODE_INFER) return;          // ignore late results after "stop"
  handDetected = on;
  digitalWrite(LED_BUILTIN, handDetected ? HIGH : LOW);
}

void startInference() {
  mode = MODE_INFER;
  handDetected = false;
  digitalWrite(LED_BUILTIN, LOW);
  lastRead = 0;
  Bridge.notify("inference", 1);
  Monitor.println("INFERENCE start. Command: stop");
}

void stopInference() {
  Bridge.notify("inference", 0);
  mode = MODE_SELECT;
  handDetected = false;
  digitalWrite(LED_BUILTIN, LOW);
  Monitor.println("INFERENCE stop");
  printMenu();
}

void handleInferenceCommand(String cmd) {
  cmd.trim();
  if (cmd == "stop") {
    stopInference();
  } else if (cmd.length() > 0) {
    Monitor.println("Inference running. Command: stop");
  }
}

void inferenceLoop() {
  if (millis() - lastRead < READ_INTERVAL_MS) return;
  lastRead = millis();

  if (!bme.performReading()) {
    Monitor.println("Reading failed");
    return;
  }

  // Same units as the training data, separate event: never ends up in a CSV
  Bridge.notify("infer_sample",
                bme.temperature,
                bme.humidity,
                bme.pressure / 100.0f,              // hPa
                bme.gas_resistance / 1000.0f);      // kOhm
}

// =====================================================================
//  MODE SELECTION (NEW)
// =====================================================================
void handleModeSelection(String cmd) {
  cmd.trim();
  if (cmd == "recording" || cmd == "record" || cmd == "rec") {
    mode = MODE_RECORD;
    Monitor.println("RECORDING mode. Commands: start | stop | label <n> | mode");
  } else if (cmd == "inference" || cmd == "infer" || cmd == "inf") {
    startInference();
  } else if (cmd.length() > 0) {
    printMenu();
  }
}

void loop() {
  if (Monitor.available()) {
    String cmd = Monitor.readStringUntil('\n');
    switch (mode) {
      case MODE_SELECT: handleModeSelection(cmd);    break;
      case MODE_RECORD: handleCommand(cmd);          break;
      case MODE_INFER:  handleInferenceCommand(cmd); break;
    }
  }

  switch (mode) {
    case MODE_RECORD: recordingLoop(); break;
    case MODE_INFER:  inferenceLoop(); break;
    default: break;                        // MODE_SELECT: idle, no sampling
  }
}