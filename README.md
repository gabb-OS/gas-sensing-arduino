# GasSensing: hand vs. open-air detection with a BME688 on the Arduino UNO Q
> [!WARNING]
> This README was generated with AI assistance and has not been reviewed in detail. Some descriptions may be inaccurate or out of date with respect to the code. When in doubt, refer to `sketch/sketch.ino` and `python/main.py`.

GasSensing classifies whether a BME688 environmental sensor is **held in a hand** or **left in open air**, using only the sensor's own readings: temperature, relative humidity, pressure and gas resistance. Classification runs live on the board, with a neural network trained in Edge Impulse.

The same app is also used to **record labelled training data**. It therefore has two operating modes, selected from the Serial Monitor at startup:

- **Recording**: logs labelled sensor data to CSV files, to be uploaded to Edge Impulse for training.
- **Inference**: runs the trained model continuously and shows the result on the board's RGB LED and in the console.

## How it works

A hand changes the sensor's micro-environment slowly. The sensor warms up, relative humidity rises from skin moisture, and the gas resistance responds to volatile compounds. A single reading is not enough to tell these changes apart from normal room variation, so the model classifies a **window of consecutive readings** (5 seconds at 1 Hz for the current model) and looks at how the four signals evolve over that window.

## Hardware and software

| Item | Notes |
|---|---|
| Arduino UNO Q | STM32U585 microcontroller (MCU) and Qualcomm QRB2210 processor running Debian Linux (MPU) |
| BME688 breakout | I2C on `Wire`, address `0x77` |
| Arduino App Lab | builds and runs the app (sketch on the MCU, Python on Linux) |
| Edge Impulse | data upload, impulse design, training, export as a Linux AARCH64 `.eim` |
| Adafruit BME680 Library | sensor driver, also compatible with the BME688 |
| `edge_impulse_linux` (pip) | Edge Impulse Linux Python SDK, used to run the `.eim` model |

## Repository structure

```
GasSensing/
├── app.yaml                 App Lab app definition
├── sketch/
│   ├── sketch.ino           MCU code: sensor, Serial Monitor commands, LEDs
│   └── sketch.yaml          sketch profile and libraries
├── python/
│   ├── main.py              Linux code: CSV recording, model inference
│   ├── requirements.txt     edge_impulse_linux
│   └── model/
│       └── model.eim        exported Edge Impulse model (not versioned)
└── data/                    CSV files produced in recording mode
```

## Pipeline

```
 BME688 --I2C--> STM32 sketch --Bridge (RPC)--> Python main.py
                     |                              |
                     |                    recording: write CSV in data/
                     |                    inference: sliding window
                     |                               -> model.eim (Edge Impulse)
                     |                               -> smoothing
                     |                               -> JSON line in console
                     |<-------- set_hand(true/false) --+
                 RGB LED 3
```

1. The **sketch** on the STM32 reads the BME688 once per second. It converts pressure to hPa and gas resistance to kOhm, the same units used in the training data.
2. Each reading is sent to the Linux side through the **Bridge**, the UNO Q's RPC link between MCU and MPU.
3. In **recording** mode, `main.py` writes the readings to a CSV file named after the current label.
4. In **inference** mode, `main.py` keeps a sliding window of the latest readings. When the window is full, it is flattened and passed to the Edge Impulse model through the Linux Python SDK, which runs the `.eim` as a separate process.
5. The per-window result is smoothed: the state changes only after several consecutive confident windows agree.
6. Every change of the confirmed state is sent back to the MCU with `set_hand`, which drives the LED.

## Operating modes

About 5 seconds after the system starts, the Serial Monitor shows the startup message followed by the mode menu:

```
=== GasSensing started ===
Sensor: BME688 @ 0x77, sampling every 1000 ms
Ready.
Select mode: recording | inference
```

The sketch waits 5 seconds before printing because the App Lab Monitor connects a few seconds after the sketch starts, and anything printed earlier is lost. If the BME688 is not found, `BME688 not found.` is printed instead of `Ready.` and the sketch stops.

The menu is printed again when you return to mode selection (`mode` in recording mode, `stop` in inference mode), and whenever an unrecognised command is typed in mode selection.

### Recording mode

Enter with `recording` (or `record`, `rec`).

| Command | Effect |
|---|---|
| `label <name>` | sets the label used in the next file name, e.g. `label hand`. Not allowed while recording. |
| `start` | starts recording until `stop` |
| `start <seconds>` | records for the given number of seconds, then stops automatically |
| `stop` | stops the current recording and closes the file |
| `mode` | returns to the mode menu. Not allowed while recording. |

The sensor is read only while a recording is active. Each recording creates `data/<label>.<YYYYMMDD-HHMMSS>.csv` with the columns:

```
timestamp, timestamp_ms, temperature, humidity, pressure, gas_resistance
```

When importing into Edge Impulse, use `timestamp` (or `timestamp_ms`) only as the time column. Do not import it as a feature axis: the model must have exactly 4 axes.

### Inference mode

Enter with `inference` (or `infer`, `inf`).

The sensor is read once per second and every reading is classified. Python loads the model on the first entry into this mode, then prints `filling window 1/N` until the first window is complete. After that it prints one JSON line per second. `stop` ends inference and returns to the mode menu.

## LED reference

All indications use RGB LED 3, marked "STM 3" on the board. The UNO Q RGB LEDs are active-low: `LOW` turns them on.

| Color | Meaning |
|---|---|
| Red, steady | recording in progress |
| Green | inference: HAND confirmed |
| Blue | inference: AIR confirmed |
| Off | mode menu, or inference running with no confirmed state yet |

## Reading the inference output

Example line:

```json
{"scores": {"air": 0.254, "hand": 0.746}, "top": "hand", "state": "hand", "timing_ms": {"anomaly": 0, "classification": 0, "dsp": 0, "json": 0, "msg_handler": 0, "stdin": 337}}
```

| Field | Meaning |
|---|---|
| `scores` | probability for each class on the current window. The scores sum to about 1. The model is quantized to int8, so scores move in steps of 1/256 and the maximum is 0.996. |
| `top` | the highest-scoring class for this single window. This is the raw answer and can be noisy. |
| `state` | the confirmed state that drives the LED. It is `null` until the first confirmation. |
| `timing_ms` | time spent inside the model, in milliseconds. `dsp` and `classification` are below 1 ms for this model. `stdin` increases steadily from line to line and should not be read as per-inference latency. |

When the confirmed state changes, a line such as `[state] >>> HAND (conf 0.75)` is printed just before the JSON line that triggered it.

## Setup

1. **Export the model.** In Edge Impulse Studio, open Deployment, choose the Linux (AARCH64) EIM, and build.
2. **Copy the model into the app folder** on the board. The Python side runs in a container that only sees the app folder.
   ```sh
   mkdir -p ~/ArduinoApps/<app-folder>/python/model
   cp <downloaded>.eim ~/ArduinoApps/<app-folder>/python/model/model.eim
   chmod +x ~/ArduinoApps/<app-folder>/python/model/model.eim
   ```
3. **Check the model.**
   ```sh
   ~/ArduinoApps/<app-folder>/python/model/model.eim --print-info
   ```
   The output must show `axis_count` 4, `interval_ms` 1000, and labels that include `hand`.
4. **Python dependency.** `python/requirements.txt` must contain `edge_impulse_linux`. App Lab installs it on the next run.
5. **Sensor library.** Add the Adafruit BME680 Library to the sketch from the App Lab library manager.
6. **Run** the app in App Lab, open the Serial Monitor, and choose a mode.

## Consistency between training and inference

The model only works on data collected the same way as its training data. The following must be identical in recording and inference, and are shared by construction because both modes use the same sketch:

- sampling interval: `READ_INTERVAL_MS` = 1000 ms
- axis order: temperature, humidity, pressure, gas resistance
- units: degC, %RH, hPa, kOhm
- rounding: 2 decimals (3 for gas resistance)
- sensor settings: oversampling, IIR filter, gas heater at 320 degC for 150 ms

If any of these change, record new data and retrain.

## Configuration

### Sketch (`sketch.ino`)

| Constant | Default | Purpose |
|---|---|---|
| `BME_ADDR` | `0x77` | I2C address of the BME688. Use `0x76` if SDO is tied to GND. |
| `READ_INTERVAL_MS` | `1000` | sampling period; must match the model's `interval_ms` |
| `LED_REC`, `LED_HAND`, `LED_AIR` | `LED3_R`, `LED3_G`, `LED3_B` | LED channels used for each indication |

### Python (`main.py`)

| Constant | Default | Purpose |
|---|---|---|
| `MODEL_PATH` | `python/model/model.eim` | location of the exported model |
| `HAND_LABEL` | `"hand"` | label that turns the green LED on |
| `MIN_CONFIDENCE` | `0.70` | a window counts toward a decision only if its top score reaches this value |
| `CONFIRMATIONS` | `3` | consecutive agreeing windows needed to change the confirmed state |
| `EXPECTED_MS` | `1000` | expected interval between samples |
| `GAP_FACTOR` | `2.5` | a gap longer than `GAP_FACTOR * EXPECTED_MS` clears the window |

Lowering `MIN_CONFIDENCE` makes detection faster but increases false positives. Raising `CONFIRMATIONS` makes the state more stable but slower to change.

## Bridge messages

| Direction | Name | Arguments | Sent when |
|---|---|---|---|
| MCU to Linux | `recording` | `1` or `0` | a recording starts or stops |
| MCU to Linux | `label` | label string | `label <name>` command |
| MCU to Linux | `sample` | ms since start, temperature, humidity, pressure, gas | every second during a recording |
| MCU to Linux | `inference` | `1` or `0` | inference starts or stops |
| MCU to Linux | `infer_sample` | temperature, humidity, pressure, gas | every second in inference mode |
| Linux to MCU | `set_hand` | `true` (HAND) or `false` (AIR) | the confirmed state changes |

MCU-to-Linux messages use `Bridge.notify`, which does not wait for a reply. `set_hand` is sent from Python with `Bridge.call` and is registered on the MCU with `Bridge.provide_safe`, so it runs in the main loop thread and can safely use the LEDs and the Monitor.

## Function reference

### Sketch (`sketch/sketch.ino`)

**Utilities**

| Function | Description |
|---|---|
| `ledsOff()` | Turns off all three channels of RGB LED 3. |
| `printMenu()` | Prints the mode selection menu. |

**Startup and main loop**

| Function | Description |
|---|---|
| `setup()` | Starts the Monitor and the Bridge, configures the three LED channels as outputs and turns them off, and registers `set_hand`. It waits 5 seconds so the Monitor can connect, then prints the startup message (sensor address and sampling interval). It initializes the BME688; if the sensor is not found, it prints `BME688 not found.` and stops. It then applies the sensor settings, prints `Ready.` and the menu, and flushes the Monitor. |
| `loop()` | On each pass it reads a command from the Monitor, if one is available, and dispatches it to the handler of the current mode. It then runs the work function of the current mode: `recordingLoop()` or `inferenceLoop()`. In mode selection it does nothing until a command arrives. It never blocks, apart from the sensor reading itself. |

**Mode selection**

| Function | Description |
|---|---|
| `handleModeSelection(cmd)` | Accepts `recording` or `inference` and their short forms. Any other non-empty input reprints the menu. |

**Recording**

| Function | Description |
|---|---|
| `handleCommand(cmd)` | Handles `start`, `start <seconds>`, `stop`, `label <name>` and `mode`. It prevents label and mode changes while a recording is active. `mode` returns to mode selection and prints the menu. |
| `setRecording(on)` | Starts or stops a recording: updates the red LED, resets the start time, notifies Python with `recording`, and logs the change. |
| `recordingLoop()` | Stops a timed recording when its duration expires. While recording, it reads the sensor once per `READ_INTERVAL_MS` and sends a `sample` message with the elapsed time and the four readings. |

**Inference**

| Function | Description |
|---|---|
| `startInference()` | Switches to inference mode, turns the LEDs off, resets the sampling timer so the first reading is immediate, and notifies Python with `inference 1`. |
| `stopInference()` | Notifies Python with `inference 0`, turns the LEDs off, returns to mode selection, and prints the menu. |
| `handleInferenceCommand(cmd)` | Accepts only `stop` and reminds the user of it for any other input. |
| `inferenceLoop()` | Reads the sensor once per `READ_INTERVAL_MS` and sends an `infer_sample` message with the four readings. These samples are never written to a CSV. |
| `setHand(on)` | Called by Python through the Bridge. It shows the confirmed result on the LED (green for HAND, blue for AIR). The result is printed in the Python console, not on the Monitor. Results that arrive after inference has stopped are ignored. |

### Python (`python/main.py`)

The Python side runs on two threads. The **Bridge thread** runs the `on_*` handlers when a message arrives from the sketch. The **App thread** runs `loop()` repeatedly. Inference handlers only put events into the `events` queue, and all model work happens in `loop()`. Long operations therefore never block the Bridge, and `Bridge.call` is never made from inside a Bridge handler.

**Recording**

| Function | Description |
|---|---|
| `on_label(name)` | Bridge handler. Stores the label for the next recording, replacing spaces and dots with underscores. |
| `on_recording(state)` | Bridge handler. On `1` it stores the start time, opens `data/<label>.<date-time>.csv`, and writes the header. On `0` it closes the file. |
| `on_sample(ts, temp, hum, pres, gas)` | Bridge handler. If a file is open, it writes one row with an absolute timestamp (start time plus `ts`), the elapsed milliseconds, and the rounded readings, then flushes the file to disk. |

**Inference**

| Function | Description |
|---|---|
| `on_inference(state)` | Bridge handler. Puts a `("start",)` or `("stop",)` event in the queue. |
| `on_infer_sample(temp, hum, pres, gas)` | Bridge handler. Rounds the readings as in the training CSVs and puts `("sample", arrival_time, [t, h, p, g])` in the queue. |
| `load_model()` | Loads the model once, on the first inference start. It imports the SDK, starts the `.eim` with `ImpulseRunner.init()`, prints the model information, and checks that the model has 4 axes and contains `HAND_LABEL`. It then sizes the sliding window as `input_features_count / axis_count`. Returns `False` and prints the reason on any error. |
| `reset_inference()` | Clears the window, the gap detection time, and the decision state. Called on every start and stop, so each session begins from an empty window. |
| `classify_sample(t, values)` | Clears the window if more than `GAP_FACTOR * EXPECTED_MS` passed since the previous sample, then appends the new reading. Once the window is full, it flattens it to `[t0, h0, p0, g0, t1, h1, ...]`, runs the model, selects the top class, calls `update_decision`, and prints the JSON line. |
| `update_decision(label, conf)` | Smoothing logic. A window below `MIN_CONFIDENCE` resets the streak. Otherwise the streak grows while the same label repeats. After `CONFIRMATIONS` windows in a row, if the label differs from the confirmed state, the state changes, `[state] >>> ...` is printed, and `set_hand` is sent to the MCU. |
| `loop()` | App thread. Takes one event from the queue (waiting up to 1 s). `start` resets the state and loads the model. `stop` deactivates inference and resets the state. `sample` is classified only while inference is active. |

**Program flow**

The handlers are registered with `Bridge.provide`, then `App.run(user_loop=loop)` runs the app until it is stopped from App Lab. A `finally` block stops the `.eim` process on exit.

**State container**

`Inf` is a class used only as a namespace for the inference state (`runner`, `window_len`, `window`, `active`, `last_t`, `current`, `candidate`, `streak`), so functions can update it without `global` declarations.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Nothing on the Serial Monitor | The startup message appears about 5 s after start. If it never appears, the sketch did not compile or upload (check the App Lab output), or the Monitor connected later than 5 s: increase the `delay` in `setup()`. Typing any text in mode selection reprints the menu. |
| `BME688 not found.` | Check wiring and `BME_ADDR`, then restart the app. |
| `[model] cannot load ...` | Wrong path, missing execute permission (`chmod +x`), or `edge_impulse_linux` not installed. Type `stop`, fix the cause, and select `inference` again. |
| `model expects 5 axes` | The timestamp column was imported as a feature in Edge Impulse. Re-import the data without it and retrain. |
| `gap in data -> window reset` | Samples arrived more than 2.5 s apart. Occasional resets are harmless; frequent ones point to a blocked sketch or an overloaded Bridge. |
| Detection is slow on the first grip | The thermal change takes several seconds to build up. Consider a lower `MIN_CONFIDENCE`, or a longer window in Edge Impulse (Impulse design, Window size) followed by retraining. |