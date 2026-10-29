/*
  AGRO - Arduino Uno firmware
  Libraries (Library Manager): Servo (built in), LiquidCrystal I2C, DHT sensor library (Adafruit) + Adafruit Unified Sensor

  Serial protocol (9600 baud, newline-terminated), Pi -> Arduino:
    F  B  L  R  S        drive forward / back / steer left / steer right / stop
    C                    steering off (spring returns wheels to center)
    SPD:<0-255>          drive speed
    AUTO:1  AUTO:0       autonomous obstacle avoidance on/off
    SOIL                 take a soil reading now (probe down, read, probe up)
    PROBE:D  PROBE:U     probe down / up
    PUMP:<ms>            run pump for <ms> (capped by MAX_PUMP_MS)
    WATERMODE:1/0        auto watering from soil reading on/off
  Arduino -> Pi, every 2 s:
    DATA,<soil%>,<tempC>,<hum%>,<waterLevel%>,<distCm>,<battV>,<pump>,<auto>
*/

#include <Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

// ---------- Pins ----------
#define TRIG_PIN   2
#define ECHO_PIN   7
#define ENA        3    // drive motor speed (PWM)
#define IN1        4
#define IN2        6
#define ENB        5    // steering motor speed (PWM)
#define IN3        8
#define IN4        11
#define PROBE_PIN  10
#define PUMP_PIN   12   // relay, active LOW
#define DHT_PIN    13
#define SOIL_PIN   A0
#define WATER_PIN  A1
#define BATT_PIN   A2

// ---------- Calibration (CHANGE THESE) ----------
const int SOIL_DRY_RAW = 620;   // reading with probe in air
const int SOIL_WET_RAW = 260;   // reading with probe in water
const int WATER_EMPTY_RAW = 0;
const int WATER_FULL_RAW  = 500;
const float BATT_DIVIDER = 3.0; // (R1+R2)/R2 of your divider, 3.0 = 20k over 10k

const int STEER_PWM = 255;   // steering motor power, lower it if it strains the linkage
const int PROBE_UP = 20, PROBE_DOWN = 110;

const int   OBSTACLE_CM      = 25;
const int   SOIL_WATER_BELOW = 35;       // % moisture that triggers watering
const int   SOIL_STOP_ABOVE  = 55;       // % moisture to stop (hysteresis)
const unsigned long MAX_PUMP_MS     = 8000;
const unsigned long WATER_COOLDOWN  = 600000UL;  // 10 min between auto waterings
const unsigned long SOIL_INTERVAL   = 300000UL;  // auto soil check every 5 min
const unsigned long FAILSAFE_MS     = 1500;      // manual drive stops if Pi goes quiet

// ---------- Objects ----------
Servo probe;
LiquidCrystal_I2C lcd(0x27, 16, 2);  // try 0x3F if blank
DHT dht(DHT_PIN, DHT22);

// ---------- State ----------
int driveSpeed = 180;
bool autoMode = false;
bool autoWater = true;
bool moving = false;
bool pumpOn = false;
unsigned long pumpOffAt = 0;
unsigned long lastCmd = 0, lastReport = 0, lastLcd = 0;
unsigned long lastSoilCheck = 0, lastWatered = 0;
int soilPct = -1;
float tempC = NAN, hum = NAN;

// ---------- Helpers ----------
void driveForward() { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);  analogWrite(ENA, driveSpeed); moving = true; }
void driveBack()    { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); analogWrite(ENA, driveSpeed); moving = true; }
// Steering motor: hold = turn, release = spring returns wheels to center.
// If left/right are swapped on your car, swap the IN3/IN4 levels below.
void steerLeft()   { digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);  analogWrite(ENB, STEER_PWM); }
void steerRight()  { digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); analogWrite(ENB, STEER_PWM); }
void steerOff()    { digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW);  analogWrite(ENB, 0); }
void driveStop()    { digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);  analogWrite(ENA, 0);          moving = false; }

int readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long d = pulseIn(ECHO_PIN, HIGH, 25000UL);
  if (d == 0) return 400;  // no echo = nothing close
  return (int)(d * 0.0343 / 2);
}

int avgAnalog(uint8_t pin, uint8_t n = 10) {
  long s = 0;
  for (uint8_t i = 0; i < n; i++) { s += analogRead(pin); delay(5); }
  return s / n;
}

int rawToPct(int raw, int lo, int hi) {   // lo -> 0%, hi -> 100%
  return constrain(map(raw, lo, hi, 0, 100), 0, 100);
}

void pumpStart(unsigned long ms) {
  ms = min(ms, MAX_PUMP_MS);
  digitalWrite(PUMP_PIN, LOW);            // relay ON
  pumpOn = true;
  pumpOffAt = millis() + ms;
}
void pumpStop() { digitalWrite(PUMP_PIN, HIGH); pumpOn = false; }

int waterLevelPct() { return rawToPct(avgAnalog(WATER_PIN, 5), WATER_EMPTY_RAW, WATER_FULL_RAW); }

// Lower probe, let it settle, average reading, raise probe. Blocks ~2.5 s, only call when stopped.
void takeSoilReading() {
  driveStop();
  probe.write(PROBE_DOWN);
  delay(1500);
  int raw = avgAnalog(SOIL_PIN, 15);
  probe.write(PROBE_UP);
  soilPct = rawToPct(raw, SOIL_DRY_RAW, SOIL_WET_RAW);
  lastSoilCheck = millis();

  // sensor-triggered watering (skips if reservoir is nearly empty)
  if (autoWater && soilPct < SOIL_WATER_BELOW && !pumpOn &&
      millis() - lastWatered > WATER_COOLDOWN && waterLevelPct() > 10) {
    pumpStart(MAX_PUMP_MS / 2);
    lastWatered = millis();
  }
}

void avoidObstacle() {
  driveStop();
  delay(150);
  steerOff();
  driveBack(); delay(700); driveStop();
  if (random(2)) steerLeft(); else steerRight();   // Ackermann: steer, then move to swing around
  delay(300);
  driveForward(); delay(900); driveStop();
  steerOff();
}

// ---------- Serial ----------
void handleCommand(String c) {
  c.trim();
  if (c.length() == 0) return;
  lastCmd = millis();

  if      (c == "F") { if (!autoMode) driveForward(); }
  else if (c == "B") { if (!autoMode) driveBack(); }
  else if (c == "S") { driveStop(); }
  else if (c == "L") { steerLeft(); }
  else if (c == "R") { steerRight(); }
  else if (c == "C") { steerOff(); }
  else if (c.startsWith("SPD:"))  { driveSpeed = constrain(c.substring(4).toInt(), 0, 255); }
  else if (c.startsWith("AUTO:")) { autoMode = c.substring(5).toInt() == 1; if (!autoMode) driveStop(); }
  else if (c.startsWith("WATERMODE:")) { autoWater = c.substring(10).toInt() == 1; }
  else if (c == "SOIL")    { takeSoilReading(); }
  else if (c == "PROBE:D") { probe.write(PROBE_DOWN); }
  else if (c == "PROBE:U") { probe.write(PROBE_UP); }
  else if (c.startsWith("PUMP:")) { if (waterLevelPct() > 10) pumpStart(c.substring(5).toInt()); }
}

void report(int dist) {
  float battV = analogRead(BATT_PIN) * (5.0 / 1023.0) * BATT_DIVIDER;
  Serial.print("DATA,");
  Serial.print(soilPct);   Serial.print(',');
  Serial.print(tempC, 1);  Serial.print(',');
  Serial.print(hum, 0);    Serial.print(',');
  Serial.print(waterLevelPct()); Serial.print(',');
  Serial.print(dist);      Serial.print(',');
  Serial.print(battV, 2);  Serial.print(',');
  Serial.print(pumpOn);    Serial.print(',');
  Serial.println(autoMode);
}

void updateLcd() {
  lcd.setCursor(0, 0);
  lcd.print("Soil:");
  if (soilPct < 0) lcd.print("--"); else lcd.print(soilPct);
  lcd.print("% W:");
  lcd.print(waterLevelPct());
  lcd.print("%   ");
  lcd.setCursor(0, 1);
  lcd.print(isnan(tempC) ? 0 : (int)tempC);
  lcd.print("C ");
  lcd.print(isnan(hum) ? 0 : (int)hum);
  lcd.print("% ");
  lcd.print(autoMode ? "AUTO" : "MAN ");
  lcd.print(pumpOn ? " P" : "  ");
}

// ---------- Setup / loop ----------
void setup() {
  digitalWrite(PUMP_PIN, HIGH);          // relay OFF before pinMode to avoid a glitch
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT); pinMode(ENA, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT); pinMode(ENB, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT);
  driveStop();
  steerOff();

  probe.attach(PROBE_PIN);  probe.write(PROBE_UP);

  Serial.begin(9600);
  Serial.setTimeout(20);
  dht.begin();
  lcd.init(); lcd.backlight();
  lcd.print("AGRO starting...");
  randomSeed(analogRead(A3));
  delay(1000);
  lcd.clear();
}

void loop() {
  // commands from the Pi
  if (Serial.available()) handleCommand(Serial.readStringUntil('\n'));

  // pump timeout
  if (pumpOn && millis() >= pumpOffAt) pumpStop();

  // obstacle check (only while moving, to keep the loop fast)
  int dist = 400;
  if (moving || autoMode) {
    dist = readDistanceCm();
    if (autoMode) {
      if (dist < OBSTACLE_CM) avoidObstacle();
      else if (!moving) driveForward();
    } else if (dist < OBSTACLE_CM) {
      driveStop();                       // manual mode: just stop, don't override the steering
    }
  }

  // manual drive failsafe if the Pi stops talking
  if (!autoMode && (moving || digitalRead(IN3) != digitalRead(IN4)) && millis() - lastCmd > FAILSAFE_MS) { driveStop(); steerOff(); }

  // periodic soil check (only while stopped)
  if (!moving && autoWater && millis() - lastSoilCheck > SOIL_INTERVAL) takeSoilReading();

  // slow tasks
  if (millis() - lastLcd > 1000) {
    float t = dht.readTemperature(), h = dht.readHumidity();
    if (!isnan(t)) tempC = t;
    if (!isnan(h)) hum = h;
    updateLcd();
    lastLcd = millis();
  }
  if (millis() - lastReport > 2000) {
    report(dist);
    lastReport = millis();
  }
}
