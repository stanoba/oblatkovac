#pragma once

// Shared configuration for the Nano and ESP32 firmware.
#define WAFFLE_DEFAULT_TEMPERATURE_C 134 //134
#define WAFFLE_MIN_TEMPERATURE_C 100     //100
#define WAFFLE_MAX_TEMPERATURE_C 180     //180

#define WAFFLE_DEFAULT_BAKE_TIME_S 30   //30
#define WAFFLE_MIN_BAKE_TIME_S 10       //10
#define WAFFLE_MAX_BAKE_TIME_S 90       //90

#define WAFFLE_ENCODER_EDIT_TIMEOUT_MS 10000UL  // Exit edit mode after 10 s idle.
#define WAFFLE_BUZZER_BEEP_MS 1000UL
#define WAFFLE_DEBUG 0  // Set to 1 to print verbose UART frame diagnostics.
