// Display.h - OLED Display Functions (ESP32-S3)
#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "Config.h"
#include "Types.h"
#include <esp_task_wdt.h>   // top of Display.h


extern SensorData       sensors;
extern CalibrationData  calibration;
extern RegulationState  regulation;
extern PulsedRegulation pulse;
extern BagInitState    bagInit;
extern AntiHuntingState antiHunt;
extern PowerSaveState   powerSave;
extern ManualControlState manualControl;
extern byte             targetPercent;
extern U8G2_SH1107_SEEED_128X128_F_HW_I2C u8g2;

// ============================================================================
// I2C BUS RECOVERY
// ============================================================================

// Toggle SCL to release a stuck slave holding SDA low, then issue a STOP
// condition.  Returns true if both lines idle HIGH afterwards (bus usable),
// false if SDA or SCL is still held LOW (wiring short, missing pull-up,
// hung/damaged display) — callers must then skip the display entirely.
bool recoverI2CBus() {
  // NOTE: Do NOT call Wire.end() here — on ESP32-S3 it corrupts the GPIO
  // mux assignment and prevents Wire.begin() from properly reclaiming the
  // pins, leaving the I2C peripheral non-functional.
  // Instead, temporarily override pin function via the GPIO matrix
  // (pinMode calls take precedence over the I2C peripheral output).

  // Sense bus state with internal pull-ups.
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  delayMicroseconds(20);

  if (digitalRead(I2C_SDA_PIN) == LOW) {
    // Slave holding SDA low — clock up to 18 SCL pulses until it releases.
    // 18 covers worst case: 9-bit byte x 2 (mid second byte of a command).
    pinMode(I2C_SCL_PIN, OUTPUT);
    for (byte i = 0; i < 18; i++) {
      digitalWrite(I2C_SCL_PIN, HIGH); delayMicroseconds(10);
      digitalWrite(I2C_SCL_PIN, LOW);  delayMicroseconds(10);
      if (digitalRead(I2C_SDA_PIN) == HIGH) break;
    }
    pinMode(I2C_SCL_PIN, INPUT_PULLUP);
    delayMicroseconds(20);
  }

  // Issue a clean STOP condition.
  pinMode(I2C_SDA_PIN, OUTPUT);
  pinMode(I2C_SCL_PIN, OUTPUT);
  digitalWrite(I2C_SDA_PIN, LOW);  delayMicroseconds(10);
  digitalWrite(I2C_SCL_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(I2C_SDA_PIN, HIGH); delayMicroseconds(10);

  // Bus idle check: with only pull-ups, both lines must read HIGH now.
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  delayMicroseconds(20);
  bool sdaHigh = (digitalRead(I2C_SDA_PIN) == HIGH);
  bool sclHigh = (digitalRead(I2C_SCL_PIN) == HIGH);

  // Hand pins back to I2C peripheral at 200 kHz.
  // 200 kHz gives better noise margin vs 400 kHz when WiFi is active.
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(200000UL);
  Wire.setTimeOut(I2C_TIMEOUT_MS);   // bound every transaction (see Config.h)
  delay(20);

  if (!sdaHigh || !sclHigh) {
    Serial.print(F("I2C bus stuck: SDA="));
    Serial.print(sdaHigh ? F("HIGH") : F("LOW"));
    Serial.print(F(" SCL="));
    Serial.print(sclHigh ? F("HIGH") : F("LOW"));
    Serial.println(F(" — check OLED wiring / pull-ups (GPIO 8/9)"));
    return false;
  }
  return true;
}

// ============================================================================
// DISPLAY FUNCTIONS
// ============================================================================

// Initialise the OLED — safe against a missing display AND a stuck bus.
//
// u8g2.begin() reports success unconditionally and issues ~100 I2C
// transactions, so on a stuck bus it used to block for seconds (each
// transaction waiting for its timeout) and trip the task watchdog → endless
// boot loop.  Now: recover + check the bus, then probe the display address
// with ONE short transaction; only a display that ACKs gets u8g2.begin().
// On failure the controller simply runs headless (regulation, buttons and
// web UI don't need the OLED); displayHealthWatchdog() retries later.
bool initDisplay() {
  for (byte attempt = 0; attempt < 2; attempt++) {
    #if WATCHDOG_ENABLED
      esp_task_wdt_reset();               // each attempt starts with a fresh budget
    #endif
    if (!recoverI2CBus()) {               // SDA/SCL held low — no point probing
      delay(50);
      continue;
    }
    Wire.beginTransmission(SH1107_I2C_ADDR);
    byte err = Wire.endTransmission();    // ≤ I2C_TIMEOUT_MS
    if (err == 0) {
      u8g2.begin();
      u8g2.setPowerSave(0);
      u8g2.setContrast(64);
      u8g2.clearBuffer();                 // ensure buffer is clean on first use
      #if WATCHDOG_ENABLED
        esp_task_wdt_reset();
      #endif
      return true;
    }
    Serial.print(F("Display not responding at 0x"));
    Serial.print(SH1107_I2C_ADDR, HEX);
    Serial.print(F(" (I2C err "));
    Serial.print(err);                    // 2 = no ACK (absent), 5 = timeout
    Serial.println(F(")"));
    delay(50);
  }
  Serial.println(F("Display init FAILED — running headless, will retry"));
  return false;
}

// Mode selection menu — shown after both-button short press.
// Cycling highlight menu (scales past two options):
//   UP   = move highlight to next entry
//   DOWN = select highlighted entry
//   both held / timeout = cancel
// Returns: 0 = cancel, 1 = Manual, 2 = Calibrate, 3 = toggle WiFi, 4 = Bag init
byte showModeSelect() {
  extern bool wifiEnabled;
  const byte ENTRY_COUNT = 4;
  byte sel = 0;
  unsigned long deadline = millis() + 20000;

  auto draw = [&](byte s) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvB10_tf);
    u8g2.drawBox(0, 0, 128, 14);
    u8g2.setDrawColor(0);
    u8g2.setCursor(8, 11);
    u8g2.print("Menu");
    u8g2.setDrawColor(1);

    const char* labels[ENTRY_COUNT];
    labels[0] = "Manual";
    labels[1] = "Calibrate";
    labels[2] = wifiEnabled ? "WiFi: ON" : "WiFi: OFF";
    labels[3] = "Bag init";

    u8g2.setFont(u8g2_font_9x15_tf);
    for (byte i = 0; i < ENTRY_COUNT; i++) {
      byte y = 34 + i * 20;
      if (i == s) {
        u8g2.drawBox(0, y - 13, 128, 18);
        u8g2.setDrawColor(0);
        u8g2.setCursor(8, y);  u8g2.print(">");
        u8g2.setCursor(24, y); u8g2.print(labels[i]);
        u8g2.setDrawColor(1);
      } else {
        u8g2.setCursor(24, y); u8g2.print(labels[i]);
      }
    }

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor(4, 124);
    u8g2.print("UP=next  DN=select");
    u8g2.sendBuffer();
  };

  draw(sel);
  Serial.println(F("Menu: UP=next  DN=select"));

  bool upWas = false, dnWas = false;

  while (millis() < deadline) {
    #if WATCHDOG_ENABLED
      esp_task_wdt_reset();
    #endif
    bool upNow = (digitalRead(BUTTON_UP)   == LOW);
    bool dnNow = (digitalRead(BUTTON_DOWN) == LOW);

    if (upNow && dnNow) {
      delay(50);
      if (digitalRead(BUTTON_UP) == LOW && digitalRead(BUTTON_DOWN) == LOW) {
        Serial.println(F("Menu: cancelled"));
        return 0;
      }
    }

    if (!upNow && upWas) {
      sel = (sel + 1) % ENTRY_COUNT;
      deadline = millis() + 20000;
      draw(sel);
    }
    if (!dnNow && dnWas) {
      Serial.print(F("Menu: -> ")); Serial.println(sel + 1);
      return sel + 1;
    }

    upWas = upNow;
    dnWas = dnNow;
    delay(10);
  }

  Serial.println(F("Menu: timeout"));
  return 0;
}

// Bag-init screen: prompts and running phases (FAULT uses the normal screen
// with a "FAULT" title so the bars stay visible).
void drawBagInitScreen(unsigned long now) {
  u8g2.clearBuffer();
  u8g2.setContrast(64);

  // Title bar
  u8g2.setFont(u8g2_font_helvB10_tf);
  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setDrawColor(0);
  u8g2.setCursor(8, 11);
  u8g2.print(F("Bag init"));
  u8g2.setDrawColor(1);

  u8g2.setFont(u8g2_font_9x15_tf);
  u8g2.setCursor(4, 34);
  switch (bagInit.phase) {
    case BI_PROMPT:        u8g2.print(biReasonText());      break;
    case BI_RETRY_PROMPT:  u8g2.print(biReasonText());      break;
    case BI_DEFLATE:       u8g2.print(F("Deflate probe"));  break;
    case BI_DEFLATE_EXTRA: u8g2.print(F("Deflate +")); u8g2.print(bagInit.step); break;
    case BI_INFLATE:
      u8g2.print(F("Inflate "));
      if (bagInit.inflL) u8g2.print('L');
      if (bagInit.inflR) u8g2.print('R');
      u8g2.print(' '); u8g2.print(bagInit.step + 1);
      u8g2.print('/'); u8g2.print(BAGINIT_INFLATE_PERIODS);
      break;
    default: break;
  }

  u8g2.setCursor(4, 54);
  if (bagInit.phase == BI_PROMPT)            u8g2.print(F("Init bags?"));
  else if (bagInit.phase == BI_RETRY_PROMPT) u8g2.print(F("Retry 3x10s?"));
  else { u8g2.print(biSecondsLeft(now)); u8g2.print(F(" s")); }

  // Live values: percent and raw (raw is what the init judges)
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(4, 76);
  u8g2.print(F("L ")); u8g2.print(sensors.leftPercent);  u8g2.print(F("% ")); u8g2.print(sensors.avgLeft);
  u8g2.setCursor(4, 90);
  u8g2.print(F("R ")); u8g2.print(sensors.rightPercent); u8g2.print(F("% ")); u8g2.print(sensors.avgRight);

  u8g2.setCursor(4, 124);
  if (bagInit.phase == BI_PROMPT || bagInit.phase == BI_RETRY_PROMPT) {
    u8g2.print(F("UP=Yes  DN=No  "));
    u8g2.print(biSecondsLeft(now)); u8g2.print('s');
  } else {
    u8g2.print(F("Both=Cancel"));
  }
  u8g2.sendBuffer();
}

void updateDisplay(unsigned long currentMillis) {
  if (bagInit.phase != BI_IDLE && bagInit.phase != BI_FAULT) {
    drawBagInitScreen(currentMillis);
    return;
  }

  static byte animOffset = 0;
  animOffset = (animOffset + 1) % 8;

  u8g2.clearBuffer();
  {
    bool blink = (currentMillis / 2000) % 2 == 0;

    u8g2.setFont(u8g2_font_helvB18_tf);
    u8g2.setCursor(10, 20);

    if (manualControl.active) {
      u8g2.print(F("MANUAL"));
      u8g2.setFont(u8g2_font_9x15_tf);
      u8g2.setCursor(42, 60);
      const char* valveName;
      switch (manualControl.selectedValve) {
        case 0: valveName = "L-UP"; break;
        case 1: valveName = "L-DN"; break;
        case 2: valveName = "R-UP"; break;
        case 3: valveName = "R-DN"; break;
        default: valveName = "???";
      }
      u8g2.println(F("VALVE: "));
      u8g2.setCursor(44, 75);
      u8g2.print(valveName);
    } else {
      if (regulation.errorState && blink) {
        u8g2.setContrast(254);
        u8g2.print(F("ERR: "));
        u8g2.print(targetPercent);
        u8g2.print('%');
      } else if (bagInit.phase == BI_FAULT) {
        u8g2.setContrast(64);
        u8g2.print(F("FAULT"));
      } else if (isSetpointOff(targetPercent)) {
        u8g2.setContrast(64);
        u8g2.print(F("OFF"));
      } else {
        if (!calibration.valid) { u8g2.setContrast(64); u8g2.print(F("NC: ")); }
        else                    { u8g2.setContrast(64); u8g2.print(F("SET: ")); }
        u8g2.print(targetPercent);
        u8g2.print('%');
      }
      u8g2.setFont(u8g2_font_9x15_tf);
      u8g2.setCursor(10, 40);
      u8g2.print(F("L:"));
      u8g2.print(sensors.leftPercent);
      u8g2.print('%');
      u8g2.setCursor(80, 40);
      u8g2.print(F("R:"));
      u8g2.print(sensors.rightPercent);
      u8g2.print('%');
    }

    if (manualControl.valveActive) {
      u8g2.setCursor(42, 100);
      u8g2.print(F("PULSE"));
    }

    if (regulation.isRegulating && !manualControl.active) {
      u8g2.setFont(u8g2_font_9x15_tf);
      if (pulse.inValvePulse) {
        u8g2.setCursor(48, 55);
        u8g2.print(F("ADJ"));
      } else if (pulse.inSettlingPeriod) {
        u8g2.setCursor(48, 55);
        u8g2.print(F("WAIT"));
      }
    }

    byte barLeftHeight  = (sensors.leftPercent  * MAX_BAR_HEIGHT) / 100;
    byte barRightHeight = (sensors.rightPercent * MAX_BAR_HEIGHT) / 100;
    byte targetY        = 128 - ((targetPercent * MAX_BAR_HEIGHT) / 100);

    u8g2.drawFrame(20, 128 - MAX_BAR_HEIGHT, 20, MAX_BAR_HEIGHT);
    u8g2.drawBox  (20, 128 - barLeftHeight,  20, barLeftHeight);
    u8g2.drawFrame(88, 128 - MAX_BAR_HEIGHT, 20, MAX_BAR_HEIGHT);
    u8g2.drawBox  (88, 128 - barRightHeight, 20, barRightHeight);
    if (!isSetpointOff(targetPercent)) {   // no target line in OFF/standby
      u8g2.drawLine(10, targetY, 40,  targetY);
      u8g2.drawLine(88, targetY, 118, targetY);
    }

    // Driving, lockout and arrows — hidden in manual mode
    if (!manualControl.active) {
      if (isVehicleDriving()) {
        u8g2.setFont(u8g2_font_9x15_tf);
        u8g2.setCursor(44, 70);
        u8g2.print(F("Drive"));
      }
      if (antiHunt.inLockout) {
        u8g2.setFont(u8g2_font_9x15_tf);
        u8g2.setCursor(48, 90);
        u8g2.print(F("LOCK"));
        unsigned long lockoutRemaining = (antiHunt.lockoutUntil - currentMillis) / 1000;
        u8g2.setCursor(48, 105);
        u8g2.print(lockoutRemaining);
        u8g2.print(F("s"));
      }
      if (regulation.leftShouldRegulate) {
        byte x = 40;
        byte y = sensors.leftPercent < targetPercent ?
                 128 - barLeftHeight - animOffset : 128 - barLeftHeight + animOffset;
        if (sensors.leftPercent < targetPercent) {
          u8g2.drawTriangle(x, y, x + 10, y - 15, x + 20, y);
        } else {
          u8g2.drawTriangle(x, y, x + 10, y + 15, x + 20, y);
        }
      }
      if (regulation.rightShouldRegulate) {
        byte x = 68;
        byte y = sensors.rightPercent < targetPercent ?
                 128 - barRightHeight - animOffset : 128 - barRightHeight + animOffset;
        if (sensors.rightPercent < targetPercent) {
          u8g2.drawTriangle(x, y, x + 10, y - 15, x + 20, y);
        } else {
          u8g2.drawTriangle(x, y, x + 10, y + 15, x + 20, y);
        }
      }
    }
  }
  u8g2.sendBuffer();
}

#endif // DISPLAY_H
