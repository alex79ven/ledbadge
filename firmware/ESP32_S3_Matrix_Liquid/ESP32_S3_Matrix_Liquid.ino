/*
 * LED badge: clock, calendar and Home Assistant lamp dimmer
 * ----------------------------------------------------------
 * For the Waveshare ESP32-S3-Matrix (SKU 27119), ESP32-S3.
 *
 * The onboard 8x8 RGB LED matrix (WS2811, GPIO14) shows a clock. Six widgets
 * live on a virtual 16x24 canvas: hours, minutes, temperature, an animated
 * weather icon, a tear-off calendar sheet with today's date, and a scrolling
 * weekday ticker that follows the sheet. The 8x8 window glides from page to
 * page, and the picture is rotated to stay upright using the onboard QMI8658
 * accelerometer. Time comes from NTP over Wi-Fi, weather from the free
 * Open-Meteo API.
 *
 * The second effect is a lamp dimmer: rotating the badge like a knob changes
 * the brightness of a real lamp in Home Assistant. The accelerometer gives an
 * absolute angle while the badge is held upright; the gyro takes over when it
 * lies flat. The percentage is published to MQTT and Home Assistant maps it to
 * light.turn_on / brightness_pct (see the README).
 *
 * MQTT also carries a scrolling text overlay, an 8x8 bitmap overlay, the
 * availability topic, and Home Assistant MQTT discovery (one "LED Badge"
 * device appears by itself). An optional BLE scan relays advertisements as
 * presence data.
 *
 * A short press of the BOOT button (GPIO0) switches the base colour; a double
 * press cycles the effect (clock -> dimmer). Holding the button ramps the
 * animation dynamics. All choices are remembered in NVS.
 */

#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define LED_PIN       14  // onboard 8x8 RGB LED matrix (WS2811)
#define RGB_COUNT     64
#define MODE_BUTTON   0   // BOOT button (GPIO0)

#define I2C_SDA       11  // QMI8658 on the Waveshare board
#define I2C_SCL       12
#define QMI_I2C_ADDR  0x6B

#define BRIGHTNESS    60  // keep moderate: high brightness heats the board

// ---- dimmer tuning ------------------------------------------------------
// Turning the badge like a knob changes the lamp brightness in Home
// Assistant. One full turn (360 degrees) spans the whole 0..100 % range.
#define KNOB_DEG_PER_PCT   3.6f   // degrees of rotation per percent
#define KNOB_DEADBAND_DEG  0.15f  // smaller accelerometer-angle jitter is ignored
#define KNOB_FLAT_G        0.35f  // in-plane gravity below this -> badge is flat
#define KNOB_GYRO_SIGN     1.0f   // flip to -1 if a flat turn works backwards
#define KNOB_GYRO_DEADBAND 0.8f   // dps ignored as tremor
#define KNOB_GYRO_BIAS_MS  1500UL // still time before the gyro zero is relearnt
#define KNOB_PUBLISH_MS    200UL  // MQTT publish throttle

// Set to 1 to stream tilt and knob values over USB serial (keep 0 stand-alone).
#define DEBUG_KNOB      0

// ---- clock / weather ----------------------------------------------------
#define CLOCK_WIFI_SSID   "YOUR_WIFI_SSID"
#define CLOCK_WIFI_PASS   "YOUR_WIFI_PASSWORD"
// Default POSIX TZ string (Europe/Madrid). Can be changed from the web page.
#define CLOCK_TZ          "CET-1CEST,M3.5.0,M10.5.0/3"
#define CLOCK_W           16     // canvas is 2 pages wide ...
#define CLOCK_H           24     // ... and 3 pages tall (row 0: calendar + weekday)
#define CLOCK_STAGES      6      // hours, minutes, temp, weather, calendar, weekday
#define CLOCK_MOVE_MS     700    // glide time for one page at 100 % dynamics
#define CLOCK_UPRIGHT_DEG 14.0f  // tilt dead zone before the picture rotates
#define WEEKDAY_SPEED     16.0f  // weekday ticker, pixels per second
#define WEATHER_REFRESH_MS (15UL * 60UL * 1000UL)

// ---- Wi-Fi setup hotspot -------------------------------------------------
// If the saved network cannot be reached the badge starts its own access
// point so the Wi-Fi settings can be fixed from a phone.
#define AP_NAME_PREFIX    "LedBadge"   // hotspot shows up as LedBadge-XXXX
#define AP_PASSWORD       "ledbadge"   // WPA2 needs at least 8 characters

// ---- MQTT ----------------------------------------------------------------
// Messages published to this topic scroll across the matrix, e.g. from
// Home Assistant. The status topic carries "online"/"offline" (LWT).
#define MQTT_DEFAULT_TOPIC "ledbadge/text"
#define MQTT_STATUS_TOPIC  "ledbadge/status"
#define MSG_MAX_LEN        160
#define MSG_SPEED          16.0f  // scrolling speed in pixels per second

// ---- Home Assistant MQTT discovery --------------------------------------
// Every entity is announced (retained) when the broker link comes up, so Home
// Assistant adds the badge by itself under Settings -> Devices.
#define MQTT_STATE_TOPIC        "ledbadge/state"
#define MQTT_BRIGHT_SET_TOPIC   "ledbadge/bright/set"
#define MQTT_BRIGHT_STATE_TOPIC "ledbadge/bright"
// Lamp dimmer: the badge publishes {"pct":N} here whenever the knob is turned.
#define MQTT_KNOB_TOPIC         "ledbadge/knob"
// 8x8 picture overlay: publish a bitmap here to draw it full screen.
#define MQTT_BITMAP_TOPIC       "ledbadge/bitmap"
#define BMP_DEFAULT_MS          20000UL  // how long a bitmap stays without :seconds
#define MQTT_DISCOVERY_PREFIX   "homeassistant"

// ---- Bluetooth -> MQTT bridge --------------------------------------------
// NimBLE listens for BLE advertisements and republishes them under this base
// topic (<base>/<mac without colons>). This is a relay, not the ESPHome
// Bluetooth Proxy: Home Assistant consumes it through MQTT.
#define BLE_DEFAULT_TOPIC "ledbadge/ble"
#define BLE_QUEUE_LEN     8
#define BLE_MAX_MDATA     29
#define BLE_PUBLISH_MS    5000UL   // one message per device per 5 s

// Default weather location when the IP lookup is off: Madrid.
#define FIXED_LAT         40.4168f
#define FIXED_LON         -3.7038f

Adafruit_NeoPixel pixels(RGB_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);
Preferences prefs;
Preferences clockPrefs;
WebServer webServer(80);
bool webRunning = false;
WiFiClient mqttNet;
PubSubClient mqttClient(mqttNet);
char apSsid[24] = AP_NAME_PREFIX;
char mqttNodeId[20] = "ledbadge";  // discovery id, filled in setup()

// ---- scrolling MQTT text ------------------------------------------------
// The MQTT client runs in its own task; a message is handed to the renderer
// through msgStaging/msgArrived so the blocking socket never stalls the LEDs.
volatile bool msgArrived = false;
char msgStaging[MSG_MAX_LEN + 1] = "";
char msgText[MSG_MAX_LEN + 1] = "";
float msgX = 8.0f;
unsigned long msgStart = 0;
bool msgActive = false;
bool bootMsgPending = false;  // IP ticker waiting to be started
bool bootMsgShown = false;    // the IP ticker is shown once per boot
char bootMsgLine[48] = "";

// ---- 8x8 picture overlay from MQTT ----------------------------------------
// The payload is parsed in the MQTT task; the final 64 RGB pixels are handed
// to the renderer through bmpStaging + bmpArrived, like the scrolling text.
volatile bool bmpArrived = false;
volatile bool bmpClearStaging = false;
uint8_t bmpStaging[192];          // 64 pixels, RGB, row-major
uint32_t bmpMsStaging = BMP_DEFAULT_MS;
uint8_t bmpPixels[192];           // the bitmap currently shown
bool bmpActive = false;
uint32_t bmpUntil = 0;            // millis deadline, 0 = until cleared

// Brightness requested over MQTT: the MQTT task only records the value, the
// main loop applies it (and writes it to NVS) from the animation task.
volatile int brightCmd = -1;
volatile bool mqttPubState = false;  // publish a fresh state snapshot soon

// ---- Bluetooth -> MQTT bridge (configuration) ----
char bleTopic[48] = BLE_DEFAULT_TOPIC;
uint8_t bleEnabled = 0;
uint8_t bleDisc = 0;               // create HA sensors for BLE devices
volatile bool bleDirty = false;    // enabled/disabled or topic changed

// Base hues (0..65535) of the available colours.
const uint16_t BASE_HUES[] = {
  0,      // red
  5500,   // orange
  10922,  // yellow
  21845,  // green
  32768,  // cyan
  43690,  // blue
  50000,  // violet
  58000,  // magenta
};
const char *const COLOR_NAMES[] = {
  "red", "orange", "yellow", "green", "cyan", "blue", "violet", "magenta"
};
const uint8_t COLOR_COUNT = sizeof(BASE_HUES) / sizeof(BASE_HUES[0]);

const char *const EFFECT_NAMES[] = { "clock", "dimmer" };
const uint8_t EFFECT_COUNT = sizeof(EFFECT_NAMES) / sizeof(EFFECT_NAMES[0]);

uint8_t colorIndex = 0;
uint16_t targetHue = 0;
float huePos = 0.0f;  // animated hue, follows targetHue smoothly

// last smoothed tilt angles (degrees) from the accelerometer
float tiltAngleX = 0.0f;
float tiltAngleY = 0.0f;
// last raw accelerometer sample in g (the dimmer needs it unsmoothed)
float tiltAX = 0.0f, tiltAY = 0.0f, tiltAZ = 0.0f;

// ---- lamp dimmer state ----
int knobPct = 50;              // lamp brightness sent to Home Assistant, 0..100
volatile int knobPub = -1;     // pending value for the MQTT task
int knobPublished = -1;        // last value handed to the MQTT task
float knobPctF = 50.0f;        // unrounded value, keeps slow turns smooth
float knobAngle = 0.0f;        // last screen-plane gravity angle (degrees)
bool knobAngleValid = false;
float gyroBias = 0.0f;         // measured gyro zero point while flat and still
unsigned long gyroStillSince = 0;
unsigned long knobLastPub = 0;
unsigned long knobChangedAt = 0;
bool knobSaveDirty = false;

// Debounced BOOT button. The raw level must be stable for DEBOUNCE_MS before
// it is accepted, and a real press is only accepted 1.8 s after boot: the USB
// auto-reset circuit can pull GPIO0 low when a serial monitor opens.
bool buttonStable = HIGH;
bool buttonChangeActive = false;
unsigned long buttonChangeTime = 0;
unsigned long buttonPressStart = 0;
const unsigned long DEBOUNCE_MS = 30;
const unsigned long BOOT_GUARD_MS = 1800;
const unsigned long PRESS_MAX_MS = 1500;

// Single click = next colour, double click = next effect.
uint8_t effectIndex = 0;      // 0 = clock, 1 = lamp dimmer
bool pendingClick = false;    // a short click waits to see if a second follows
unsigned long lastClickRelease = 0;
const unsigned long DOUBLE_CLICK_MS = 350;

// Holding BOOT ramps the animation dynamics: it speeds up step by step, and
// once it reaches the top it walks back down again (ping-pong), so one hold
// covers both directions. The value is stored as a percentage in NVS.
uint8_t dynPct = 100;         // 50 .. 200 %
uint8_t ledBrightness = BRIGHTNESS; // 1 .. 255, adjustable from the web page
bool longPressActive = false;
int8_t dynDir = 1;
unsigned long lastRampMs = 0;
const unsigned long LONG_PRESS_MS = 500;
const unsigned long DYN_RAMP_MS = 120;
const uint8_t DYN_MIN = 50;
const uint8_t DYN_MAX = 200;
const uint8_t DYN_STEP = 10;

unsigned long lastLoopMs = 0;

// ===================== minimal QMI8658 access =====================
bool qmiWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(QMI_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
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

void qmiConfigure() {
  qmiWriteReg(0x02, 0x40); // CTRL1: address auto-increment (required for burst reads)
  qmiWriteReg(0x03, 0x13); // CTRL2: accel 4G range, ODR 1000 Hz
  qmiWriteReg(0x04, 0x23); // CTRL3: gyro 64 dps, ODR 896.8 Hz
  qmiWriteReg(0x06, 0x71); // CTRL5: accel LPF on (mode 0), gyro LPF on (mode 3)
  qmiWriteReg(0x08, 0x03); // CTRL7: enable accel (bit0) + gyro (bit1)
}

void qmiRecover() {
  Wire.end();
  delay(5);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  qmiConfigure();
  delay(20);
}

// accelerometer data ready flag (STATUS0 bit 0)
bool qmiAccelReady() {
  uint8_t st = 0;
  if (!qmiReadReg(0x2E, &st, 1)) return false;
  return (st & 0x01) != 0;
}

// read accelerometer x/y/z in g (4G range -> 4/32768 g per LSB)
bool qmiReadAccel(float &x, float &y, float &z) {
  uint8_t buf[6];
  if (!qmiReadReg(0x35, buf, 6)) return false;
  int16_t ax = (int16_t)((buf[1] << 8) | buf[0]);
  int16_t ay = (int16_t)((buf[3] << 8) | buf[2]);
  int16_t az = (int16_t)((buf[5] << 8) | buf[4]);
  const float scale = 4.0f / 32768.0f;
  x = ax * scale;
  y = ay * scale;
  z = az * scale;
  return true;
}

// ===================== setup / loop =====================
// Forward declarations for the pieces used before they are defined.
struct BleAdv;
void bleStart();
void bleStop();
void blePublish(const BleAdv &a);
bool bleDiscPublish(const BleAdv &a, const char *plain);
void webBegin();
void wifiBegin();
void wifiApply();
void netLoop();
void renderMessage();
void renderBitmap();
void showFrame(const uint8_t *fr, const uint8_t *fg, const uint8_t *fb);
void weatherTask(void *);
void mqttTask(void *);
void clockEnter();
void clockLeave();
void updateClock(float dt);
void renderClock();
void updateClockOrientation();
bool clockTimeReady();
void updateTilt(float dt);
void updateDimmer(float dt);
void renderDimmer();
void showFrameOrient(const uint8_t *fr, const uint8_t *fg, const uint8_t *fb,
                     uint8_t orient);
uint8_t brightnessNow();

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // a full USB CDC buffer must never block the animation
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);
  Serial.println("LED BADGE BOOT");

  // Stable discovery id for Home Assistant (last 24 bits of the MAC).
  snprintf(mqttNodeId, sizeof(mqttNodeId), "ledbadge_%06X",
           (unsigned)(ESP.getEfuseMac() & 0xFFFFFF));

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  qmiConfigure();
  delay(50);

  uint8_t who = 0;
  if (qmiReadReg(0x00, &who, 1)) {
    Serial.print("QMI WHO_AM_I=0x");
    Serial.println(who, HEX);
  } else {
    Serial.println("QMI READ FAIL");
  }

  pixels.begin();
  pixels.setBrightness(BRIGHTNESS);
  pixels.clear();
  pixels.show();

  pinMode(MODE_BUTTON, INPUT_PULLUP);

  prefs.begin("liquid", false);
  colorIndex = prefs.getUChar("color", 0);
  if (colorIndex >= COLOR_COUNT) colorIndex = 0;
  targetHue = BASE_HUES[colorIndex];
  huePos = (float)targetHue;

  effectIndex = prefs.getUChar("effect", 0);
  if (effectIndex >= EFFECT_COUNT) effectIndex = 0;

  dynPct = prefs.getUChar("dyn", 100);
  if (dynPct < DYN_MIN) dynPct = DYN_MIN;
  if (dynPct > DYN_MAX) dynPct = DYN_MAX;

  ledBrightness = prefs.getUChar("bright", BRIGHTNESS);
  if (ledBrightness < 1) ledBrightness = 1;
  pixels.setBrightness(ledBrightness);

  // startup self-test: brief white flash
  for (int i = 0; i < RGB_COUNT; i++) {
    pixels.setPixelColor(i, pixels.Color(70, 70, 70));
  }
  pixels.show();
  delay(300);
  pixels.clear();
  pixels.show();

  buttonStable = digitalRead(MODE_BUTTON);
  Serial.print("EFFECT: ");
  Serial.println(EFFECT_NAMES[effectIndex]);
  Serial.print("COLOUR: ");
  Serial.println(COLOR_NAMES[colorIndex]);
  Serial.print("DYNAMICS: ");
  Serial.println(dynPct);

  // background workers: blocking HTTPS/MQTT never stalls the animation
  xTaskCreatePinnedToCore(weatherTask, "weather", 16384, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(mqttTask, "mqtt", 8192, nullptr, 1, nullptr, 0);

  loadClockConfig();
  pixels.setBrightness(brightnessNow());
  if (bleEnabled) bleDirty = true;  // start scanning once the MQTT link is up

  // Wi-Fi runs in every effect, so the settings page and MQTT messages stay
  // available even while the clock or the dimmer is showing. The radio must be
  // initialised before the web server binds its socket.
  wifiBegin();
  webBegin();

  if (effectIndex == 0) clockEnter();
}

void loop() {
  // ---- debounced BOOT button: a short press switches the colour ----
  bool raw = digitalRead(MODE_BUTTON);
  if (raw != buttonStable) {
    if (!buttonChangeActive) {
      buttonChangeActive = true;
      buttonChangeTime = millis();
    } else if (millis() - buttonChangeTime >= DEBOUNCE_MS) {
      buttonChangeActive = false;
      buttonStable = raw;

      if (buttonStable == LOW) {
        buttonPressStart = millis();
      } else {
        unsigned long held = millis() - buttonPressStart;
        if (longPressActive) {
          // ---- a hold just ended: keep the chosen dynamics ----
          longPressActive = false;
          dynDir = 1;
          pendingClick = false;
          prefs.putUChar("dyn", dynPct);
          Serial.print("DYNAMICS: ");
          Serial.println(dynPct);
        } else if (buttonPressStart >= BOOT_GUARD_MS && held <= PRESS_MAX_MS) {
          unsigned long released = millis();
          if (pendingClick && released - lastClickRelease <= DOUBLE_CLICK_MS) {
            // ---- double click: cycle the effect ----
            pendingClick = false;
            setEffect((uint8_t)((effectIndex + 1) % EFFECT_COUNT));
          } else {
            // wait briefly: a second click would turn this into a double click
            pendingClick = true;
            lastClickRelease = released;
          }
        }
      }
    }
  } else {
    buttonChangeActive = false;
  }

  // ---- holding BOOT ramps the dynamics up, then back down ----
  if (buttonStable == LOW && buttonPressStart >= BOOT_GUARD_MS) {
    unsigned long nowMs = millis();
    if (!longPressActive && nowMs - buttonPressStart >= LONG_PRESS_MS) {
      longPressActive = true;
      pendingClick = false; // a hold is not a click
      dynDir = 1;
      lastRampMs = nowMs;
    }
    if (longPressActive && nowMs - lastRampMs >= DYN_RAMP_MS) {
      lastRampMs = nowMs;
      int v = (int)dynPct + (int)dynDir * (int)DYN_STEP;
      if (v >= (int)DYN_MAX) {
        v = DYN_MAX;
        dynDir = -1;
      } else if (v <= (int)DYN_MIN) {
        v = DYN_MIN;
        dynDir = 1;
      }
      dynPct = (uint8_t)v;
    }
  }

  // a single click that was not followed by a second one: next colour
  if (pendingClick && buttonStable == HIGH &&
      millis() - lastClickRelease > DOUBLE_CLICK_MS) {
    pendingClick = false;
    colorIndex = (uint8_t)((colorIndex + 1) % COLOR_COUNT);
    targetHue = BASE_HUES[colorIndex];
    prefs.putUChar("color", colorIndex);
    Serial.print("COLOUR: ");
    Serial.print(COLOR_NAMES[colorIndex]);
    Serial.print(" t=");
    Serial.println(millis());
  }

  // ---- frame timing ----
  unsigned long now = millis();
  float dt = (now - lastLoopMs) / 1000.0f;
  if (lastLoopMs == 0 || dt <= 0.0f || dt > 0.1f) dt = 0.016f;
  lastLoopMs = now;

  updateTilt(dt);

  // ---- boot greeting: scroll the IP address once after Wi-Fi comes up ----
  if (bootMsgPending) {
    bootMsgPending = false;
    strncpy(msgText, bootMsgLine, sizeof(msgText) - 1);
    msgText[sizeof(msgText) - 1] = '\0';
    msgX = 8.0f;
    msgStart = millis();
    msgActive = (msgText[0] != '\0');
    Serial.printf("MSG: %s\n", msgText);
  }

  // ---- incoming MQTT message: begin scrolling it across the matrix ----
  if (msgArrived) {
    msgArrived = false;
    strncpy(msgText, msgStaging, sizeof(msgText) - 1);
    msgText[sizeof(msgText) - 1] = '\0';
    msgX = 8.0f;
    msgStart = millis();
    msgActive = (msgText[0] != '\0');
    Serial.printf("MSG: %s\n", msgText);
  }
  if (msgActive) {
    if (millis() - msgStart >= 600UL) msgX -= MSG_SPEED * dt;
    if (msgX <= -(float)((int)strlen(msgText) * 6)) msgActive = false;
  }

  // ---- network: settings page, MQTT, NTP and weather ----
  netLoop();

  // ---- brightness requested over MQTT (slider in Home Assistant) ----
  if (brightCmd >= 0) {
    ledBrightness = (uint8_t)brightCmd;
    brightCmd = -1;
    pixels.setBrightness(brightnessNow());
    prefs.putUChar("bright", ledBrightness);
    mqttPubState = true;
    Serial.printf("BRIGHT: %u (MQTT)\n", (unsigned)ledBrightness);
  }

  // ---- day/night brightness ----
  // Check once a second; the value only changes when the local clock crosses
  // one of the configured times.
  static unsigned long lastBrightCheck = 0;
  if (now - lastBrightCheck >= 1000UL) {
    lastBrightCheck = now;
    uint8_t want = brightnessNow();
    if (want != pixels.getBrightness()) pixels.setBrightness(want);
  }

  // ---- smooth hue transition (shortest way around the colour wheel) ----
  float diff = (float)((int32_t)targetHue - (int32_t)huePos);
  diff = fmodf(diff + 32768.0f, 65536.0f);
  if (diff < 0.0f) diff += 65536.0f;
  diff -= 32768.0f;
  huePos += diff * 0.12f;
  if (huePos < 0.0f) huePos += 65536.0f;
  if (huePos >= 65536.0f) huePos -= 65536.0f;

  // ---- MQTT 8x8 picture overlay ----
  if (bmpArrived) {
    bmpArrived = false;
    if (bmpClearStaging) {
      bmpActive = false;
      Serial.println("BMP: cleared");
    } else {
      memcpy(bmpPixels, bmpStaging, sizeof(bmpPixels));
      bmpActive = true;
      bmpUntil = bmpMsStaging ? (millis() + bmpMsStaging) : 0;
      msgActive = false;  // a picture replaces the text ticker
      Serial.println("BMP: shown");
    }
  }
  if (bmpActive && bmpUntil != 0 && (int32_t)(millis() - bmpUntil) >= 0) {
    bmpActive = false;
    Serial.println("BMP: timed out");
  }

  bool overlay = msgActive || bmpActive;
  if (effectIndex == 0) {
    updateClock(dt);
    if (!overlay) renderClock();
  } else {
    updateDimmer(dt);
    if (!overlay) renderDimmer();
  }
  if (bmpActive) renderBitmap();
  else if (msgActive) renderMessage();

#if DEBUG_KNOB
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug >= 500) {
    lastDebug = millis();
    Serial.print("t="); Serial.print(millis());
    Serial.print(" angX="); Serial.print(tiltAngleX, 2);
    Serial.print(" angY="); Serial.print(tiltAngleY, 2);
    Serial.print(" | a="); Serial.print(tiltAX, 2);
    Serial.print(","); Serial.print(tiltAY, 2);
    Serial.print(","); Serial.print(tiltAZ, 2);
    Serial.print(" | knob="); Serial.println(knobPct);
  }
#endif

  delay(16); // ~60 fps
}

// 3x5 digits for the calendar sheet (date) and the dimmer percentage.
static const uint8_t CAL_FONT[10][5] = {
  {0b111, 0b101, 0b101, 0b101, 0b111}, // 0
  {0b010, 0b110, 0b010, 0b010, 0b111}, // 1
  {0b111, 0b001, 0b111, 0b100, 0b111}, // 2
  {0b111, 0b001, 0b111, 0b001, 0b111}, // 3
  {0b101, 0b101, 0b111, 0b001, 0b001}, // 4
  {0b111, 0b100, 0b111, 0b001, 0b111}, // 5
  {0b111, 0b100, 0b111, 0b101, 0b111}, // 6
  {0b111, 0b001, 0b010, 0b010, 0b010}, // 7
  {0b111, 0b101, 0b111, 0b101, 0b111}, // 8
  {0b111, 0b101, 0b111, 0b001, 0b111}, // 9
};

// ===================== tilt sensing =====================
// Accelerometer gravity vector -> two tilt angles in degrees. The clock uses
// them to stay upright; the dimmer uses the angle in the screen plane to read
// how far the badge has been turned.
void updateTilt(float dt) {
  static float angleX = 0.0f, angleY = 0.0f;
  static unsigned long lastGoodSample = 0;
  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  bool gotSample = false;

  if (qmiAccelReady() && qmiReadAccel(ax, ay, az)) {
    gotSample = true;
    tiltAX = ax;
    tiltAY = ay;
    tiltAZ = az;

    // Same angle convention as the spirit-level firmware.
    float rawX = atan2f(ay, sqrtf(ax * ax + az * az)) * 180.0f / PI;
    float rawY = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / PI;

    float aAng = 1.0f - expf(-dt / 0.06f);
    angleX += aAng * (rawX - angleX);
    angleY += aAng * (rawY - angleY);
  }

  tiltAngleX = angleX;
  tiltAngleY = angleY;

  // sensor watchdog: restart bus + sensor if no fresh sample for 2 s
  if (gotSample) {
    lastGoodSample = millis();
  } else if (millis() - lastGoodSample > 2000UL) {
    qmiRecover();
    lastGoodSample = millis();
  }
}

// ===================== lamp dimmer =====================
// The badge is the knob. While it is held more or less upright, gravity lies
// in the screen plane and its angle (atan2(ay, ax)) follows the rotation
// absolutely: turn the badge clockwise and the value rises, no drift, any
// number of turns. Laid flat on the table gravity is perpendicular to the
// screen and carries no angle at all, so there the gyro takes over and its
// rate is integrated (with the bias relearned while the badge is still).
void qmiReadGyro(float &gx, float &gy, float &gz) {
  uint8_t buf[6];
  gx = gy = gz = 0.0f;
  if (!qmiReadReg(0x3B, buf, 6)) return;
  const float scale = 64.0f / 32768.0f; // CTRL3: 64 dps full scale
  gx = (int16_t)((buf[1] << 8) | buf[0]) * scale;
  gy = (int16_t)((buf[3] << 8) | buf[2]) * scale;
  gz = (int16_t)((buf[5] << 8) | buf[4]) * scale;
}

void updateDimmer(float dt) {
  unsigned long now = millis();
  float delta = 0.0f;  // percent, before smoothing/clamping

  float inPlane = sqrtf(tiltAX * tiltAX + tiltAY * tiltAY);

  if (inPlane >= KNOB_FLAT_G) {
    // ---- upright: absolute angle from gravity ----
    float ang = atan2f(tiltAY, tiltAX) * 180.0f / PI;
    if (!knobAngleValid) {
      knobAngle = ang;
      knobAngleValid = true;
    }
    float d = ang - knobAngle;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    knobAngle = ang;

    // Ignore sensor noise, then clockwise (negative d) raises brightness.
    if (fabsf(d) < KNOB_DEADBAND_DEG) d = 0.0f;
    // Near flat the in-plane angle becomes ill-conditioned; a big jump there
    // is a lift/tilt, not a turn, so cap the per-frame step (real turns stay
    // far below this at 60 fps).
    if (d > 20.0f) d = 20.0f;
    if (d < -20.0f) d = -20.0f;
    delta = -d / KNOB_DEG_PER_PCT;
    gyroStillSince = now;
  } else {
    // ---- flat on the table: integrate the gyro rate ----
    knobAngleValid = false;
    float gx, gy, gz;
    qmiReadGyro(gx, gy, gz);
    (void)gx;
    (void)gy;

    // While the badge is still, the gyro output is pure bias: keep learning
    // it so a slow turn does not fight a drifting zero point. Only very small
    // rates count as "still" - a slow deliberate turn must not be absorbed.
    if (fabsf(gz - gyroBias) < 0.6f) {
      if (now - gyroStillSince > KNOB_GYRO_BIAS_MS) {
        gyroBias += (gz - gyroBias) * 0.01f;
      }
    } else {
      gyroStillSince = now;
    }

    float dps = (gz - gyroBias) * KNOB_GYRO_SIGN;
    if (fabsf(dps) < KNOB_GYRO_DEADBAND) dps = 0.0f;
    delta = dps * dt / KNOB_DEG_PER_PCT;
  }

  if (delta != 0.0f) {
    knobPctF += delta;
    if (knobPctF < 0.0f) knobPctF = 0.0f;
    if (knobPctF > 100.0f) knobPctF = 100.0f;

    int v = (int)lroundf(knobPctF);
    if (v != knobPct) {
      knobPct = v;
      knobChangedAt = now;
      knobSaveDirty = true;
    }
  }

  // Store only once the knob has come to rest: NVS writes are slow and the
  // flash should not be rewritten on every frame of a turn.
  if (knobSaveDirty && now - knobChangedAt > 1500UL) {
    knobSaveDirty = false;
    clockPrefs.putUChar("knob", (uint8_t)knobPct);
  }

  // Hand a throttled value to the MQTT task (it owns the client).
  if (knobPct != knobPublished && now - knobLastPub >= KNOB_PUBLISH_MS) {
    knobPublished = knobPct;
    knobLastPub = now;
    knobPub = knobPct;
  }
}

static void dimSet(uint8_t *fr, uint8_t *fg, uint8_t *fb, int x, int y,
                   int r, int g, int b) {
  if (x < 0 || x > 7 || y < 0 || y > 7) return;
  int i = y * 8 + x;
  fr[i] = (uint8_t)constrain(r, 0, 255);
  fg[i] = (uint8_t)constrain(g, 0, 255);
  fb[i] = (uint8_t)constrain(b, 0, 255);
}

static void dimDigit(uint8_t *fr, uint8_t *fg, uint8_t *fb, int x, int y,
                     int d, int r, int g, int b) {
  for (int row = 0; row < 5; row++) {
    uint8_t bits = CAL_FONT[d][row];
    for (int col = 0; col < 3; col++) {
      if (bits & (4 >> col)) dimSet(fr, fg, fb, x + col, y + row, r, g, b);
    }
  }
}

// 8x8 dimmer face: just the percentage, in 3x5 digits with a clear two-pixel
// gap between them. The frame is pushed without the upright correction, so the
// reading turns with the badge exactly like a real knob.
void renderDimmer() {
  uint8_t fr[64], fg[64], fb[64];
  memset(fr, 0, sizeof(fr));
  memset(fg, 0, sizeof(fg));
  memset(fb, 0, sizeof(fb));

  int pct = knobPct;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  // warm lamp white, a little brighter as the lamp gets brighter
  float k = 0.55f + 0.45f * (float)pct / 100.0f;
  int r = (int)(255 * k), g = (int)(198 * k), b = (int)(120 * k);

  if (pct >= 100) {
    // "100" does not fit in 8 columns with a gap: the hundreds digit is a
    // 1-px stroke at the left edge, then the two zeros fill the rest.
    for (int row = 0; row < 5; row++) dimSet(fr, fg, fb, 0, 1 + row, r, g, b);
    dimDigit(fr, fg, fb, 2, 1, 0, r, g, b);
    dimDigit(fr, fg, fb, 5, 1, 0, r, g, b);
  } else if (pct >= 10) {
    // two digits at the outer edges, columns 3 and 4 stay dark
    dimDigit(fr, fg, fb, 0, 1, pct / 10, r, g, b);
    dimDigit(fr, fg, fb, 5, 1, pct % 10, r, g, b);
  } else {
    dimDigit(fr, fg, fb, 3, 1, pct, r, g, b);
  }

  // Rotation is shown as raw device orientation on purpose.
  showFrameOrient(fr, fg, fb, 0);
}

// ===================== clock + weather =====================
// Six full-screen pages live on a 16x24 virtual canvas arranged as a 2x3
// grid. The 8x8 window visits them in a loop, gliding one whole screen at a
// time, so every page is calm and readable:
//
//      +-----------+-----------+
//      | calendar  | weekday   |
//      |   "14"    | SATURDAY  |
//      +-----------+-----------+
//      | weather   | hours     |
//      |  icon     |   HH      |
//      +-----------+-----------+
//      | temp      | minutes   |
//      |  "24°"    |   MM      |
//      +-----------+-----------+
//
//   hours --down--> minutes --left--> temp --up--> icon --up--> calendar
//      --right--> weekday --down--> hours
//
// The weekday ticker scrolls in from the right after the calendar sheet has
// slid away. Time comes from NTP over Wi-Fi, weather from Open-Meteo. While
// the badge is running it also serves a small configuration page (its IP is
// printed on the serial console) where you can pick the time zone, the
// weather city, MQTT and Wi-Fi settings.

// 3x7 digits. Each row is a 3-bit pattern, the leftmost pixel is bit 2.
static const uint8_t CLOCK_FONT[11][7] = {
  {0b111, 0b101, 0b101, 0b101, 0b101, 0b101, 0b111}, // 0
  {0b010, 0b110, 0b010, 0b010, 0b010, 0b010, 0b111}, // 1
  {0b111, 0b001, 0b001, 0b111, 0b100, 0b100, 0b111}, // 2
  {0b111, 0b001, 0b001, 0b111, 0b001, 0b001, 0b111}, // 3
  {0b101, 0b101, 0b101, 0b111, 0b001, 0b001, 0b001}, // 4
  {0b111, 0b100, 0b100, 0b111, 0b001, 0b001, 0b111}, // 5
  {0b111, 0b100, 0b100, 0b111, 0b101, 0b101, 0b111}, // 6
  {0b111, 0b001, 0b001, 0b001, 0b001, 0b001, 0b001}, // 7
  {0b111, 0b101, 0b101, 0b111, 0b101, 0b101, 0b111}, // 8
  {0b111, 0b101, 0b101, 0b111, 0b001, 0b001, 0b111}, // 9
  {0b000, 0b000, 0b000, 0b111, 0b000, 0b000, 0b000}, // -
};


enum WeatherKind {
  W_UNKNOWN = 0, W_CLEAR, W_PARTLY, W_CLOUDY, W_FOG, W_RAIN, W_SNOW, W_STORM
};

// ---- canvas, 2 x 3 full-screen pages of 8 x 8 ----
uint8_t clockR[CLOCK_W * CLOCK_H];
uint8_t clockG[CLOCK_W * CLOCK_H];
uint8_t clockB[CLOCK_W * CLOCK_H];

// ---- page order and viewport position ----
// 0 = hours (second row, right), 1 = minutes (third row, right),
// 2 = temperature (third row, left), 3 = weather icon (second row, left),
// 4 = calendar sheet (top row, left), 5 = weekday ticker (top row, right).
static const uint8_t CLOCK_STOP[CLOCK_STAGES][2] = {
  { 8, 8 }, { 8, 16 }, { 0, 16 }, { 0, 8 }, { 0, 0 }, { 8, 0 }
};
static const uint16_t CLOCK_PAUSE[CLOCK_STAGES] = {
  3500, 2600, 3500, 4200, 3200, 4200
};

// weekday ticker: position inside its page, advanced while stage 5 is shown
float weekdayX = 8.0f;

uint16_t clockStage = 0;
float clockViewX = 8.0f, clockViewY = 0.0f; // top-left canvas cell on screen
bool clockMoving = false;
float clockMoveFromX = 8.0f, clockMoveFromY = 0.0f;
float clockMoveToX = 8.0f, clockMoveToY = 0.0f;
unsigned long clockMoveStart = 0, clockMoveDur = 0;
unsigned long clockNextMove = 0;

// ---- upright orientation (from the accelerometer) ----
// 0 = 0 deg, 1 = 90 deg clockwise, 2 = 180 deg, 3 = 270 deg clockwise.
uint8_t clockOrient = 0;
uint8_t clockOrientCand = 0;
unsigned long clockOrientSince = 0;

// ---- network ----
enum ClockNetState {
  CLOCK_NET_IDLE,
  CLOCK_NET_CONNECTING,
  CLOCK_NET_ONLINE,
  CLOCK_NET_RETRY,
  CLOCK_NET_AP        // own setup hotspot is running
};
ClockNetState clockNetState = CLOCK_NET_IDLE;
unsigned long clockNetTimer = 0;
unsigned long clockLastNtp = 0;
bool clockTimeLogged = false;
uint8_t wifiFailCount = 0;

volatile bool weatherRequest = false;
volatile bool weatherValid = false;
volatile int16_t weatherTemp = 20;
volatile uint8_t weatherCode = 0;
volatile uint8_t weatherIsDay = 1;
volatile unsigned long weatherLastTry = 0;
bool weatherGeoDone = false;
float weatherIpLat = FIXED_LAT;
float weatherIpLon = FIXED_LON;

// ---- MQTT ----
char mqttHost[64] = "";
uint16_t mqttPort = 1883;
char mqttUser[33] = "";
char mqttPass[65] = "";
char mqttTopic[64] = MQTT_DEFAULT_TOPIC;
uint8_t mqttEnabled = 0;
volatile bool mqttDirty = false;   // settings changed, reconnect needed

// ---- Bluetooth -> MQTT bridge (configuration moved near the top) ----

// ---- Wi-Fi credentials (editable from the web page) ----
char wifiSsid[33] = CLOCK_WIFI_SSID;
char wifiPass[65] = CLOCK_WIFI_PASS;

// ---- user configuration (persisted in NVS namespace "clock") ----
char clockTz[40] = CLOCK_TZ;
char clockCity[28] = "Madrid";
float clockLat = FIXED_LAT;
float clockLon = FIXED_LON;
uint8_t clockUseIp = 1;

// page colours: time digits and temperature
uint32_t clockTimeColor = 0xFFFFFF;
uint8_t clockTimeColorSet = 0;      // 0 = follow the BOOT base colour
uint32_t clockTempColor = 0xFFDC82;
uint8_t clockTempAuto = 1;          // 1 = colour by temperature value

// day/night brightness schedule (minutes from midnight)
uint8_t brightSched = 0;
uint16_t dayStartMin = 8 * 60;
uint16_t nightStartMin = 22 * 60;
uint8_t dayBright = 180;
uint8_t nightBright = 25;

// Effective LED brightness: the manual value unless the day/night schedule is
// enabled and the local time is known. Handles windows that wrap midnight.
uint8_t brightnessNow() {
  if (!brightSched) return ledBrightness;
  time_t tt = time(nullptr);
  if (tt < 1700000000) return ledBrightness; // NTP not synced yet

  struct tm tmv;
  localtime_r(&tt, &tmv);
  int mins = tmv.tm_hour * 60 + tmv.tm_min;
  bool day;
  if (dayStartMin <= nightStartMin) {
    day = (mins >= (int)dayStartMin && mins < (int)nightStartMin);
  } else {
    day = !(mins >= (int)nightStartMin && mins < (int)dayStartMin);
  }
  return day ? dayBright : nightBright;
}


struct TzOption { const char *label; const char *posix; };
static const TzOption TZ_OPTIONS[] = {
  { "Madrid (CET/CEST)",        "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "London (GMT/BST)",         "GMT0BST,M3.5.0/1,M10.5.0" },
  { "Lisbon (WET/WEST)",        "WET0WEST,M3.5.0/1,M10.5.0" },
  { "Paris / Berlin (CET)",     "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Kyiv (EET/EEST)",          "EET-2EEST,M3.5.0/3,M10.5.0/4" },
  { "Moscow (MSK)",             "MSK-3" },
  { "Dubai (GST)",              "GST-4" },
  { "Tokyo (JST)",              "JST-9" },
  { "New York (EST/EDT)",       "EST5EDT,M3.2.0,M11.1.0" },
  { "Los Angeles (PST/PDT)",    "PST8PDT,M3.2.0,M11.1.0" },
  { "UTC",                      "UTC0" },
};
static const uint8_t TZ_OPTION_COUNT = sizeof(TZ_OPTIONS) / sizeof(TZ_OPTIONS[0]);

// ===================== tiny drawing helpers =====================
inline void canvasSet(int x, int y, float r, float g, float b) {
  if (x < 0 || x >= CLOCK_W || y < 0 || y >= CLOCK_H) return;
  int i = y * CLOCK_W + x;
  clockR[i] = (uint8_t)constrain(r + 0.5f, 0.0f, 255.0f);
  clockG[i] = (uint8_t)constrain(g + 0.5f, 0.0f, 255.0f);
  clockB[i] = (uint8_t)constrain(b + 0.5f, 0.0f, 255.0f);
}

// A pair of 3x7 digits with a two-pixel gap (exactly 8 columns wide).
void drawClockPair(int x, int y, const char *txt, float r, float g, float b,
                   float k) {
  for (int i = 0; i < 2; i++) {
    char ch = txt[i];
    uint8_t idx = (ch >= '0' && ch <= '9') ? (uint8_t)(ch - '0') : 10;
    for (int row = 0; row < 7; row++) {
      uint8_t bits = CLOCK_FONT[idx][row];
      for (int col = 0; col < 3; col++) {
        if (bits & (4 >> col)) {
          canvasSet(x + i * 5 + col, y + row, r * k, g * k, b * k);
        }
      }
    }
  }
}

bool clockTimeReady() {
  return time(nullptr) > 1700000000; // sane epoch: SNTP has synced
}

// ===================== pages =====================
void clockDigitColor(float &r, float &g, float &b) {
  if (clockTimeColorSet) {
    r = (float)((clockTimeColor >> 16) & 0xFF);
    g = (float)((clockTimeColor >> 8) & 0xFF);
    b = (float)(clockTimeColor & 0xFF);
  } else {
    uint32_t c = pixels.ColorHSV((uint16_t)huePos, 255, 255);
    r = (float)((c >> 16) & 0xFF);
    g = (float)((c >> 8) & 0xFF);
    b = (float)(c & 0xFF);
  }
}

void drawClockHours(float t) {
  bool ok = clockTimeReady();
  char txt[3] = "--";
  if (ok) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(txt, sizeof(txt), "%02d", tmv.tm_hour);
  }

  float r, g, b;
  clockDigitColor(r, g, b);

  drawClockPair(8, 8, txt, r, g, b, 1.0f);

  // one short pulse per second in the free bottom-right corner
  if (ok) {
    float ph = fmodf(t, 1.0f);
    float k = 0.30f + 0.70f * expf(-ph * 7.0f);
    canvasSet(15, 15, r * 0.7f * k, g * 0.7f * k, b * 0.7f * k);
  }
}

void drawClockMinutes(float t) {
  bool ok = clockTimeReady();
  char txt[3] = "--";
  if (ok) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(txt, sizeof(txt), "%02d", tmv.tm_min);
  }

  float r, g, b;
  clockDigitColor(r, g, b);

  drawClockPair(8, 16, txt, r, g, b, 1.0f);

  if (ok) {
    float ph = fmodf(t, 1.0f);
    float k = 0.30f + 0.70f * expf(-ph * 7.0f);
    canvasSet(15, 23, r * 0.7f * k, g * 0.7f * k, b * 0.7f * k);
  }
}

// ===================== weather icons =====================
// The icons live on one 8x8 page of the canvas (left column, middle row).
// Every moving element is placed at a fractional position and splatted over
// the neighbouring pixels, so clouds, drops and rays glide smoothly instead of
// hopping pixel to pixel.
#define ICON_PAGE_X 0
#define ICON_PAGE_Y 8

// Icon preview: set WEATHER_DEMO to 1 to show every weather icon full screen,
// day then night, one every WEATHER_DEMO_MS. 0 = normal clock behaviour.
#define WEATHER_DEMO     0
#define WEATHER_DEMO_MS  2000UL

inline void iconAdd(int x, int y, float r, float g, float b) {
  if (x < 0 || x > 7 || y < 0 || y > 7) return;
  x += ICON_PAGE_X;
  y += ICON_PAGE_Y;
  int i = y * CLOCK_W + x;
  clockR[i] = (uint8_t)constrain((float)clockR[i] + r, 0.0f, 255.0f);
  clockG[i] = (uint8_t)constrain((float)clockG[i] + g, 0.0f, 255.0f);
  clockB[i] = (uint8_t)constrain((float)clockB[i] + b, 0.0f, 255.0f);
}

// Soft point: the four surrounding pixels get bilinear weights.
void iconDot(float x, float y, float r, float g, float b) {
  int x0 = (int)floorf(x);
  int y0 = (int)floorf(y);
  float fx = x - (float)x0;
  float fy = y - (float)y0;

  float w00 = (1.0f - fx) * (1.0f - fy);
  float w10 = fx * (1.0f - fy);
  float w01 = (1.0f - fx) * fy;
  float w11 = fx * fy;

  iconAdd(x0, y0, r * w00, g * w00, b * w00);
  iconAdd(x0 + 1, y0, r * w10, g * w10, b * w10);
  iconAdd(x0, y0 + 1, r * w01, g * w01, b * w01);
  iconAdd(x0 + 1, y0 + 1, r * w11, g * w11, b * w11);
}

void drawSunIcon(float t) {
  // Shape taken from the 8x8 reference bitmap: a round 4x4 core with cut
  // corners and eight one-pixel rays spaced around it.
  static const uint8_t CORE[8] = {
    0b00000000,
    0b00000000,
    0b00011000,
    0b00111100,
    0b00111100,
    0b00011000,
    0b00000000,
    0b00000000,
  };
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      if (CORE[y] & (0x80 >> x)) iconAdd(x, y, 255, 238, 70);
    }
  }

  // The eight ray pixels in clockwise order. A slow sine wave runs around
  // them, so each ray fades out and in smoothly, one after another.
  static const int8_t RAY_X[8] = { 2, 5, 7, 7, 5, 2, 0, 0 };
  static const int8_t RAY_Y[8] = { 0, 0, 2, 5, 7, 7, 5, 2 };
  for (int i = 0; i < 8; i++) {
    float wave = 0.5f + 0.5f * sinf(t * 1.6f - (float)i * 0.7853982f);
    float br = 0.12f + 0.88f * wave;
    iconAdd(RAY_X[i], RAY_Y[i], 255 * br, 228 * br, 55 * br);
  }
}

void drawMoonIcon(float t) {
  // Golden crescent and single-pixel stars, matching the 8x8 night icon
  // reference (a crescent with a handful of stars around it).
  static const uint8_t MOON[8] = {
    0b00000000,
    0b00100000,
    0b01100000,
    0b01100000,
    0b01110000,
    0b01111110,
    0b00111100,
    0b00000000,
  };
  static const uint8_t STARS[8] = {
    0b10010001,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000001,
    0b00000000,
    0b00000000,
    0b10000000,
  };

  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      if (!(MOON[y] & (0x80 >> x))) continue;
      // Slightly deeper gold inside the crescent, warm highlight on the rim.
      float k = (y == 5 || x == 1) ? 1.0f : 0.82f;
      float shimmer = 0.94f + 0.06f * sinf(t * 1.3f + (float)(x + y) * 0.4f);
      iconAdd(x, y, 235 * k * shimmer, 208 * k * shimmer, 128 * k * shimmer);
    }
  }

  // Sharp single-pixel stars, each twinkling on its own phase.
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      if (!(STARS[y] & (0x80 >> x))) continue;
      float k = 0.35f + 0.65f * (0.5f + 0.5f * sinf(t * 2.1f + (float)(x * 3 + y * 5)));
      iconAdd(x, y, 255 * k, 252 * k, 225 * k);
    }
  }

  // A shooting star crosses the sky about every seven seconds.
  float cyc = fmodf(t, 7.0f);
  if (cyc < 1.1f) {
    float p = cyc / 1.1f;
    float hx = 1.0f + p * 5.5f;
    float hy = 0.3f + p * 3.0f;
    float fade = 1.0f - p;
    iconDot(hx, hy, 255 * fade, 250 * fade, 240 * fade);
    iconDot(hx - 0.9f, hy - 0.45f, 170 * fade, 175 * fade, 200 * fade);
    iconDot(hx - 1.7f, hy - 0.8f, 80 * fade, 85 * fade, 110 * fade);
  }
}

// A small 3x3 golden crescent used by the cloudy night icons.
void drawMoonBadge(float x, float y, float br) {
  const float r = 235 * br, g = 208 * br, b = 128 * br;
  iconDot(x + 0.4f, y + 0.4f, r, g, b);
  iconDot(x + 1.4f, y + 0.4f, r, g, b);
  iconDot(x + 0.4f, y + 1.4f, r, g, b);
  iconDot(x + 0.4f, y + 2.4f, r, g, b);
  iconDot(x + 1.4f, y + 2.4f, r, g, b);
}

// Two sharp stars in the free top corners, for the cloudy night icons.
void drawNightStars(float t, float br) {
  const float sx[2] = { 0.7f, 7.2f };
  const float sy[2] = { 0.5f, 0.6f };
  for (int i = 0; i < 2; i++) {
    float k = br * (0.35f + 0.65f * (0.5f + 0.5f * sinf(t * 2.3f + (float)i * 2.4f)));
    iconDot(sx[i], sy[i], 255 * k, 252 * k, 225 * k);
  }
}

// Shared 8x8 cloud body. `dx` may be fractional, so the cloud drifts smoothly.
// `dark` gives a stormy grey, `bright` makes the whole cloud breathe.
static const uint8_t CLOUD_BITS[5] = {
  0b00111100,
  0b01111110,
  0b11111111,
  0b11111111,
  0b01111110,
};

void drawCloudShape(float dx, int y0, bool dark, float bright, bool night = false) {
  float v = (dark ? 0.5f : 0.92f) * bright;
  for (int row = 0; row < 5; row++) {
    float k = 1.0f;
    if (row == 0) k = 0.65f;
    else if (row == 4) k = 0.55f;

    float r, g, b;
    if (night) {
      // Night clouds keep a cold blue-grey tint instead of the daylight white.
      r = (dark ? 70.0f : 92.0f) * v * k;
      g = (dark ? 88.0f : 116.0f) * v * k;
      b = (dark ? 128.0f : 165.0f) * v * k;
    } else {
      r = (dark ? 150.0f : 200.0f) * v * k;
      g = (dark ? 165.0f : 214.0f) * v * k;
      b = (dark ? 185.0f : 232.0f) * v * k;
    }

    for (int x = 0; x < 8; x++) {
      if (!(CLOUD_BITS[row] & (0x80 >> x))) continue;
      iconDot((float)x + dx, (float)(y0 + row), r, g, b);
    }
  }
}

void drawPartlyIcon(bool day, float t) {
  if (day) {
    // Small sun peeking from the top-left, its ray pulsing.
    for (int y = 0; y <= 2; y++) {
      for (int x = 0; x <= 2; x++) {
        float k = (x == 0 && y == 0) ? 0.55f : 1.0f;
        iconAdd(x, y, 255 * k, 210 * k, 95 * k);
      }
    }
    float p = 0.35f + 0.65f * (0.5f + 0.5f * sinf(t * 2.6f));
    iconDot(4.2f + 0.5f * sinf(t * 0.8f), 0.6f, 255 * p, 160 * p, 40 * p);
  } else {
    // Golden crescent badge and two stars, matching the night reference icon.
    drawMoonBadge(0.2f, 0.0f, 0.95f);
    drawNightStars(t, 0.9f);
  }

  // Back layer drifts one way, the front layer the other: the icon gains depth.
  drawCloudShape(-sinf(t * 0.37f) * 0.8f, 2, false, 0.62f, !day);
  drawCloudShape(sinf(t * 0.5f + 1.0f) * 1.0f, 3, false, 0.95f, !day);
}

// Cloudy sky with no sun or moon visible: two layers at day, a moon badge and
// stars behind dark blue-grey clouds at night.
void drawCloudyIcon(bool day, float t) {
  if (day) {
    drawCloudShape(-sinf(t * 0.31f) * 0.9f, 1, false, 0.6f);
    drawCloudShape(sinf(t * 0.44f + 0.8f) * 1.0f, 2, false, 0.88f);
  } else {
    drawMoonBadge(0.2f, 0.0f, 0.8f);
    drawNightStars(t, 0.8f);
    drawCloudShape(-sinf(t * 0.31f) * 0.9f, 1, true, 1.0f, true);
    drawCloudShape(sinf(t * 0.44f + 0.8f) * 1.0f, 2, false, 0.8f, true);
  }
}

void drawRainIcon(bool day, float t) {
  float breath = 0.9f + 0.1f * sinf(t * 1.6f);
  if (!day) drawNightStars(t, 0.75f);
  drawCloudShape(sinf(t * 0.45f) * 0.8f, 1, false, breath, !day);

  for (int i = 0; i < 5; i++) {
    float speed = 1.7f + 0.35f * (float)(i % 3); // drops fall at different rates
    float ph = fmodf(t * speed + (float)i * 0.23f, 1.0f);
    float x = 0.8f + (float)i * 1.55f + 0.18f * sinf(t * 2.0f + (float)i);
    float y = 5.0f + ph * 3.0f;
    float k = 0.55f + 0.45f * sinf(ph * 3.14159f);

    iconDot(x, y - 0.55f, 45 * k, 95 * k, 190 * k); // motion streak
    iconDot(x, y, 90 * k, 175 * k, 255 * k);

    // tiny splash when the drop hits the bottom row
    if (ph > 0.86f) {
      float s = (ph - 0.86f) / 0.14f;
      iconDot(x - 0.6f, 7.4f, 70 * (1 - s), 150 * (1 - s), 255 * (1 - s));
      iconDot(x + 0.6f, 7.4f, 70 * (1 - s), 150 * (1 - s), 255 * (1 - s));
    }
  }
}

void drawSnowIcon(bool day, float t) {
  float breath = 0.9f + 0.1f * sinf(t * 1.6f);
  if (!day) drawNightStars(t, 0.75f);
  drawCloudShape(sinf(t * 0.4f) * 0.8f, 1, false, breath, !day);

  for (int i = 0; i < 5; i++) {
    float speed = 0.55f + 0.12f * (float)(i % 3);
    float ph = fmodf(t * speed + (float)i * 0.19f, 1.0f);
    float x = 0.7f + (float)i * 1.55f + 0.75f * sinf(t * 1.4f + (float)i * 1.9f);
    float y = 5.0f + ph * 3.0f;
    float k = 0.45f + 0.55f * (0.5f + 0.5f * sinf(t * 3.2f + (float)i * 1.1f));
    iconDot(x, y, 235 * k, 245 * k, 255 * k);
  }
}

void drawStormIcon(bool day, float t) {
  // Double flash with a fast fade and a warm afterglow.
  float cyc = fmodf(t, 2.8f);
  float flash = 0.0f;
  if (cyc < 0.10f) flash = 1.0f;
  else if (cyc < 0.22f) flash = 0.35f;
  else if (cyc > 0.34f && cyc < 0.44f) flash = 0.8f;
  else if (cyc > 0.44f && cyc < 0.75f) flash = 0.35f * (1.0f - (cyc - 0.44f) / 0.31f);

  float cloudBright = 0.45f + 0.55f * flash + 0.05f * sinf(t * 1.2f);
  drawCloudShape(sinf(t * 0.4f) * 0.7f, 1, true, cloudBright, !day);

  // Faint rain under the cloud.
  for (int i = 0; i < 3; i++) {
    float ph = fmodf(t * 2.2f + (float)i * 0.31f, 1.0f);
    iconDot(1.4f + (float)i * 2.3f, 5.2f + ph * 2.2f, 35, 70, 120);
  }

  // Lightning bolt with an afterglow halo.
  const int BOLT[6][2] = { { 4, 4 }, { 3, 5 }, { 4, 5 }, { 3, 6 }, { 2, 6 }, { 2, 7 } };
  float k = 0.28f + 0.72f * flash;
  for (int i = 0; i < 6; i++) {
    iconAdd(BOLT[i][0], BOLT[i][1], 255 * k, 235 * k, 90 * k);
    iconAdd(BOLT[i][0], BOLT[i][1], 255 * k * 0.25f, 220 * k * 0.25f, 60 * k * 0.25f);
  }
  if (flash > 0.2f) {
    // The halo makes the flash feel much bigger than six pixels.
    iconAdd(2, 5, 60 * flash, 55 * flash, 20 * flash);
    iconAdd(5, 5, 60 * flash, 55 * flash, 20 * flash);
    iconAdd(3, 7, 60 * flash, 55 * flash, 20 * flash);
  }
}

void drawFogIcon(bool day, float t) {
  // A faint golden moon shows through the mist at night.
  if (!day) drawMoonBadge(5.2f, 0.0f, 0.5f);
  for (int band = 0; band < 4; band++) {
    int y = 1 + band * 2;
    float speed = 1.1f + 0.25f * (float)band;
    for (int x = 0; x < 8; x++) {
      float wave = 0.5f + 0.5f * sinf((float)x * 0.95f + t * speed + (float)band * 1.4f);
      float k = 0.22f + 0.78f * wave * wave;
      // The band thins towards its ends, looking like a drifting wisp.
      k *= 1.0f - 0.35f * fabsf((float)x - 3.5f) / 3.5f;
      float cr = day ? 175.0f : 110.0f;
      float cg = day ? 188.0f : 132.0f;
      float cb = day ? 215.0f : 175.0f;
      iconDot((float)x, (float)y + 0.25f * sinf((float)x * 0.7f + t * 1.3f),
              cr * k, cg * k, cb * k);
    }
  }
}

void drawLoadingIcon(float t) {
  float breath = 0.42f + 0.14f * sinf(t * 2.0f);
  drawCloudShape(0, 0, true, breath);

  // Three dots bob in a smooth wave.
  for (int i = 0; i < 3; i++) {
    float k = 0.25f + 0.75f * (0.5f + 0.5f * sinf(t * 4.0f - (float)i * 1.2f));
    iconDot(2.0f + (float)i * 2.0f,
            7.0f + 0.35f * sinf(t * 3.0f - (float)i * 1.2f),
            235 * k, 240 * k, 250 * k);
  }
}

uint8_t weatherKind(uint8_t code) {
  if (code == 0) return W_CLEAR;
  if (code == 1 || code == 2) return W_PARTLY;
  if (code == 3) return W_CLOUDY;
  if (code == 45 || code == 48) return W_FOG;
  if (code >= 51 && code <= 67) return W_RAIN;
  if (code >= 71 && code <= 77) return W_SNOW;
  if (code >= 80 && code <= 82) return W_RAIN;
  if (code == 85 || code == 86) return W_SNOW;
  if (code >= 95) return W_STORM;
  return W_CLOUDY;
}

void drawWeatherIconPage(float t) {
#if WEATHER_DEMO
  // Day and night variant of each kind, two seconds apart, looping forever.
  static const uint8_t DEMO[7] = { W_CLEAR, W_PARTLY, W_CLOUDY, W_FOG,
                                   W_RAIN, W_SNOW, W_STORM };
  uint32_t step = (uint32_t)(millis() / WEATHER_DEMO_MS) % 14UL;
  uint8_t kind = DEMO[step / 2];
  bool day = (step % 2) == 0;
#else
  if (!weatherValid) {
    drawLoadingIcon(t);
    return;
  }
  uint8_t kind = weatherKind(weatherCode);
  bool day = weatherIsDay != 0;
#endif

  switch (kind) {
    case W_CLEAR:
      if (day) drawSunIcon(t);
      else drawMoonIcon(t);
      break;
    case W_PARTLY:
      drawPartlyIcon(day, t);
      break;
    case W_FOG:
      drawFogIcon(day, t);
      break;
    case W_RAIN:
      drawRainIcon(day, t);
      break;
    case W_SNOW:
      drawSnowIcon(day, t);
      break;
    case W_STORM:
      drawStormIcon(day, t);
      break;
    default:
      drawCloudyIcon(day, t);
      break;
  }
}

void drawTempPage() {
  bool valid = weatherValid;
  int temp = (int)weatherTemp;

  float r, g, b;
  if (!clockTempAuto) {
    r = (float)((clockTempColor >> 16) & 0xFF);
    g = (float)((clockTempColor >> 8) & 0xFF);
    b = (float)(clockTempColor & 0xFF);
  } else if (!valid) {
    r = 90; g = 95; b = 105;
  } else if (temp <= 0) {
    r = 140; g = 200; b = 255; // freezing
  } else if (temp < 12) {
    r = 180; g = 225; b = 255; // cold
  } else if (temp < 22) {
    r = 235; g = 240; b = 250; // mild
  } else if (temp < 30) {
    r = 255; g = 220; b = 130; // warm
  } else {
    r = 255; g = 160; b = 70;  // hot
  }

  // two digits at x0..2 and x4..6, degree dot in the top-right corner
  char txt[3] = "--";
  if (valid) {
    int v = temp;
    if (v < 0) v = -v;
    if (v > 99) v = 99;
    if (v >= 10) {
      snprintf(txt, sizeof(txt), "%02d", v);
    } else if (temp < 0) {
      txt[0] = '-';
      txt[1] = (char)('0' + v);
    } else {
      txt[0] = ' ';
      txt[1] = (char)('0' + v);
    }
  }

  for (int i = 0; i < 2; i++) {
    char ch = txt[i];
    if (ch == ' ') continue;
    uint8_t idx = (ch >= '0' && ch <= '9') ? (uint8_t)(ch - '0') : 10;
    for (int row = 0; row < 7; row++) {
      uint8_t bits = CLOCK_FONT[idx][row];
      for (int col = 0; col < 3; col++) {
        if (bits & (4 >> col)) {
          canvasSet(i * 4 + col, 16 + row, r, g, b);
        }
      }
    }
  }

  canvasSet(7, 16, r * 0.95f, g * 0.95f, b * 0.95f);
}

// ===================== calendar page =====================
// A tear-off calendar sheet: red binding strip, off-white paper and today's
// date in two 3x5 digits. The sheet slides away to the left and the weekday
// ticker follows it (drawn by drawWeekdayPage()).
void drawCalendarPage() {
  // paper
  for (int y = 1; y < 8; y++) {
    for (int x = 0; x < 8; x++) canvasSet(x, y, 172, 170, 160);
  }
  // red binding strip along the top
  for (int x = 0; x < 8; x++) canvasSet(x, 0, 205, 30, 26);

  if (!clockTimeReady()) return;

  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  int day = tmv.tm_mday;

  char txt[3];
  int len;
  if (day < 10) {
    txt[0] = (char)('0' + day);
    len = 1;
  } else {
    snprintf(txt, sizeof(txt), "%02d", day);
    len = 2;
  }

  int x0 = (8 - (len * 4 - 1)) / 2; // centre one or two 3-px digits
  for (int i = 0; i < len; i++) {
    int d = txt[i] - '0';
    for (int row = 0; row < 5; row++) {
      uint8_t bits = CAL_FONT[d][row];
      for (int col = 0; col < 3; col++) {
        if (bits & (4 >> col)) {
          canvasSet(x0 + i * 4 + col, 2 + row, 30, 26, 24);
        }
      }
    }
  }
}

// ===================== page rendering =====================
// Push a logical 8x8 frame to the strip, rotated by the given orientation
// (0 = as drawn, 1 = 90 deg cw, 2 = 180 deg, 3 = 270 deg cw).
void showFrameOrient(const uint8_t *fr, const uint8_t *fg, const uint8_t *fb,
                     uint8_t orient) {
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      int sx = x, sy = y;
      switch (orient & 3) {
        case 1:  sx = y;     sy = 7 - x; break; // 90 deg cw
        case 2:  sx = 7 - x; sy = 7 - y; break; // 180 deg
        case 3:  sx = 7 - y; sy = x;     break; // 270 deg cw
        default: break;
      }
      int src = sy * 8 + sx;
      pixels.setPixelColor(y * 8 + x,
                           pixels.Color(fr[src], fg[src], fb[src]));
    }
  }
  pixels.show();
}

// Clock and message frames follow the accelerometer so they read upright.
void showFrame(const uint8_t *fr, const uint8_t *fg, const uint8_t *fb) {
  showFrameOrient(fr, fg, fb, clockOrient);
}

// 5x7 font, ASCII 32..90 (lowercase is folded to uppercase).
static const uint8_t MSG_FONT[59][7] = {
  { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000 }, // ' '
  { 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100 }, // '!'
  { 0b01010, 0b01010, 0b01010, 0b00000, 0b00000, 0b00000, 0b00000 }, // '"'
  { 0b01010, 0b01010, 0b11111, 0b01010, 0b11111, 0b01010, 0b01010 }, // '#'
  { 0b00100, 0b01111, 0b10100, 0b01110, 0b00101, 0b11110, 0b00100 }, // '$'
  { 0b11001, 0b11010, 0b00010, 0b00100, 0b01000, 0b01011, 0b10011 }, // '%'
  { 0b01100, 0b10010, 0b10100, 0b01000, 0b10101, 0b10010, 0b01101 }, // '&'
  { 0b00100, 0b00100, 0b01000, 0b00000, 0b00000, 0b00000, 0b00000 }, // "'"
  { 0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010 }, // '('
  { 0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000 }, // ')'
  { 0b00000, 0b10101, 0b01110, 0b11111, 0b01110, 0b10101, 0b00000 }, // '*'
  { 0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000 }, // '+'
  { 0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00100, 0b01000 }, // ','
  { 0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000 }, // '-'
  { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100 }, // '.'
  { 0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000 }, // '/'
  { 0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110 }, // '0'
  { 0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 }, // '1'
  { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111 }, // '2'
  { 0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110 }, // '3'
  { 0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010 }, // '4'
  { 0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110 }, // '5'
  { 0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110 }, // '6'
  { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000 }, // '7'
  { 0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110 }, // '8'
  { 0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100 }, // '9'
  { 0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000 }, // ':'
  { 0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b00100, 0b01000 }, // ';'
  { 0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010 }, // '<'
  { 0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000 }, // '='
  { 0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000 }, // '>'
  { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100 }, // '?'
  { 0b01110, 0b10001, 0b10111, 0b10101, 0b10111, 0b10000, 0b01110 }, // '@'
  { 0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 }, // 'A'
  { 0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110 }, // 'B'
  { 0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110 }, // 'C'
  { 0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100 }, // 'D'
  { 0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111 }, // 'E'
  { 0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000 }, // 'F'
  { 0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111 }, // 'G'
  { 0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 }, // 'H'
  { 0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 }, // 'I'
  { 0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100 }, // 'J'
  { 0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001 }, // 'K'
  { 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111 }, // 'L'
  { 0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001 }, // 'M'
  { 0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001 }, // 'N'
  { 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 }, // 'O'
  { 0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000 }, // 'P'
  { 0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101 }, // 'Q'
  { 0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001 }, // 'R'
  { 0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110 }, // 'S'
  { 0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100 }, // 'T'
  { 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 }, // 'U'
  { 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100 }, // 'V'
  { 0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001 }, // 'W'
  { 0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001 }, // 'X'
  { 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100 }, // 'Y'
  { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111 }, // 'Z'
};

// The MQTT message scrolls right-to-left across the full matrix. Rows 0..6
// hold the glyph, row 7 stays dark so descenders have room.
void renderMessage() {
  uint8_t fr[64], fg[64], fb[64];
  memset(fr, 0, sizeof(fr));
  memset(fg, 0, sizeof(fg));
  memset(fb, 0, sizeof(fb));

  uint32_t c = clockTimeColorSet
                   ? clockTimeColor
                   : (pixels.ColorHSV((uint16_t)huePos, 255, 255) & 0xFFFFFF);
  uint8_t r = (c >> 16) & 0xFF;
  uint8_t g = (c >> 8) & 0xFF;
  uint8_t b = c & 0xFF;

  int len = (int)strlen(msgText);
  int x0 = (int)floorf(msgX);
  for (int i = 0; i < len; i++) {
    uint8_t ch = (uint8_t)msgText[i];
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    int gi = (ch >= 32 && ch <= 90) ? (int)ch - 32 : ('?' - 32);
    int cx = x0 + i * 6;
    if (cx > 7 || cx + 4 < 0) continue;
    for (int row = 0; row < 7; row++) {
      uint8_t bits = MSG_FONT[gi][row];
      for (int col = 0; col < 5; col++) {
        if (!(bits & (0x10 >> col))) continue;
        int px = cx + col;
        if (px < 0 || px > 7) continue;
        int dst = row * 8 + px;
        fr[dst] = r;
        fg[dst] = g;
        fb[dst] = b;
      }
    }
  }

  showFrame(fr, fg, fb);
}

// Full-screen 8x8 picture received over MQTT. The payload has already been
// expanded to 64 RGB pixels, so drawing is a straight copy.
void renderBitmap() {
  uint8_t fr[64], fg[64], fb[64];
  for (int i = 0; i < 64; i++) {
    fr[i] = bmpPixels[i * 3];
    fg[i] = bmpPixels[i * 3 + 1];
    fb[i] = bmpPixels[i * 3 + 2];
  }
  showFrame(fr, fg, fb);
}

// Scrolling weekday for the top-right page. The text enters from the right
// edge and is redrawn every frame; x is page-local (0..7 is visible).
void drawWeekdayPage() {
  if (!clockTimeReady()) return;

  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  static const char *const DAYS[7] = {
    "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"
  };
  const char *txt = DAYS[tmv.tm_wday % 7];

  // Pure white so the ticker stands out from the coloured clock digits.
  const float r = 255.0f, g = 255.0f, b = 255.0f;

  int len = (int)strlen(txt);
  int x0 = (int)floorf(weekdayX);
  for (int i = 0; i < len; i++) {
    int gi = (int)txt[i] - 32;
    if (gi < 0 || gi >= 59) continue;
    int cx = x0 + i * 6;
    if (cx > 7 || cx + 4 < 0) continue;
    for (int row = 0; row < 7; row++) {
      uint8_t bits = MSG_FONT[gi][row];
      for (int col = 0; col < 5; col++) {
        if (!(bits & (0x10 >> col))) continue;
        int px = cx + col;
        if (px < 0 || px > 7) continue;
        canvasSet(8 + px, row, r, g, b);
      }
    }
  }
}

void renderClock() {
  memset(clockR, 0, sizeof(clockR));
  memset(clockG, 0, sizeof(clockG));
  memset(clockB, 0, sizeof(clockB));

  float t = (float)millis() * 0.001f;
#if WEATHER_DEMO
  // Preview mode: no clock, just the icon page filling the matrix.
  drawWeatherIconPage(t);
  clockViewX = (float)ICON_PAGE_X;
  clockViewY = (float)ICON_PAGE_Y;
#else
  drawClockHours(t);
  drawClockMinutes(t);
  drawTempPage();
  drawWeatherIconPage(t);
  drawCalendarPage();
  drawWeekdayPage();
#endif

  // Sample an 8x8 window from the canvas with bilinear interpolation.
  // The viewport rests exactly on page boundaries, so still pages are sharp;
  // during a glide the motion is sub-pixel smooth.
  uint8_t frameR[64], frameG[64], frameB[64];
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      float sx = clockViewX + (float)x;
      float sy = clockViewY + (float)y;
      if (sx < 0.0f) sx = 0.0f;
      if (sx > CLOCK_W - 1) sx = CLOCK_W - 1;
      if (sy < 0.0f) sy = 0.0f;
      if (sy > CLOCK_H - 1) sy = CLOCK_H - 1;

      int x0 = (int)sx, y0 = (int)sy;
      int x1 = (x0 < CLOCK_W - 1) ? x0 + 1 : x0;
      int y1 = (y0 < CLOCK_H - 1) ? y0 + 1 : y0;
      float fx = sx - (float)x0;
      float fy = sy - (float)y0;

      int i00 = y0 * CLOCK_W + x0;
      int i10 = y0 * CLOCK_W + x1;
      int i01 = y1 * CLOCK_W + x0;
      int i11 = y1 * CLOCK_W + x1;

      float topR = clockR[i00] + (clockR[i10] - clockR[i00]) * fx;
      float topG = clockG[i00] + (clockG[i10] - clockG[i00]) * fx;
      float topB = clockB[i00] + (clockB[i10] - clockB[i00]) * fx;
      float botR = clockR[i01] + (clockR[i11] - clockR[i01]) * fx;
      float botG = clockG[i01] + (clockG[i11] - clockG[i01]) * fx;
      float botB = clockB[i01] + (clockB[i11] - clockB[i01]) * fx;

      int dst = y * 8 + x;
      frameR[dst] = (uint8_t)(topR + (botR - topR) * fy);
      frameG[dst] = (uint8_t)(topG + (botG - topG) * fy);
      frameB[dst] = (uint8_t)(topB + (botB - topB) * fy);
    }
  }

  // Rotate the logical frame so the text stays upright in the real world.
  showFrame(frameR, frameG, frameB);
}

// Keep the picture upright: the accelerometer tells which matrix edge points
// to world "up" and the frame is rotated to match. A dead zone plus a short
// confirmation delay stop the picture from twitching near 45 degrees.
void updateClockOrientation() {
  unsigned long now = millis();
  float ux = tiltAngleX; // world "up" in matrix coordinates
  float uy = tiltAngleY;
  float mag = sqrtf(ux * ux + uy * uy);

  uint8_t want = clockOrient;
  if (mag >= CLOCK_UPRIGHT_DEG) {
    if (fabsf(ux) > fabsf(uy)) {
      want = (ux > 0.0f) ? 1 : 3;
    } else {
      want = (uy > 0.0f) ? 2 : 0;
    }
  }

  if (want == clockOrient) {
    clockOrientCand = clockOrient;
    return;
  }
  if (want != clockOrientCand) {
    clockOrientCand = want;
    clockOrientSince = now;
  } else if (now - clockOrientSince >= 450UL) {
    clockOrient = want;
    clockOrientCand = want;
  }
}

// ===================== clock state =====================
void clockGotoStage(uint16_t next, unsigned long now) {
  clockStage = next;
  clockMoveFromX = clockViewX;
  clockMoveFromY = clockViewY;
  clockMoveToX = (float)CLOCK_STOP[next][0];
  clockMoveToY = (float)CLOCK_STOP[next][1];
  clockMoveStart = now;

  // The weekday starts off the right edge every time its page is shown.
  if (next == 5) weekdayX = 8.0f;

  float dyn = (float)dynPct / 100.0f;
  clockMoveDur = (unsigned long)((float)CLOCK_MOVE_MS / dyn);
  if (clockMoveDur < 260UL) clockMoveDur = 260UL;
  if (clockMoveDur > 1600UL) clockMoveDur = 1600UL;
  clockMoving = true;
}

void updateClock(float dt) {
  unsigned long now = millis();

  updateClockOrientation();

  if (!clockMoving) {
    if (clockStage == 5) {
      // Weekday ticker: slide left, restart once the word has left the page.
      weekdayX -= WEEKDAY_SPEED * dt;
      if (!clockTimeReady()) {
        weekdayX = 8.0f;
      } else {
        time_t tt = time(nullptr);
        struct tm tmv;
        localtime_r(&tt, &tmv);
        static const uint8_t DAY_LEN[7] = { 6, 6, 7, 9, 8, 6, 8 };
        if (weekdayX < -(float)(DAY_LEN[tmv.tm_wday % 7] * 6)) weekdayX = 8.0f;
      }
    }

    if ((long)(now - clockNextMove) >= 0) {
      clockGotoStage((uint16_t)((clockStage + 1) % CLOCK_STAGES), now);
    }
    return;
  }

  float p = (float)(now - clockMoveStart) / (float)clockMoveDur;
  if (p >= 1.0f) {
    clockViewX = clockMoveToX;
    clockViewY = clockMoveToY;
    clockMoving = false;
    clockNextMove = now + CLOCK_PAUSE[clockStage];
    return;
  }

  float e = p * p * (3.0f - 2.0f * p); // smoothstep
  clockViewX = clockMoveFromX + (clockMoveToX - clockMoveFromX) * e;
  clockViewY = clockMoveFromY + (clockMoveToY - clockMoveFromY) * e;
}

void clockEnter() {
  clockStage = 0;
  clockViewX = (float)CLOCK_STOP[0][0];
  clockViewY = (float)CLOCK_STOP[0][1];
  weekdayX = 8.0f;
  clockMoving = false;
  clockNextMove = millis() + 1500UL;
  clockTimeLogged = false;

  // Fetch the weather on the first entry; afterwards the network task keeps
  // the 15-minute refresh (and the 60-second retry until the first success).
  if (weatherLastTry == 0 ||
      millis() - weatherLastTry >=
          (weatherValid ? WEATHER_REFRESH_MS : 60000UL)) {
    weatherRequest = true;
  }
}

void clockLeave() {
  // The radio stays up so the settings page and MQTT messages keep working in
  // every effect; only the weather download is paused outside the clock.
  weatherRequest = false;
}

void setEffect(uint8_t next) {
  if (next == effectIndex) return;
  if (effectIndex == 0) clockLeave();
  effectIndex = next;
  if (effectIndex == 0) {
    clockEnter();
  } else {
    // Entering the dimmer: start from a known angle and refresh Home
    // Assistant with the current value.
    knobAngleValid = false;
    knobPctF = (float)knobPct;
    knobPublished = -1;
    knobPub = knobPct;
  }
  prefs.putUChar("effect", effectIndex);
  Serial.print("EFFECT: ");
  Serial.println(EFFECT_NAMES[effectIndex]);
}

// ===================== configuration =====================
void loadClockConfig() {
  if (!clockPrefs.begin("clock", false)) {
    Serial.println("CLOCK: config namespace unavailable");
  }
  String tz = clockPrefs.getString("tz", CLOCK_TZ);
  if (tz.length() > 0 && tz.length() < sizeof(clockTz)) {
    strncpy(clockTz, tz.c_str(), sizeof(clockTz) - 1);
    clockTz[sizeof(clockTz) - 1] = '\0';
  }
  String city = clockPrefs.getString("city", "Madrid");
  if (city.length() > 0) {
    strncpy(clockCity, city.c_str(), sizeof(clockCity) - 1);
    clockCity[sizeof(clockCity) - 1] = '\0';
  }
  clockLat = clockPrefs.getFloat("lat", FIXED_LAT);
  clockLon = clockPrefs.getFloat("lon", FIXED_LON);
  clockUseIp = clockPrefs.getUChar("ipgeo", 1) ? 1 : 0;

  clockTimeColor = clockPrefs.getUInt("tcolor", 0xFFFFFF);
  clockTimeColorSet = clockPrefs.getUChar("tcset", 0) ? 1 : 0;
  clockTempColor = clockPrefs.getUInt("pcolor", 0xFFDC82);
  clockTempAuto = clockPrefs.getUChar("pauto", 1) ? 1 : 0;

  brightSched = clockPrefs.getUChar("sched", 0) ? 1 : 0;
  dayStartMin = clockPrefs.getUShort("dstart", 8 * 60);
  nightStartMin = clockPrefs.getUShort("nstart", 22 * 60);
  dayBright = clockPrefs.getUChar("dbright", 180);
  nightBright = clockPrefs.getUChar("nbright", 25);
  if (dayStartMin >= 1440) dayStartMin = 8 * 60;
  if (nightStartMin >= 1440) nightStartMin = 22 * 60;
  if (dayBright < 1) dayBright = 1;
  if (nightBright < 1) nightBright = 1;

  String ssid = clockPrefs.getString("wssid", CLOCK_WIFI_SSID);
  if (ssid.length() > 0 && ssid.length() < sizeof(wifiSsid)) {
    strncpy(wifiSsid, ssid.c_str(), sizeof(wifiSsid) - 1);
    wifiSsid[sizeof(wifiSsid) - 1] = '\0';
  }
  String pass = clockPrefs.getString("wpass", CLOCK_WIFI_PASS);
  if (pass.length() < sizeof(wifiPass)) {
    strncpy(wifiPass, pass.c_str(), sizeof(wifiPass) - 1);
    wifiPass[sizeof(wifiPass) - 1] = '\0';
  }

  String mh = clockPrefs.getString("mhost", "");
  if (mh.length() < sizeof(mqttHost)) {
    strncpy(mqttHost, mh.c_str(), sizeof(mqttHost) - 1);
    mqttHost[sizeof(mqttHost) - 1] = '\0';
  }
  mqttPort = clockPrefs.getUShort("mport", 1883);
  if (mqttPort == 0) mqttPort = 1883;
  String mu = clockPrefs.getString("muser", "");
  if (mu.length() < sizeof(mqttUser)) {
    strncpy(mqttUser, mu.c_str(), sizeof(mqttUser) - 1);
    mqttUser[sizeof(mqttUser) - 1] = '\0';
  }
  String mp = clockPrefs.getString("mpass", "");
  if (mp.length() < sizeof(mqttPass)) {
    strncpy(mqttPass, mp.c_str(), sizeof(mqttPass) - 1);
    mqttPass[sizeof(mqttPass) - 1] = '\0';
  }
  String mt = clockPrefs.getString("mtopic", MQTT_DEFAULT_TOPIC);
  if (mt.length() > 0 && mt.length() < sizeof(mqttTopic)) {
    strncpy(mqttTopic, mt.c_str(), sizeof(mqttTopic) - 1);
    mqttTopic[sizeof(mqttTopic) - 1] = '\0';
  }
  mqttEnabled = clockPrefs.getUChar("men", 0) ? 1 : 0;

  String bt = clockPrefs.getString("bletopic", BLE_DEFAULT_TOPIC);
  if (bt.length() > 0 && bt.length() < sizeof(bleTopic)) {
    strncpy(bleTopic, bt.c_str(), sizeof(bleTopic) - 1);
    bleTopic[sizeof(bleTopic) - 1] = '\0';
  }
  bleEnabled = clockPrefs.getUChar("bleen", 0) ? 1 : 0;
  bleDisc = clockPrefs.getUChar("bledisc", 0) ? 1 : 0;

  knobPct = clockPrefs.getInt("knob", 50);
  if (knobPct < 0) knobPct = 0;
  if (knobPct > 100) knobPct = 100;
  knobPctF = (float)knobPct;

  clockPrefs.end();

  setenv("TZ", clockTz, 1);
  tzset();

  Serial.printf("CLOCK: config tz=[%s] city=[%s] %.3f,%.3f ipgeo=%u\n",
                clockTz, clockCity, clockLat, clockLon, (unsigned)clockUseIp);
  Serial.printf("WIFI: saved ssid=[%s]  MQTT: %s %s:%u topic=[%s]\n",
                wifiSsid, mqttEnabled ? "on" : "off", mqttHost,
                (unsigned)mqttPort, mqttTopic);
}

void saveClockConfig() {
  if (!clockPrefs.begin("clock", false)) return;
  clockPrefs.putString("tz", clockTz);
  clockPrefs.putString("city", clockCity);
  clockPrefs.putFloat("lat", clockLat);
  clockPrefs.putFloat("lon", clockLon);
  clockPrefs.putUChar("ipgeo", clockUseIp);
  clockPrefs.putUInt("tcolor", clockTimeColor);
  clockPrefs.putUChar("tcset", clockTimeColorSet);
  clockPrefs.putUInt("pcolor", clockTempColor);
  clockPrefs.putUChar("pauto", clockTempAuto);
  clockPrefs.putUChar("sched", brightSched);
  clockPrefs.putUShort("dstart", dayStartMin);
  clockPrefs.putUShort("nstart", nightStartMin);
  clockPrefs.putUChar("dbright", dayBright);
  clockPrefs.putUChar("nbright", nightBright);
  clockPrefs.putString("wssid", wifiSsid);
  clockPrefs.putString("wpass", wifiPass);
  clockPrefs.putString("mhost", mqttHost);
  clockPrefs.putUShort("mport", mqttPort);
  clockPrefs.putString("muser", mqttUser);
  clockPrefs.putString("mpass", mqttPass);
  clockPrefs.putString("mtopic", mqttTopic);
  clockPrefs.putUChar("men", mqttEnabled);
  clockPrefs.putString("bletopic", bleTopic);
  clockPrefs.putUChar("bleen", bleEnabled);
  clockPrefs.putUChar("bledisc", bleDisc);
  clockPrefs.end();
}

// ===================== Wi-Fi + time =====================
void webBegin();

// Start (or restart) the station connection. In setup-hotspot mode the AP
// stays up, so the settings page remains reachable until the new network works.
void wifiBegin() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  clockNetTimer = millis();

  if (clockNetState == CLOCK_NET_AP) {
    WiFi.begin(wifiSsid, wifiPass);
    Serial.printf("WIFI: retrying [%s] while the setup hotspot is on\n", wifiSsid);
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);
  clockNetState = CLOCK_NET_CONNECTING;
  Serial.printf("WIFI: connecting to [%s]\n", wifiSsid);
}

// The saved network cannot be reached: bring up an open settings hotspot.
void wifiStartAp() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apSsid, sizeof(apSsid), "%s-%02X%02X", AP_NAME_PREFIX, mac[4], mac[5]);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid, AP_PASSWORD);
  WiFi.begin(wifiSsid, wifiPass); // keep trying the real network in parallel

  clockNetState = CLOCK_NET_AP;
  clockNetTimer = millis();

  Serial.printf("WIFI: [%s] unreachable, setup hotspot '%s' (pass '%s')\n",
                wifiSsid, apSsid, AP_PASSWORD);
  Serial.print("WIFI: open http://");
  Serial.println(WiFi.softAPIP());
  webBegin();

  // If the home network is not reachable at boot, scroll the hotspot address.
  if (!bootMsgShown) {
    bootMsgShown = true;
    snprintf(bootMsgLine, sizeof(bootMsgLine), "HOTSPOT %s 192.168.4.1", apSsid);
    bootMsgPending = true;
  }
}

// New credentials were stored on the web page: reconnect right away.
void wifiApply() {
  Serial.printf("WIFI: applying new network [%s]\n", wifiSsid);
  wifiFailCount = 0;
  mqttDirty = true;

  if (clockNetState == CLOCK_NET_AP) {
    WiFi.begin(wifiSsid, wifiPass);
    clockNetTimer = millis();
    return;
  }

  if (webRunning) {
    webServer.stop();
    webRunning = false;
  }
  WiFi.disconnect(true);
  clockNetState = CLOCK_NET_IDLE;
  wifiBegin();
  webBegin(); // the page stays reachable while the link is rebuilt
}

void wifiOnline(unsigned long now) {
  wifiFailCount = 0;
  if (clockNetState == CLOCK_NET_AP) {
    WiFi.softAPdisconnect(true);
    Serial.println("WIFI: setup hotspot closed");
  }
  clockNetState = CLOCK_NET_ONLINE;
  Serial.print("WIFI: ok, IP ");
  Serial.println(WiFi.localIP());
  Serial.print("WIFI: settings http://");
  Serial.println(WiFi.localIP());

  // One-time boot greeting: scroll the address so it can be read on the LEDs.
  if (!bootMsgShown) {
    bootMsgShown = true;
    snprintf(bootMsgLine, sizeof(bootMsgLine), "IP %s",
             WiFi.localIP().toString().c_str());
    bootMsgPending = true;
  }

  configTzTime(clockTz, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  clockLastNtp = now;
  clockTimeLogged = false;
}

void netLoop() {
  unsigned long now = millis();

  // The settings page is served whenever the network stack is up: on the home
  // network, during a reconnect and on the setup hotspot.
  if (webRunning) webServer.handleClient();

  switch (clockNetState) {
    case CLOCK_NET_IDLE:
      break;

    case CLOCK_NET_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        wifiOnline(now);
      } else if (now - clockNetTimer > 15000UL) {
        wifiFailCount++;
        if (wifiFailCount >= 2) {
          wifiStartAp();
        } else {
          Serial.println("WIFI: timeout, retrying");
          clockNetState = CLOCK_NET_RETRY;
          clockNetTimer = now;
        }
      }
      break;

    case CLOCK_NET_RETRY:
      if (WiFi.status() == WL_CONNECTED) {
        wifiOnline(now);
      } else if (now - clockNetTimer > 5000UL) {
        if (wifiFailCount >= 2) {
          wifiStartAp();
        } else {
          wifiFailCount++;
          wifiBegin();
        }
      }
      break;

    case CLOCK_NET_AP:
      if (WiFi.status() == WL_CONNECTED) {
        wifiOnline(now);
      } else if (now - clockNetTimer > 20000UL) {
        clockNetTimer = now;
        WiFi.begin(wifiSsid, wifiPass);
      }
      break;

    case CLOCK_NET_ONLINE:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WIFI: link lost");
        wifiFailCount = 1;
        clockNetState = CLOCK_NET_RETRY;
        clockNetTimer = now;
      } else {
        if (!clockTimeReady() && now - clockLastNtp > 20000UL) {
          configTzTime(clockTz, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
          clockLastNtp = now;
        }
        if (!clockTimeLogged && clockTimeReady()) {
          clockTimeLogged = true;
          time_t tt = time(nullptr);
          struct tm tmv;
          localtime_r(&tt, &tmv);
          Serial.printf("CLOCK: time %02d:%02d:%02d\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        }
        // Weather is only interesting while the clock effect is showing.
        if (effectIndex == 0) {
          unsigned long since = now - weatherLastTry;
          unsigned long period = weatherValid ? WEATHER_REFRESH_MS : 60000UL;
          if (!weatherRequest && since >= period) weatherRequest = true;
        }
      }
      break;
  }
}

// ===================== weather download =====================
bool jsonFloat(const String &s, const char *key, float &out) {
  int k = s.indexOf(key);
  if (k < 0) return false;
  int c = s.indexOf(':', k + (int)strlen(key));
  if (c < 0) return false;
  out = s.substring(c + 1).toFloat();
  return true;
}

bool jsonInt(const String &s, const char *key, int &out) {
  int k = s.indexOf(key);
  if (k < 0) return false;
  int c = s.indexOf(':', k + (int)strlen(key));
  if (c < 0) return false;
  out = s.substring(c + 1).toInt();
  return true;
}

bool httpGet(const String &url, String &out, bool secure) {
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(8000);
  bool ok = false;

  if (secure) {
    WiFiClientSecure client;
    client.setInsecure();
    if (http.begin(client, url)) {
      int code = http.GET();
      if (code == 200) {
        out = http.getString();
        ok = true;
      } else {
        Serial.printf("CLOCK: HTTP %d\n", code);
      }
      http.end();
    }
  } else {
    WiFiClient client;
    if (http.begin(client, url)) {
      int code = http.GET();
      if (code == 200) {
        out = http.getString();
        ok = true;
      } else {
        Serial.printf("CLOCK: HTTP %d\n", code);
      }
      http.end();
    }
  }
  return ok;
}

String urlEncode(const String &s) {
  String out;
  char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

// Open-Meteo geocoding: turn a city name into coordinates.
bool geocodeCity(const String &city, float &lat, float &lon, String &label) {
  String url = "https://geocoding-api.open-meteo.com/v1/search?name=";
  url += urlEncode(city);
  url += "&count=1&language=en&format=json";

  String body;
  if (!httpGet(url, body, true)) return false;
  if (body.indexOf("\"results\"") < 0) return false;

  float la, lo;
  if (!jsonFloat(body, "\"latitude\"", la) || !jsonFloat(body, "\"longitude\"", lo)) {
    return false;
  }

  String name = "";
  int nk = body.indexOf("\"name\":\"");
  if (nk >= 0) {
    int e = body.indexOf('"', nk + 8);
    if (e > nk) name = body.substring(nk + 8, e);
  }
  if (!name.length()) name = city;

  lat = la;
  lon = lo;
  label = name;
  return true;
}

bool fetchWeather() {
  weatherLastTry = millis();
  if (WiFi.status() != WL_CONNECTED) return false;

  float lat = clockLat;
  float lon = clockLon;

  if (clockUseIp) {
    if (!weatherGeoDone) {
      String geo;
      if (httpGet("http://ip-api.com/json/?fields=status,lat,lon", geo, false) &&
          geo.indexOf("\"status\":\"success\"") >= 0) {
        float la, lo;
        if (jsonFloat(geo, "\"lat\"", la) && jsonFloat(geo, "\"lon\"", lo)) {
          weatherIpLat = la;
          weatherIpLon = lo;
          lat = la;
          lon = lo;
          weatherGeoDone = true;
          Serial.printf("CLOCK: IP location %.2f, %.2f\n", la, lo);
        }
      }
    } else {
      // remember where the IP lookup pointed, so refreshes stay consistent
      lat = weatherIpLat;
      lon = weatherIpLon;
    }
  } else {
    weatherGeoDone = false;
  }

  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(lat, 3);
  url += "&longitude=";
  url += String(lon, 3);
  url += "&current=temperature_2m,weather_code,is_day&timezone=auto";

  String body;
  if (!httpGet(url, body, true)) {
    Serial.println("CLOCK: weather fetch failed");
    return false;
  }

  int ci = body.indexOf("\"current\":{");
  if (ci < 0) {
    Serial.println("CLOCK: weather parse failed");
    return false;
  }
  String cur = body.substring(ci);

  float temp = 0.0f;
  int code = -1;
  int day = 1;
  if (!jsonFloat(cur, "\"temperature_2m\"", temp) ||
      !jsonInt(cur, "\"weather_code\"", code)) {
    Serial.println("CLOCK: weather fields missing");
    return false;
  }
  jsonInt(cur, "\"is_day\"", day);

  weatherTemp = (int16_t)lroundf(temp);
  weatherCode = (uint8_t)code;
  weatherIsDay = day ? 1 : 0;
  weatherValid = true;

  Serial.printf("CLOCK: weather code=%d temp=%d day=%d\n",
                code, (int)weatherTemp, day);
  return true;
}

// Runs on core 0 so the blocking HTTPS request never freezes the animation.
void weatherTask(void *) {
  for (;;) {
    if (weatherRequest && WiFi.status() == WL_CONNECTED) {
      weatherRequest = false;
      fetchWeather();
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ===================== Bluetooth -> MQTT bridge =====================
// NimBLE runs a passive scan in the background. Every advertisement is copied
// into a small single-producer/single-consumer queue; the MQTT task publishes
// it, so neither the radio nor a slow broker can stall the LED animation.
// This is a BLE relay, not the ESPHome Bluetooth Proxy: the data reaches Home
// Assistant through MQTT, not through its Bluetooth integration.
struct BleAdv {
  char addr[18];
  char name[20];
  int8_t rssi;
  uint8_t mlen;
  uint8_t mdata[BLE_MAX_MDATA];
};

BleAdv bleQueue[BLE_QUEUE_LEN];
volatile uint8_t bleQHead = 0;
volatile uint8_t bleQTail = 0;
bool bleRunning = false;

class BleScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    uint8_t next = (uint8_t)((bleQHead + 1) % BLE_QUEUE_LEN);
    if (next == bleQTail) return;  // queue full: drop this advertisement

    BleAdv &a = bleQueue[bleQHead];
    snprintf(a.addr, sizeof(a.addr), "%s", dev->getAddress().toString().c_str());
    const char *nm = dev->haveName() ? dev->getName().c_str() : "";
    strncpy(a.name, nm, sizeof(a.name) - 1);
    a.name[sizeof(a.name) - 1] = '\0';
    a.rssi = dev->getRSSI();

    std::string md = dev->getManufacturerData();
    a.mlen = md.size() > BLE_MAX_MDATA ? BLE_MAX_MDATA : (uint8_t)md.size();
    if (a.mlen) memcpy(a.mdata, md.data(), a.mlen);

    bleQHead = next;
  }
};

static BleScanCallbacks bleCallbacks;

void bleStart() {
  if (bleRunning) return;
  if (!NimBLEDevice::init("LedBadge")) {
    Serial.println("BLE: init failed");
    return;
  }

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&bleCallbacks, true);  // true = report duplicates
  scan->setActiveScan(true);                    // ask for scan responses: that
                                                // is where most fobs put their name
  scan->setInterval(320);                       // 320 ms period, 80 ms window
  scan->setWindow(80);                          // = 25 % duty, leaves Wi-Fi room
  scan->setMaxResults(0);
  scan->start(0, false, false);                 // run until stopped

  bleRunning = true;
  Serial.println("BLE: scan started, advertisements go to MQTT");
}

void bleStop() {
  if (!bleRunning) return;
  NimBLEDevice::getScan()->stop();
  NimBLEDevice::deinit(true);
  bleRunning = false;
  bleQHead = bleQTail = 0;
  Serial.println("BLE: scan stopped");
}

// Publish one advertisement, throttled per device so a busy room cannot flood
// the broker. Called from the MQTT task only.
void blePublish(const BleAdv &a) {
  static char seen[16][18];
  static unsigned long seenAt[16];
  static bool seenDisc[16];
  static uint8_t seenCount = 0;
  static uint8_t seenNext = 0;
  unsigned long now = millis();

  int slot = -1;
  for (int i = 0; i < seenCount; i++) {
    if (strcmp(seen[i], a.addr) == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    slot = (seenCount < 16) ? seenCount++ : seenNext;
    seenNext = (uint8_t)((slot + 1) % 16);
    strncpy(seen[slot], a.addr, sizeof(seen[slot]) - 1);
    seen[slot][sizeof(seen[slot]) - 1] = '\0';
    seenAt[slot] = 0;
    seenDisc[slot] = false;
    // One line per newly heard device: short, rare, and very useful when a tag
    // does not show up in Home Assistant.
    Serial.printf("BLE: %s rssi=%d name=[%s]\n", a.addr, (int)a.rssi, a.name);
  }

  char plain[18];
  int j = 0;
  for (int i = 0; a.addr[i] && j < (int)sizeof(plain) - 1; i++) {
    if (a.addr[i] != ':') plain[j++] = a.addr[i];
  }
  plain[j] = '\0';

  // First sighting of a named device: let Home Assistant create its sensor.
  if (bleDisc && !seenDisc[slot] && a.name[0] != '\0') {
    seenDisc[slot] = bleDiscPublish(a, plain);
  }

  if (seenAt[slot] != 0 && now - seenAt[slot] < BLE_PUBLISH_MS) return;
  seenAt[slot] = now ? now : 1;

  char payload[200];
  int n = snprintf(payload, sizeof(payload),
                   "{\"addr\":\"%s\",\"rssi\":%d,\"name\":\"%s\"",
                   a.addr, (int)a.rssi, a.name);
  if (n < 0) return;

  if (a.mlen && n < (int)sizeof(payload) - 4) {
    n += snprintf(payload + n, sizeof(payload) - n, ",\"mdata\":\"");
    for (int i = 0; i < a.mlen && n < (int)sizeof(payload) - 3; i++) {
      n += snprintf(payload + n, sizeof(payload) - n, "%02x", a.mdata[i]);
    }
    n += snprintf(payload + n, sizeof(payload) - n, "\"");
  }
  snprintf(payload + n, sizeof(payload) - n, "}");

  char topic[80];
  snprintf(topic, sizeof(topic), "%s/%s", bleTopic, plain);
  mqttClient.publish(topic, payload);
}

// ===================== MQTT =====================
// Text published to mqttTopic is handed to the renderer through msgStaging,
// so a slow broker never freezes the animation. The client runs on core 0.

// ---- 8x8 bitmap payload ------------------------------------------------
// Accepted bodies (whitespace ignored):
//   64 chars of 0/1        one colour, default white
//   16 hex digits          same, 8 row bytes (MSB = left pixel)
//   192 hex digits         full colour, RGB444: three hex digits per pixel
// Optional suffix: :RRGGBB (or :#RRGGBB) and :seconds (0 = stay until cleared,
// default 20 s). An empty payload, "off" or "clear" removes the picture.
static int bmpNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static char *bmpTrim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                   e[-1] == '\n')) {
    *--e = '\0';
  }
  return s;
}

// Expand a mask into 64 RGB pixels, row-major, top-left first.
static bool bmpExpand(const char *s, uint32_t rgb, uint8_t out[192]) {
  char clean[194];
  int n = 0;
  for (const char *p = s; *p; p++) {
    if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
    if (n >= 192) return false;
    clean[n++] = *p;
  }
  clean[n] = '\0';

  if (n == 192) {
    for (int i = 0; i < 64; i++) {
      int rh = bmpNibble(clean[i * 3]);
      int gh = bmpNibble(clean[i * 3 + 1]);
      int bh = bmpNibble(clean[i * 3 + 2]);
      if (rh < 0 || gh < 0 || bh < 0) return false;
      out[i * 3] = (uint8_t)(rh * 17);
      out[i * 3 + 1] = (uint8_t)(gh * 17);
      out[i * 3 + 2] = (uint8_t)(bh * 17);
    }
    return true;
  }

  uint8_t rows[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
  if (n == 16) {
    for (int i = 0; i < 8; i++) {
      int hi = bmpNibble(clean[i * 2]);
      int lo = bmpNibble(clean[i * 2 + 1]);
      if (hi < 0 || lo < 0) return false;
      rows[i] = (uint8_t)((hi << 4) | lo);
    }
  } else if (n == 64) {
    for (int i = 0; i < 64; i++) {
      char c = clean[i];
      if (c != '0' && c != '1') return false;
      if (c == '1') rows[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    }
  } else {
    return false;
  }

  uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      bool on = (rows[y] & (0x80 >> x)) != 0;
      int i = (y * 8 + x) * 3;
      out[i] = on ? r : 0;
      out[i + 1] = on ? g : 0;
      out[i + 2] = on ? b : 0;
    }
  }
  return true;
}

static void mqttHandleBitmap(byte *payload, unsigned int len) {
  char buf[224];
  unsigned int n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = '\0';
  char *body = bmpTrim(buf);

  if (body[0] == '\0' || strcasecmp(body, "off") == 0 ||
      strcasecmp(body, "clear") == 0 || strcasecmp(body, "none") == 0) {
    bmpClearStaging = true;
    bmpArrived = true;
    Serial.println("BMP: cleared by request");
    return;
  }

  char *color = nullptr;
  char *secs = nullptr;
  char *colon = strchr(body, ':');
  if (colon) {
    *colon = '\0';
    color = bmpTrim(colon + 1);
    char *colon2 = strchr(color, ':');
    if (colon2) {
      *colon2 = '\0';
      secs = bmpTrim(colon2 + 1);
    }
  }

  uint32_t rgb = 0xFFFFFF;
  if (color && color[0]) {
    const char *c = (color[0] == '#') ? color + 1 : color;
    if (strlen(c) == 6) {
      int h[6];
      bool ok = true;
      for (int i = 0; i < 6; i++) {
        h[i] = bmpNibble(c[i]);
        if (h[i] < 0) ok = false;
      }
      if (ok) {
        rgb = ((uint32_t)(h[0] * 16 + h[1]) << 16) |
              ((uint32_t)(h[2] * 16 + h[3]) << 8) |
              (uint32_t)(h[4] * 16 + h[5]);
      }
    }
  }

  uint32_t ms = BMP_DEFAULT_MS;
  if (secs && secs[0]) {
    unsigned long sec = strtoul(secs, nullptr, 10);
    if (sec > 3600UL) sec = 3600UL;
    ms = (uint32_t)sec * 1000UL;  // 0 keeps the picture until cleared
  }

  if (!bmpExpand(bmpTrim(body), rgb, bmpStaging)) {
    Serial.println("BMP: bad payload (need 64 bits, 16 hex or 192 hex digits)");
    return;
  }

  bmpMsStaging = ms;
  bmpClearStaging = false;
  bmpArrived = true;
  Serial.printf("BMP: received, %lu s\n", (unsigned long)(ms / 1000UL));
}

// ---- Home Assistant MQTT discovery --------------------------------------
// All entities share one device entry, so Home Assistant shows a single
// "LED Badge" device. Payloads are retained: the entities survive reboots and
// broker restarts, nothing has to be added by hand.
// JSON string escape for text that comes from the radio (BLE names).
static String jsonEscape(const char *s) {
  String o;
  for (const char *p = s; *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if ((unsigned char)c < 0x20) {
      o += ' ';
    } else {
      o += c;
    }
  }
  return o;
}

static String discCommon(const char *name, const char *uniq) {
  String s = "{\"name\":\"";
  s += name;
  s += "\",\"uniq_id\":\"";
  s += mqttNodeId;
  s += "_";
  s += uniq;
  s += "\",\"dev\":{\"ids\":[\"";
  s += mqttNodeId;
  s += "\"],\"name\":\"LED Badge\",\"mf\":\"Waveshare\","
       "\"mdl\":\"ESP32-S3-Matrix\",\"sw\":\"1.1\"}";
  s += ",\"avty_t\":\"" MQTT_STATUS_TOPIC "\",\"pl_avail\":\"online\","
       "\"pl_not_avail\":\"offline\"";
  return s;
}

static bool discPublish(const char *component, const char *object,
                        const String &payload) {
  char topic[96];
  snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config",
           MQTT_DISCOVERY_PREFIX, component, mqttNodeId, object);
  bool ok = mqttClient.publish(topic, payload.c_str(), true);
  Serial.printf("MQTT: discovery %s (%u bytes) %s\n", topic,
                (unsigned)payload.length(), ok ? "ok" : "FAILED");
  return ok;
}

// One sensor per named BLE device, created on its first sighting. The budget
// keeps rotating phone addresses from filling the Home Assistant registry.
bool bleDiscPublish(const BleAdv &a, const char *plain) {
  static uint8_t created = 0;
  if (a.name[0] == '\0' || created >= 24) return false;

  String nm = jsonEscape(a.name);
  String p = "{\"name\":\"BLE ";
  p += nm;
  p += "\",\"uniq_id\":\"";
  p += mqttNodeId;
  p += "_ble_";
  p += plain;
  p += "\",\"stat_t\":\"";
  p += bleTopic;
  p += "/";
  p += plain;
  p += "\",\"val_tpl\":\"{{ value_json.rssi }}\",\"unit_of_meas\":\"dBm\","
       "\"dev_cla\":\"signal_strength\",\"exp_aft\":60,"
       "\"ic\":\"mdi:bluetooth\",\"dev\":{\"ids\":[\"";
  p += mqttNodeId;
  p += "_ble_";
  p += plain;
  p += "\"],\"name\":\"";
  p += nm;
  p += "\",\"via_device\":\"";
  p += mqttNodeId;
  p += "\",\"mf\":\"BLE\",\"mdl\":\"advertisement\"}}";

  char object[24];
  snprintf(object, sizeof(object), "ble_%s", plain);
  bool ok = discPublish("sensor", object, p);
  if (ok) created++;
  return ok;
}

void mqttPublishDiscovery() {
  String p;

  p = discCommon("Wi-Fi signal", "wifi");
  p += ",\"stat_t\":\"" MQTT_STATE_TOPIC "\","
       "\"val_tpl\":\"{{ value_json.wifi }}\",\"unit_of_meas\":\"dBm\","
       "\"dev_cla\":\"signal_strength\",\"ent_cat\":\"diagnostic\","
       "\"ic\":\"mdi:wifi\"}";
  discPublish("sensor", "wifi", p);

  p = discCommon("IP address", "ip");
  p += ",\"stat_t\":\"" MQTT_STATE_TOPIC "\","
       "\"val_tpl\":\"{{ value_json.ip }}\",\"ic\":\"mdi:ip-network\","
       "\"ent_cat\":\"diagnostic\"}";
  discPublish("sensor", "ip", p);

  p = discCommon("Uptime", "uptime");
  p += ",\"stat_t\":\"" MQTT_STATE_TOPIC "\","
       "\"val_tpl\":\"{{ value_json.uptime }}\",\"dev_cla\":\"duration\","
       "\"unit_of_meas\":\"s\",\"ent_cat\":\"diagnostic\","
       "\"ic\":\"mdi:timer-outline\"}";
  discPublish("sensor", "uptime", p);

  p = discCommon("Free memory", "heap");
  p += ",\"stat_t\":\"" MQTT_STATE_TOPIC "\","
       "\"val_tpl\":\"{{ value_json.heap }}\",\"unit_of_meas\":\"B\","
       "\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:memory\"}";
  discPublish("sensor", "heap", p);

  // The dimmer's percentage, used by a Home Assistant automation that sets the
  // real lamp: rotate the badge -> {"pct":N} on ledbadge/knob -> light.turn_on.
  p = discCommon("Knob", "knob");
  p += ",\"stat_t\":\"" MQTT_KNOB_TOPIC "\","
       "\"val_tpl\":\"{{ value_json.pct }}\",\"unit_of_meas\":\"%\","
       "\"ic\":\"mdi:knob\",\"ent_cat\":\"diagnostic\"}";
  discPublish("sensor", "knob", p);

  // Writable entities: a brightness slider (1..255, step 1) and a text box
  // whose value is published straight to the scrolling-message topic.
  p = discCommon("Brightness", "bright");
  p += ",\"stat_t\":\"" MQTT_BRIGHT_STATE_TOPIC "\","
       "\"cmd_t\":\"" MQTT_BRIGHT_SET_TOPIC "\","
       "\"min\":1,\"max\":255,\"step\":1,\"mode\":\"slider\","
       "\"ic\":\"mdi:brightness-6\"}";
  discPublish("number", "bright", p);

  p = discCommon("Message", "message");
  p += ",\"cmd_t\":\"";
  p += mqttTopic;
  p += "\",\"max\":";
  p += String(MSG_MAX_LEN);
  p += ",\"ic\":\"mdi:message-text\"}";
  discPublish("text", "message", p);

  // 8x8 picture box: the payload format is documented in the README.
  p = discCommon("Bitmap 8x8", "bitmap");
  p += ",\"cmd_t\":\"" MQTT_BITMAP_TOPIC "\",\"max\":205,"
       "\"ic\":\"mdi:image-outline\"}";
  discPublish("text", "bitmap", p);
}

// One retained JSON snapshot feeds the diagnostic sensors; the brightness
// number gets its own plain-value topic so the slider snaps back correctly.
void mqttPublishState() {
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString()
                                              : String("0.0.0.0");
  char payload[192];
  snprintf(payload, sizeof(payload),
           "{\"wifi\":%d,\"ip\":\"%s\",\"uptime\":%lu,\"heap\":%u}",
           (int)WiFi.RSSI(), ip.c_str(), millis() / 1000UL,
           (unsigned)ESP.getFreeHeap());
  mqttClient.publish(MQTT_STATE_TOPIC, payload, true);

  char b[8];
  snprintf(b, sizeof(b), "%u", (unsigned)ledBrightness);
  mqttClient.publish(MQTT_BRIGHT_STATE_TOPIC, b, true);
}

void mqttCallback(char *topic, byte *payload, unsigned int len) {
  // Brightness slider from Home Assistant. Only record it here; the animation
  // task applies the value and stores it in NVS.
  if (strcmp(topic, MQTT_BRIGHT_SET_TOPIC) == 0) {
    int v = 0;
    bool any = false;
    for (unsigned int i = 0; i < len && i < 8; i++) {
      if (payload[i] >= '0' && payload[i] <= '9') {
        v = v * 10 + (payload[i] - '0');
        any = true;
      }
    }
    if (!any) return;
    if (v < 1) v = 1;
    if (v > 255) v = 255;
    brightCmd = v;
    return;
  }

  if (strcmp(topic, MQTT_BITMAP_TOPIC) == 0) {
    mqttHandleBitmap(payload, len);
    return;
  }

  if (strcmp(topic, mqttTopic) != 0) return;

  unsigned int n = len < MSG_MAX_LEN ? len : MSG_MAX_LEN;
  for (unsigned int i = 0; i < n; i++) {
    char ch = (char)payload[i];
    if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    msgStaging[i] = ch;
  }
  msgStaging[n] = '\0';
  while (n > 0 && msgStaging[n - 1] == ' ') msgStaging[--n] = '\0';
  if (n == 0) return;

  msgArrived = true;
}

void mqttTask(void *) {
  // Discovery payloads are larger than the library default of 256 bytes.
  mqttClient.setBufferSize(512);

  for (;;) {
    if (mqttDirty) {
      mqttDirty = false;
      if (mqttClient.connected()) mqttClient.disconnect();
    }

    // BLE scanning is controlled here, on the same core as the radio stack.
    if (bleDirty) {
      bleDirty = false;
      if (bleEnabled) bleStart();
      else bleStop();
    }

    if (!mqttEnabled || mqttHost[0] == '\0' ||
        WiFi.status() != WL_CONNECTED) {
      if (mqttClient.connected()) mqttClient.disconnect();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!mqttClient.connected()) {
      mqttClient.setServer(mqttHost, mqttPort);
      mqttClient.setCallback(mqttCallback);
      mqttClient.setKeepAlive(30);

      char cid[24];
      snprintf(cid, sizeof(cid), "ledbadge-%06X",
               (unsigned)(ESP.getEfuseMac() & 0xFFFFFF));

      bool ok = mqttClient.connect(
          cid,
          mqttUser[0] ? mqttUser : nullptr,
          mqttPass[0] ? mqttPass : nullptr,
          MQTT_STATUS_TOPIC, 1, true, "offline");

      if (ok) {
        mqttClient.subscribe(mqttTopic);
        mqttClient.subscribe(MQTT_BRIGHT_SET_TOPIC);
        mqttClient.subscribe(MQTT_BITMAP_TOPIC);
        mqttClient.publish(MQTT_STATUS_TOPIC, "online", true);
        // Announce the entities so Home Assistant adds the badge by itself.
        mqttPublishDiscovery();
        mqttPublishState();
        // Publish the retained knob position right away.
        knobPublished = -1;
        knobPub = knobPct;
        Serial.printf("MQTT: connected to %s:%u, topic [%s]\n",
                      mqttHost, (unsigned)mqttPort, mqttTopic);
        if (bleEnabled) bleStart();
      } else {
        Serial.printf("MQTT: connect failed, rc=%d\n", mqttClient.state());
        vTaskDelay(pdMS_TO_TICKS(5000));
      }
      continue;
    }

    // Forward queued BLE advertisements while the broker link is up.
    if (bleEnabled) {
      while (bleQTail != bleQHead) {
        blePublish(bleQueue[bleQTail]);
        bleQTail = (uint8_t)((bleQTail + 1) % BLE_QUEUE_LEN);
      }
    }

    // Refresh the state sensors periodically, and immediately after a
    // brightness change applied by the animation task.
    static unsigned long lastStatePub = 0;
    if (mqttPubState || millis() - lastStatePub >= 15000UL) {
      mqttPubState = false;
      lastStatePub = millis();
      mqttPublishState();
    }

    mqttClient.loop();

    // Publish the dimmer value recorded by the animation task (already
    // throttled there; this is the only place that touches the client).
    int pendingKnob = knobPub;
    if (pendingKnob >= 0) {
      char kb[24];
      snprintf(kb, sizeof(kb), "{\"pct\":%d}", pendingKnob);
      if (mqttClient.publish(MQTT_KNOB_TOPIC, kb, true)) knobPub = -1;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ===================== web configuration page =====================
String webMsg = "";

String hexColor(uint32_t c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%06X", (unsigned)(c & 0xFFFFFF));
  return String(buf);
}

String hhmm(uint16_t mins) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(mins / 60), (unsigned)(mins % 60));
  return String(buf);
}

int parseHHMM(const String &s) {
  int c = s.indexOf(':');
  if (c <= 0) return -1;
  int h = s.substring(0, c).toInt();
  int m = s.substring(c + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

uint32_t shownTimeColor() {
  if (clockTimeColorSet) return clockTimeColor;
  return pixels.ColorHSV((uint16_t)huePos, 255, 255) & 0xFFFFFF;
}

// " #RRGGBB " (the browser sends the hash percent-encoded, WebServer decodes it).
bool parseColor(const String &s, uint32_t &out) {
  String v = s;
  v.trim();
  if (v.startsWith("#")) v.remove(0, 1);
  if (v.length() != 6) return false;
  char *end = nullptr;
  unsigned long c = strtoul(v.c_str(), &end, 16);
  if (!end || *end != '\0') return false;
  out = (uint32_t)c & 0xFFFFFFUL;
  return true;
}

String htmlEscape(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

void handleRoot() {
  String page;
  page.reserve(9000);
  page += F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>LED badge clock</title><style>"
    "body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#0f1115;"
    "color:#e8ecf3;margin:0;padding:24px;line-height:1.45}"
    ".card{max-width:520px;margin:0 auto;background:#171b23;border:1px solid #262c38;"
    "border-radius:14px;padding:22px 24px}"
    "h1{font-size:20px;margin:0 0 4px}p.sub{color:#8d97a8;margin:0 0 18px;font-size:14px}"
    "label{display:block;margin:14px 0 6px;font-size:13px;color:#aab4c4}"
    "select,input[type=text],input[type=password],input[type=time]{width:100%;box-sizing:border-box;"
    "padding:10px 12px;border-radius:9px;border:1px solid #303848;background:#0f131a;"
    "color:#e8ecf3;font-size:15px}"
    "input[type=range]{width:100%;margin:0;padding:0;accent-color:#3b82f6}"
    "input[type=color]{width:56px;height:42px;padding:2px;border-radius:9px;"
    "border:1px solid #303848;background:#0f131a;flex:0 0 auto}"
    "button{margin-top:20px;width:100%;padding:12px;border:0;border-radius:9px;"
    "background:#3b82f6;color:#fff;font-size:15px;font-weight:600}"
    ".row{display:flex;gap:10px;align-items:center;margin-top:16px;font-size:14px;color:#aab4c4}"
    ".grid{display:flex;gap:12px;align-items:center}"
    ".grid .grow{flex:1;min-width:0}"
    "h2{font-size:12px;margin:24px 0 2px;color:#cfd7e3;text-transform:uppercase;"
    "letter-spacing:.08em;border-top:1px solid #262c38;padding-top:16px}"
    ".status{margin-top:18px;padding:10px 12px;border-radius:9px;background:#0f131a;"
    "border:1px solid #262c38;font-size:13px;color:#8d97a8}"
    ".status b{color:#cfd7e3;font-weight:600}"
    ".ok{color:#7ee2a8}.warn{color:#f0b45a}"
    ".banner{margin:0 0 16px;padding:12px 14px;border-radius:10px;background:#2a2113;"
    "border:1px solid #6b5220;color:#f0c86a;font-size:14px}"
    ".hint{color:#6f7a8b;font-size:12px;margin:6px 0 0}"
    "code{background:#0f131a;padding:1px 5px;border-radius:4px;font-size:12px}"
    "</style></head><body><div class='card'>"
    "<h1>LED badge</h1>"
    "<p class='sub'>Wi-Fi, time, weather and MQTT settings</p>");

  if (clockNetState == CLOCK_NET_AP) {
    page += F("<div class='banner'><b>Setup mode.</b> Connect this device to Wi-Fi <b>");
    page += apSsid;
    page += F("</b> (password <b>" AP_PASSWORD "</b>), then enter your home network below. "
              "This page is at http://");
    page += WiFi.softAPIP().toString();
    page += F("/</div>");
  }

  page += "<div class='status'><b>Time</b> ";
  if (clockTimeReady()) {
    time_t tt = time(nullptr);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    page += buf;
  } else {
    page += "not synced";
  }
  page += "<br><b>Weather</b> ";
  if (weatherValid) {
    page += String((int)weatherTemp) + " &deg;C, code " + String((int)weatherCode);
  } else {
    page += "waiting for the first download";
  }
  page += "<br><b>Location</b> ";
  if (clockUseIp) page += "from this network's public IP";
  else page += htmlEscape(clockCity) + " (" + String(clockLat, 3) + ", " + String(clockLon, 3) + ")";
  page += "<br><b>Brightness</b> ";
  page += String((int)pixels.getBrightness());
  page += brightSched ? " of 255 (day/night schedule)" : " of 255 (manual)";
  page += "<br><b>Effect</b> ";
  page += EFFECT_NAMES[effectIndex];
  page += " &middot; lamp knob ";
  page += String(knobPct);
  page += " %";
  page += "<br><b>Wi-Fi</b> ";
  if (clockNetState == CLOCK_NET_AP) {
    page += "setup hotspot " + String(apSsid) + " &middot; http://" +
            WiFi.softAPIP().toString();
  } else if (WiFi.status() == WL_CONNECTED) {
    page += htmlEscape(wifiSsid) + " &middot; " + WiFi.localIP().toString();
  } else {
    page += "connecting to " + htmlEscape(wifiSsid) + "&hellip;";
  }
  page += "<br><b>MQTT</b> ";
  if (!mqttEnabled) {
    page += "off";
  } else if (mqttClient.connected()) {
    page += "connected to " + htmlEscape(mqttHost);
  } else {
    page += htmlEscape(String(mqttHost[0] ? mqttHost : "host not set"));
  }
  page += "</div>";

  if (webMsg.length()) {
    page += "<div class='status ok'>" + htmlEscape(webMsg) + "</div>";
    webMsg = "";
  }

  page += F("<form action='/save' method='get'>");
  page += F("<h2>Wi-Fi</h2>");
  page += F("<label>Home network name (SSID)</label><input type='text' name='ssid' value='");
  page += htmlEscape(wifiSsid);
  page += F("'>");
  page += F("<label>Password</label><input type='password' name='wpass' value='' "
            "placeholder='leave blank to keep the current password' autocomplete='new-password'>");
  page += F("<p class='hint'>If this network cannot be reached the badge starts the hotspot "
            "<b>LedBadge-XXXX</b> (password <b>" AP_PASSWORD "</b>). Connect to it and open "
            "http://192.168.4.1/ to fix the settings.</p>");

  page += F("<h2>Time</h2>");
  page += F("<label>Time zone</label><select name='tz'>");
  bool matched = false;
  for (uint8_t i = 0; i < TZ_OPTION_COUNT; i++) {
    bool sel = strcmp(TZ_OPTIONS[i].posix, clockTz) == 0;
    if (sel) matched = true;
    page += "<option value='";
    page += TZ_OPTIONS[i].posix;
    page += "'";
    if (sel) page += " selected";
    page += ">";
    page += TZ_OPTIONS[i].label;
    page += "</option>";
  }
  if (!matched) {
    page += "<option value='";
    page += clockTz;
    page += "' selected>Custom (";
    page += clockTz;
    page += ")</option>";
  }
  page += F("</select>");

  page += F("<h2>Location</h2>");
  page += F("<label>Weather city</label><input type='text' name='city' value='");
  page += htmlEscape(clockCity);
  page += F("' placeholder='e.g. Madrid'>");

  page += F("<div class='row'><input type='checkbox' id='ipgeo' name='ipgeo' value='1'");
  if (clockUseIp) page += F(" checked");
  page += F("><label for='ipgeo' style='margin:0'>Locate automatically by IP address</label></div>");

  page += F("<h2>Colours</h2>");
  page += F("<label>Time digits</label><div class='grid'>");
  page += F("<input type='color' name='tcolor' value='");
  page += hexColor(shownTimeColor());
  page += F("'><div class='grow'><div class='row' style='margin:0'>"
            "<input type='checkbox' id='tfollow' name='tfollow' value='1'");
  if (!clockTimeColorSet) page += F(" checked");
  page += F("><label for='tfollow' style='margin:0'>Follow the base colour "
            "(changed by a single click on BOOT)</label></div></div></div>");

  page += F("<label>Temperature digits</label><div class='grid'>");
  page += F("<input type='color' name='pcolor' value='");
  page += hexColor(clockTempColor);
  page += F("'><div class='grow'><div class='row' style='margin:0'>"
            "<input type='checkbox' id='pauto' name='pauto' value='1'");
  if (clockTempAuto) page += F(" checked");
  page += F("><label for='pauto' style='margin:0'>Colour by temperature "
            "(blue = cold, orange = hot)</label></div></div></div>");

  page += F("<h2>Brightness</h2>");
  page += F("<label>Manual brightness: <span id='bval'>");
  page += String((int)ledBrightness);
  page += F("</span></label>");
  page += F("<input type='range' name='bright' min='1' max='255' step='1' value='");
  page += String((int)ledBrightness);
  page += F("' oninput=\"document.getElementById('bval').textContent=this.value\">");

  page += F("<div class='row'><input type='checkbox' id='sched' name='sched' value='1'");
  if (brightSched) page += F(" checked");
  page += F("><label for='sched' style='margin:0'>Use a day/night schedule</label></div>");
  page += F("<p class='hint'>The schedule needs a synced clock; until then the "
            "manual brightness is used.</p>");

  page += F("<label>Day starts</label><input type='time' name='dstart' value='");
  page += hhmm(dayStartMin);
  page += F("'>");
  page += F("<label>Night starts</label><input type='time' name='nstart' value='");
  page += hhmm(nightStartMin);
  page += F("'>");

  page += F("<label>Day brightness: <span id='dval'>");
  page += String((int)dayBright);
  page += F("</span></label>");
  page += F("<input type='range' name='dbright' min='1' max='255' step='1' value='");
  page += String((int)dayBright);
  page += F("' oninput=\"document.getElementById('dval').textContent=this.value\">");

  page += F("<label>Night brightness: <span id='nval'>");
  page += String((int)nightBright);
  page += F("</span></label>");
  page += F("<input type='range' name='nbright' min='1' max='255' step='1' value='");
  page += String((int)nightBright);
  page += F("' oninput=\"document.getElementById('nval').textContent=this.value\">");

  page += F("<h2>Lamp knob</h2>");
  page += F("<p class='hint'>The second effect (<b>clock &rarr; dimmer</b>, double-press BOOT) "
            "turns the badge into a lamp knob. Hold it upright and rotate it clockwise; the "
            "accelerometer measures the absolute angle, so it never drifts. Laid flat on the "
            "table the gyro takes over. The percentage is published to <code>");
  page += MQTT_KNOB_TOPIC;
  page += F("</code> as <code>{\"pct\":N}</code> (retained, at most 5 times per second).</p>");
  page += F("<p class='hint'>In Home Assistant create an automation: trigger on an MQTT "
            "message on <code>");
  page += MQTT_KNOB_TOPIC;
  page += F("</code>, then call <code>light.turn_on</code> on your lamp with "
            "<code>brightness_pct: {{ trigger.payload_json.pct }}</code>. The badge also "
            "announces a <b>Knob</b> sensor through discovery.</p>");

  page += F("<h2>MQTT</h2>");
  page += F("<div class='row'><input type='checkbox' id='men' name='men' value='1'");
  if (mqttEnabled) page += F(" checked");
  page += F("><label for='men' style='margin:0'>Enable MQTT (Home Assistant)</label></div>");

  page += F("<label>Broker host</label><input type='text' name='mhost' value='");
  page += htmlEscape(mqttHost);
  page += F("' placeholder='192.168.1.50'>");
  page += F("<label>Port</label><input type='text' name='mport' value='");
  page += String((unsigned)mqttPort);
  page += F("'>");
  page += F("<label>Username (optional)</label><input type='text' name='muser' value='");
  page += htmlEscape(mqttUser);
  page += F("'>");
  page += F("<label>Password</label><input type='password' name='mpass' value='' "
            "placeholder='leave blank to keep the current password' autocomplete='new-password'>");
  page += F("<label>Message topic</label><input type='text' name='mtopic' value='");
  page += htmlEscape(mqttTopic);
  page += F("'>");
  page += F("<p class='hint'>Text published to this topic scrolls across the matrix in any "
            "effect, e.g. <code>mosquitto_pub -h BROKER -t ");
  page += htmlEscape(mqttTopic);
  page += F(" -m 'Dinner is ready'</code>. The LWT status is published to <b>"
            MQTT_STATUS_TOPIC "</b>.</p>");
  page += F("<p class='hint'>Home Assistant discovery is automatic: on every connect the "
            "badge announces a <b>LED Badge</b> device with Wi-Fi signal, IP, uptime, memory, "
            "a 1&ndash;255 brightness slider and a message text box. Nothing has to be added "
            "by hand &mdash; open <b>Settings &rarr; Devices &amp; Services &rarr; MQTT</b>.</p>");

  page += F("<h2>Bluetooth &rarr; MQTT</h2>");
  page += F("<div class='row'><input type='checkbox' id='bleen' name='bleen' value='1'");
  if (bleEnabled) page += F(" checked");
  page += F("><label for='bleen' style='margin:0'>Forward BLE advertisements (presence)</label></div>");
  page += F("<div class='row'><input type='checkbox' id='bledisc' name='bledisc' value='1'");
  if (bleDisc) page += F(" checked");
  page += F("><label for='bledisc' style='margin:0'>Create Home Assistant sensors for BLE devices</label></div>");
  page += F("<label>Base topic</label><input type='text' name='bletopic' value='");
  page += htmlEscape(bleTopic);
  page += F("'>");
  page += F("<p class='hint'>Each device is published as JSON to <code>BASE/&lt;mac&gt;</code> "
            "every 5 s while it is nearby: <code>{\"addr\",\"rssi\",\"name\",\"mdata\"}</code>. "
            "Use it in Home Assistant as an MQTT sensor with <code>expire_after</code> for "
            "arrival/departure automations. Fixed-MAC beacons and key fobs work best; phones "
            "rotate their private address, so use the Home Assistant companion app for them. "
            "This is a BLE relay, not the ESPHome Bluetooth Proxy, and it needs MQTT enabled.</p>");

  page += F("<button type='submit'>Save</button></form>");
  page += F("<p class='hint' style='margin-top:16px'>The city is looked up with the "
            "Open-Meteo geocoding service. When IP location is enabled the city "
            "field is ignored. All changes apply immediately and survive a reboot.</p>");
  page += F("</div></body></html>");

  webServer.send(200, "text/html; charset=utf-8", page);
}

void handleSave() {
  String msg;

  String tz = webServer.arg("tz");
  tz.trim();
  if (tz.length() && tz != String(clockTz) && tz.length() < sizeof(clockTz)) {
    strncpy(clockTz, tz.c_str(), sizeof(clockTz) - 1);
    clockTz[sizeof(clockTz) - 1] = '\0';
    setenv("TZ", clockTz, 1);
    tzset();
    configTzTime(clockTz, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    clockLastNtp = millis();
    clockTimeLogged = false;
    msg += "Time zone updated. ";
  }

  int bri = webServer.arg("bright").toInt();
  if (bri >= 1 && bri <= 255 && bri != (int)ledBrightness) {
    ledBrightness = (uint8_t)bri;
    pixels.setBrightness(ledBrightness);
    prefs.putUChar("bright", ledBrightness);
    msg += "Brightness " + String(bri) + ". ";
  }

  uint8_t useIp = webServer.hasArg("ipgeo") ? 1 : 0;
  if (useIp != clockUseIp) {
    clockUseIp = useIp;
    weatherGeoDone = false;
    msg += useIp ? "Weather location: public IP. " : "Weather location: chosen city. ";
  }

  String city = webServer.arg("city");
  city.trim();
  if (!clockUseIp && city.length() && city != String(clockCity)) {
    float la, lo;
    String label;
    if (geocodeCity(city, la, lo, label)) {
      clockLat = la;
      clockLon = lo;
      strncpy(clockCity, label.c_str(), sizeof(clockCity) - 1);
      clockCity[sizeof(clockCity) - 1] = '\0';
      weatherGeoDone = false;
      msg += "City: " + label + " (" + String(la, 2) + ", " + String(lo, 2) + "). ";
      Serial.printf("CLOCK: city %s -> %.3f, %.3f\n", clockCity, la, lo);
    } else {
      msg += "City lookup failed; kept " + String(clockCity) + ". ";
    }
  }

  bool follow = webServer.hasArg("tfollow");
  if (follow) {
    if (clockTimeColorSet) {
      clockTimeColorSet = 0;
      msg += "Time colour follows the base colour. ";
    }
  } else {
    uint32_t c = 0;
    if (parseColor(webServer.arg("tcolor"), c) &&
        (!clockTimeColorSet || c != clockTimeColor)) {
      clockTimeColor = c;
      clockTimeColorSet = 1;
      msg += "Time colour set. ";
    }
  }

  bool autoTemp = webServer.hasArg("pauto");
  if (autoTemp) {
    if (!clockTempAuto) {
      clockTempAuto = 1;
      msg += "Temperature colour follows the value. ";
    }
  } else {
    uint32_t c = 0;
    if (parseColor(webServer.arg("pcolor"), c) &&
        (clockTempAuto || c != clockTempColor)) {
      clockTempColor = c;
      clockTempAuto = 0;
      msg += "Temperature colour set. ";
    }
  }

  bool sched = webServer.hasArg("sched");
  if (sched != (bool)brightSched) {
    brightSched = sched ? 1 : 0;
    msg += sched ? "Day/night schedule on. " : "Day/night schedule off. ";
  }

  int ds = parseHHMM(webServer.arg("dstart"));
  if (ds >= 0 && ds != (int)dayStartMin) {
    dayStartMin = (uint16_t)ds;
    msg += "Day starts " + hhmm(dayStartMin) + ". ";
  }
  int ns = parseHHMM(webServer.arg("nstart"));
  if (ns >= 0 && ns != (int)nightStartMin) {
    nightStartMin = (uint16_t)ns;
    msg += "Night starts " + hhmm(nightStartMin) + ". ";
  }

  int db = webServer.arg("dbright").toInt();
  if (db >= 1 && db <= 255 && db != (int)dayBright) {
    dayBright = (uint8_t)db;
    msg += "Day brightness " + String(db) + ". ";
  }
  int nb = webServer.arg("nbright").toInt();
  if (nb >= 1 && nb <= 255 && nb != (int)nightBright) {
    nightBright = (uint8_t)nb;
    msg += "Night brightness " + String(nb) + ". ";
  }

  // ---- home Wi-Fi credentials ----
  bool wifiChanged = false;
  String ssid = webServer.arg("ssid");
  ssid.trim();
  if (ssid.length() && ssid != String(wifiSsid) && ssid.length() < sizeof(wifiSsid)) {
    strncpy(wifiSsid, ssid.c_str(), sizeof(wifiSsid) - 1);
    wifiSsid[sizeof(wifiSsid) - 1] = '\0';
    wifiChanged = true;
  }
  String wp = webServer.arg("wpass");
  if (wp.length() && wp != String(wifiPass) && wp.length() < sizeof(wifiPass)) {
    strncpy(wifiPass, wp.c_str(), sizeof(wifiPass) - 1);
    wifiPass[sizeof(wifiPass) - 1] = '\0';
    wifiChanged = true;
  }
  if (wifiChanged) msg += "Wi-Fi credentials saved; reconnecting. ";

  // ---- MQTT settings ----
  bool mqttChanged = false;
  String mh = webServer.arg("mhost");
  mh.trim();
  if (mh.length() < sizeof(mqttHost) && mh != String(mqttHost)) {
    strncpy(mqttHost, mh.c_str(), sizeof(mqttHost) - 1);
    mqttHost[sizeof(mqttHost) - 1] = '\0';
    mqttChanged = true;
  }
  int mp = webServer.arg("mport").toInt();
  if (mp > 0 && mp <= 65535 && mp != (int)mqttPort) {
    mqttPort = (uint16_t)mp;
    mqttChanged = true;
  }
  String mu = webServer.arg("muser");
  mu.trim();
  if (mu.length() < sizeof(mqttUser) && mu != String(mqttUser)) {
    strncpy(mqttUser, mu.c_str(), sizeof(mqttUser) - 1);
    mqttUser[sizeof(mqttUser) - 1] = '\0';
    mqttChanged = true;
  }
  String mpw = webServer.arg("mpass");
  if (mpw.length() && mpw.length() < sizeof(mqttPass) && mpw != String(mqttPass)) {
    strncpy(mqttPass, mpw.c_str(), sizeof(mqttPass) - 1);
    mqttPass[sizeof(mqttPass) - 1] = '\0';
    mqttChanged = true;
  }
  String mt = webServer.arg("mtopic");
  mt.trim();
  if (mt.length() && mt.length() < sizeof(mqttTopic) && mt != String(mqttTopic)) {
    strncpy(mqttTopic, mt.c_str(), sizeof(mqttTopic) - 1);
    mqttTopic[sizeof(mqttTopic) - 1] = '\0';
    mqttChanged = true;
  }
  bool men = webServer.hasArg("men");
  if (men != (bool)mqttEnabled) {
    mqttEnabled = men ? 1 : 0;
    mqttChanged = true;
  }
  if (mqttChanged) {
    mqttDirty = true;
    msg += "MQTT settings saved. ";
  }

  // ---- Bluetooth -> MQTT bridge ----
  bool bleChanged = false;
  String bt = webServer.arg("bletopic");
  bt.trim();
  if (bt.length() && bt.length() < sizeof(bleTopic) && bt != String(bleTopic)) {
    strncpy(bleTopic, bt.c_str(), sizeof(bleTopic) - 1);
    bleTopic[sizeof(bleTopic) - 1] = '\0';
    bleChanged = true;
  }
  bool ben = webServer.hasArg("bleen");
  if (ben != (bool)bleEnabled) {
    bleEnabled = ben ? 1 : 0;
    bleChanged = true;
  }
  bool bd = webServer.hasArg("bledisc");
  if (bd != (bool)bleDisc) {
    bleDisc = bd ? 1 : 0;
    bleChanged = true;
  }
  if (bleChanged) {
    bleDirty = true;
    msg += "Bluetooth bridge settings saved. ";
  }

  webMsg = msg.length() ? msg : "No changes.";

  saveClockConfig();
  pixels.setBrightness(brightnessNow());
  weatherLastTry = 0;
  weatherRequest = true;

  webServer.sendHeader("Location", "/");
  webServer.send(303, "text/plain", "");

  // Reconnect only after the reply has been sent, otherwise the browser would
  // lose the connection in the middle of the redirect.
  if (wifiChanged) wifiApply();
}

void webBegin() {
  if (webRunning) return;
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_GET, handleSave);
  webServer.onNotFound(handleRoot);
  webServer.begin();
  webRunning = true;
  Serial.println("WEB: settings page listening on port 80");
}
