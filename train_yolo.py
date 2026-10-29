"""
Hello! This is where you will be fine-tuning YOLO 11n with your own plant photos. I didn't upload my own dataset.

Fine-tune YOLO11n on your own plant photos.
Run this on a PC or Google Colab with a GPU, not on the Pi.

Dataset layout (YOLO format, labeled with Roboflow, LabelImg, or CVAT):
  dataset/
    data.yaml
    images/train/*.jpg   images/val/*.jpg
    labels/train/*.txt   labels/val/*.txt

data.yaml:
  path: dataset
  train: images/train
  val: images/val
  names:
    0: pechay
    1: (add your other classes)

Photo tips: shoot with the same webcam and the same bed the robot will see, at the
heights and angles the robot will use, in different light (morning, noon, shade),
at different growth stages. Start with roughly 100 to 300 labeled photos per class,
then check the validation results and add more where it fails.
"""

import os
import shutil

from ultralytics import YOLO

model = YOLO("yolo11n.pt")
model.train(
    data="dataset/data.yaml",
    epochs=100,
    imgsz=640,
    batch=16,
    patience=20,
    project="runs",
    name="agro_plants",
)

best = os.path.join("runs", "agro_plants", "weights", "best.pt")
os.makedirs("weights", exist_ok=True)
shutil.copy(best, "weights/agro_plants.pt")
print("Saved weights/agro_plants.pt. Copy it to the Pi's pi/weights/ folder.")
