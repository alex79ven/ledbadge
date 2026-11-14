# ESP32-S3-Matrix — clock, calendar & Home Assistant lamp knob

Two effects on the onboard 8×8 RGB LED matrix of the
**Waveshare ESP32-S3-Matrix**:

1. **Clock** — a six-page carousel on a 16×24 virtual canvas: hours, minutes,
   temperature, an animated weather icon, a tear-off **calendar sheet** with
   today's date, and a **scrolling weekday** (`SATURDAY`) that follows the
   sheet. Time comes from NTP over Wi-Fi, weather from the Open-Meteo API. The
   picture is rotated with the accelerometer so it stays upright however you
   hold the board.
2. **Lamp knob** — the badge becomes a dimmer for a real lamp in Home
   Assistant. Hold it upright and turn it like a knob: the accelerometer gives
   an absolute angle (no drift), the gyro takes over when the badge lies flat,
   and the percentage is published to MQTT. A Home Assistant automation maps it
   to `light.turn_on` / `brightness_pct`.

A small **web page** on port 80 configures Wi-Fi, time zone, weather city,
brightness, colours, MQTT and the BLE bridge; if the saved network disappears
the badge opens its own setup hotspot. **MQTT** messages (for example from
Home Assistant) scroll across the matrix in every effect, and **MQTT
discovery** adds the badge to Home Assistant with no manual YAML.

This is a **separate sketch** from the spirit level. Both live side by side in
`firmware/` and either can be flashed to the same board.

## Dependencies

Install these Arduino libraries once:

```bash
arduino-cli lib install Adafruit_NeoPixel PubSubClient NimBLE-Arduino
```

## Hardware

| Item | Value |
|---|---|
| Board | Waveshare ESP32-S3-Matrix (ESP32-S3, 4 MB flash / 2 MB PSRAM) |
| LED matrix | 8×8 WS2811, data on **GPIO14**, 64 LEDs, `NEO_RGB + NEO_KHZ800` |
| IMU | QMI8658 on I²C, address `0x6B`, SDA **GPIO11**, SCL **GPIO12** |
| Button | BOOT on **GPIO0** (active low, internal pull-up) |

## Controls

| Action | Effect |
|---|---|
| Single click **BOOT** | next base colour (both effects) |
| Double click **BOOT** | next effect: clock → lamp knob → clock |
| Hold **BOOT** (≥ 0.5 s) | animation dynamics: ramps 50 → 200 %, then back down while still held |
| Rotate the badge upright, or turn it flat on the table | lamp knob: dimmer percentage (gyro when flat) |
| Web page | Wi-Fi, time zone, weather city, colours, brightness, MQTT and BLE |
| MQTT message | scrolls across the matrix in any effect |

The selected colour, effect, dynamics level and brightness are stored in NVS
(`liquid/color`, `liquid/effect`, `liquid/dyn`, `liquid/bright`); the knob
position lives in `clock/knob`. All are restored on boot.

Colour order: red → orange → yellow → green → cyan → blue → violet → magenta.

A single click is applied ~350 ms after release, because the firmware waits to
see whether a second click turns it into a double click. A press shorter than
0.5 s is a click; a longer one is a dynamics hold and never changes the colour.

### Dynamics hold

Hold BOOT and the level steps by 10 % every 120 ms. It starts from the stored
value, walks up to 200 %, then reverses and walks down while you keep holding,
so a single hold reaches the level you want in either direction. Release to
keep it; the current value is saved and printed as `DYNAMICS: <percent>`.

In the **clock** effect the dynamics factor scales the page glide speed; the
**lamp knob** is unaffected by it.

## Lamp knob (Home Assistant dimmer)

The second effect turns the badge into a rotary dimmer for a real lamp:

- **Upright** (the normal way to hold it): gravity lies in the screen plane and
  `atan2(ay, ax)` gives the absolute rotation angle. Turning the badge
  clockwise raises the value, a full 360° turn spans 0–100 %, and there is no
  drift — the angle is absolute, so it works after any number of turns. A small
  dead band (0.15°) ignores sensor noise, and single-frame jumps above 20° are
  rejected so picking the badge up does not slam the lamp to full brightness.
- **Flat on the table**: gravity is perpendicular to the screen and carries no
  angle, so the gyro Z rate is integrated instead (with hysteresis at
  `KNOB_FLAT_G`, 0.8 dps dead band and the zero point relearned while still).
- **Display** — just the percentage, in 3×5 digits spread to the outer three
  columns each so the two digits never touch. Fixed to the board (no upright
  correction), so it turns with the badge like a real knob.
- **Publishing** — `{"pct":N}` on `ledbadge/knob`, retained, at most 5
  messages/s and only when the whole percent changes.
- **Persistence** — the knob position is written to NVS once it has been still
  for 1.5 s, so a turn does not wear out the flash.

Home Assistant automation (adjust `entity_id`):

```yaml
alias: Badge knob -> lamp
trigger:
  - platform: mqtt
    topic: ledbadge/knob
action:
  - service: light.turn_on
    target:
      entity_id: light.living_room
    data:
      brightness_pct: "{{ trigger.payload_json.pct }}"
mode: queued
```

Because the value is a percentage, `brightness_pct` is exactly the right
parameter; no 0–255 conversion is needed. `mode: queued` keeps rapid turns in
order, and `mode: single` with `max_exceeded: silent` is a calmer alternative
for a lamp.

If a flat turn works backwards on your board, flip `KNOB_GYRO_SIGN` to `-1`.

## Clock

Six **full-screen** pages live on a 16×24 virtual canvas arranged as a 2×3
grid. The 8×8 window visits them one whole screen at a time:

```
+-----------+-----------+
| calendar  | weekday   |
|    14     | SATURDAY  |
+-----------+-----------+
| weather   | hours     |
|  icon     |   09      |
+-----------+-----------+
| temp      | minutes   |
|   24°     |   58      |
+-----------+-----------+
```

The window glides around the grid in this order, so the picture is never
cramped and every page uses the whole matrix:

```
hours --down--> minutes --left--> temp --up--> icon --up--> calendar
   --right--> weekday --down--> hours ...
```

- **Hours** and **minutes** — two big 3×7 digits, on the base colour by
  default (the web page can pin an explicit colour). A single pixel in the
  corner pulses once per second so the clock visibly ticks.
- **Temperature** — two 3×7 digits plus a degree dot, coloured by the value
  (icy blue when freezing, warm yellow/orange when hot) unless an explicit
  colour is chosen on the web page.
- **Calendar sheet** — a tear-off calendar: an off-white paper sheet with a red
  binding strip along the top and today's date in 3×5 digits (centred for one
  or two digits, dark ink on paper).
- **Weekday ticker** — after the calendar has slid away, the weekday scrolls in
  from the right edge and crosses the page (`SATURDAY`) in pure white, so it
  stands out from the coloured clock digits. It restarts from the right once
  the word has left the screen.
- **Weather icon** — a full-screen animated picture in natural colours. Every
  element moves at a fractional position and is blended over the neighbouring
  LEDs, so the pictures glide instead of stepping pixel by pixel. When Open-Meteo
  reports `is_day = 0` (night or after sunset) the daytime sun is replaced by a
  night set built from a reference 8×8 bitmap: a golden crescent, single-pixel
  pale stars and cool blue-grey clouds.
   - **Sun** — the 8×8 reference bitmap: a round 4×4 core with cut corners and
     eight one-pixel rays. The rays are animated by a slow wave travelling
     around them, so each fades out and in smoothly, one after another.
   - **Moon** — the reference gold crescent and five twinkling single-pixel
     stars, with a shooting star that crosses the sky every ~7 s.
   - **Partly cloudy** — small pulsing sun by day; at night a golden crescent
     badge and two stars behind the clouds.
   - **Cloudy** — two cloud layers drifting in opposite directions; at night the
     layers turn blue-grey and the moon badge plus stars show behind them.
   - **Rain** — drifting cloud, five drops falling at different speeds with
     motion streaks, tiny splashes on the bottom row. At night the cloud is
     blue-grey and stars sit in the free top corners.
   - **Snow** — five flakes swaying side to side while falling, each twinkling
     on its own rhythm; same night tint and stars as rain.
   - **Thunderstorm** — dark cloud with a double lightning flash, a warm
     afterglow halo around the bolt and faint rain underneath; the cloud takes a
     cold blue-grey tone at night.
   - **Fog** — four wavy bands drifting at different speeds and bending as they
     move; at night the bands are dimmer and a faint golden crescent shows
     through the mist.
   - **Loading** — grey cloud with three bobbing dots until the first
     download arrives.
- **Glide** — the viewport moves one screen (8 px) per step with a smoothstep
  ease and a short pause on each page. The glide scales with the dynamics hold
  (`CLOCK_MOVE_MS`); each page rests 2.6–4.2 s.
- **Upright orientation** — the accelerometer reports which matrix edge points
  to world "up", and the final frame is rotated in 90° steps to match. The
  picture therefore stays readable when the board is turned by 90°, 180° or
  270°. A 14° dead zone plus a 450 ms confirmation delay keep it from
  twitching near 45°.

### Network

| Item | Value |
|---|---|
| Wi-Fi SSID | `YOUR_WIFI_SSID` (editable on the web page) |
| Wi-Fi password | `YOUR_WIFI_PASSWORD` (editable on the web page) |
| Fallback hotspot | `LedBadge-XXXX`, password `ledbadge`, page at `http://192.168.4.1/` |
| Time | NTP (`pool.ntp.org`, `time.google.com`, `time.cloudflare.com`), Europe/Madrid by default |
| Weather | Open-Meteo `current=temperature_2m,weather_code,is_day`, HTTPS |
| Location | public IP (`ip-api.com`) by default; the fixed fallback is Madrid |
| Refresh | weather every 15 min, retry every 60 s until the first success |

**Wi-Fi runs in every effect**, not only while the clock is visible, so the
settings page and MQTT messages stay reachable during the dimmer too. Only
the weather download is paused outside the clock effect.

As soon as the address is known, the badge scrolls it once across the matrix
(`IP 192.168.1.50`) so you do not need the serial console to find the page.
If the home network is unreachable, the hotspot name and `192.168.4.1` scroll
instead.

If the saved network cannot be reached (after roughly 35 s of retries) the
badge switches to `WIFI_AP_STA` and starts its own hotspot:

```
WIFI: [YOUR_WIFI_SSID] unreachable, setup hotspot 'LedBadge-3A1C' (pass 'ledbadge')
WIFI: open http://192.168.4.1
```

Connect a phone to that hotspot, open `http://192.168.4.1/` and enter the new
network name and password. The badge keeps retrying the real network every
20 s and closes the hotspot as soon as it connects. Nothing needs to be
reflashed when you move to another Wi-Fi.

The blocking HTTPS weather download runs in a FreeRTOS task pinned to core 0,
so the animation never stalls (requests time out after 8 s).

### MQTT (Home Assistant)

The badge is an MQTT client (PubSubClient 2.8). Configure it on the web page:

| Setting | Default |
|---|---|
| Enable MQTT | off |
| Broker host / port | empty / `1883` |
| Username / password | empty |
| Message topic | `ledbadge/text` |

- Text published to the **message topic** scrolls right-to-left across the
  matrix in any effect, is folded to upper case and is capped at 160
  characters.
- A picture published to `ledbadge/bitmap` is drawn full screen (see below); it
  temporarily overrides both the clock and the scrolling text.
- The badge publishes its availability with a retained **last will** on
  `ledbadge/status` (`online` / `offline`).
- The message and picture are handed from the MQTT task (core 0) to the
  renderer through small staging buffers, so a slow broker never stutters the
  LEDs.

Test from a shell:

```bash
mosquitto_pub -h 192.168.1.50 -t ledbadge/text -m "Dinner is ready"
```

Home Assistant automation example:

```yaml
alias: Badge message
trigger:
  - platform: state
    entity_id: input_button.badge_message
action:
  - service: mqtt.publish
    data:
      topic: ledbadge/text
      payload: "Front door is open"
```

### 8×8 bitmap over MQTT

Publish a picture to **`ledbadge/bitmap`**. It appears immediately, replaces
the current effect for the chosen time (20 s by default), then the clock or
dimmer resumes. A picture also stops a scrolling text. Payload whitespace is
ignored, so the rows can be pasted on separate lines.

| Body | Meaning |
|---|---|
| 64 characters of `0`/`1` | monochrome mask, row-major, top-left first |
| 16 hex digits | the same mask as 8 row bytes, MSB = left pixel |
| 192 hex digits | full colour, 3 hex digits per pixel (`RGB444`, 64 × 3) |

Optional suffix `:<RRGGBB>` (or `:#RRGGBB`) sets the colour of a monochrome
mask, and `:<seconds>` sets how long it stays (`0` = until replaced or
cleared). Max payload ~205 characters; the HA **Bitmap 8x8** text entity
accepts the same string.

```bash
# heart, white, 20 s
mosquitto_pub -h 192.168.1.50 -t ledbadge/bitmap \
  -m "000000000110011011111111111111111111111101111110001111000001100000000000"

# same mask as hex, orange, one minute
mosquitto_pub -h 192.168.1.50 -t ledbadge/bitmap -m "0066FFFFFF7E3C18:FF8800:60"

# sticky until cleared
mosquitto_pub -h 192.168.1.50 -t ledbadge/bitmap -m "0066FFFFFF7E3C18:FFFFFF:0"

# clear now (or publish an empty payload / "off")
mosquitto_pub -h 192.168.1.50 -t ledbadge/bitmap -m "off"
```

Full-colour example: 64 pixels × 3 hex digits = 192 characters. A red/black
checkerboard therefore starts `F00000F00000…` (`F00` = red pixel, `000` =
black pixel, and so on). The picture follows the accelerometer orientation,
exactly like the clock and the scrolling text.

Home Assistant discovery is automatic. On every successful connect the badge
publishes retained configs under `homeassistant/<component>/ledbadge_XXXXXX/…`,
so a single **LED Badge** device appears in Home Assistant by itself:

| Entity | Type | Topic |
|---|---|---|
| Wi-Fi signal | sensor (dBm) | `ledbadge/state` → `wifi` |
| IP address | sensor | `ledbadge/state` → `ip` |
| Uptime | sensor (s) | `ledbadge/state` → `uptime` |
| Free memory | sensor (B) | `ledbadge/state` → `heap` |
| Brightness | number, 1–255 step 1 | `ledbadge/bright/set` / `ledbadge/bright` |
| Knob | sensor (%) | `ledbadge/knob` → `pct` |
| Message | text (max 160) | publishes to the message topic |
| Bitmap 8x8 | text (max 205) | publishes to `ledbadge/bitmap` |

The state snapshot is retained and refreshed every 15 s; the brightness topic
is plain (`128`) so the slider follows both MQTT and web changes. Discovery
uses `homeassistant` as prefix — change `MQTT_DISCOVERY_PREFIX` if your broker
uses another one. Nothing is hard-coded to a network: the node id is derived
from the chip MAC (`ledbadge_XXXXXX`).

The scrolling overlay shows the clock colour when one is pinned, otherwise the
current base hue.

If the broker answers `rc=5` (`MQTT_CONNECT_UNAUTHORIZED`) the username or
password is wrong — Mosquitto rejects anonymous clients unless `allow_anonymous
true` is set. Use the MQTT credentials from Home Assistant.

### Bluetooth → MQTT (presence)

> This is a **relay, not the ESPHome Bluetooth Proxy**. Home Assistant reads
> the data through MQTT; its own Bluetooth integration will not list the badge.

Enable it on the web page and set a base topic (default `ledbadge/ble`). The
badge then scans BLE advertisements (NimBLE, ~25 % duty so Wi-Fi keeps working)
and publishes one JSON message per device per 5 s. The scan is active, so the
badge also asks for scan responses: that is where most tags keep their name.
Each sighting looks like this:

```
topic:   ledbadge/ble/a4c1382f9b1e
payload: {"addr":"a4:c1:38:2f:9b:1e","rssi":-63,"name":"ATC_2f9b1e","mdata":"1a18..."}
```

- `mdata` is the manufacturer data in hex (iBeacon UUID/major/minor, Xiaomi
  sensor frames, etc.), `addr` the current BLE address.
- The serial console prints `BLE: <addr> rssi=<n> name=[<name>]` once for every
  newly heard device, so you can see whether a tag is received at all even when
  Home Assistant shows nothing.
- Optional: tick **Create Home Assistant sensors for BLE devices** and every
  *named* device gets its own RSSI sensor (with `expire_after: 60`), grouped
  under the badge device. Handy for fobs; phones with rotating addresses would
  add a new entity each time, so the feature is off by default and capped at
  24 entities per boot.
- Presence detection works best with devices that advertise continuously and
  keep a fixed MAC: iBeacon tags, key fobs, ESPresense-style beacons.
  **Phones rotate their private BLE address**, so they keep appearing as new
  devices — use the Home Assistant companion app or router/zone tracking for
  phones instead.
- Without the auto-discovery checkbox, add a sensor by hand with
  `expire_after` so it turns `unavailable` when no advertisements arrive:

```yaml
mqtt:
  sensor:
    - name: "Badge key fob RSSI"
      state_topic: "ledbadge/ble/a4c1382f9b1e"
      value_template: "{{ value_json.rssi }}"
      expire_after: 60
```

The bridge needs MQTT enabled and a broker connection; scanning starts when
the MQTT link is up and stops when it drops. A short range and the 5 s throttle
mean presence reacts within a few seconds rather than instantly.

### Web settings page

The badge serves a small configuration page on port 80 whenever it is on a
network. The address is printed on the serial console right after the Wi-Fi
connects:

```
WIFI: ok, IP 192.168.1.50
WIFI: settings http://192.168.1.50
```

Open that address from any device on the same network to change:

- **Wi-Fi network name and password** — the page sends the new credentials,
  the badge reconnects and keeps serving the page during the reconnect. Leave
  the password blank to keep the current one. If the network is lost the
  hotspot fallback described above starts automatically.
- **Time zone** — a list of common POSIX zones (Madrid, London, Lisbon,
  Paris/Berlin, Kyiv, Moscow, Dubai, Tokyo, New York, Los Angeles, UTC) plus a
  custom entry stored as-is.
- **Weather city** — any city name, resolved to coordinates through the
  Open-Meteo geocoding service.
- **Locate automatically by IP** — use the public IP instead of the city.
- **Time colour** — a colour picker for the hour and minute digits. Tick
  *Follow the base colour* to let a single click on BOOT change them.
- **Temperature colour** — a colour picker for the degrees, or tick *Colour by
  temperature* to keep the automatic blue-to-orange scale.
- **Manual brightness** — 1–255, stored as `liquid/bright`.
- **Day/night schedule** — when enabled, the badge uses *day brightness*
  between *day starts* and *night starts* and *night brightness* for the rest
  of the day (the window may wrap midnight). It needs a synced clock; before
  the first NTP sync the manual brightness is used. Both times default to
  08:00 and 22:00.
- **Lamp knob** — a short explanation of the dimmer with the MQTT topic and a
  ready-to-paste Home Assistant automation.
- **MQTT** — enable, broker host/port, username, password and message topic.
  Home Assistant discovery is automatic once the connection succeeds.
- **Bluetooth → MQTT** — enable the BLE advertisement relay, set its base
  topic, and optionally create Home Assistant sensors for named devices.

Settings are stored in NVS (namespace `clock`: `tz`, `city`, `lat`, `lon`,
`ipgeo`, `tcolor`, `tcset`, `pcolor`, `pauto`, `sched`, `dstart`, `nstart`,
`dbright`, `nbright`, `wssid`, `wpass`, `mhost`, `mport`, `muser`, `mpass`,
`mtopic`, `men`, `bletopic`, `bleen`, `bledisc`, `knob`) and survive reboots.
The badge's own brightness lives in the `liquid` namespace (`bright`); the
lamp knob position is `clock/knob`. The defaults are compiled in at the top of
the sketch (`CLOCK_WIFI_SSID`, `CLOCK_WIFI_PASS`, `CLOCK_TZ`, `FIXED_LAT`,
`FIXED_LON`), so the badge works before any configuration.

## Flashing

The sketch now uses **NimBLE**, so it no longer fits the default 1.2 MB app
partition. Use the `min_spiffs` partition scheme (1.9 MB app):

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=min_spiffs" firmware/ESP32_S3_Matrix_Liquid
arduino-cli upload  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=min_spiffs" --port /dev/cu.usbmodem14201 firmware/ESP32_S3_Matrix_Liquid
```

`CDCOnBoot=cdc` is only needed for the USB serial console. The animation does
not depend on it.

Expected boot log:

```
LED BADGE BOOT
QMI WHO_AM_I=0x5
EFFECT: clock
COLOUR: yellow
```

## Tuning

All behaviour constants are at the top of the sketch:

| Constant | Meaning |
|---|---|
| `KNOB_DEG_PER_PCT` | degrees of badge rotation per brightness percent (3.6 = one full turn) |
| `KNOB_DEADBAND_DEG` | accelerometer-angle jitter that is ignored |
| `KNOB_FLAT_G` | in-plane gravity below which the gyro takes over |
| `KNOB_GYRO_SIGN` | flip to `-1` if a flat turn works backwards |
| `KNOB_GYRO_DEADBAND` | gyro rate ignored as tremor (dps) |
| `KNOB_PUBLISH_MS` | MQTT publish throttle for the knob (200 ms) |
| `CLOCK_UPRIGHT_DEG` | tilt needed before the clock picture rotates (14°) |
| `CLOCK_MOVE_MS` | time to glide one full screen at 100 % dynamics |
| `WEEKDAY_SPEED` | weekday ticker speed (16 px/s) |
| `CLOCK_TZ` | POSIX time-zone string (Europe/Madrid) |
| `WEATHER_REFRESH_MS` | how often the weather is downloaded (15 min) |
| `WEATHER_DEMO` / `WEATHER_DEMO_MS` | icon preview: 1 = cycle all weather icons full screen |
| `AP_NAME_PREFIX` / `AP_PASSWORD` | setup hotspot name prefix and password |
| `MQTT_DEFAULT_TOPIC` | default topic for scrolling messages |
| `MQTT_STATUS_TOPIC` | retained `online`/`offline` availability topic |
| `MQTT_KNOB_TOPIC` | dimmer topic (`ledbadge/knob`, `{"pct":N}`) |
| `MQTT_BITMAP_TOPIC` | 8×8 picture topic (`ledbadge/bitmap`) |
| `BMP_DEFAULT_MS` | how long a picture stays without `:seconds` (20 s) |
| `MQTT_DISCOVERY_PREFIX` | Home Assistant discovery prefix (`homeassistant`) |
| `MSG_MAX_LEN` / `MSG_SPEED` | longest message and scroll speed (px/s) |
| `BLE_DEFAULT_TOPIC` | base topic for BLE advertisements (`base/<mac>`) |
| `BLE_PUBLISH_MS` | per-device publish throttle (5 s) |
| `DOUBLE_CLICK_MS` | double-click window (default 350 ms) |

`DEBUG_KNOB 1` streams tilt angles, the raw accelerometer and the current knob
percentage over USB serial at 2 Hz. Keep it at `0` for stand-alone use: a
blocking `Serial.print` into a full USB-CDC buffer (no monitor attached) would
freeze the animation.

## Robustness

- The QMI8658 is driven with plain `Wire` (the vendor SensorLib panics on this
  core version).
- A sensor watchdog reinitialises the I²C bus and the IMU if no fresh sample
  arrives for 2 seconds.
- The BOOT button is debounced (30 ms stable level, 1.8 s boot guard) so the
  USB auto-reset circuit cannot change the colour accidentally.
