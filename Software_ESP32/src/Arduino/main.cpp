#include <Arduino.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <thermistor.h>
#include "pins.h"
#include "config.h"
#include "protocol.h"
#include "firmware_version.h"

SoftwareSerial espLink(bridge::kNanoRxPin, bridge::kNanoTxPin);
U8G2_SH1106_128X64_NONAME_1_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, i2c_scl_pin, i2c_sda_pin);
// Sensor 0 corresponds to TEMP_SENSOR_0 (table 80) in Configuration.h and index 0 in heater_ttbl_map
thermistor therm1(thermistor_pin, 0);

static float temperatureC = NAN;
static int targetC = default_temp;
static int durationS = default_time;
static bool runRequested = false;
static bool runInterlockLatch = false;
static bool heaterOn = false;
static bool lidClosed = false;
static bool linkUp = false;
static bool faultSensor = false;
static bool linkLogInitialized = false, lastLoggedLinkUp = false;
static bool faultLogInitialized = false, lastLoggedFault = false;
static bool lastLoggedHeater = false;
static uint32_t lastHeartbeat = 0;
static uint32_t lastSample = 0;
static uint32_t lastFallbackStatus = 0;
static uint32_t txSeq = 0;
static uint32_t lastCommandSeq = 0;
static bool haveCommandSeq = false;
static char rxLine[bridge::kMaxLineLength];
static uint8_t rxLen = 0;
static bool buttonStable = HIGH;
static bool buttonRaw = HIGH;
static uint32_t buttonChangedAt = 0;
static bool prevLidClosed = false;
static uint32_t lidTransitionAt = 0;
static uint32_t openStartedAt = 0;
static uint32_t lidOpenSeconds = 0;
static uint32_t displayCount = 0;
static uint32_t displayRemaining = 0;
static uint32_t displayOvertime = 0;
static uint8_t displayMode = 0;
static uint8_t encoderState = 0;
static int8_t encoderSteps = 0;
static bool encoderButtonStable = HIGH;
static bool encoderButtonRaw = HIGH;
static uint32_t encoderButtonChangedAt = 0;
static uint32_t lastDisplay = 0;
static bool displayNeedsUpdate = true;
static const int8_t encoderTable[16] = {0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};

// Encoder edit mode: 0 = idle, 1 = edit temperature, 2 = edit time
static uint8_t encoderEditMode = 0;
static uint32_t encoderIdleMs = 0;   // millis() when encoderEditMode was last changed
static bool blinkState = false;
static uint32_t lastBlinkMs = 0;

// Synchronized event queue: events are sent immediately following command reception
// to avoid bus contention on half-duplex SoftwareSerial.
static int8_t pendingEncSteps = 0;
static bool pendingMode = false;
static bool pendingStart = false;
static bool pendingStop = false;

static uint8_t crc8(const char *s, size_t n) {
  uint8_t c = 0;
  while (n--) { c ^= (uint8_t)*s++; for (uint8_t b=0;b<8;b++) c = (c & 0x80) ? (uint8_t)((c<<1)^0x07) : (uint8_t)(c<<1); }
  return c;
}

static bool validFrame(char *line) {
  char *star = strrchr(line, '*');
  if (!star || strlen(star) != 3) {
    Serial.print(F("[Nano Format ERR] star="));
    Serial.println(star ? star : "null");
    return false;
  }
  uint8_t got = (uint8_t)strtoul(star + 1, nullptr, 16);
  uint8_t calc = crc8(line, star - line);
  if (calc != got) {
    Serial.print(F("[Nano CRC ERR] calc=0x")); Serial.print(calc, HEX);
    Serial.print(F(" got=0x")); Serial.print(got, HEX);
    Serial.print(F(" line=")); Serial.println(line);
    return false;
  }
  *star = '\0';
  return true;
}

static void sendFrame(const char *payload) {
  espLink.print(payload); espLink.print('*');
  char hex[3]; snprintf(hex, sizeof(hex), "%02X", crc8(payload, strlen(payload)));
  espLink.println(hex);
}

static void sendEvent(const char *event, const char *value = nullptr) {
  char payload[40];
  if (value) snprintf(payload, sizeof(payload), "EV,%lu,%s,%s", (unsigned long)++txSeq, event, value);
  else snprintf(payload, sizeof(payload), "EV,%lu,%s", (unsigned long)++txSeq, event);
  sendFrame(payload);
}

static void sendAck(const char *seq, bool accepted) {
  char payload[32];
  snprintf(payload, sizeof(payload), "ACK,%s,%s", seq, accepted ? "OK" : "ERR");
  sendFrame(payload);
}

static void sendTelemetry() {
  char out[bridge::kMaxLineLength];
  snprintf(out, sizeof(out), "IO,%lu,%.1f,%u,%u,%u,%u,%lu,%u,%d,%d",
    (unsigned long)++txSeq,
    isnan(temperatureC) || faultSensor ? -1.0f : temperatureC,
    lidClosed ? 1 : 0,
    heaterOn ? 1 : 0,
    faultSensor ? 1 : 0,
    0,
    (unsigned long)lidOpenSeconds,
    linkUp ? 1 : 0,
    targetC,
    durationS);
  sendFrame(out);
  lidOpenSeconds = 0;
}

static void flushPendingEvents() {
  if (pendingStop) {
    sendEvent("STOP");
    pendingStop = false;
  }
  if (pendingStart) {
    sendEvent("START");
    pendingStart = false;
  }
  // pendingMode is now handled locally on Nano (encoder edit mode cycles 0->1->2->0)
  pendingMode = false;
  // Only send accumulated encoder steps when back in idle mode (not while still editing)
  if (encoderEditMode == 0 && pendingEncSteps != 0) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", (int)pendingEncSteps);
    sendEvent("ENC", buf);
    pendingEncSteps = 0;
  }
}

static void rejectCommand(const char *seq, const char *reason, const char *command = nullptr) {
  Serial.print(F("[Nano] Rejected ESP command seq=")); Serial.print(seq);
  Serial.print(F(" reason=")); Serial.print(reason);
  if (command) { Serial.print(F(" command=")); Serial.print(command); }
  Serial.print(F(" lastSeq=")); Serial.print(lastCommandSeq);
  Serial.print(F(" target=")); Serial.print(targetC);
  Serial.print(F(" time=")); Serial.println(durationS);
  sendAck(seq, false);
}

static void handleFrame(char *line) {
  if (!validFrame(line)) return;
  char *save = nullptr;
  char *type = strtok_r(line, ",", &save);
  char *seq = strtok_r(nullptr, ",", &save);
  if (!type || !seq || strcmp(type, "CMD") != 0) return;
  uint32_t incomingSeq = strtoul(seq, nullptr, 10);
  char *cmd = strtok_r(nullptr, ",", &save);
  if (!cmd) return;
  if (strcmp(cmd, "HELLO") == 0) {
    Serial.print(F("[Nano] Accepted HELLO seq=")); Serial.println(incomingSeq);
    lastCommandSeq = incomingSeq; haveCommandSeq = true;
    lastHeartbeat = millis(); linkUp = true; runRequested = false; runInterlockLatch=true; heaterOn = false;
    digitalWrite(relay_pin, LOW);
    sendAck(seq, true);
    flushPendingEvents();
    sendTelemetry();
    displayNeedsUpdate = true;
    return;
  }
  if (haveCommandSeq && (int32_t)(incomingSeq - lastCommandSeq) <= 0) {
    // If ESP rebooted or sequence wrapped by a large jump, resynchronize
    if ((int32_t)(lastCommandSeq - incomingSeq) > 50) {
      lastCommandSeq = incomingSeq;
    } else {
      rejectCommand(seq, "sequence_not_newer", cmd);
      return;
    }
  }
  if (strcmp(cmd, "STATE") != 0) { rejectCommand(seq, "unknown_command", cmd); return; }
  char *t = strtok_r(nullptr, ",", &save);
  char *d = strtok_r(nullptr, ",", &save);
  char *run = strtok_r(nullptr, ",", &save);
  char *count = strtok_r(nullptr, ",", &save);
  char *remaining = strtok_r(nullptr, ",", &save);
  char *overtime = strtok_r(nullptr, ",", &save);
  char *mode = strtok_r(nullptr, ",", &save);
  if (!(t && d && run && count && remaining && overtime && mode)) { rejectCommand(seq, "missing_state_field"); return; }
  int newTarget = atoi(t), newDuration = atoi(d);
  if (newTarget < min_temp || newTarget > max_temp || newDuration < min_time || newDuration > max_time) {
    Serial.print(F("[Nano] Rejected state values: target=")); Serial.print(newTarget);
    Serial.print(F(" C time=")); Serial.print(newDuration); Serial.println(F(" s"));
    rejectCommand(seq, "value_out_of_range"); return;
  }
  lastCommandSeq = incomingSeq; haveCommandSeq = true; lastHeartbeat = millis(); linkUp = true;
  Serial.print(F("[Nano] Accepted STATE seq=")); Serial.println(incomingSeq);
  // Only overwrite target/duration from ESP32 when encoder is idle (not while user is editing)
  if (encoderEditMode == 0) {
    targetC = newTarget; durationS = newDuration;
  }
  bool requested = atoi(run) != 0;
  if (!requested) runInterlockLatch = false;
  runRequested = requested && !runInterlockLatch;
  displayCount = strtoul(count, nullptr, 10);
  displayRemaining = strtoul(remaining, nullptr, 10);
  displayOvertime = strtoul(overtime, nullptr, 10);
  displayMode = (uint8_t)atoi(mode);

  // Synchronous response: ACK -> pending events -> IO telemetry
  sendAck(seq, true);
  flushPendingEvents();
  sendTelemetry();
  displayNeedsUpdate = true;
}

static void pollSerial() {
  while (espLink.available()) {
    char c = (char)espLink.read();
    if (c == '\n' || c == '\r') {
      if (rxLen) {
        rxLine[rxLen] = 0;
        Serial.print(F("[Nano RX] ")); Serial.println(rxLine);
        handleFrame(rxLine);
        rxLen = 0;
      }
    }
    else if (rxLen < sizeof(rxLine) - 1) rxLine[rxLen++] = c;
    else {
      Serial.println(F("[Nano RX overflow]"));
      rxLen = 0;
    }
  }
}

static void pollEncoder(uint32_t now) {
  uint8_t next = (digitalRead(encoder_clk_pin) ? 2 : 0) | (digitalRead(encoder_dt_pin) ? 1 : 0);
  encoderSteps += encoderTable[(encoderState << 2) | next];
  encoderState = next;
  if (encoderSteps >= 4) {
    if (encoderEditMode == 1) {
      // Edit temperature locally; send delta to ESP32
      targetC = constrain(targetC + 1, min_temp, max_temp);
      pendingEncSteps += 1;
      displayNeedsUpdate = true;
    } else if (encoderEditMode == 2) {
      durationS = constrain(durationS + 1, min_time, max_time);
      pendingEncSteps += 1;
      displayNeedsUpdate = true;
    }
    encoderSteps = 0;
  } else if (encoderSteps <= -4) {
    if (encoderEditMode == 1) {
      targetC = constrain(targetC - 1, min_temp, max_temp);
      pendingEncSteps -= 1;
      displayNeedsUpdate = true;
    } else if (encoderEditMode == 2) {
      durationS = constrain(durationS - 1, min_time, max_time);
      pendingEncSteps -= 1;
      displayNeedsUpdate = true;
    }
    encoderSteps = 0;
  }

  bool raw = digitalRead(encoder_sw_pin);
  if (raw != encoderButtonRaw) { encoderButtonRaw = raw; encoderButtonChangedAt = now; }
  if (now - encoderButtonChangedAt >= 30 && raw != encoderButtonStable) {
    encoderButtonStable = raw;
    if (encoderButtonStable == LOW) {
      // Cycle: 0 (idle) -> 1 (temp) -> 2 (time) -> 0 (idle)
      encoderEditMode = (encoderEditMode + 1) % 3;
      encoderIdleMs = now;
      blinkState = true;
      lastBlinkMs = now;
      displayNeedsUpdate = true;
    }
  }

  // Auto-return to idle after 5 seconds of no button press
  if (encoderEditMode != 0 && now - encoderIdleMs >= 5000) {
    encoderEditMode = 0;
    displayNeedsUpdate = true;
  }

  // Blink timer: 500 ms period
  if (encoderEditMode != 0 && now - lastBlinkMs >= 500) {
    blinkState = !blinkState;
    lastBlinkMs = now;
    displayNeedsUpdate = true;
  }
}

static void renderDisplay() {
  oled.firstPage(); do {
    oled.setFont(u8g2_font_6x12_tf);
    oled.setCursor(0, 12); oled.print(linkUp ? "ESP online" : "ESP offline - heater OFF");
    oled.setCursor(0, 27); oled.print("Temp: ");
    if (isnan(temperatureC)) oled.print("ERR");
    else { oled.print(temperatureC,1); oled.print(" C"); }

    // Line 3: Set temperature - blinks when encoderEditMode == 1
    oled.setCursor(0, 42);
    oled.print("Set: ");
    if (encoderEditMode == 1 && !blinkState) {
      oled.print("   ");  // blank during blink-off phase
    } else {
      oled.print(targetC); oled.print(" C");
    }
    oled.print("  "); oled.print(heaterOn ? "HEAT ON" : "OFF");

    // Line 4: Time / countdown - blinks when encoderEditMode == 2
    oled.setCursor(0, 57);
    if (displayMode == 2) {
      oled.print("+ "); oled.print(displayOvertime); oled.print(" s");
    } else if (displayMode == 1) {
      oled.print("Time: "); oled.print(displayRemaining); oled.print(" s");
    } else {
      oled.print("Set: ");
      if (encoderEditMode == 2 && !blinkState) {
        oled.print("   ");  // blank during blink-off phase
      } else {
        oled.print(durationS); oled.print(" s");
      }
    }
    oled.print("  #"); oled.print(displayCount);
  } while (oled.nextPage());
}

void setup() {
  Serial.begin(115200);
  Serial.println(F("[Nano] Booting I/O and thermostat firmware"));
  Serial.print(F("[Nano] Firmware v")); Serial.print(FIRMWARE_VERSION); Serial.print(' '); Serial.print(FIRMWARE_BUILD_DATE); Serial.print(' '); Serial.println(FIRMWARE_BUILD_TIME);
  pinMode(relay_pin, OUTPUT); pinMode(buzzer_pin, OUTPUT);
  pinMode(led_green_pin, OUTPUT); pinMode(led_red_pin, OUTPUT); pinMode(led_blue_pin, OUTPUT);
  pinMode(encoder_sw_pin, INPUT); pinMode(start_stop_button_pin, INPUT);
  pinMode(encoder_dt_pin, INPUT); pinMode(encoder_clk_pin, INPUT);
  pinMode(reed_contact_pin, INPUT);
  digitalWrite(relay_pin, LOW); digitalWrite(buzzer_pin, LOW);
  if (EEPROM.read(0) >= min_temp && EEPROM.read(0) <= max_temp) targetC=EEPROM.read(0);
  if (EEPROM.read(1) >= min_time && EEPROM.read(1) <= max_time) durationS=EEPROM.read(1);
  Serial.print(F("[Nano] Legacy settings: target=")); Serial.print(targetC);
  Serial.print(F(" C, duration=")); Serial.print(durationS); Serial.println(F(" s"));
  oled.begin();
  oled.setBusClock(400000); // Set U8g2 I2C bus clock to 400kHz
  espLink.begin(bridge::kBaudRate);
  encoderState=(digitalRead(encoder_clk_pin)?2:0)|(digitalRead(encoder_dt_pin)?1:0);
  encoderButtonStable=encoderButtonRaw=digitalRead(encoder_sw_pin);
  buttonStable=buttonRaw=digitalRead(start_stop_button_pin);
  lastHeartbeat=0;
  lastFallbackStatus=millis();
}

void loop() {
  pollSerial();
  const uint32_t now=millis();
  pollEncoder(now);

  // The existing PCB uses LOW for the open/reset state and HIGH for closed.
  bool rawClosed = digitalRead(reed_contact_pin) == HIGH;
  if (rawClosed != prevLidClosed && now-lidTransitionAt >= 50) {
    prevLidClosed=rawClosed; lidClosed=rawClosed; lidTransitionAt=now;
    if (!lidClosed) openStartedAt=now;
    else if (openStartedAt) { lidOpenSeconds=(now-openStartedAt)/1000; openStartedAt=0; }
  } else if (rawClosed == prevLidClosed && now-lidTransitionAt >= 50) lidClosed=rawClosed;

  bool rawButton = digitalRead(start_stop_button_pin);
  if (rawButton != buttonRaw) { buttonRaw=rawButton; buttonChangedAt=now; }
  if (now-buttonChangedAt>=30 && buttonRaw!=buttonStable) {
    buttonStable=rawButton;
    if (buttonStable==LOW) {
      if (runRequested || heaterOn) {
        Serial.println(F("[Nano] Local STOP pressed; relay disabled immediately"));
        runRequested=false; runInterlockLatch=true; heaterOn=false;
        digitalWrite(relay_pin,LOW);
        pendingStop=true;
        sendEvent("STOP"); // Send immediately so ESP reacts at once
      }
      else if (linkUp && now-lastHeartbeat < 2000) {
        Serial.println(F("[Nano] Local START requested"));
        pendingStart=true;
      }
    }
  }

  linkUp = haveCommandSeq && (now - lastHeartbeat < 2000);
  if (!linkUp) {
    runRequested = false;
    runInterlockLatch = true;
    heaterOn = false;
    digitalWrite(relay_pin, LOW);
  }
  if (now-lastSample >= 500) {
    lastSample=now;
    temperatureC=therm1.analog2temp();
    faultSensor = isnan(temperatureC) || temperatureC < 1.0f || temperatureC > 250.0f;
    if (!linkUp || faultSensor || !runRequested) heaterOn=false;
    else if (temperatureC >= targetC + hysteresis) heaterOn=false;
    else if (temperatureC <= targetC - hysteresis) heaterOn=true;
    digitalWrite(relay_pin, heaterOn ? HIGH : LOW);
    digitalWrite(led_red_pin, heaterOn ? HIGH : LOW);
    digitalWrite(led_green_pin, runRequested && !heaterOn ? HIGH : LOW);
    digitalWrite(led_blue_pin, (!linkUp || faultSensor) ? HIGH : (!runRequested ? HIGH : LOW));
  }
  if (!linkLogInitialized || linkUp != lastLoggedLinkUp) {
    Serial.println(linkUp ? F("[Nano] ESP UART link online") : F("[Nano] ESP UART link offline; heater forced OFF"));
    lastLoggedLinkUp=linkUp; linkLogInitialized=true;
  }
  if (!faultLogInitialized || faultSensor != lastLoggedFault) {
    if (faultSensor) {
      Serial.print(F("[Nano] ERROR: thermistor fault (temp=")); Serial.print(temperatureC);
      Serial.print(F(" C, raw ADC=")); Serial.print(analogRead(thermistor_pin)); Serial.println(F(")"));
    } else {
      Serial.print(F("[Nano] Thermistor reading valid: ")); Serial.print(temperatureC); Serial.println(F(" C"));
    }
    lastLoggedFault=faultSensor; faultLogInitialized=true;
  }
  if (heaterOn != lastLoggedHeater) {
    Serial.println(heaterOn ? F("[Nano] Heater relay ON") : F("[Nano] Heater relay OFF"));
    lastLoggedHeater=heaterOn;
  }

  // Fallback telemetry sent only when link is down, to assist resynchronization
  if (!linkUp && now-lastFallbackStatus >= 1000) {
    lastFallbackStatus=now;
    sendTelemetry();
  }

  // Render display during the safe quiet window after serial frame processing
  if (displayNeedsUpdate && now-lastDisplay >= 200) {
    lastDisplay=now;
    displayNeedsUpdate=false;
    renderDisplay();
  }
}
