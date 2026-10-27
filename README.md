# Smart Board

Turn a balance board into a Bluetooth game controller and a motion-data recorder.
An **ESP32-C6** with an **MPU6050** (gyro + accelerometer) sits inside the board.

- **Game controller:** the board pairs with your PC as a Bluetooth keyboard. Lean to press W/A/S/D (diagonals too). If the board's edge touches the ground, keys release and you get a warning; the 3rd touch locks the keys for up to 5 s.
- **Data streaming:** sends `gx,gy,gz,ax,ay,az,timestamp` over BLE every 50 ms (Nordic UART Service).
- **Labelling app:** record labelled motion (neutral, tilt left, touching right, circle...) into CSV for training a model.

The ESP32-C6 has **no Classic Bluetooth**, so everything uses BLE (no `BluetoothSerial`).

## What's in here

| Path | What it is |
|---|---|
| `firmware/smart_board_controller/` | Game-controller firmware (BLE HID keyboard). Logic is in `board_logic.h` (plain C++). |
| `firmware/smart_board_stream/` | Streams raw sensor lines over BLE (NUS) at 20 Hz. |
| `firmware/i2c_scan/` | Finds which pins your MPU6050 is wired to and its chip ID. |
| `tools/smart_board_logger.py` | Connects to the stream firmware, prints lines live, saves a CSV to your Desktop. |
| `tools/smart_board_labeler.py` | Desktop app (tkinter) to record labelled takes. `--demo` runs without a board. |
| `web/` | The project's landing page. |

## Hardware

- ESP32-C6 board (tested on a Pcbcupid Glyph C6)
- MPU6050 module (MPU6500-type clones that report WHO_AM_I `0x70` also work)
- Wiring used here: **SDA = GPIO17, SCL = GPIO16**, VCC = 3.3 V, GND = GND

Different wiring? Flash `firmware/i2c_scan`, open Serial Monitor at 115200, and put the pins it finds into `I2C_SDA` / `I2C_SCL` at the top of the firmware.

## Build and flash (Arduino CLI)

Needs the ESP32 Arduino core 3.x. Board: `esp32:esp32:Pcbcupid_GLYPH_C6` (or `esp32:esp32:esp32c6`).

```bash
arduino-cli core install esp32:esp32
arduino-cli compile --fqbn esp32:esp32:Pcbcupid_GLYPH_C6 firmware/smart_board_controller
arduino-cli upload -p COM17 --fqbn esp32:esp32:Pcbcupid_GLYPH_C6 firmware/smart_board_controller
```

Replace `COM17` with your board's port. The stream firmware also needs the
[MPU6050 library by Electronic Cats](https://github.com/electroniccats/mpu6050) (`arduino-cli lib install MPU6050`). If another installed library also provides `MPU6050.h`, pass `--library <path to this one>` when compiling.

## Using the game controller

1. Pair once: Windows Settings > Bluetooth > Add device > **Smart Board**.
2. Calibrate once: hold **BOOT** for 2 s (or type `c` in Serial Monitor at 115200). Within 4 s step on, stand **level and still**, then **lean forward firmly and hold**. It prints `Calibrated and saved. Play!`.
3. Lean about 4 degrees to press a key.

Serial commands: `c` recalibrate, `d` live data on/off, `r` factory calibration, `?` help.

## Recording data

Flash `firmware/smart_board_stream`, then:

```bash
pip install bleak
python tools/smart_board_logger.py      # live lines + CSV on your Desktop
python tools/smart_board_labeler.py     # labelling app (add --demo to try without a board)
```

Only one program can connect to the board at a time.

## Troubleshooting

- **Compiler fails with `cc1plus.exe: CreateProcess`, `Access is denied` or `Permission denied` (Windows):** a third-party antivirus may be blocking the compiler from being started by `arduino-cli`. Excluding the toolchain folders or using Windows Defender fixed it here.
- **`MPU6050 not found`:** run the I2C scanner. Both lines reading 0 V means the sensor has no power or isn't on those pins.
- **Crash at `hid->manufacturer(...)` on core 3.3.x:** use `hid->manufacturer()->setValue("...")` (already done here).

## License

[MIT](LICENSE)
