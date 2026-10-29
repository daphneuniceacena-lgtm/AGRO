# AGRO

Autonomous Growth and Resources Observer. An agricultural robot built on an RC car chassis. It drives around, avoids obstacles, takes soil moisture readings with a probe that is lowered into the soil, and waters plants based on the reading.

This repository holds the Arduino firmware and the Raspberry Pi script.

## Hardware

| Part | Role |
|---|---|
| Arduino Uno | Motor, sensor, and actuator control |
| Raspberry Pi 4 | Crop detection (YOLO11n), logging, dashboard. Talks to the Uno over USB serial |
| L298N motor driver | Drive motor (channel A) and steering motor (channel B) |
| SG90 / hobby servo | Lowers and raises the soil probe |
| Capacitive soil moisture sensor | On the probe |
| HC-SR04 | Obstacle detection |
| DHT22 | Air temperature and humidity |
| Water level sensor | Reservoir level |
| Relay + pump | Watering |
| 16x2 I2C LCD | Status display |

## Pin layout (Arduino Uno)

| Pin | Connected to |
|---|---|
| D0 / D1 | Free (USB serial to the Pi) |
| D2 | HC-SR04 TRIG |
| D3 (PWM) | L298N ENA (drive speed) |
| D4 | L298N IN1 |
| D5 (PWM) | L298N ENB (steering power) |
| D6 | L298N IN2 |
| D7 | HC-SR04 ECHO |
| D8 | L298N IN3 |
| D9 | Free |
| D10 (PWM) | Probe servo signal (orange wire) |
| D11 | L298N IN4 |
| D12 | Pump relay IN (active LOW) |
| D13 | DHT22 data (10k pull-up) |
| A0 | Soil moisture sensor AO |
| A1 | Water level sensor |
| A2 | Battery voltage divider |
| A4 / A5 | LCD SDA / SCL |

## Power notes

- Tie all grounds together: Pi, Arduino, L298N, servo, sensors.
- Do not power the servo or pump from the Uno's 5V pin. Use a separate 5V buck converter (3A or more) with a shared ground.
- Add a 470 to 1000 uF capacitor across the servo's 5V and GND, close to the servo.
- Servo wires: brown = GND, red = 5V, orange = signal.
- Steering is a DC motor with a return spring. Holding the motor turns the wheels, releasing it centers them. If left and right are swapped, swap the IN3 / IN4 levels in `steerLeft()` and `steerRight()`.

## Sketches

### `firmware/agro_arduino`

Main firmware. Takes serial commands from the Pi, reports sensor data, runs obstacle avoidance, and does sensor-triggered watering.

Libraries (Arduino Library Manager): `LiquidCrystal I2C`, `DHT sensor library` (Adafruit), `Adafruit Unified Sensor`. `Servo` is built in.

Serial, 9600 baud, newline-terminated.

Pi to Arduino:

| Command | Action |
|---|---|
| `F` `B` `S` | Forward, back, stop |
| `L` `R` `C` | Steer left, steer right, release steering |
| `SPD:<0-255>` | Drive speed |
| `AUTO:1` / `AUTO:0` | Obstacle avoidance driving on/off |
| `SOIL` | Lower probe, read soil, raise probe |
| `PROBE:D` / `PROBE:U` | Probe down / up |
| `PUMP:<ms>` | Run pump (capped at 8 s) |
| `WATERMODE:1` / `WATERMODE:0` | Automatic watering on/off |

Arduino to Pi, every 2 s:

```
DATA,<soil%>,<tempC>,<hum%>,<waterLevel%>,<distCm>,<battV>,<pump>,<auto>
```

### `firmware/agro_drive_probe_test`

Bench test. Drives with obstacle avoidance, stops every 10 s of driving, lowers the probe, shows the soil result on the LCD, raises the probe, and continues. The LCD shows mock data while driving. With `USE_MOCK_SOIL = true` the result is always DRY, so it works without the soil sensor connected.

Libraries: `LiquidCrystal I2C`.

## Raspberry Pi (`pi/`)

| File | Purpose |
|---|---|
| `agro_pi_main.py` | Webcam to YOLO11n, with the TFLite SSD model as a fallback. Counts crops, logs to `logs/inventory.csv`, optionally posts to the dashboard, reads the Arduino `DATA` line |
| `soil_forecast.py` | Soil drying forecast. Fits an exponential drying curve to probe readings and predicts the minutes until the soil reaches the watering threshold |
| `train_yolo.py` | Fine-tunes YOLO11n on your own labeled plant photos (run on a PC or Colab) |
| `requirements.txt` | Python packages |

Put these in place on the Pi:

```
pi/weights/agro_plants.pt    your fine-tuned YOLO11n weights (output of train_yolo.py)
pi/models/ssd.tflite         TFLite SSD model
pi/models/labels.txt         SSD labels, one per line
```

Run: `python3 agro_pi_main.py` (add `--show` for a preview, `--no-serial` without the Arduino, `--auto` to start obstacle-avoidance driving).

If `weights/agro_plants.pt` is missing, the script uses the stock `yolo11n.pt` and labels it as stock in the log and on screen. Dashboard settings go in the `AGRO_DASHBOARD_URL` and `AGRO_DASHBOARD_KEY` environment variables, not in the code.

## Math doc (`docs/`)

| File | Contents |
|---|---|
| `agro_soil_forecast_math.md` | Drying model, how it is fitted, temperature and humidity dependence, how to test it |

## Calibration

Set these constants before real use.

| Constant | How |
|---|---|
| `SOIL_DRY_RAW` / `SOIL_WET_RAW` | Read A0 with the probe in air and in water |
| `SOIL_DRY_ABOVE_RAW` (test sketch) | Raw value above which the soil counts as dry |
| `PROBE_UP` / `PROBE_DOWN` | Servo angles. Test with the probe free first |
| `PROBE_STEP_MS` | Delay per degree. Higher is slower |
| `WATER_EMPTY_RAW` / `WATER_FULL_RAW` | Water level sensor dry and submerged |
| `BATT_DIVIDER` | (R1 + R2) / R2 of the battery divider |
| LCD address | `0x27` by default, try `0x3F` if the screen is blank |

## Upload

1. Install the libraries above.
2. Open the `.ino` file inside its folder. Arduino requires the folder name to match the sketch name.
3. Select Arduino Uno and the correct port, then upload.
