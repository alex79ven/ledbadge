/*
 * DIY Digital Spirit Level
 * -----------------------
 *
 * Adapted from: ESP32-Digital-Spirit-Level by SMDVIBE
 *              firmware/ESP32_C3_Digital_Level/ESP32_C3_Digital_Level.ino
 *
 * Ported to: Waveshare ESP32-S3-Matrix (ESP32-S3, SKU 27119)
 *
 * Hardware mapping changes against the original ESP32-C3 firmware:
 *   - 1.9" ST7789 IPS TFT (SPI)  -> onboard 8x8 RGB LED matrix (WS2811 on GPIO14)
 *   - MPU-6050 (I2C, addr 0x68)   -> onboard QMI8658 (I2C, addr 0x6B, SDA=11/SCL=12)
 *   - MODE button GPIO5 (C3)       -> BOOT button GPIO0 (ESP32-S3-Matrix)
 *
 * The bubble visualisation is rendered into the onboard 8x8 RGB LED matrix
 * instead of a TFT framebuffer. The measurement, filtering, display-mapping and
 * calibration logic of the original firmware is preserved.
 *
 * The QMI8658 is accessed directly over plain I2C (Wire) to avoid issues with
 * the third-party SensorLib I2C layer on this board.
 */

#define MODE_BUTTON 0   // BOOT button on the ESP32-S3-Matrix (GPIO0)

#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <math.h>

// ===================== ESP32-S3-Matrix =====================
#define I2C_SDA       11  // QMI8658 I2C data line
#define I2C_SCL       12  // QMI8658 I2C clock line
#define QMI_I2C_ADDR  0x6B // QMI8658 I2C slave address

#define LED_PIN       14  // onboard 8x8 RGB LED matrix (WS2811)
#define RGB_COUNT     64

Adafruit_NeoPixel pixels(RGB_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);
Preferences preferences;

// ===================== Display =====================
// The display is the onboard 8x8 RGB LED matrix (64 WS2811 LEDs).
// Coordinates: column 0..7 (left -> right), row 0..7 (top -> bottom).
// LED index = row * 8 + column (Waveshare ESP32-S3-Matrix layout).

// Set to 1 to stream debug values over USB serial (never leave it on when
// running stand-alone: without an open serial monitor the USB CDC buffer
// fills up and blocking prints would freeze the animation).
#define DEBUG_SERIAL 0

const float FIELD_CENTER = 3.5f; // level field centre in matrix cells
const float MAX_TRAVEL   = 3.5f; // bubble travel in cells at +/-15 degrees (edge to edge)
const float SINGLE_ROW   = 4.0f; // row of the horizontal single-axis bubble

float angleX = 0.0f;
float angleY = 0.0f;

float bubbleX = FIELD_CENTER; // bubble position in field cells (float)
float bubbleY = FIELD_CENTER;

// Display modes cycled by a short press of the BOOT button:
//   0 = two axes + reference cross
//   1 = single axis + reference line
//   2 = two axes, bubble only (no reference axes)
//   3 = digital readout of the tilt angle in degrees
uint8_t modeIndex = 0;
bool dualMode = true;  // both axes active
bool showAxes = true;  // draw the dim reference axis/cross
bool lastButtonState = HIGH;
bool buttonWasPressed = false;
unsigned long buttonPressStart = 0;
unsigned long lastButtonTime = 0;
bool longPressHandled = false;

void nextMode() {
  modeIndex = (uint8_t)((modeIndex + 1) % 4);
  dualMode = (modeIndex != 1);
  showAxes = (modeIndex == 0 || modeIndex == 1);
}

uint8_t calibrationMessage = 0; // 0 = none, 1 = CALIBRATING..., 2 = ZERO SET!
unsigned long calibrationMessageUntil = 0;

float zeroSingleX = 0.0f;
float zeroDualX = 0.0f;
float zeroDualY = 0.0f;

// Time constants (seconds) of the exponential filters. Using a time constant
// instead of a per-frame factor keeps the behaviour identical regardless of
// the actual loop rate.
const float ANGLE_TAU  = 0.10f; // sensor angle smoothing
const float BUBBLE_TAU = 0.16f; // visual bubble inertia
const float ZERO_DEAD_ZONE = 0.12f; // degrees

unsigned long lastLoopMs = 0;

// ---- minimal QMI8658 access over plain I2C ----
bool qmiWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(QMI_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  if (Wire.endTransmission() != 0) return false;
  return true;
}

// Full sensor configuration (registers as used by the Waveshare demo).
void qmiConfigure() {
  qmiWriteReg(0x02, 0x40); // CTRL1: address auto-increment (required for burst reads)
  qmiWriteReg(0x03, 0x13); // CTRL2: accel 4G range, ODR 1000 Hz
  qmiWriteReg(0x04, 0x23); // CTRL3: gyro 64 dps, ODR 896.8 Hz
  qmiWriteReg(0x06, 0x71); // CTRL5: accel LPF on (mode 0), gyro LPF on (mode 3)
  qmiWriteReg(0x08, 0x03); // CTRL7: enable accel (bit0) + gyro (bit1)
}

// Recover from a (rare) stuck I2C bus: restart the peripheral and the sensor.
void qmiRecover() {
  Wire.end();
  delay(5);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  qmiConfigure();
  delay(20);
}

bool qmiReadReg(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(QMI_I2C_ADDR);
  Wire.write(reg);
  // repeated start: no stop condition between the register write and the read
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)QMI_I2C_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) {
    if (Wire.available()) buf[i] = Wire.read();
    else return false;
  }
  return true;
}

// accelerometer data ready flag (STATUS0 bit 0)
bool qmiAccelReady() {
  uint8_t st = 0;
  if (!qmiReadReg(0x2E, &st, 1)) return false;
  return (st & 0x01) != 0;
}

// read accelerometer x/y/z in g (4G range -> 1/8192 g per LSB)
bool qmiReadAccel(float &x, float &y, float &z) {
  uint8_t buf[6];
  if (!qmiReadReg(0x35, buf, 6)) return false;
  int16_t ax = (int16_t)((buf[1] << 8) | buf[0]);
  int16_t ay = (int16_t)((buf[3] << 8) | buf[2]);
  int16_t az = (int16_t)((buf[5] << 8) | buf[4]);
  float scale = 4.0f / 32768.0f; // 4 g full scale (CTRL2 range bits = 001)
  x = ax * scale;
  y = ay * scale;
  z = az * scale;
  return true;
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // never let a full USB CDC TX buffer block the render loop
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);
  Serial.println("SPIRIT LEVEL BOOT");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000); // fast-mode I2C, plenty for the QMI8658

  // configure QMI8658 (values match the Waveshare demo: accel 4G/1000Hz,
  // gyro 64dps/896.8Hz, accel LPF mode 0, gyro LPF mode 3)
  qmiConfigure();
  delay(100);

  uint8_t who = 0;
  if (qmiReadReg(0x00, &who, 1)) {
    Serial.print("QMI WHO_AM_I=0x");
    Serial.println(who, HEX);
  } else {
    Serial.println("QMI READ FAIL");
  }

  pixels.begin();
  Serial.println("PIXELS OK");
  pixels.setBrightness(60);
  for (int i = 0; i < RGB_COUNT; i++) {
    pixels.setPixelColor(i, pixels.Color(0, 0, 0));
  }
  pixels.show();

  // startup self-test: light the whole matrix briefly so it is obvious
  // that the display is alive, even without a serial console attached
  for (int i = 0; i < RGB_COUNT; i++) {
    pixels.setPixelColor(i, pixels.Color(90, 90, 90));
  }
  pixels.show();
  delay(350);
  for (int i = 0; i < RGB_COUNT; i++) {
    pixels.setPixelColor(i, pixels.Color(0, 0, 0));
  }
  pixels.show();

  pinMode(MODE_BUTTON, INPUT_PULLUP);

  preferences.begin("level-cal", false);
  zeroSingleX = preferences.getFloat("singleX", 0.0f);
  zeroDualX = preferences.getFloat("dualX", 0.0f);
  zeroDualY = preferences.getFloat("dualY", 0.0f);
}

void loop() {
  bool buttonState = digitalRead(MODE_BUTTON);

  if (lastButtonState == HIGH && buttonState == LOW) {
    buttonWasPressed = true;
    buttonPressStart = millis();
    longPressHandled = false;
  }

  if (buttonWasPressed && buttonState == LOW && !longPressHandled &&
      millis() - buttonPressStart >= 2500) {
    calibrationMessage = 1; // CALIBRATING...
    calibrationMessageUntil = millis() + 600;
  }

  if (buttonWasPressed && buttonState == LOW && !longPressHandled &&
      millis() - buttonPressStart >= 3000) {
    longPressHandled = true;

    if (dualMode) {
      zeroDualX = angleX;
      zeroDualY = angleY;
      preferences.putFloat("dualX", zeroDualX);
      preferences.putFloat("dualY", zeroDualY);
#if DEBUG_SERIAL
      Serial.println("DUAL AXIS ZERO CALIBRATED (X/Y)");
#endif
    } else {
      zeroSingleX = angleX;
      preferences.putFloat("singleX", zeroSingleX);
#if DEBUG_SERIAL
      Serial.println("SINGLE AXIS ZERO CALIBRATED (X)");
#endif
    }
    calibrationMessage = 2; // ZERO SET!
    calibrationMessageUntil = millis() + 1300;
  }

  if (buttonWasPressed && lastButtonState == LOW && buttonState == HIGH) {
    unsigned long pressDuration = millis() - buttonPressStart;

    if (pressDuration < 3000 && millis() - lastButtonTime > 250) {
      nextMode();
      lastButtonTime = millis();
    }

    buttonWasPressed = false;
  }

  lastButtonState = buttonState;

  // ---- frame timing (seconds since the previous iteration) ----
  unsigned long now = millis();
  float dt = (now - lastLoopMs) / 1000.0f;
  if (lastLoopMs == 0 || dt <= 0.0f || dt > 0.2f) dt = 0.02f; // first frame / after a stall
  lastLoopMs = now;

  // ---- read the onboard QMI8658 accelerometer (6-axis IMU) ----
  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  bool gotSample = false;
  if (qmiAccelReady()) {
    if (qmiReadAccel(ax, ay, az)) {
      gotSample = true;
      float rawX = atan2(ay, sqrt(ax * ax + az * az)) * 180.0f / PI;
      float rawY = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0f / PI;

      float aAng = 1.0f - expf(-dt / ANGLE_TAU);
      angleX += aAng * (rawX - angleX);
      angleY += aAng * (rawY - angleY);
    }
  }

  // sensor watchdog: if no fresh sample arrives for 2 s, restart bus + sensor
  static unsigned long lastGoodSample = 0;
  if (gotSample) {
    lastGoodSample = now;
  } else if (now - lastGoodSample > 2000UL) {
    qmiRecover();
    lastGoodSample = now;
  }

  // ---- apply calibration offsets ----
  float calibratedSingleX = angleX - zeroSingleX;
  float calibratedDualX = angleX - zeroDualX;
  float calibratedDualY = angleY - zeroDualY;

  if (fabs(calibratedSingleX) < ZERO_DEAD_ZONE) calibratedSingleX = 0;
  if (fabs(calibratedDualX) < ZERO_DEAD_ZONE) calibratedDualX = 0;
  if (fabs(calibratedDualY) < ZERO_DEAD_ZONE) calibratedDualY = 0;

  // ---- map the calibrated angle to a bubble position on the matrix ----
  float targetBX;
  float targetBY;
  if (dualMode) {
    float rotatedX = calibratedDualX;
    float rotatedY = calibratedDualY; // vertical axis inverted vs. the raw sensor sign
    targetBX = FIELD_CENTER + constrain(rotatedX, -15.0f, 15.0f) * (MAX_TRAVEL / 15.0f);
    targetBY = FIELD_CENTER + constrain(rotatedY, -15.0f, 15.0f) * (MAX_TRAVEL / 15.0f);
  } else {
    targetBX = FIELD_CENTER + constrain(calibratedSingleX, -15.0f, 15.0f) * (MAX_TRAVEL / 15.0f);
    targetBY = SINGLE_ROW;
  }

  // exponential smoothing with dt -> smooth, frame-rate independent glide
  float aBub = 1.0f - expf(-dt / BUBBLE_TAU);
  bubbleX += aBub * (targetBX - bubbleX);
  bubbleY += aBub * (targetBY - bubbleY);
  bubbleX = constrain(bubbleX, 0.0f, 7.0f);
  bubbleY = constrain(bubbleY, 0.0f, 7.0f);

  float maxAngle = dualMode ? max(fabs(calibratedDualX), fabs(calibratedDualY)) : fabs(calibratedSingleX);

  uint32_t bubbleColor;
  if (maxAngle < 0.8f) {
    bubbleColor = pixels.Color(0, 255, 0);    // green = level
  } else if (maxAngle < 4.0f) {
    bubbleColor = pixels.Color(255, 255, 0); // yellow = tilt
  } else {
    bubbleColor = pixels.Color(255, 0, 0);   // red = tilt
  }

  if (calibrationMessage != 0 && millis() < calibrationMessageUntil) {
    bubbleColor = pixels.Color(255, 255, 255);
  } else if (calibrationMessage != 0) {
    calibrationMessage = 0;
  }

  if (modeIndex == 3) {
    // digital readout: show the dominant axis angle in whole degrees
    float signedAngle = (fabs(calibratedDualX) >= fabs(calibratedDualY))
                          ? calibratedDualX : calibratedDualY;
    renderNumber(signedAngle, bubbleColor);
  } else {
    renderFrame(bubbleColor, bubbleX, bubbleY);
  }

#if DEBUG_SERIAL
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug >= 500) {
    lastDebug = millis();
    Serial.print("ax="); Serial.print(ax, 2);
    Serial.print(" ay="); Serial.print(ay, 2);
    Serial.print(" az="); Serial.print(az, 2);
    Serial.print(" | angX="); Serial.print(angleX, 1);
    Serial.print(" angY="); Serial.print(angleY, 1);
    Serial.print(" | bubble="); Serial.print(bubbleX, 2);
    Serial.print(","); Serial.println(bubbleY, 2);
  }
#endif

  delay(8); // ~120 fps: smooth motion, still cheap for 64 WS2811 LEDs
}

// Draw the current frame into the 8x8 RGB LED matrix and push it out.
// The bubble is drawn with bilinear sub-pixel weights, so it glides smoothly
// between LED cells instead of jumping from cell to cell.
void renderFrame(uint32_t bubbleColor, float bx, float by) {
  static float bufR[RGB_COUNT];
  static float bufG[RGB_COUNT];
  static float bufB[RGB_COUNT];

  for (int i = 0; i < RGB_COUNT; i++) {
    bufR[i] = 0.0f;
    bufG[i] = 0.0f;
    bufB[i] = 0.0f;
  }

  // dim blue reference axis so the level orientation is visible
  if (showAxes) {
    const float refR = 0.0f, refG = 0.0f, refB = 45.0f;
    for (int i = 0; i < 8; i++) {
      int idx;
      if (dualMode) {
        idx = 4 * 8 + i;          // horizontal centre line
        bufR[idx] = refR; bufG[idx] = refG; bufB[idx] = refB;
        idx = i * 8 + 4;          // vertical centre line
        bufR[idx] = refR; bufG[idx] = refG; bufB[idx] = refB;
      } else {
        idx = (int)SINGLE_ROW * 8 + i; // single-axis tube
        bufR[idx] = refR; bufG[idx] = refG; bufB[idx] = refB;
      }
    }
  }

  // bubble: bilinear interpolation between the four nearest LED cells
  float cr = (float)((bubbleColor >> 16) & 0xFF);
  float cg = (float)((bubbleColor >> 8) & 0xFF);
  float cb = (float)(bubbleColor & 0xFF);

  int x0 = (int)floorf(bx);
  int y0 = (int)floorf(by);
  float fx = bx - (float)x0;
  float fy = by - (float)y0;

  for (int k = 0; k < 4; k++) {
    int px = x0 + (k & 1);
    int py = y0 + (k >> 1);
    if (px < 0 || px > 7 || py < 0 || py > 7) continue;

    float wx = (k & 1) ? fx : (1.0f - fx);
    float wy = (k >> 1) ? fy : (1.0f - fy);
    float w = wx * wy;
    int idx = py * 8 + px;

    bufR[idx] += cr * w;
    bufG[idx] += cg * w;
    bufB[idx] += cb * w;
  }

  for (int i = 0; i < RGB_COUNT; i++) {
    int r = (int)(bufR[i] + 0.5f);
    int g = (int)(bufG[i] + 0.5f);
    int b = (int)(bufB[i] + 0.5f);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    pixels.setPixelColor(i, pixels.Color((uint8_t)r, (uint8_t)g, (uint8_t)b));
  }
  pixels.show();
}

// ===================== digital readout =====================
// Minimal 3x5 pixel font for the digits 0..9. Each row is 3 bits:
// bit2 = left column, bit1 = middle, bit0 = right.
static const uint8_t FONT3X5[10][5] = {
  {7, 5, 5, 5, 7}, // 0
  {2, 6, 2, 2, 7}, // 1
  {7, 1, 7, 4, 7}, // 2
  {7, 1, 7, 1, 7}, // 3
  {5, 5, 7, 1, 1}, // 4
  {7, 4, 7, 1, 7}, // 5
  {7, 4, 7, 5, 7}, // 6
  {7, 1, 2, 2, 2}, // 7
  {7, 5, 7, 5, 7}, // 8
  {7, 5, 7, 1, 7}, // 9
};

void drawDigit(uint8_t d, int x0, int y0, uint32_t color) {
  for (int r = 0; r < 5; r++) {
    uint8_t bits = FONT3X5[d][r];
    for (int c = 0; c < 3; c++) {
      if (bits & (4 >> c)) {
        int x = x0 + c;
        int y = y0 + r;
        if (x >= 0 && x < 8 && y >= 0 && y < 8) {
          pixels.setPixelColor(y * 8 + x, color);
        }
      }
    }
  }
}

// Show the tilt angle in whole degrees (0..99) as two digits.
// A single LED at the left edge marks a negative angle.
void renderNumber(float angle, uint32_t color) {
  int value = (int)(fabsf(angle) + 0.5f);
  if (value > 99) value = 99;

  for (int i = 0; i < RGB_COUNT; i++) {
    pixels.setPixelColor(i, 0);
  }

  drawDigit((uint8_t)(value / 10), 1, 1, color);
  drawDigit((uint8_t)(value % 10), 5, 1, color);

  if (angle < -0.5f) {
    pixels.setPixelColor(3 * 8 + 0, color); // minus marker at the left edge
  }

  pixels.show();
}
