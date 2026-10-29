"""
AGRO - Raspberry Pi main script

- Webcam -> YOLO11n (primary detector)
- If YOLO finds nothing, the TFLite SSD model runs on the same frame (fallback).
  Set RUN_BOTH = True to always run both and merge the results.
- Counts detected crops (inventory), logs to CSV, optionally POSTs to a dashboard
- Every SOIL_PERIOD_S, asks the Arduino for a probe reading and updates the soil drying
  forecast (soil_forecast.py): estimated minutes until the soil reaches the watering threshold
- Reads the Arduino DATA line over USB serial and can send drive/probe commands

Run:  python3 agro_pi_main.py            (headless)
      python3 agro_pi_main.py --show     (preview window)
      python3 agro_pi_main.py --no-serial   (test the camera models without the Arduino)

Model weights:
  weights/agro_plants.pt   YOLO11n fine-tuned on your own plant photos (see train_yolo.py)
  models/ssd.tflite        TFLite SSD model
  models/labels.txt        one label per line, in class order
If agro_plants.pt is missing, the script falls back to the stock yolo11n.pt (COCO classes)
and says so on screen and in the log. Those are NOT custom-trained weights.
"""

import argparse
import csv
import json
import os
import threading
import time
from collections import Counter
from datetime import datetime

import cv2
import numpy as np

from soil_forecast import SoilForecaster

# ---------------- Config ----------------
SERIAL_PORT = "/dev/ttyACM0"
BAUD = 9600
CAMERA_INDEX = 0
FRAME_W, FRAME_H = 640, 480

CUSTOM_WEIGHTS = "weights/agro_plants.pt"
FALLBACK_WEIGHTS = "yolo11n.pt"
YOLO_CONF = 0.40
YOLO_IMGSZ = 320                 # lower = faster on the Pi

SSD_MODEL = "models/ssd.tflite"
SSD_LABELS = "models/labels.txt"
SSD_CONF = 0.50
RUN_BOTH = False

SOIL_PERIOD_S = 600               # take a probe reading this often (feeds the forecast)
SOIL_THRESHOLD = 35              # match SOIL_WATER_BELOW in the Arduino sketch
FORECAST_FILE = "logs/soil_episodes.json"

LOG_INTERVAL_S = 10              # one inventory row per window
LOG_FILE = "logs/inventory.csv"
DASHBOARD_URL = os.environ.get("AGRO_DASHBOARD_URL", "")   # e.g. https://your-dashboard/api/inventory
DASHBOARD_KEY = os.environ.get("AGRO_DASHBOARD_KEY", "")   # keep keys out of the code


# ---------------- Arduino link ----------------
class ArduinoLink:
    FIELDS = ["soil", "temp", "hum", "water", "dist", "batt", "pump", "auto"]

    def __init__(self, port, baud):
        import serial
        self.ser = serial.Serial(port, baud, timeout=1)
        time.sleep(2)                       # Uno resets when the port opens
        self.latest = {}
        self._run = True
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while self._run:
            try:
                line = self.ser.readline().decode(errors="ignore").strip()
            except Exception:
                continue
            if line.startswith("DATA,"):
                parts = line.split(",")[1:]
                if len(parts) == len(self.FIELDS):
                    try:
                        self.latest = {k: float(v) for k, v in zip(self.FIELDS, parts)}
                    except ValueError:
                        pass

    def send(self, cmd):
        self.ser.write((cmd.strip() + "\n").encode())

    def close(self):
        self._run = False
        self.ser.close()


# ---------------- Detectors ----------------
class YoloDetector:
    def __init__(self):
        from ultralytics import YOLO
        if os.path.exists(CUSTOM_WEIGHTS):
            self.weights, self.custom = CUSTOM_WEIGHTS, True
        else:
            self.weights, self.custom = FALLBACK_WEIGHTS, False
            print(f"[YOLO] {CUSTOM_WEIGHTS} not found. Using stock {FALLBACK_WEIGHTS} (not custom-trained).")
        self.model = YOLO(self.weights)

    def detect(self, frame):
        r = self.model(frame, conf=YOLO_CONF, imgsz=YOLO_IMGSZ, verbose=False)[0]
        out = []
        for b in r.boxes:
            x1, y1, x2, y2 = [int(v) for v in b.xyxy[0].tolist()]
            out.append((self.model.names[int(b.cls)], float(b.conf), (x1, y1, x2, y2), "yolo"))
        return out


class SSDDetector:
    def __init__(self):
        try:
            from tflite_runtime.interpreter import Interpreter
        except ImportError:
            try:
                from ai_edge_litert.interpreter import Interpreter
            except ImportError:
                from tensorflow.lite.python.interpreter import Interpreter
        self.interp = Interpreter(model_path=SSD_MODEL)
        self.interp.allocate_tensors()
        self.inp = self.interp.get_input_details()[0]
        self.outs = self.interp.get_output_details()
        self.h, self.w = self.inp["shape"][1:3]
        self.labels = []
        if os.path.exists(SSD_LABELS):
            self.labels = [l.strip() for l in open(SSD_LABELS) if l.strip()]

    def detect(self, frame):
        img = cv2.resize(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB), (self.w, self.h))
        if self.inp["dtype"] == np.float32:
            data = (np.float32(img) - 127.5) / 127.5
        else:
            data = img
        self.interp.set_tensor(self.inp["index"], np.expand_dims(data, 0))
        self.interp.invoke()
        # Standard TF detection export order: boxes, classes, scores, count.
        # If your model orders them differently, change these indices.
        boxes = self.interp.get_tensor(self.outs[0]["index"])[0]
        classes = self.interp.get_tensor(self.outs[1]["index"])[0]
        scores = self.interp.get_tensor(self.outs[2]["index"])[0]
        fh, fw = frame.shape[:2]
        out = []
        for box, c, s in zip(boxes, classes, scores):
            if s < SSD_CONF:
                continue
            ymin, xmin, ymax, xmax = box
            ci = int(c)
            name = self.labels[ci] if ci < len(self.labels) else str(ci)
            out.append((name, float(s),
                        (int(xmin * fw), int(ymin * fh), int(xmax * fw), int(ymax * fh)), "ssd"))
        return out


# ---------------- Helpers ----------------
def draw(frame, dets, tag):
    for name, conf, (x1, y1, x2, y2), src in dets:
        color = (0, 200, 0) if src == "yolo" else (0, 160, 255)
        cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
        cv2.putText(frame, f"{name} {conf:.2f} [{src}]", (x1, max(15, y1 - 5)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1)
    cv2.putText(frame, tag, (8, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)


def log_row(counts, sensors, weights_tag):
    os.makedirs(os.path.dirname(LOG_FILE), exist_ok=True)
    new = not os.path.exists(LOG_FILE)
    with open(LOG_FILE, "a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["timestamp", "counts", "soil", "temp", "hum", "water", "batt", "next_water_min", "model"])
        w.writerow([datetime.now().isoformat(timespec="seconds"), json.dumps(counts),
                    sensors.get("soil", ""), sensors.get("temp", ""), sensors.get("hum", ""),
                    sensors.get("water", ""), sensors.get("batt", ""),
                    sensors.get("next_water_min", ""), weights_tag])


def post_dashboard(counts, sensors):
    if not DASHBOARD_URL:
        return
    try:
        import requests
        requests.post(DASHBOARD_URL,
                      json={"time": datetime.now().isoformat(timespec="seconds"),
                            "counts": counts, "sensors": sensors},
                      headers={"Authorization": f"Bearer {DASHBOARD_KEY}"} if DASHBOARD_KEY else {},
                      timeout=3)
    except Exception as e:
        print("[dashboard] post failed:", e)


# ---------------- Main ----------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--show", action="store_true", help="show a preview window")
    ap.add_argument("--no-serial", action="store_true", help="run without the Arduino")
    ap.add_argument("--auto", action="store_true", help="send AUTO:1 to the Arduino on start")
    args = ap.parse_args()

    link = None
    if not args.no_serial:
        try:
            link = ArduinoLink(SERIAL_PORT, BAUD)
            if args.auto:
                link.send("AUTO:1")
        except Exception as e:
            print("[serial] could not open", SERIAL_PORT, "-", e, "(continuing without Arduino)")

    yolo = YoloDetector()
    ssd = SSDDetector() if os.path.exists(SSD_MODEL) else None
    if ssd is None:
        print(f"[SSD] {SSD_MODEL} not found. Running YOLO only.")
    weights_tag = f"{yolo.weights} ({'custom' if yolo.custom else 'stock'})"

    cap = cv2.VideoCapture(CAMERA_INDEX)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, FRAME_W)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, FRAME_H)
    if not cap.isOpened():
        raise SystemExit("Webcam not found. Check CAMERA_INDEX.")

    forecaster = SoilForecaster(SOIL_THRESHOLD, dt_s=SOIL_PERIOD_S)
    forecaster.load(FORECAST_FILE)
    next_soil_at = time.time()
    soil_read_at = None
    next_water_min = ""

    window_max = Counter()
    window_start = time.time()
    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                continue

            dets = yolo.detect(frame)
            if ssd and (RUN_BOTH or not dets):
                dets += ssd.detect(frame)

            # inventory: highest per-frame count of each crop within the window
            window_max |= Counter(d[0] for d in dets)

            # periodic probe reading -> soil drying forecast
            if link and soil_read_at is None and time.time() >= next_soil_at:
                link.send("SOIL")                       # Arduino lowers the probe and reads
                soil_read_at = time.time() + 6          # give it time to finish
                next_soil_at += SOIL_PERIOD_S
            if link and soil_read_at and time.time() >= soil_read_at:
                soil_read_at = None
                d = link.latest
                if d.get("soil", -1) >= 0:
                    forecaster.add_reading(time.time(), d["soil"], d.get("temp"), d.get("hum"))
                    forecaster.save(FORECAST_FILE)
                    secs = forecaster.predict()
                    next_water_min = "" if secs is None else round(secs / 60, 1)

            if time.time() - window_start >= LOG_INTERVAL_S:
                counts = dict(window_max)
                sensors = dict(link.latest) if link else {}
                sensors["next_water_min"] = next_water_min
                log_row(counts, sensors, weights_tag)
                post_dashboard(counts, sensors)
                print(datetime.now().strftime("%H:%M:%S"), counts, sensors)
                window_max.clear()
                window_start = time.time()

            if args.show:
                draw(frame, dets, weights_tag)
                cv2.imshow("AGRO", frame)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break
    except KeyboardInterrupt:
        pass
    finally:
        cap.release()
        cv2.destroyAllWindows()
        if link:
            link.send("S")
            link.close()


if __name__ == "__main__":
    main()
