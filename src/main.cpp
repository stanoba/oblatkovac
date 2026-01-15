/*******************************************************************************
 * Electric Wafer Maker Controller (Oblátkovač)
 * GitHub: https://github.com/stanoba/oblatkovac
 * Date: 2025-12
 * Description: Main firmware — temperature sensing, heater control, UI,
 *              rotary encoders and buzzer logic.
 ******************************************************************************/

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <RotaryEncoder.h>
#include <thermistor.h>
#include <debounce.h>
#include "pins.h"

// ############################  Static Variables  #############################

const double hysteresis = 0.5;
const int default_temp = 134; // 134
const int min_temp = 100; // 100
const int max_temp = 160;
const int default_time = 30;
const int min_time = 10;
const int max_time = 90;
#define DEBUG 0 // Set to 1 to enable debug output via Serial

// ###############################  Definitions  ###############################

// Use paged API (no full framebuffer) to save RAM on AVR
// Use the paged SH1106 constructor (mode 1) for HW I2C
U8G2_SH1106_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, i2c_scl_pin, i2c_sda_pin);
thermistor therm1(thermistor_pin, 1); // Analog Pin which is connected to the 3950 temperature sensor
RotaryEncoder encoder1(encoder_clk_pin, encoder_dt_pin, RotaryEncoder::LatchMode::FOUR3); // Temperature encoder
RotaryEncoder encoder2(encoder_clk_pin, encoder_dt_pin, RotaryEncoder::LatchMode::FOUR3); // Time encoder

int target_temp = default_temp;
int time_countdown = default_time;
int set_time = default_time;
double temp = 0;
boolean heat_on = 1;
int rotary_steps = 1;
unsigned long previousTime = 0;
const unsigned long timeout = 500; //1000; // 1s in ms, update interval
boolean tick = 0;
boolean target_reached_once = 0; // Track whether we've already signaled reaching the target temperature
boolean buzzer_active = 0;       // Buzzer one-shot active flag and expiry time (ms)
unsigned long buzzer_off_time = 0;
int menu_positon = 1;
int lastPos1 = -1;
int lastPos2 = -1;
int run_state = 0;

// ################################  Functions  ################################

static void encoder_button_handler(uint8_t btnId, uint8_t btnState)
{
  if (btnState == BTN_PRESSED) {
    menu_positon++;
    if (menu_positon > 3) {
      menu_positon = 1;
    }

#if DEBUG
    Serial.print(F("Menu: "));
    Serial.println(menu_positon);
    Serial.println("");
#endif
  }
}

static void start_stop_button_handler(uint8_t btnId, uint8_t btnState)
{
  if (btnState == BTN_PRESSED) {
    if (run_state == 0) {
      run_state = 1;
    }else{
      run_state = 0;
    }

#if DEBUG
    Serial.println(F("Button start/stop pressed"));
    Serial.print(F("Run state: "));
    Serial.println(run_state);
    Serial.println("");
#endif
  }
}

static Button encoder_button(0, encoder_button_handler);
static Button start_stop_button(1, start_stop_button_handler);

// ##################################  Setup  ##################################
void setup(void)
{
#if DEBUG
  Serial.begin(9600);
  Serial.println(F("Booting up..."));
  Serial.println("");
#endif

  u8g2.begin(); // initialize the display

  // Initialize I2C/Wire with platform-specific pins when available.
#if defined(ESP8266) || defined(ESP32) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ESP32_C3)
  Wire.begin(i2c_sda_pin, i2c_scl_pin);
#else
  Wire.begin();
#endif

  pinMode(relay_pin, OUTPUT);
  pinMode(buzzer_pin, OUTPUT);
  pinMode(led_green_pin, OUTPUT);
  pinMode(led_red_pin, OUTPUT);
  pinMode(led_blue_pin, OUTPUT);
  pinMode(encoder_sw_pin, INPUT);
  pinMode(reed_contact_pin, INPUT);

  // Indicate startup with blue LED
  digitalWrite(led_blue_pin, HIGH);

  encoder1.setPosition(default_temp);
  encoder2.setPosition(default_time);
}

static void pollButtons()
{
  encoder_button.update(digitalRead(encoder_sw_pin));
  start_stop_button.update(digitalRead(start_stop_button_pin));
}

// ##################################  Loop  ##################################
void loop(void)
{
  if (menu_positon == 2) {
    encoder1.tick();
  }

  if (menu_positon == 3) {
    encoder2.tick();
  }

  pollButtons(); // Poll your buttons every loop.

  int reed_contact = digitalRead(reed_contact_pin);

  // Timer for update interval (1 second)
  if (millis() >= (previousTime)) {
    previousTime = previousTime + timeout;
    tick = 1;
  } else {
    tick = 0;
  }

  if (reed_contact == LOW) {
    time_countdown = set_time;
  } else {
    if (time_countdown > 0 && target_reached_once == 1) {
      unsigned long currentTime = millis();
      static unsigned long lastCountdownTime = 0;
      if (currentTime - lastCountdownTime >= 1000) { // Decrease every second
        time_countdown--;
        lastCountdownTime = currentTime;
      }
    }
  }

  // Expire one-shot buzzer if time elapsed
  if (buzzer_active && (millis() >= buzzer_off_time)) {
    buzzer_active = 0;
  }

  // Buzzer priority: one-shot target-beep overrides countdown beep
  if (buzzer_active) {
    digitalWrite(buzzer_pin, HIGH);
  } else if (time_countdown == 1) {
    digitalWrite(buzzer_pin, HIGH);
  } else {
    digitalWrite(buzzer_pin, LOW);
  }

  // If it's time to update
  if (tick) {
    // read temperature
    temp = therm1.analog2temp();

    // Draw UI using paged API to avoid allocating a full framebuffer
    u8g2.firstPage();
    do {

      if (temp < 1){
        heat_on = 0;
        run_state = 0;
        digitalWrite(relay_pin, LOW);
        digitalWrite(led_green_pin, LOW);
        
        if (((previousTime / 500) % 2) == 0) {
          digitalWrite(led_blue_pin, HIGH);
          digitalWrite(led_red_pin, LOW);
        }else{
          digitalWrite(led_blue_pin, LOW);
          digitalWrite(led_red_pin, HIGH);
        }
          

        if (((previousTime / 500) % 2) == 0) {
          u8g2.setFont(u8g2_font_streamline_interface_essential_circle_triangle_t);
          u8g2.drawGlyph(55, 25, 0x33);
        }
          u8g2.setFont(u8g2_font_neuecraft_tr);
          u8g2.setCursor(10, 50);
          u8g2.print(F("No temp sensor!"));

      }else{

      // Icony
      // Thermometer
      u8g2.setFont(u8g2_font_streamline_weather_t);
      u8g2.drawGlyph(4, 28, 0x36);

      // Clock
      u8g2.setFont(u8g2_font_streamline_interface_essential_circle_triangle_t);
      u8g2.drawGlyph(0, 60, 0x36);

      // Blinking heater icon when ON
      // if (heat_on == 1) {
      //   if (((previousTime / 1000) % 2) == 0) {
      //     u8g2.setFont(u8g2_font_streamline_interface_essential_circle_triangle_t);
      //     u8g2.drawGlyph(105, 40, 0x33);
      //   }
      // }

      // Font text
      u8g2.setFont(u8g2_font_neuecraft_tr);

      // print temperature
      u8g2.setCursor(30, 15);
      u8g2.print(F("Temp: "));
      u8g2.print(temp, 1);
      u8g2.print(F(" °C"));



      if (run_state == 1) {
        // Turn heater OFF
        if (temp >= (target_temp + hysteresis) and heat_on == 1) {
          digitalWrite(relay_pin, LOW);
          heat_on = 0;
        }

        // Turn heater ON
        if (temp <= (target_temp - hysteresis) and run_state == 1) {
          digitalWrite(relay_pin, HIGH);
          heat_on = 1;
        }
      }else{
        // Ensure heater is OFF in stop mode
        digitalWrite(relay_pin, LOW);
        heat_on = 0;
        time_countdown = set_time;
      }

      if (run_state == 1 and heat_on == 1) {
        digitalWrite(led_red_pin, HIGH);
        digitalWrite(led_green_pin, LOW);
        digitalWrite(led_blue_pin, LOW);
      }else if (run_state == 1 and heat_on == 0) {
        digitalWrite(led_red_pin, LOW);
        digitalWrite(led_green_pin, HIGH);
        digitalWrite(led_blue_pin, LOW);
      }else{
        digitalWrite(led_red_pin, LOW);
        digitalWrite(led_green_pin, LOW);
        digitalWrite(led_blue_pin, HIGH);
      }


      // If target temperature is reached for the first time, beep buzzer for 1 second
      if (!target_reached_once && (temp >= target_temp)) {
        target_reached_once = 1;
        buzzer_active = 1;
        buzzer_off_time = millis() + 1000; // 1 second
        digitalWrite(buzzer_pin, HIGH);     // immediate start of beep
      }

      u8g2.setCursor(30, 30);
      u8g2.print(F("Set: "));

      if (menu_positon == 2) {
        if (((previousTime / 500) % 2) == 0) {
          u8g2.print(target_temp);
          u8g2.print(F(" °C"));
        }
      } else {
        u8g2.print(target_temp);
        u8g2.print(F(" °C"));
      }

      u8g2.setCursor(30, 55);
      u8g2.print(F("Time: "));

      if (menu_positon == 3) {
        if (((previousTime / 500) % 2) == 0) {
          u8g2.print(set_time);
          u8g2.print(F(" s"));
        } else {
          u8g2.print(F("    "));
        }
      } else {
        u8g2.print(time_countdown);
        u8g2.print(F(" s"));
      }
     } // end temp==0 check
    } while (u8g2.nextPage());
  }

  // Rotary Encoder menu - temperature setting
  if (menu_positon == 2) {
    // get the current physical position and calc the logical position
    int newPos = encoder1.getPosition() * rotary_steps;

    if (newPos < min_temp) {
      encoder1.setPosition(min_temp / rotary_steps);
      newPos = min_temp;
    } else if (newPos > max_temp) {
      encoder1.setPosition(max_temp / rotary_steps);
      newPos = max_temp;
    }

    if (lastPos1 != newPos) {
      lastPos1 = newPos;
      target_temp = lastPos1;
    }
  }

  // Rotary Encoder menu - time setting
  if (menu_positon == 3) {
    // get the current physical position and calc the logical position
    int newPos2 = encoder2.getPosition() * rotary_steps;

    if (newPos2 < min_time) {
      encoder2.setPosition(min_time / rotary_steps);
      newPos2 = min_time;
    } else if (newPos2 > max_time) {
      encoder2.setPosition(max_time / rotary_steps);
      newPos2 = max_time;
    }

    if (lastPos2 != newPos2) {
      lastPos2 = newPos2;
      set_time = lastPos2;
    }
  }

  // Periodic global debug message every 5 seconds
#if DEBUG
  static unsigned long lastDebugTime = 0;
  const unsigned long debugInterval = 5000UL; // ms
  if (millis() - lastDebugTime >= debugInterval) {
    lastDebugTime = millis();
    Serial.println(F("=========== DEBUG ==========="));
    Serial.print(F("Curent Temp: "));
    Serial.println(temp, 1);
    Serial.print(F("Set Temp: "));
    Serial.println(target_temp);
    Serial.print(F("Heater: "));
    Serial.println(heat_on);
    Serial.print(F("Time Set: "));
    Serial.println(set_time);
    Serial.print(F("Time Left: "));
    Serial.println(time_countdown);
    Serial.print(F("Reed contact: "));
    Serial.println(reed_contact);
    Serial.print(F("Menu: "));
    Serial.println(menu_positon);
    Serial.println(F("==========================="));
    Serial.println("");
  }
#endif
}
// End