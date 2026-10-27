// Smart balance board: ESP32-C6 (Pcbcupid Glyph C6) + MPU6050.
// ESP32-C6 has no Classic Bluetooth (no BluetoothSerial), so data is sent
// over BLE using the Nordic UART Service (NUS) as notifications.
// Line format: gx,gy,gz,ax,ay,az,timestamp
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <Wire.h>
#include <MPU6050.h>  // jrowberg i2cdevlib MPU6050 (forced via --library at build time)

#define DEVICE_NAME      "smart board"
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define I2C_SDA 17  // wiring found by I2C scan
#define I2C_SCL 16
#define SAMPLE_MS 50

MPU6050 mpu;
BLECharacteristic *txChar;
volatile bool bleConnected = false;
bool mpuOk = false;

// The board's sensor reports WHO_AM_I 0x70 (MPU6500-style clone), which the
// library's testConnection() rejects. Accept the common compatible IDs.
bool mpuPresent() {
  uint8_t who = 0;
  Wire.beginTransmission(0x68);
  Wire.write(0x75);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((uint8_t)0x68, (uint8_t)1) != 1) return false;
  who = Wire.read();
  return who == 0x68 || who == 0x70 || who == 0x71 || who == 0x72 || who == 0x73 || who == 0x98;
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override { bleConnected = true; }
  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    BLEDevice::startAdvertising();  // allow reconnect
  }
};

void sendLine(const String &line) {
  if (bleConnected) {
    txChar->setValue((uint8_t *)line.c_str(), line.length());
    txChar->notify();
  }
  Serial.print(line);
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  // Don't stall the 50 ms loop when no serial monitor is open on the PC.
  Serial.setTxTimeoutMs(0);
#endif

  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(185);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  BLEService *service = server->createService(NUS_SERVICE_UUID);
  txChar = service->createCharacteristic(NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->addDescriptor(new BLE2902());
  service->createCharacteristic(NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  service->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  mpu.initialize();
  mpuOk = mpuPresent();
  Serial.println(mpuOk ? "MPU6050 initialized" : "MPU6050 connection failed");
}

void loop() {
  static uint32_t next = 0;
  uint32_t now = millis();
  if ((int32_t)(now - next) < 0) { delay(1); return; }
  next = now + SAMPLE_MS;

  if (!mpuOk) {
    // Keep BLE alive and report the problem instead of hanging.
    static uint32_t lastRetry = 0;
    if (now - lastRetry > 1000) {
      lastRetry = now;
      mpu.initialize();
      mpuOk = mpuPresent();
      if (!mpuOk) sendLine("ERR,MPU6050 not found\n");
    }
    return;
  }

  int16_t ax, ay, az, gx, gy, gz;
  mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);

  char line[80];
  snprintf(line, sizeof(line), "%d,%d,%d,%d,%d,%d,%lu\n",
           gx, gy, gz, ax, ay, az, (unsigned long)now);
  sendLine(line);
}
