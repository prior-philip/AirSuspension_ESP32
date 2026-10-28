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
extern void toggleWiFi();          // defined in the .ino (shared with the menu)
extern bool wifiEnabled;
extern bool          wifiAutoStarted;      // .ino: power-on auto-start active
extern unsigned long wifiAutoStartedAt;    // .ino: its start time (auto-off timer)
bool saveWifiPassword(const char* pw);     // WebInterface.h
bool isValidWifiPassword(const char* pw);  // WebInterface.h

struct SerialControlState {
  bool valveActive;
  unsigned long pulseStartTime;
};

// PROGMEM removed — ESP32-S3 has 512KB RAM, string literals live in flash anyway
const char HELPSTR[] = "Serial: 7=L↑ 8=↑↑ 9=R↑ 4=-10% 6=+10% 1=L↓ 2=↓↓ 3=R↓ c=Cal w=WiFi on/off p=WiFi password";

extern SerialControlState serialControl;

// ============================================================================
// WIFI PASSWORD INPUT ('p')
// ============================================================================
// "p" starts a non-blocking line input: the next line (Enter) becomes the new
// AP password.  Also works in one go: "p MyNewPass123".  Line endings right
// after "p" are skipped; input shorter than 8 chars is rejected (= cancel).
// No input for PW_INPUT_TIMEOUT_MS cancels.  The loop, regulation and the
// watchdog keep running while typing.
static const unsigned long PW_INPUT_TIMEOUT_MS = 60000;
static bool          pwInputActive = false;
static char          pwBuf[65];
static byte          pwLen         = 0;
static bool          pwTooLong     = false;
static bool          pwSepSkipped  = false;
static unsigned long pwLastInput   = 0;

static void startPasswordInput(unsigned long now) {
  pwInputActive = true; pwLen = 0; pwTooLong = false; pwSepSkipped = false;
  pwLastInput = now;
  Serial.println(F("New WiFi password (8-63 chars, Enter = save; 60 s timeout):"));
}

static void finishPasswordInput() {
  pwInputActive = false;
  pwBuf[pwLen] = 0;
  if (pwTooLong || !isValidWifiPassword(pwBuf)) {
    Serial.println(F("Rejected: 8-63 printable characters required — password unchanged"));
    return;
  }
  if (!saveWifiPassword(pwBuf)) {
    Serial.println(F("Error: could not store password — unchanged"));
    return;
  }
  Serial.print(F("WiFi password saved: ")); Serial.println(pwBuf);
  if (wifiEnabled) {
    Serial.println(F("[WiFi] Restarting access point — connected devices must reconnect"));
    // A password change is not a manual WiFi switch: keep a running
    // power-on auto-off timer (toggleWiFi() would otherwise cancel it).
    bool          wasAuto = wifiAutoStarted;
    unsigned long autoAt  = wifiAutoStartedAt;
    toggleWiFi();   // off
    toggleWiFi();   // on, with the new password
    wifiAutoStarted   = wasAuto;
    wifiAutoStartedAt = autoAt;
  } else {
    Serial.println(F("Applies at the next WiFi start"));
  }
}

static void servicePasswordInput(unsigned long now) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    pwLastInput = now;
    if (c == '\r' || c == '\n') {
      if (pwLen == 0 && !pwTooLong) continue;          // skip line ending after 'p'
      finishPasswordInput();
      return;
    }
    if (pwLen == 0 && c == ' ' && !pwSepSkipped) {      // "p <password>" one-liner
      pwSepSkipped = true;
      continue;
    }
    if (pwLen < 64) pwBuf[pwLen++] = c; else pwTooLong = true;
  }
  if (now - pwLastInput >= PW_INPUT_TIMEOUT_MS) {
    pwInputActive = false;
    Serial.println(F("Password input timed out — unchanged"));
  }
}

// ============================================================================
// SERIAL VALVE CONTROL
// ============================================================================

void handleSerialControl(unsigned long currentMillis) {
  if (pwInputActive) {                 // collecting a new WiFi password
    servicePasswordInput(currentMillis);
    powerSave.lastActivityTime = currentMillis;
    return;
  }
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

    case 'p':
    case 'P':
      startPasswordInput(currentMillis);
      validCommand = false;
      return;

    case 'w':
    case 'W':
      toggleWiFi();             // same path as Menu → WiFi
      validCommand = false;
      break;

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