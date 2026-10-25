// SerialControl.h - Serial Control for Manual Valve Operation (ESP32-S3)
#ifndef SERIAL_CONTROL_H
#define SERIAL_CONTROL_H

#include <Arduino.h>
#include "Config.h"
#include "Valves.h"
#include "PowerManagement.h"
#include "Regulation.h"
#include "EEPROM_Manager.h"

extern PowerSaveState powerSave;
extern byte targetPercent;
extern void notifyUserChange(unsigned long);
extern bool calibrationMode;
extern void enterCalibrationMode();

struct SerialControlState {
  bool valveActive;
  unsigned long pulseStartTime;
};

// PROGMEM removed — ESP32-S3 has 512KB RAM, string literals live in flash anyway
const char HELPSTR[] = "Serial: 7=L↑ 8=↑↑ 9=R↑ 4=-10% 6=+10% 1=L↓ 2=↓↓ 3=R↓ c=Cal";

extern SerialControlState serialControl;

// ============================================================================
// SERIAL VALVE CONTROL
// ============================================================================

void handleSerialControl(unsigned long currentMillis) {
  if (!Serial.available()) {
    if (serialControl.valveActive) {
      if (currentMillis - serialControl.pulseStartTime >= VALVE_PULSE_TIME) {
        allValvesOff();
        serialControl.valveActive = false;
        Serial.println(F("Pulse done"));
      }
    }
    return;
  }

  char cmd = Serial.read();

  // Ignore CR/LF/whitespace the Serial Monitor appends to a line.  Previously
  // the first read took the command, then `while(available()) read()` blind-
  // flushed the rest — but at USB-CDC speeds the trailing '\n' often arrives a
  // moment later, on its own, and was then read as a bogus command (hitting
  // default:), which also swallowed the *next* real keypress.  That race is
  // why keys needed several attempts.  We now skip terminators explicitly and
  // do NOT blind-flush: any genuine following character is handled on the next
  // loop() pass.  This check is BEFORE the valveActive return so a keypress
  // during a 500 ms pulse isn't lost either.
  if (cmd == '\r' || cmd == '\n' || cmd == ' ' || cmd == '\t') {
    return;
  }

  resetWatchdog();
  powerSave.lastActivityTime = currentMillis;

  if (serialControl.valveActive) return;

  bool validCommand = true;
  allValvesOff();

  switch (cmd) {
    case '8':
      digitalWrite(INFLATE_LEFT_PIN,  HIGH);
      digitalWrite(INFLATE_RIGHT_PIN, HIGH);
      Serial.println(F("UP"));
      break;
    case '2':
      digitalWrite(DEFLATE_LEFT_PIN,  HIGH);
      digitalWrite(DEFLATE_RIGHT_PIN, HIGH);
      Serial.println(F("DN"));
      break;
    case '7':
      digitalWrite(INFLATE_LEFT_PIN, HIGH);
      Serial.println(F("L UP"));
      break;
    case '9':
      digitalWrite(INFLATE_RIGHT_PIN, HIGH);
      Serial.println(F("R UP"));
      break;
    case '1':
      digitalWrite(DEFLATE_LEFT_PIN, HIGH);
      Serial.println(F("L DN"));
      break;
    case '3':
      digitalWrite(DEFLATE_RIGHT_PIN, HIGH);
      Serial.println(F("R DN"));
      break;
    case '4':
      if (targetPercent > 0) {
        targetPercent = max(targetPercent - STEP_SIZE, 0);
        saveSetpoint();
        resetRegulationState();
        notifyUserChange(currentMillis);
        Serial.print(F("SET: "));
        Serial.print(targetPercent);
        Serial.println(F("%"));
      }
      validCommand = false;
      break;
    case '6':
      if (targetPercent < 100) {
        targetPercent = min(targetPercent + STEP_SIZE, 100);
        saveSetpoint();
        resetRegulationState();
        notifyUserChange(currentMillis);
        Serial.print(F("SET: "));
        Serial.print(targetPercent);
        Serial.println(F("%"));
      }
      validCommand = false;
      break;
    case 'c':
    case 'C':
      allValvesOff();
      serialControl.valveActive = false;
      Serial.println(F("-> Calibration mode (navigate with physical buttons)"));
      enterCalibrationMode();
      calibrationMode = true;
      return;

    case '5':
      allValvesOff();
      Serial.println(F("REBOOT"));
      validCommand = false;
      reboot();
      break;
    case '?':
    case 'h':
    case 'H':
      Serial.println(HELPSTR);
      validCommand = false;
      break;
    default:
      validCommand = false;
      break;
  }

  if (validCommand) {
    serialControl.valveActive    = true;
    serialControl.pulseStartTime = currentMillis;
    digitalWrite(LED_PIN, HIGH);
  }
}

void printSerialControlInfo() {
  Serial.println(HELPSTR);
}

#endif // SERIAL_CONTROL_H