/*
  AGRO - drive + obstacle avoidance + probe test
  - Drives forward and avoids obstacles
  - Every 10 s of driving: stops, lowers the probe (servo), shows the soil result, raises the probe, continues
  - LCD shows MOCK data while driving (soil sensor is disconnected)
  - While the probe is down, the LCD says DRY

  Library needed: LiquidCrystal I2C  (Servo is built in)

  Pins (same layout as before):
    D2  HC-SR04 TRIG        D7  HC-SR04 ECHO
    D3  L298N ENA           D4  IN1     D6  IN2      (drive motor)
    D5  L298N ENB           D8  IN3     D11 IN4      (steering motor)
    D10 probe servo signal (orange)
    D12 pump relay (kept OFF here)
    A0  soil moisture AO
    A4  LCD SDA             A5  LCD SCL
*/

#include <Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------- Pins ----------
#define TRIG_PIN  2
#define ECHO_PIN  7
#define ENA       3
#define IN1       4
#define IN2       6
#define ENB       5
#define IN3       8
#define IN4       11
#define PROBE_PIN 10
#define PUMP_PIN  12
#define SOIL_PIN  A0

// ---------- Settings (adjust these) ----------
const bool USE_MOCK_SOIL = true;         // true: always shows DRY. false: reads the real sensor on A0
const int  SOIL_DRY_ABOVE_RAW = 500;     // real sensor: raw reading above this = DRY

const unsigned long DRIVE_TIME_MS   = 10000;  // drive this long between probe stops
const unsigned long PROBE_SETTLE_MS = 600;    // wait after the probe reaches the soil
const unsigned long RESULT_SHOW_MS  = 3000;   // keep the DRY/WET message on screen

const int PROBE_UP = 175, PROBE_DOWN = 5;      // servo angles (reversed + longer travel), calibrate on your actuator
const int PROBE_STEP_MS = 5;                  // delay per degree, higher = slower probe
const int DRIVE_SPEED = 180;                  // 0-255
const int STEER_PWM   = 255;                  // 0-255, lower it if the steering strains
const int OBSTACLE_CM = 25;

// Mock readings shown on the LCD while driving
const int MOCK_SOIL = 42, MOCK_WATER = 80, MOCK_TEMP = 29, MOCK_HUM = 65;

// ---------- Objects ----------
Servo probe;
LiquidCrystal_I2C lcd(0x27, 16, 2);   // try 0x3F if the screen stays blank

unsigned long driveStart = 0;

// ---------- Motor helpers ----------
void driveForward() { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);  analogWrite(ENA, DRIVE_SPEED); }
void driveBack()    { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); analogWrite(ENA, DRIVE_SPEED); }
void driveStop()    { digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);  analogWrite(ENA, 0); }

// Steering: holding = turn, release = spring returns wheels to center.
// If left/right are swapped on your car, swap the HIGH/LOW levels below.
void steerLeft()  { digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);  analogWrite(ENB, STEER_PWM); }
void steerRight() { digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); analogWrite(ENB, STEER_PWM); }
void steerOff()   { digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW);  analogWrite(ENB, 0); }

int readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long d = pulseIn(ECHO_PIN, HIGH, 25000UL);
  if (d == 0) return 400;               // no echo = nothing close
  return (int)(d * 0.0343 / 2);
}

void avoidObstacle() {
  driveStop();
  delay(150);
  steerOff();
  driveBack(); delay(700); driveStop();
  if (random(2)) steerLeft(); else steerRight();   // steer, then move to swing around
  delay(300);
  driveForward(); delay(900); driveStop();
  steerOff();
}

// Move the probe gradually instead of jumping: smoother, less current spike, no brownout
int probeAngle = PROBE_UP;
void moveProbe(int target) {
  while (probeAngle != target) {
    probeAngle += (target > probeAngle) ? 1 : -1;
    probe.write(probeAngle);
    delay(PROBE_STEP_MS);
  }
}

// ---------- LCD helper (pads each line to 16 chars so old text is wiped) ----------
char lastLine[2][17] = {"", ""};
void lcdLine(uint8_t row, const char *text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);
  if (strcmp(buf, lastLine[row]) == 0) return;   // same text: don't touch the LCD, no flicker
  strcpy(lastLine[row], buf);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void showMockData() {
  char l1[17], l2[17];
  snprintf(l1, sizeof(l1), "S:%d%% W:%d%%", MOCK_SOIL, MOCK_WATER);
  snprintf(l2, sizeof(l2), "T:%dC H:%d%% DRV", MOCK_TEMP, MOCK_HUM);
  lcdLine(0, l1);
  lcdLine(1, l2);
}

bool soilIsDry() {
  if (USE_MOCK_SOIL) return true;
  long s = 0;
  for (int i = 0; i < 15; i++) { s += analogRead(SOIL_PIN); delay(5); }
  return (s / 15) > SOIL_DRY_ABOVE_RAW;
}

// Stop, lower the probe, show the result, raise the probe
void probeStop() {
  driveStop();
  steerOff();

  lcdLine(0, "Probe going down");
  lcdLine(1, "");
  moveProbe(PROBE_DOWN);
  lcdLine(0, "Reading soil...");
  delay(PROBE_SETTLE_MS);

  bool dry = soilIsDry();
  lcdLine(0, dry ? "Soil: DRY" : "Soil: WET");
  lcdLine(1, "Probe is DOWN");
  Serial.println(dry ? "SOIL,DRY" : "SOIL,WET");
  delay(RESULT_SHOW_MS);

  lcdLine(0, "Probe going up");
  lcdLine(1, "");
  moveProbe(PROBE_UP);
  delay(300);
}

// ---------- Setup / loop ----------
void setup() {
  digitalWrite(PUMP_PIN, HIGH);          // relay OFF (active LOW), set before pinMode
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT); pinMode(ENA, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT); pinMode(ENB, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT);
  driveStop(); steerOff();

  probe.attach(PROBE_PIN);
  probe.write(PROBE_UP);
  probeAngle = PROBE_UP;

  Serial.begin(9600);
  randomSeed(analogRead(A3));

  lcd.init(); lcd.backlight();
  lcdLine(0, "AGRO test");
  lcdLine(1, "Starting in 3s");
  delay(3000);

  showMockData();
  driveStart = millis();
}

void loop() {
  // time to stop and probe?
  if (millis() - driveStart >= DRIVE_TIME_MS) {
    probeStop();
    showMockData();
    driveStart = millis();     // the 10 s counts driving time only
    return;
  }

  // obstacle avoidance
  int dist = readDistanceCm();
  if (dist < OBSTACLE_CM) avoidObstacle();
  else                    driveForward();
}
