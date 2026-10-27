// I2C scanner for ESP32-C6: finds which pins your MPU6050 is wired to.
// Open Serial Monitor at 115200. Each hit prints the pins, the address and
// the chip ID (WHO_AM_I). 0x68 = genuine MPU6050, 0x70/0x71/0x72/0x98 = compatible clones.
#include <Wire.h>

// SDA, SCL pairs to try (both orders of each are tried)
const int pairs[][2] = {{4, 5}, {6, 7}, {16, 17}, {22, 23}, {2, 3}, {18, 19}, {20, 21}, {10, 11}};

void setup() {
  Serial.begin(115200);
  delay(1500);
}

void scan(int sda, int scl) {
  Wire.end();
  pinMode(sda, INPUT); pinMode(scl, INPUT);
  bool idle = digitalRead(sda) && digitalRead(scl);  // pull-ups present = something is connected
  if (!Wire.begin(sda, scl, 100000)) return;
  Wire.setTimeOut(20);
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() != 0) continue;
    uint8_t who = 0xFF;
    Wire.beginTransmission(a); Wire.write(0x75);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(a, (uint8_t)1) == 1) who = Wire.read();
    Serial.printf("FOUND  SDA=%d SCL=%d  addr=0x%02X  whoami=0x%02X%s\n", sda, scl, a, who,
                  idle ? "" : "  (lines not idle-high: check wiring)");
  }
}

void loop() {
  Serial.println("--- scan start");
  for (auto &p : pairs) { scan(p[0], p[1]); scan(p[1], p[0]); }
  Serial.println("--- scan end (nothing listed = check power, GND and wiring)");
  delay(3000);
}
