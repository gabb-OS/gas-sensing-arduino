import csv
import time
from datetime import datetime, timedelta
from pathlib import Path
 
from arduino.app_utils import App, Bridge
 
current_label = "air"
DATA_DIR = Path(__file__).parent.parent / "data"
DATA_DIR.mkdir(exist_ok=True)
 
_file = None
_writer = None
 
start_dt: datetime = datetime.now()
 
 
def on_label(name: str) -> None:
    global current_label
    current_label = name.strip().replace(" ", "_").replace(".", "_")
    print(f"Label set to {current_label}")
    
 
def on_recording(state):
    global _file, _writer
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
 
 
Bridge.provide("recording", on_recording)
Bridge.provide("sample", on_sample)
Bridge.provide("label", on_label)
 
 
def loop():
    time.sleep(1)
 
 
App.run(user_loop=loop)
