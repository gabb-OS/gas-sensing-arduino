import csv
import json
import queue
import time
from collections import deque
from datetime import datetime, timedelta
from pathlib import Path

from arduino.app_utils import App, Bridge
from edge_impulse_linux.runner import ImpulseRunner   # python/requirements.txt

current_label = "air"
DATA_DIR = Path(__file__).parent.parent / "data"
DATA_DIR.mkdir(exist_ok=True)

_file = None
_writer = None

start_dt: datetime = datetime.now()


# =====================================================================
#  RECORDING (unchanged, apart from the start_dt fix)
# =====================================================================
def on_label(name: str) -> None:
    global current_label
    current_label = name.strip().replace(" ", "_").replace(".", "_")
    print(f"Label set to {current_label}")


def on_recording(state):
    global _file, _writer, start_dt
    if state:
        if _file:
            _file.close()
        start_dt = datetime.now()
        name = DATA_DIR / f"{current_label}.{datetime.now():%Y%m%d-%H%M%S}.csv"
        _file = open(name, "w", newline="")
        _writer = csv.writer(_file)
        _writer.writerow(["timestamp", "timestamp_ms", "temperature", "humidity", "pressure", "gas_resistance"])
        print(f"Recording to {name}")
    else:
        if _file:
            _file.close()
            print("Recording stopped, file saved")
        _file = _writer = None


def on_sample(ts, temp, hum, pres, gas):
    if _writer:
        stamp = (start_dt + timedelta(milliseconds=ts)).isoformat(timespec="milliseconds")
        _writer.writerow([stamp, int(ts), round(temp, 2), round(hum, 2), round(pres, 2), round(gas, 3)])
        _file.flush()          # survive unexpected power loss
        print(f"recorded {stamp}")


# =====================================================================
#  INFERENCE
# =====================================================================
MODEL_PATH = Path(__file__).parent / "model" / "airball.eim"
HAND_LABEL = "hand"      # label name used in Edge Impulse for "held in hand"
MIN_CONFIDENCE = 0.70    # top score below this -> window ignored for decisions
CONFIRMATIONS = 3        # consecutive agreeing windows needed to switch state: how many consecutive agreeing windows are needed to change state
EXPECTED_MS = 1000       # sketch READ_INTERVAL_MS
GAP_FACTOR = 2.5         # gap > GAP_FACTOR * EXPECTED_MS -> window reset: a gap longer than 2.5 s between samples resets the window

# Bridge callbacks only enqueue events; all inference work happens in loop()
events = queue.Queue(maxsize=1000)


class Inf:
    runner = None        #the loaded model
    window_len = None    #the size of the sliding window
    window = None
    active = False       #whether inference is running
    last_t = None        #arrival time of the last sample, for gap detection
    current = None       #the confirmed state, shown as state in the JSON
    candidate = None     #the label being confirmed
    streak = 0           #how many windows in a row agreed


def on_inference(state):
    events.put(("start",) if state else ("stop",))


def on_infer_sample(temp, hum, pres, gas):
    try:
        # same rounding as the training CSVs
        events.put_nowait(("sample", time.monotonic(),
                           [round(temp, 2), round(hum, 2), round(pres, 2), round(gas, 3)]))
    except queue.Full:
        pass


def load_model():
    """Loaded on the first 'inference' command, so recording works even without a model."""
    if Inf.runner is not None:
        return True
    try:
        runner = ImpulseRunner(str(MODEL_PATH))
        info = runner.init()
    except Exception as e:
        print(f"[model] cannot load {MODEL_PATH}: {e}")
        return False
    mp = info["model_parameters"]
    axes = int(mp["axis_count"])
    labels = list(mp["labels"])
    print(f"[model] {info['project']['owner']} / {info['project']['name']}")
    print(f"[model] axes={axes} window={int(mp['input_features_count']) // axes} samples "
          f"interval={mp['interval_ms']}ms labels={labels}")
    if axes != 4:
        print(f"[model] ERROR: model expects {axes} axes, sketch sends 4. If it is 5, "
              f"'timestamp_ms' was imported as a feature: re-import without it and retrain.")
        runner.stop()
        return False
    if HAND_LABEL not in labels:
        print(f"[model] ERROR: HAND_LABEL '{HAND_LABEL}' not in {labels}")
        runner.stop()
        return False
    Inf.runner = runner
    Inf.window_len = int(mp["input_features_count"]) // axes
    Inf.window = deque(maxlen=Inf.window_len)
    return True


def reset_inference():
    if Inf.window is not None:
        Inf.window.clear()
    Inf.last_t = None
    Inf.current = Inf.candidate = None
    Inf.streak = 0


def update_decision(label, conf):
    if conf < MIN_CONFIDENCE:
        Inf.candidate, Inf.streak = None, 0
        return
    if label == Inf.candidate:
        Inf.streak += 1
    else:
        Inf.candidate, Inf.streak = label, 1
    if Inf.streak >= CONFIRMATIONS and label != Inf.current:
        Inf.current = label
        print(f"[state] >>> {label.upper()} (conf {conf:.2f})")
        try:
            Bridge.call("set_hand", label == HAND_LABEL)
        except Exception as e:
            print(f"[bridge] set_hand failed: {e}")


def classify_sample(t, values):
    if Inf.last_t is not None and (t - Inf.last_t) * 1000 > GAP_FACTOR * EXPECTED_MS:
        print("[stream] gap in data -> window reset")
        Inf.window.clear()
    Inf.last_t = t

    Inf.window.append(values)
    if len(Inf.window) < Inf.window_len:
        print(f"[inference] filling window {len(Inf.window)}/{Inf.window_len}")
        return

    # Edge Impulse raw layout: [t0,h0,p0,g0, t1,h1,p1,g1, ...]
    features = [v for frame in Inf.window for v in frame]
    res = Inf.runner.classify(features)

    scores = res["result"]["classification"]
    label, conf = max(scores.items(), key=lambda kv: kv[1])
    update_decision(label, conf)
    print(json.dumps({"scores": {k: round(v, 3) for k, v in scores.items()},
                      "top": label, "state": Inf.current,
                      "timing_ms": res.get("timing", {})}))


Bridge.provide("recording", on_recording)
Bridge.provide("sample", on_sample)
Bridge.provide("label", on_label)
Bridge.provide("inference", on_inference)
Bridge.provide("infer_sample", on_infer_sample)


def loop():
    try:
        event = events.get(timeout=1.0)
    except queue.Empty:
        return

    kind = event[0]
    if kind == "start":
        reset_inference()
        Inf.active = load_model()
        if Inf.active:
            print("[inference] started")
    elif kind == "stop":
        Inf.active = False
        reset_inference()
        print("[inference] stopped")
    elif kind == "sample" and Inf.active:
        classify_sample(event[1], event[2])


try:
    App.run(user_loop=loop)
finally:
    if Inf.runner is not None:
        Inf.runner.stop()