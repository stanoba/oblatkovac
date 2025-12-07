# Electric Wafer Maker Controller (Oblátkovač)

Electric Wafer Maker Controller (Oblátkovač) is an Arduino/ESP-based controller project that reads a thermistor, accepts input from a rotary encoder, and displays status on an SH1106/SSD1306 monochrome OLED display using U8g2 library. It's been configured to build for multiple boards using PlatformIO.

**Supported build environments (in `platformio.ini`):**

- `arduino_nano_v3` — AVR Nano (ATmega328 compatible)
- `d1_mini` — WeMos D1 mini (ESP8266)
- `esp32_c3` — ESP32 C3 (ESP32-C3)


## Bundled libraries

This project includes the following libraries under the `lib/` folder for reproducible builds:

- `RotaryEncoder-1.5.3` — rotary encoder handling
- `ThermistorLibrary-1.0.6` — thermistor conversion
- `U8g2` — display graphics
- `button-debounce-0.2.1` — simple button debounce helper

See `lib/README` for more details about each bundled library.

## Development tips

- If you change pins, update `src/main.cpp` and test builds for your target environment.
- Use PlatformIO's verbose build (`-v`) to see detailed compiler/linker output when troubleshooting.
- If adding a new board environment, edit `platformio.ini` and add matching pin defines in the sketch if needed.

## License

The project code is provided as-is; each third-party library keeps its original license (see their `LICENSE` files in `lib/`).
