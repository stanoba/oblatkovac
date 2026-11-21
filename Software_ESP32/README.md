# Oblátkovač: ESP32 + Arduino Nano

This PlatformIO project contains two firmware targets. The existing `Software/`
firmware remains unchanged while the new design is developed here.

## Responsibilities

- `esp32c3` owns the application state machine, local/web settings, bake timer,
  overtime display state, RAM-only completed-bake counter, captive Wi-Fi setup,
  AJAX-updated local web controls, live temperature/heater/opening chart and event history,
  bake statistics, Prometheus metrics, and OTA updates.
- `nano` is the I/O controller. It reads the thermistor, lid sensor, encoder, and
  buttons; forwards encoder adjustments to ESP32 (turn to adjust temperature or
  time, press to switch); drives the relay, buzzer, and LEDs; and renders the OLED from display
  state sent by the ESP32. It retains local temperature regulation and turns the
  heater off immediately on local STOP, invalid sensor data, or loss of the
  ESP32 UART heartbeat. While ESP32 is disconnected, button presses are
  stop-only and cannot start heating.

## Persistent data

- ESP32 NVS stores Wi-Fi credentials, set temperature/time, and the optional UI
  password used by both web authentication and OTA.
- On first startup, if ESP32 NVS has no settings yet, Nano reports the current
  temperature/time values from its existing EEPROM so ESP32 can import them.
  After that, ESP32 NVS is authoritative for these settings.
- The completed-bake counter exists only in ESP32 RAM and resets after an ESP32
  restart or OTA update. It is never written to Nano EEPROM. ESP32 is the single
  source of truth and sends the same value to the Nano OLED and web UI.
- The web chart keeps a 180-minute rolling trace at one temperature sample
  every 10 seconds and the last 100 lid-opening events in ESP32 RAM. Relay
  transitions and debounced openings are sent immediately. This history resets
  on ESP32 restart or OTA update and is never written to flash.
- The chart plots actual temperature and the configured setpoint as separate
  traces.
- Dashboard status, statistics, chart/event updates, and control actions use
  AJAX so the page does not need a full refresh.
- Session statistics in ESP32 RAM include average bake duration (countdown plus
  overtime), average lid-open duration, and estimated wafers per hour based on
  the last 10 intervals between completed bakes. They reset after reboot/OTA.
- Prometheus metrics are available at `/metrics` on the local network in text
  exposition format 0.0.4. They include appliance state/statistics and available
  ESP32 CPU frequency, heap, flash, uptime, reset-reason, and Wi-Fi metrics; no
  credentials are exported.
- Nano EEPROM is not used for the bake counter. Existing settings are migrated
  to ESP32 NVS as part of the new application firmware.

## UART and level shifting

The link uses 9600 baud, 8N1. ESP32-C3 RX/TX are GPIO1/GPIO3. Nano RX/TX use
SoftwareSerial pins D11/D12. Connect through the level shifter with HV at 5 V,
LV at 3.3 V, and common ground. Messages carry input/status data in one
direction and target, heater-enable, and display-state commands in the other.
If Nano receives no valid heartbeat for 2 seconds, it disables the heater and
requires a fresh explicit run command after communication returns. UART buffers
are temporary only; stale commands are discarded and never replayed after
reconnection. The ESP32 starts a fresh session with HELLO after boot or link
loss; Nano checks sequence numbers and returns accepted/rejected ACK frames.
The Nano STOP button directly disables the relay without waiting for ESP32 or
UART.

## Wi-Fi, web UI, and OTA

On first boot or failed connection, the ESP32 starts a captive setup network
named `Oblatkovac-Setup-<last 4 MAC characters>`. It stores Wi-Fi credentials in
NVS and serves the dashboard over either the saved local Wi-Fi network or its
own setup network, without cloud or internet access. Physical and web controls
remain available without a router. The responsive dashboard follows the
WiFi-to-RS232 visual style and offers dark and light themes. Web authentication is disabled by
default and can be enabled in settings. OTA always requires the same configured
password, even while web authentication is disabled; if no password exists, OTA
is disabled.

The ESP32 web UI and Wi-Fi setup portal default to English. The language
selector can switch to Slovak; the choice is stored in ESP32 NVS. Translations
are kept in `include/languages/`, one header per language, so additional
translations can be added without changing the Nano firmware.

OTA supports browser upload and PlatformIO `ArduinoOTA`. For PlatformIO OTA,
set `ESP32_OTA_HOST` and `ESP32_OTA_PASSWORD` in the local environment and build
or upload with `pio run -e esp32c3_ota`. These credentials are not committed to
the repository. The custom partition table provides two OTA app slots.

## Build and upload

Build both targets with `pio run`, or one target with `pio run -e esp32c3` or
`pio run -e nano`. For Nano clones with the old bootloader, use
`pio run -e nano_oldbootloader -t upload --upload-port COMx` (replace `COMx`
with the port shown by Windows). Select one target when flashing over USB. Bench-check the
UART, thermostat and STOP behavior with the heater load disconnected before
using the appliance. The Nano should be flashed first so the ESP32 can receive
its EEPROM settings and establish the heartbeat.
