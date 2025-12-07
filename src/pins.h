// Pin definitions: support multiple platforms with sensible defaults.
// Adjust these values to match your wiring if necessary.
#if defined(ESP8266)
  // Wemos D1 mini (ESP8266) mappings — change if your wiring differs.
  // Keep I2C on the common D1/D2 pins, move encoder and relay to other free GPIOs
  // to avoid duplicating pin usage (D1/D2 are SCL/SDA by convention on many boards).
  int i2c_sda_pin = D2;       // SDA (GPIO4)
  int i2c_scl_pin = D1;       // SCL (GPIO5)
  int encoder_dt_pin = D7;    // use D7 (GPIO13)
  int encoder_clk_pin = D6;   // use D6 (GPIO12)
  int encoder_sw_pin = D5;    // use D5 (GPIO14)
  int relay_pin = D8;         // use D8 (GPIO15) for relay output
  int reed_contact_pin = -1;  // not used on this platform
  int buzzer_pin = -1;        // not used on this platform
  int thermistor_pin = A0;    // only analog pin on Wemos
#elif defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ESP32_C3)
  // ESP32-C3 defaults. These are generic choices — change to match your wiring.
  // Notes: On ESP32 cores analog pins are regular GPIO numbers. Verify ADC channel availability for your board.
  // ESP32-C3: choose pins so none overlap. Adjust to your board's ADC-capable pins.
  int encoder_dt_pin = 5;     // GPIO4
  int encoder_clk_pin = 6;    // GPIO5
  int encoder_sw_pin = 7;     // GPIO0 (watch boot strapping)
  int relay_pin = 10;         // GPIO8
  int reed_contact_pin = -1;  // not used on this platform
  int buzzer_pin = -1;        // not used on this platform
  int thermistor_pin = 0;     // GPIO2 (example ADC-capable pin — verify for your board)
  int i2c_sda_pin = 8;        // choose GPIO6 for SDA (avoid collisions)
  int i2c_scl_pin = 9;        // choose GPIO7 for SCL (avoid collisions)
#elif defined(ESP32)
  // Generic ESP32 defaults (non-C3). Adjust to your dev board.
  int encoder_dt_pin = 18;
  int encoder_clk_pin = 19;
  int encoder_sw_pin = 21;
  int relay_pin = 23;         // choose GPIO23 for relay to avoid colliding with I2C
  int reed_contact_pin = -1;  // not used on this platform
  int buzzer_pin = -1;        // not used on this platform
  int thermistor_pin = 36;    // example ADC-capable pin; change if needed
  int i2c_sda_pin = 21;       // common SDA on many ESP32 devkits
  int i2c_scl_pin = 22;       // common SCL on many ESP32 devkits
#else
  // AVR / Arduino defaults (original values)
  int encoder_dt_pin = 2;
  int encoder_clk_pin = 3;
  int encoder_sw_pin = 4;
  int relay_pin = 5;
  int reed_contact_pin = 6;
  int buzzer_pin = 7;
  int thermistor_pin = A1;
#if defined(__AVR__)
  int i2c_sda_pin = A4;       // default SDA on many AVR boards (e.g. UNO/Nano)
  int i2c_scl_pin = A5;       // default SCL on many AVR boards (e.g. UNO/Nano)
#else
  int i2c_sda_pin = -1;
  int i2c_scl_pin = -1;
#endif
#endif