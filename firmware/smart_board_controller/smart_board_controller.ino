/*
  SMART BOARD - GAME CONTROLLER FIRMWARE (ESP32-C6 + MPU6050)
  ===========================================================
  The board becomes a Bluetooth keyboard. Lean to press W/A/S/D (diagonals too).

  * Pair once:  Windows Settings > Bluetooth > Add device > "Smart Board"
  * Lean ~4 degrees  -> key pressed (W fwd, S back, A left, D right)
  * Board edge touches the ground -> keys let go + a warning
  * 3rd touch -> LOCKED: keys off until you come back to centre (max 5 seconds)
  * 20 seconds with no touch -> warnings reset to 0

  Recalibrate (only needed if you remount the sensor):
    hold BOOT for 2 s, or type  c  in Serial Monitor (115200).
    You then get 4 s to step on -> stand LEVEL and still -> LEAN FORWARD firmly and hold.
  Other Serial commands:  d = live data on/off,  r = factory calibration,  ? = help

  Only the Arduino ESP32 core is needed (BLE + Wire + Preferences are built in).
  Keep board_logic.h in the same folder as this file.
*/

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include "board_logic.h"

// ===================== SETTINGS - change these if needed =====================
// I2C pins of the MPU6050. -1 = the board's default pins (what Wire.begin() uses).
// If your working firmware had  Wire.begin(X, Y)  put X and Y here.
#define I2C_SDA  17
#define I2C_SCL  16
#define BOOT_PIN  9          // BOOT button on ESP32-C6
#define LED_PIN  -1          // set to your board's LED GPIO for blink feedback (-1 = none)
#define LED_ON_LEVEL HIGH
static const char *DEVICE_NAME = "Smart Board";

// Factory calibration, measured from your recordings (sensor on its edge, X axis up).
static const sb::V3 FACTORY_UP  = {0.9996f, 0.0190f, -0.0199f};
static const sb::V3 FACTORY_FWD = {0.0114f, 0.3749f,  0.9270f};
// =============================================================================

// HID key codes
static const uint8_t HID_W = 0x1A, HID_A = 0x04, HID_S = 0x16, HID_D = 0x07;

// Standard keyboard report (Report ID 1): modifiers, reserved, 6 keys + LED output
static const uint8_t REPORT_MAP[] = {
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
  0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
  0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
  0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
  0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
  0xC0
};

sb::BoardLogic logic;
Preferences prefs;
BLEHIDDevice *hid = nullptr;
BLECharacteristic *keyInput = nullptr;
volatile bool bleConnected = false;
volatile bool bleJustConnected = false;

uint8_t mpuAddr = 0;
uint8_t sentKeys = 0xFF;
bool streamData = false;
uint32_t calStartAt = 0;          // recalibration countdown (0 = none)
uint32_t bootDownSince = 0;
bool bootFired = false;
uint16_t sensorFails = 0;
uint32_t lastSample = 0, lastPrint = 0;

// LED feedback
uint8_t ledBlinksLeft = 0;
uint32_t ledNext = 0;
bool ledState = false;

// ------------------------------------------------------------------ BLE
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *) override { bleConnected = true; bleJustConnected = true; }
  void onDisconnect(BLEServer *) override {
    bleConnected = false;
    BLEDevice::startAdvertising();          // let it reconnect
  }
};

void setupBLE() {
  BLEDevice::init(DEVICE_NAME);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  hid = new BLEHIDDevice(server);
  keyInput = hid->inputReport(1);
  hid->outputReport(1);                     // keyboard LEDs (ignored, but hosts expect it)
  hid->manufacturer()->setValue("Smart Board");
  hid->pnp(0x02, 0x303A, 0x4001, 0x0100);
  hid->hidInfo(0x00, 0x01);

  BLESecurity *security = new BLESecurity();
  security->setAuthenticationMode(true, false, true);   // bond, no PIN, secure connections
  security->setCapability(ESP_IO_CAP_NONE);

  hid->reportMap((uint8_t *)REPORT_MAP, sizeof(REPORT_MAP));
  hid->startServices();
  hid->setBatteryLevel(100);

  BLEAdvertising *adv = server->getAdvertising();
  adv->setAppearance(HID_KEYBOARD);
  adv->addServiceUUID(hid->hidService()->getUUID());
  adv->setScanResponse(true);
  adv->start();
}

void sendKeys(uint8_t k) {
  if (!bleConnected || !keyInput) return;
  uint8_t report[8] = {0};
  int i = 2;
  if (k & sb::KEY_W) report[i++] = HID_W;
  if (k & sb::KEY_A) report[i++] = HID_A;
  if (k & sb::KEY_S) report[i++] = HID_S;
  if (k & sb::KEY_D) report[i++] = HID_D;
  keyInput->setValue(report, sizeof(report));
  keyInput->notify();
}

// ------------------------------------------------------------------ MPU6050
bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(mpuAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool setupMPU() {
  for (uint8_t a : {0x68, 0x69}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { mpuAddr = a; break; }
  }
  if (!mpuAddr) {
    Serial.print("MPU6050 not found. I2C devices seen:");
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
    }
    Serial.println("  -> check I2C_SDA / I2C_SCL at the top of the file");
    return false;
  }
  bool ok = writeReg(0x6B, 0x00)      // wake up
         && writeReg(0x1A, 0x03)      // low-pass filter ~44 Hz
         && writeReg(0x19, 0x09)      // 100 Hz
         && writeReg(0x1B, 0x00)      // gyro +-250 dps
         && writeReg(0x1C, 0x00);     // accel +-2 g
  Serial.printf("MPU6050 at 0x%02X %s\n", mpuAddr, ok ? "ready" : "setup failed");
  return ok;
}

bool readAccel(float &ax, float &ay, float &az) {
  Wire.beginTransmission(mpuAddr);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(mpuAddr, (size_t)6) != 6) return false;
  int16_t x = (Wire.read() << 8) | Wire.read();
  int16_t y = (Wire.read() << 8) | Wire.read();
  int16_t z = (Wire.read() << 8) | Wire.read();
  ax = x; ay = y; az = z;
  return !(x == 0 && y == 0 && z == 0);
}

// ------------------------------------------------------------------ calibration storage
void saveCalibration() {
  float c[6] = {logic.up().x, logic.up().y, logic.up().z, logic.fwdSig().x, logic.fwdSig().y, logic.fwdSig().z};
  prefs.putBytes("cal", c, sizeof(c));
}

void loadCalibration() {
  float c[6];
  if (prefs.getBytesLength("cal") == sizeof(c) && prefs.getBytes("cal", c, sizeof(c)) == sizeof(c)) {
    logic.setCalibration(sb::v(c[0], c[1], c[2]), sb::v(c[3], c[4], c[5]));
    Serial.println("Using your saved calibration.");
  } else {
    logic.setCalibration(FACTORY_UP, FACTORY_FWD);
    Serial.println("Using factory calibration (from your recordings).");
  }
}

void startRecalibration() {
  calStartAt = millis() + 4000;
  sendKeys(0); sentKeys = 0;
  Serial.println("\n== RECALIBRATION == step on in 4 s, stand LEVEL and still, then LEAN FORWARD and hold.");
}

// ------------------------------------------------------------------ feedback
void blink(uint8_t n) { ledBlinksLeft = n * 2; ledNext = millis(); }

void updateLed() {
  if (LED_PIN < 0) return;
  uint32_t now = millis();
  bool on;
  if (calStartAt || logic.mode() == sb::CAL_LEVEL || logic.mode() == sb::CAL_FORWARD) {
    on = (now / 500) % 2;                                  // slow blink while calibrating
  } else if (logic.mode() == sb::LOCKED) {
    on = true;                                             // solid while locked
  } else if (ledBlinksLeft) {
    if (now >= ledNext) { ledState = !ledState; ledBlinksLeft--; ledNext = now + 150; }
    on = ledState;
  } else {
    on = !bleConnected && (now / 1000) % 2;                // slow pulse while not paired
  }
  digitalWrite(LED_PIN, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
}

const char *keyName(uint8_t k) {
  static char s[5];
  int i = 0;
  if (k & sb::KEY_W) s[i++] = 'W';
  if (k & sb::KEY_A) s[i++] = 'A';
  if (k & sb::KEY_S) s[i++] = 'S';
  if (k & sb::KEY_D) s[i++] = 'D';
  if (!i) s[i++] = '-';
  s[i] = 0;
  return s;
}

void handleEvent(sb::Event e) {
  switch (e) {
    case sb::EV_LEVEL_SET:   Serial.println("Level saved. Now LEAN FORWARD firmly and hold..."); blink(1); break;
    case sb::EV_CALIBRATED:  saveCalibration(); Serial.println("Calibrated and saved. Play!"); blink(3); break;
    case sb::EV_TOUCH:       Serial.printf("GROUND TOUCH! warning %d/%d\n", logic.warnings(), logic.cfg.maxWarnings);
                             blink(logic.warnings()); break;
    case sb::EV_TOUCH_END:   Serial.println("back up - keys on again"); break;
    case sb::EV_LOCKED:      Serial.println("LOCKED - come back to centre (max 5 s)"); break;
    case sb::EV_UNLOCKED:    Serial.println("Unlocked - warnings reset"); blink(2); break;
    case sb::EV_WARNINGS_CLEARED: Serial.println("20 s clean - warnings reset"); break;
    default: break;
  }
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nSmart Board game controller");
  pinMode(BOOT_PIN, INPUT_PULLUP);
  if (LED_PIN >= 0) pinMode(LED_PIN, OUTPUT);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  while (!setupMPU()) delay(2000);

  prefs.begin("smartboard", false);
  logic.begin();
  loadCalibration();

  setupBLE();
  Serial.println("Bluetooth on - pair \"Smart Board\" from your PC. Type ? for help.");
}

void loop() {
  uint32_t now = millis();

  // serial commands
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'c') startRecalibration();
    else if (c == 'd') { streamData = !streamData; Serial.printf("live data %s\n", streamData ? "ON" : "OFF"); }
    else if (c == 'r') { prefs.remove("cal"); logic.begin(); logic.setCalibration(FACTORY_UP, FACTORY_FWD);
                         Serial.println("Back to factory calibration."); }
    else if (c == '?') Serial.println("c = recalibrate, d = live data on/off, r = factory calibration");
  }

  // BOOT button held 2 s -> recalibrate
  if (digitalRead(BOOT_PIN) == LOW) {
    if (!bootDownSince) bootDownSince = now ? now : 1;
    else if (!bootFired && now - bootDownSince > 2000) { startRecalibration(); bootFired = true; }
  } else { bootDownSince = 0; bootFired = false; }

  if (calStartAt && (int32_t)(now - calStartAt) >= 0) {
    calStartAt = 0;
    logic.recalibrate();
    Serial.println("Calibrating: stand LEVEL and still...");
  }

  // 100 Hz sensor + logic
  if (now - lastSample >= 10) {
    lastSample = now;
    float ax, ay, az;
    if (readAccel(ax, ay, az)) {
      sb::Event e = logic.update(ax, ay, az, now);
      if (e) handleEvent(e);
      sensorFails = 0;
    } else if (++sensorFails == 20) {
      Serial.println("Sensor not responding - keys released");
    }

    uint8_t k = (calStartAt || sensorFails >= 20) ? 0 : logic.keys();
    if (bleJustConnected) { bleJustConnected = false; sentKeys = 0xFF; Serial.println("PC connected"); }
    if (k != sentKeys && bleConnected) { sendKeys(k); sentKeys = k; }
    if (!bleConnected) sentKeys = 0xFF;
  }

  if (streamData && now - lastPrint >= 100) {
    lastPrint = now;
    const char *m[] = {"CAL-LEVEL", "CAL-FWD", "PLAY", "TOUCH", "LOCKED"};
    Serial.printf("tilt %5.1f  fwd %6.1f  right %6.1f  keys %-4s  %-9s warn %d  %s\n",
                  logic.tiltDeg(), logic.fwdDeg(), logic.rightDeg(), keyName(logic.keys()),
                  m[logic.mode()], logic.warnings(), bleConnected ? "BT" : "no-BT");
  }

  updateLed();
}
