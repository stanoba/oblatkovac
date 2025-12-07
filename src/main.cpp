#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <RotaryEncoder.h>
#include <thermistor.h>
#include <debounce.h>

// ###############################  Definitions  ###############################
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
  int thermistor_pin = A0;    // only analog pin on Wemos
#elif defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ESP32_C3)
  // ESP32-C3 defaults. These are generic choices — change to match your wiring.
  // Notes: On ESP32 cores analog pins are regular GPIO numbers. Verify ADC channel availability for your board.
  // ESP32-C3: choose pins so none overlap. Adjust to your board's ADC-capable pins.
  int encoder_dt_pin = 5;     // GPIO4
  int encoder_clk_pin = 6;    // GPIO5
  int encoder_sw_pin = 7;     // GPIO0 (watch boot strapping)
  int relay_pin = 10;         // GPIO8
  int thermistor_pin = 0;     // GPIO2 (example ADC-capable pin — verify for your board)
  int i2c_sda_pin = 8;        // choose GPIO6 for SDA (avoid collisions)
  int i2c_scl_pin = 9;        // choose GPIO7 for SCL (avoid collisions)
#elif defined(ESP32)
  // Generic ESP32 defaults (non-C3). Adjust to your dev board.
  int encoder_dt_pin = 18;
  int encoder_clk_pin = 19;
  int encoder_sw_pin = 21;
  int relay_pin = 23;         // choose GPIO23 for relay to avoid colliding with I2C
  int thermistor_pin = 36;    // example ADC-capable pin; change if needed
  int i2c_sda_pin = 21;       // common SDA on many ESP32 devkits
  int i2c_scl_pin = 22;       // common SCL on many ESP32 devkits
#else
  // AVR / Arduino defaults (original values)
  int encoder_dt_pin = 2;
  int encoder_clk_pin = 3;
  int encoder_sw_pin = 4;
  int relay_pin = 5;
  int thermistor_pin = A1;
#if defined(__AVR__)
  int i2c_sda_pin = A4;       // default SDA on many AVR boards (e.g. UNO/Nano)
  int i2c_scl_pin = A5;       // default SCL on many AVR boards (e.g. UNO/Nano)
#else
  int i2c_sda_pin = -1;
  int i2c_scl_pin = -1;
#endif
#endif

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, i2c_scl_pin, i2c_sda_pin); // U8G2_SH1106_128X64_NONAME_F_HW_I2C(rotation, [reset [, clk, data]])
thermistor therm1(thermistor_pin,1);  // Analog Pin which is connected to the 3950 temperature sensor, and 0 represents TEMP_SENSOR_0 (see configuration.h for more information).
int default_temp = 130; // 120
int target_temp = default_temp;
int hysteresis = 1;
int min_temp = 90; // 50
int max_temp = 190;
double temp = 0;
boolean heat_on = 1;
unsigned long previousTime = 0;
const unsigned long timeout = 500; //1000; // 1s in ms, update interval
boolean tick = 0;
int menu_positon = 1;
RotaryEncoder encoder(encoder_clk_pin, encoder_dt_pin, RotaryEncoder::LatchMode::FOUR3);
int lastPos = -1;
#define ROTARYSTEPS 1
#define ROTARYMIN min_temp
#define ROTARYMAX max_temp

// ################################  Functions  ################################

static void buttonHandler(uint8_t btnId, uint8_t btnState) {
  if (btnState == BTN_PRESSED) {
    Serial.println("Pushed button");
    menu_positon++;
    if (menu_positon > 3){
      menu_positon = 1;
    }
    Serial.print("Menu: ");
    Serial.println(menu_positon);
  }
}
static Button myButton(0, buttonHandler);

// ##################################  Setup  ##################################
void setup(void) {
  u8g2.begin(); // initialize the display
  // Initialize I2C/Wire with platform-specific pins when available.
#if defined(ESP8266) || defined(ESP32) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ESP32_C3)
  Wire.begin(i2c_sda_pin, i2c_scl_pin);
#else
  Wire.begin();
#endif
  pinMode(relay_pin, OUTPUT);
  pinMode(encoder_sw_pin, INPUT);

  Serial.begin(9600);
  Serial.println("Basic Encoder Test:");
  encoder.setPosition(default_temp);
}

//static void pollButtons() {
//  myButton.update(digitalRead(encoder_sw_pin));
//}

// ##################################  Loop  ##################################
void loop(void) {
  encoder.tick();
 // pollButtons(); // Poll your buttons every loop.

  // Timer for update interval (1 second)
  if (millis() >= (previousTime)) {
    previousTime = previousTime + timeout;
    tick=1;
  }else{
    tick=0;
  }


  // If it's time to update
  if (tick) {

  // read temperature
 temp = therm1.analog2temp();
  


  // clear the internal memory and set font
  u8g2.clearBuffer();

  // Icony
  // teplomer
  u8g2.setFont(u8g2_font_streamline_weather_t);
  u8g2.drawGlyph(4, 28, 0x36);
  
  // hodiny
  u8g2.setFont(u8g2_font_streamline_interface_essential_circle_triangle_t);
  u8g2.drawGlyph(0, 60, 0x36);

  // blikajuci vykricnik
  if (heat_on == 1){
    if (((previousTime/1000) % 2) == 0){
      u8g2.setFont(u8g2_font_streamline_interface_essential_circle_triangle_t);
      u8g2.drawGlyph(105, 60, 0x33);
    }
  }

  // Font text
  u8g2.setFont(u8g2_font_neuecraft_tr);

  // print temperature
  u8g2.setCursor(30, 15);
  u8g2.print("Temp: ");
  u8g2.print(temp,1);
  u8g2.print(" °C");


  // Turn heater OFF
  if (temp >= (target_temp + hysteresis)) {
    digitalWrite(relay_pin, LOW);
    heat_on = 0;
  }

  // Turn heater ON
  if (temp <= (target_temp - hysteresis)) {
    digitalWrite(relay_pin, HIGH);
    heat_on = 1;
  }

  u8g2.setCursor(30, 30);
  u8g2.print("Set: ");
  u8g2.print(target_temp );
  u8g2.print(" °C");
  // u8g2.print(target_temp - hysteresis);
  // u8g2.print(" <");
  // u8g2.print(target_temp + hysteresis);

  // u8g2.setFont(u8g2_font_streamline_interface_essential_circle_triangle_t);
  // u8g2.drawGlyph(0, 52, 0x36);
  // u8g2.setFont(u8g2_font_neuecraft_tr);



  u8g2.setCursor(30, 55);
  u8g2.print("Time: ");
  u8g2.print(previousTime/1000);
  u8g2.print(" s");

  // u8g2.setCursor(0, 60);
  // u8g2.print("Heat: ");
  // if (heat_on == 1){
  //   u8g2.print("ON");
  //   // }
  // }else{
  //   u8g2.print("OFF");
  // }
  

  // u8g2.setFont(u8g2_font_neuecraft_tr);


  // send buffer to display
  u8g2.sendBuffer();
    }
// end of tick

  // get the current physical position and calc the logical position
  int newPos = encoder.getPosition() * ROTARYSTEPS;

  if (newPos < ROTARYMIN) {
    encoder.setPosition(ROTARYMIN / ROTARYSTEPS);
    newPos = ROTARYMIN;

  } else if (newPos > ROTARYMAX) {
    encoder.setPosition(ROTARYMAX / ROTARYSTEPS);
    newPos = ROTARYMAX;
  } // if

  if (lastPos != newPos) {
    lastPos = newPos;
    target_temp = lastPos;
  } // if


}

