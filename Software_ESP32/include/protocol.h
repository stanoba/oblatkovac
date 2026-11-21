#pragma once

#include <stdint.h>

namespace bridge {

constexpr uint32_t kBaudRate = 9600;

// Selected UART pins on the ESP32-C3 Super Mini.
constexpr uint8_t kEspRxPin = 1;
constexpr uint8_t kEspTxPin = 3;

// UART pins on the Arduino Nano, connected through the level shifter.
constexpr uint8_t kNanoRxPin = 11;
constexpr uint8_t kNanoTxPin = 12;

constexpr uint8_t kMaxLineLength = 96;

}  // namespace bridge
