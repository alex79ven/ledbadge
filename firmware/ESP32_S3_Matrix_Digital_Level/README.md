# DIY Digital Spirit Level — ESP32-S3-Matrix port

Firmware for the **Waveshare ESP32-S3-Matrix (SKU 27119)** — adapted from the
DIY Digital Spirit Level project by **SMDVIBE**
([ESP32-Digital-Spirit-Level](https://github.com/SMDVIBE/ESP32-Digital-Spirit-Level)).

Instead of the original 1.9" ST7789 TFT + MPU-6050 setup, this port uses the
**onboard** peripherals of the ESP32-S3-Matrix:

- **8x8 RGB LED matrix** (WS2811, GPIO14) — the level "bubble" is rendered on this matrix.
- **QMI8658** 6-axis IMU (I2C, addr `0x6B`, SDA = GPIO11, SCL = GPIO12) — replaces the external MPU-6050.
- **BOOT button** (GPIO0) — replaces the external mode button (GPIO5 of the ESP32-C3).

## What was changed

| Original (ESP32-C3) | Ported (Waveshare ESP32-S3-Matrix) |
|---|---|
| ST7789 IPS display, 170x320, SPI, GFX canvas framebuffer | Onboard 8x8 RGB LED matrix, WS2811, GPIO14, Adafruit_NeoPixel |
| MPU-6050 via I2C (addr `0x68`) | Onboard QMI8658 via I2C (addr `0x6B`, SDA=11/SCL=12), minimal plain-`Wire` driver |
| Mode button GPIO5 (INPUT_PULLUP) | BOOT button GPIO0 (INPUT_PULLUP) |

The measurement logic, angle filtering, zero-calibration scheme and the
bubble-to-angle mapping follow the original firmware:

- short button press → switch between Single-axis / Dual-axis modes;
- long press (>= 3 s) → zero calibration of the active mode;
- independent calibration storage per mode (`Preferences`, NVS keys `singleX`, `dualX`, `dualY`).

## How the level is shown on the 8x8 matrix

The bubble is a soft dot rendered with **bilinear sub-pixel weights**, so it
glides smoothly between LED cells instead of jumping cell to cell.

Four display modes are cycled with a **short press of the BOOT button**:

| Mode | Axes | Reference |
|---|---|---|
| 0 (default) | two axes (X/Y) | dim blue centre cross |
| 1 | single axis | dim blue centre line |
| 2 | two axes | none — bubble only |
| 3 | two axes | digital readout: tilt angle in whole degrees |

In mode 3 the dominant axis angle is shown as two digits (3x5 font); a single
LED on the left edge marks a negative angle.

On power-up the whole matrix flashes briefly as a self-test, so it is obvious
that the display works even without a serial console.

Bubble colour encodes the status:

- green — level (`|angle| < 0.8 deg`);
- yellow — tilt (`|angle| < 4.0 deg`);
- red — tilt (`|angle| >= 4.0 deg`).

During zero calibration the bubble is briefly lit white (`CALIBRATING...` /
`ZERO SET!`).

## Arduino libraries required

- Adafruit NeoPixel
- (Wire and Preferences are provided by the ESP32 Arduino core)

> The QMI8658 is accessed **directly over `Wire`** — the third-party SensorLib
> `SensorQMI8658` driver caused a hard fault (`InstrFetchProhibited`, PC = 0) on
> this board, so the port uses a small local driver (`qmiWriteReg` /
> `qmiReadReg` / `qmiReadAccel`) instead.

QMI8658 configuration used by this port (matches the Waveshare demo):

| Register | Value | Meaning |
|---|---|---|
| `CTRL1` (0x02) | `0x40` | address auto-increment — required for burst reads |
| `CTRL2` (0x03) | `0x13` | accelerometer: 4 g range, 1000 Hz ODR |
| `CTRL3` (0x04) | `0x23` | gyroscope: 64 dps range, 896.8 Hz ODR |
| `CTRL5` (0x06) | `0x71` | accel LPF (mode 0), gyro LPF (mode 3) |
| `CTRL7` (0x08) | `0x03` | enable accelerometer + gyroscope |
| `STATUS0` (0x2E) | bit 0 | accelerometer data-ready flag |
| `AX_L` (0x35) | 6 bytes | accelerometer X/Y/Z, little-endian |

## Flashing

1. Install the **ESP32 board support** for Arduino IDE (Espressif `esp32:esp32`, tested with 3.3.12).
2. Install **Adafruit NeoPixel** via the Arduino Library Manager.
3. In Arduino IDE select:
   - Board: **ESP32S3 Dev Module** (FQBN `esp32:esp32:esp32s3`);
   - **USB CDC On Boot: Enabled** — without this the sketch runs fine, but
     `Serial` debug output goes to UART0 (GPIO43/44) instead of the USB port;
   - Port: the USB port of the connected Waveshare ESP32-S3-Matrix.
4. Open the sketch `ESP32_S3_Matrix_Digital_Level.ino` and upload.

### Command line (arduino-cli)

```sh
arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" .
arduino-cli upload  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --port /dev/cu.usbmodemXXXX .
```

## Usage

1. Power on the device.
2. A short press of the BOOT button cycles the display modes:
   **two axes + cross → single axis + line → two axes without axes → digital degrees → …**
3. Place the device on a reference surface.
4. Hold the BOOT button for ~3 s to set the zero for the active mode.
5. The bubble position / colour reflects the measured inclination in real time.

## Smoothing and robustness

- Angle smoothing and bubble motion use **time-constant exponential filters**
  (`ANGLE_TAU = 0.10 s`, `BUBBLE_TAU = 0.16 s`) computed from `dt`, so the
  animation behaves the same at any loop rate. The display runs at ~120 fps.
- A **sensor watchdog** restarts the I2C bus and re-configures the QMI8658 if no
  fresh accelerometer sample arrives for 2 seconds.
- Serial debug output is compiled in only when `DEBUG_SERIAL` is set to `1`.
  Keep it at `0` for stand-alone use: on ESP32-S3 a blocking `Serial.print`
  into a full USB-CDC buffer (no monitor attached) would freeze the animation.

> Note: the brightness of the LED matrix should not be set too high
> (the original firmware uses 60/255); excessive brightness may cause rapid
> temperature rise and board damage.

## Limitations of the port

- The matrix is only 8x8 (64 LEDs), so the bubble is rendered as a single LED
  and no on-matrix text/angle readouts are produced (the status is shown by the
  bubble colour).
- The accelerometer-based angles of the QMI8658 are used (same approach as the
  original MPU-6050 firmware).
