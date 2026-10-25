// Calibration.h - Display-guided pressure sensor calibration (ESP32-S3/C6)
//
// Entry:    both buttons short press → mode select → DOWN selects calibration
//           OR send  c  via serial
// Flow:     INTRO → DEFLATING → CONFIRM_MIN → INFLATING → CONFIRM_MAX → SAVE → DONE
// Buttons:  UP = confirm/advance   DOWN = cancel/back   Both held >1s = abort
// Serial:   u/ENTER = UP    d = DOWN    q = abort    ? = reprint help
//
// ADC ranges are 12-bit (0–4095) with ADC_11db attenuation (0–3.1V input).
// Safety limits assume a 0.5–4.5V pressure sensor on a 3.3V supply.
//
#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <Arduino.h>
#include <EEPROM.h>
#include "Config.h"
#include "Types.h"
#include "Sensors.h"
#include "Valves.h"
#include "EEPROM_Manager.h"

extern CalibrationData  calibration;
extern SensorData       sensors;
extern bool             displayAvailable;
extern U8G2_SH1107_SEEED_128X128_F_HW_I2C u8g2;

// ============================================================================
// CALIBRATION SAFETY LIMITS (12-bit ADC, ADC_11db, 0–3.1V input range)
// ============================================================================
// For a 0.5–4.5V sensor:  0.5V → ~660 raw  |  4.5V clips at 3.1V → ~4095 raw
// Typical measured ranges:  deflated 500–1200  |  inflated 2400–3900
static const int CAL_MIN_SAFETY_LOW  = 0;   // deflated reading must be above this
static const int CAL_MIN_SAFETY_HIGH = 2000;  // deflated reading must be below this
static const int CAL_MAX_SAFETY_LOW  = 2095;  // inflated reading must be above this
static const int CAL_MAX_SAFETY_HIGH = 4096;  // inflated reading must be below this
static const int CAL_MIN_SPAN        = 1000;   // minimum span between min and max

// ============================================================================
// STATE MACHINE
// ============================================================================
enum CalibState : byte {
  CAL_INTRO = 0,
  CAL_DEFLATING,
  CAL_CONFIRM_MIN,
  CAL_INFLATING,
  CAL_CONFIRM_MAX,
  CAL_VALIDATE,
  CAL_SAVE,
  CAL_DONE,
  CAL_ABORT
};

struct CalibrationSession {
  CalibState state        = CAL_INTRO;
  CalibState prevState    = CAL_ABORT;  // sentinel — forces first prompt
  bool       active       = false;
  int        leftMin      = 0;
  int        rightMin     = 0;
  int        leftMax      = 0;
  int        rightMax     = 0;
  bool       valid        = false;
  unsigned long stateEntered = 0;
};

static CalibrationSession calSession;

// ============================================================================
// INPUT HELPERS
// ============================================================================

static bool _upWasLow   = false;
static bool _downWasLow = false;
static unsigned long _upPressedAt   = 0;
static unsigned long _downPressedAt = 0;

static bool calUpReleased(unsigned long now) {
  bool low = (digitalRead(BUTTON_UP) == LOW);
  if (low && !_upWasLow)  { _upPressedAt = now;   _upWasLow = true;   }
  if (!low && _upWasLow   && now - _upPressedAt   > 40) { _upWasLow   = false; return true; }
  return false;
}

static bool calDownReleased(unsigned long now) {
  bool low = (digitalRead(BUTTON_DOWN) == LOW);
  if (low && !_downWasLow){ _downPressedAt = now;  _downWasLow = true; }
  if (!low && _downWasLow && now - _downPressedAt > 40) { _downWasLow = false; return true; }
  return false;
}

static bool calBothHeld() {
  return (digitalRead(BUTTON_UP) == LOW && digitalRead(BUTTON_DOWN) == LOW);
}

// Line buffer for multi-character serial commands (e.g.  l 684  r 3810)
static char _calLineBuf[12];
static byte _calLineIdx  = 0;
static int  _calManualVal = 0;   // populated when calSerialCmd() returns 'L' or 'R'

// Reads serial into a line buffer.  Returns:
//   'u'  confirm/next     (bare Enter, or  u  key)
//   'd'  back/redo        (d  key)
//   'q'  abort            (q  key)
//   '?'  reprint help     (? key)
//   'L'  left ADC value   — value stored in _calManualVal
//   'R'  right ADC value  — value stored in _calManualVal
//   0    nothing complete yet
// Single-char commands (u/d/q/?) fire immediately without waiting for Enter.
static char calSerialCmd() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;

    // Immediate single-char commands when buffer is empty
    if (_calLineIdx == 0) {
      if (c == 'u' || c == 'U') return 'u';
      if (c == 'd' || c == 'D') return 'd';
      if (c == 'q' || c == 'Q') return 'q';
      if (c == '?')              return '?';
    }

    if (c == '\n') {
      _calLineBuf[_calLineIdx] = '\0';
      _calLineIdx = 0;
      if (_calLineBuf[0] == '\0') return 'u';   // bare Enter = confirm

      char side = toupper(_calLineBuf[0]);
      if ((side == 'L' || side == 'R') && _calLineBuf[1] == ' ') {
        int val = atoi(_calLineBuf + 2);
        if (val >= 1 && val <= ADC_MAX_RAW) {
          _calManualVal = val;
          return side;           // 'L' or 'R'
        }
        Serial.print(F("  Bad value — must be 1–"));
        Serial.println(ADC_MAX_RAW);
        return 0;
      }
      // Single char submitted on its own line
      char fc = _calLineBuf[0];
      if (_calLineBuf[1] == '\0') {
        if (fc == 'u' || fc == 'U') return 'u';
        if (fc == 'd' || fc == 'D') return 'd';
        if (fc == 'q' || fc == 'Q') return 'q';
        if (fc == '?')              return '?';
      }
      return 0;
    }

    if (_calLineIdx < (byte)(sizeof(_calLineBuf) - 1))
      _calLineBuf[_calLineIdx++] = c;
  }
  return 0;
}

// Combine physical buttons + serial into unified signals.
// manualSide: 'L', 'R', or 0    manualVal: parsed ADC value when side != 0
static void calReadInput(unsigned long now,
                         bool &up, bool &down, bool &abort,
                         char &manualSide, int &manualVal) {
  up         = calUpReleased(now);
  down       = calDownReleased(now);
  abort      = false;
  manualSide = 0;
  manualVal  = 0;

  char sc = calSerialCmd();
  if (sc == 'u' || sc == 'U') up    = true;
  if (sc == 'd' || sc == 'D') down  = true;
  if (sc == 'q' || sc == 'Q') abort = true;
  if (sc == 'L' || sc == 'R') { manualSide = sc; manualVal = _calManualVal; }
  // '?' handled in main loop
}

// ============================================================================
// ADC SAMPLING
// ============================================================================

static int calAveragedRead(byte pin, byte n = 16) {
  long sum = 0;
  for (byte i = 0; i < n; i++) { sum += analogRead(pin); delay(5); }
  return sum / n;
}

static int calReadRight(byte n = 16) {
  int raw = calAveragedRead(SENSOR_RIGHT_PIN, n);
  return calibration.rightInverted ? (ADC_MAX_RAW - raw) : raw;
}

// ============================================================================
// VALIDATION
// ============================================================================

static bool calValidate(int lMin, int lMax, int rMin, int rMax) {
  if (lMin < CAL_MIN_SAFETY_LOW  || lMin > CAL_MIN_SAFETY_HIGH) return false;
  if (rMin < CAL_MIN_SAFETY_LOW  || rMin > CAL_MIN_SAFETY_HIGH) return false;
  if (lMax < CAL_MAX_SAFETY_LOW  || lMax > CAL_MAX_SAFETY_HIGH) return false;
  if (rMax < CAL_MAX_SAFETY_LOW  || rMax > CAL_MAX_SAFETY_HIGH) return false;
  if (lMax - lMin < CAL_MIN_SPAN || rMax - rMin < CAL_MIN_SPAN) return false;
  return true;
}

// ============================================================================
// DISPLAY
// ============================================================================

static void calDrawScreen(const char* title, const char* line1,
                          const char* line2 = nullptr,
                          const char* hint  = nullptr,
                          int liveL = -1, int liveR = -1) {
  if (!displayAvailable) return;
  u8g2.clearBuffer();
  {
    u8g2.setFont(u8g2_font_helvB10_tf);
    u8g2.drawBox(0, 0, 128, 14);
    u8g2.setDrawColor(0);
    u8g2.setCursor(4, 11);
    u8g2.print(title);
    u8g2.setDrawColor(1);

    u8g2.setFont(u8g2_font_9x15_tf);
    if (line1) { u8g2.setCursor(4, 30); u8g2.print(line1); }
    if (line2) { u8g2.setCursor(4, 48); u8g2.print(line2); }

    if (liveL >= 0) {
      u8g2.setFont(u8g2_font_6x10_tf);
      char buf[24];
      snprintf(buf, sizeof(buf), "L:%4d  R:%4d", liveL, liveR);
      u8g2.setCursor(4, 68);
      u8g2.print(buf);
    }
    if (hint) {
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.setCursor(4, 118);
      u8g2.print(hint);
    }
  }
  u8g2.sendBuffer();
}

// ============================================================================
// SERIAL PROMPTS
// ============================================================================

static void calPrintHelp() {
  Serial.println(F(""));
  Serial.println(F("=== Calibration mode ==="));
  Serial.println(F("Steps:"));
  Serial.println(F("  1. Deflate to minimum → confirm when stable"));
  Serial.println(F("  2. Inflate to maximum → confirm when stable"));
  Serial.println(F("  3. Validate ranges → save"));
  Serial.println(F(""));
  Serial.println(F("Commands (serial or physical buttons):"));
  Serial.println(F("  u / ENTER  = confirm / next step  [UP button]"));
  Serial.println(F("  d          = back / redo           [DOWN button]"));
  Serial.println(F("  q          = abort, no save        [Both buttons >1s]"));
  Serial.println(F("  ?          = reprint this help"));
  Serial.println(F(""));
  Serial.println(F("Manual value override (at min/max confirm steps):"));
  Serial.println(F("  l <value>  = set left  ADC value   (e.g.  l 684)"));
  Serial.println(F("  r <value>  = set right ADC value   (e.g.  r 3810)"));
  Serial.println(F(""));
  Serial.println(F("Safety limits (12-bit ADC):"));
  Serial.print  (F("  Deflated: "));  Serial.print(CAL_MIN_SAFETY_LOW);
  Serial.print  (F(" – "));           Serial.println(CAL_MIN_SAFETY_HIGH);
  Serial.print  (F("  Inflated: "));  Serial.print(CAL_MAX_SAFETY_LOW);
  Serial.print  (F(" – "));           Serial.println(CAL_MAX_SAFETY_HIGH);
  Serial.print  (F("  Min span: "));  Serial.println(CAL_MIN_SPAN);
  Serial.println(F(""));
}

// Printed once each time a state is entered
static void calPrintStatePrompt(CalibState s) {
  switch (s) {
    case CAL_INTRO:
      Serial.println(F("[Step 0/4] Ready to calibrate."));
      Serial.println(F("  Send  u  or ENTER to start, or  d  to cancel."));
      break;
    case CAL_DEFLATING:
      Serial.println(F("[Step 1/4] Deflating... watch L/R ADC values."));
      Serial.println(F("  Send  u  or ENTER when suspension is fully deflated."));
      Serial.println(F("  Send  d  to abort."));
      break;
    case CAL_CONFIRM_MIN:
      Serial.print  (F("[Step 2/4] Minimum recorded  L="));
      Serial.print  (calSession.leftMin);
      Serial.print  (F("  R="));
      Serial.println(calSession.rightMin);
      Serial.println(F("  u = accept    d = deflate again"));
      Serial.println(F("  l <value>  = override left min  (e.g.  l 684)"));
      Serial.println(F("  r <value>  = override right min (e.g.  r 699)"));
      break;
    case CAL_INFLATING:
      Serial.println(F("[Step 3/4] Inflating... watch L/R ADC values."));
      Serial.println(F("  Send  u  or ENTER when suspension is fully inflated."));
      Serial.println(F("  Send  d  to abort."));
      break;
    case CAL_CONFIRM_MAX:
      Serial.print  (F("[Step 4/4] Maximum recorded  L="));
      Serial.print  (calSession.leftMax);
      Serial.print  (F("  R="));
      Serial.println(calSession.rightMax);
      Serial.println(F("  u = validate  d = inflate again"));
      Serial.println(F("  l <value>  = override left max  (e.g.  l 3810)"));
      Serial.println(F("  r <value>  = override right max (e.g.  r 3795)"));
      break;
    case CAL_VALIDATE:
      if (calSession.valid) {
        Serial.println(F("[Validate] PASS"));
        Serial.print  (F("  Span L=")); Serial.print(calSession.leftMax  - calSession.leftMin);
        Serial.print  (F("  R="));      Serial.println(calSession.rightMax - calSession.rightMin);
        Serial.println(F("  Send  u  to save,  d  to restart."));
      } else {
        Serial.println(F("[Validate] FAIL — values outside safety limits."));
        Serial.println(F("  Send  u  to save anyway,  d  to restart."));
      }
      break;
    case CAL_SAVE:
      break;  // printed inline
    case CAL_DONE:
      Serial.println(F("[Done] Calibration saved to EEPROM."));
      Serial.print  (F("  L: ")); Serial.print(calibration.leftMinRaw);
      Serial.print  (F(" – "));   Serial.println(calibration.leftMaxRaw);
      Serial.print  (F("  R: ")); Serial.print(calibration.rightMinRaw);
      Serial.print  (F(" – "));   Serial.println(calibration.rightMaxRaw);
      break;
    case CAL_ABORT:
      Serial.println(F("[Aborted] No changes saved."));
      break;
  }
}

// ============================================================================
// PUBLIC API
// ============================================================================

void enterCalibrationMode() {
  calSession            = CalibrationSession();
  calSession.active     = true;
  calSession.state      = CAL_INTRO;
  calSession.prevState  = CAL_ABORT;  // force first prompt
  calSession.stateEntered = millis();
  _upWasLow = false;
  _downWasLow = false;
  allValvesOff();
  calPrintHelp();
  calDrawScreen("CALIBRATION", "Guided sensor", "calibration",
                "UP=start  DN=cancel");
}

bool calibrationActive() { return calSession.active; }

bool handleCalibrationMode(unsigned long now) {
  if (!calSession.active) return false;

  // Print state prompt once on each state entry
  if (calSession.state != calSession.prevState) {
    calPrintStatePrompt(calSession.state);
    calSession.prevState = calSession.state;
  }

  // Global abort: both buttons held >1s
  static unsigned long bothHeldSince = 0;
  if (calBothHeld()) {
    if (bothHeldSince == 0) bothHeldSince = now;
    if (now - bothHeldSince > 1000) { calSession.state = CAL_ABORT; bothHeldSince = 0; }
  } else {
    bothHeldSince = 0;
  }

  bool up, down, abort;
  char manualSide; int manualVal;
  calReadInput(now, up, down, abort, manualSide, manualVal);
  if (abort) { calSession.state = CAL_ABORT; }

  // '?' reprint help
  {
    char sc = 0;
    if (Serial.available()) sc = Serial.peek();
    if (sc == '?') { Serial.read(); calPrintHelp(); calPrintStatePrompt(calSession.state); }
  }

  switch (calSession.state) {

    // ── INTRO ───────────────────────────────────────────────────────────────
    case CAL_INTRO:
      if (up) {
        calSession.state = CAL_DEFLATING;
        calSession.stateEntered = now;
        digitalWrite(DEFLATE_LEFT_PIN,  HIGH);
        digitalWrite(DEFLATE_RIGHT_PIN, HIGH);
        calDrawScreen("CALIBRATE 1/4", "Deflating...", nullptr, "u=done  d=cancel");
      } else if (down) {
        calSession.state = CAL_ABORT;
      }
      break;

    // ── DEFLATING ───────────────────────────────────────────────────────────
    case CAL_DEFLATING: {
      static unsigned long lastRefresh = 0;
      if (now - lastRefresh > 500) {
        int lv = analogRead(SENSOR_LEFT_PIN);
        int rv = calibration.rightInverted
                 ? (ADC_MAX_RAW - analogRead(SENSOR_RIGHT_PIN))
                 :  analogRead(SENSOR_RIGHT_PIN);
        Serial.print(F("  ADC  L=")); Serial.print(lv);
        Serial.print(F("  R="));     Serial.println(rv);
        calDrawScreen("CALIBRATE 1/4", "Deflating...",
                      "Press UP at bottom", "u=done  d=cancel", lv, rv);
        lastRefresh = now;
      }
      if (up) {
        allValvesOff();
        Serial.println(F("  Valves off — settling 1 s..."));
        delay(1000);
        calSession.leftMin  = calAveragedRead(SENSOR_LEFT_PIN);
        calSession.rightMin = calReadRight();
        calSession.state    = CAL_CONFIRM_MIN;
        calSession.stateEntered = now;
        char buf[24];
        snprintf(buf, sizeof(buf), "L:%4d R:%4d", calSession.leftMin, calSession.rightMin);
        calDrawScreen("MIN RECORDED", buf, nullptr, "u=ok  d=redo");
      } else if (down) {
        allValvesOff();
        calSession.state = CAL_ABORT;
      }
      break;
    }

    // ── CONFIRM MIN ─────────────────────────────────────────────────────────
    case CAL_CONFIRM_MIN:
      // Manual ADC override via serial  l <val>  or  r <val>
      if (manualSide == 'L' || manualSide == 'R') {
        if (manualSide == 'L') {
          calSession.leftMin = manualVal;
          Serial.print(F("  Left min set to ")); Serial.println(manualVal);
        } else {
          calSession.rightMin = manualVal;
          Serial.print(F("  Right min set to ")); Serial.println(manualVal);
        }
        Serial.print(F("  Current min  L=")); Serial.print(calSession.leftMin);
        Serial.print(F("  R="));              Serial.println(calSession.rightMin);
        char buf[24];
        snprintf(buf, sizeof(buf), "L:%4d R:%4d", calSession.leftMin, calSession.rightMin);
        calDrawScreen("MIN UPDATED", buf, nullptr, "u=ok  d=redo");
        calSession.prevState = CAL_ABORT;   // force re-prompt on next state print
        break;
      }
      if (up) {
        if (calSession.leftMin  < CAL_MIN_SAFETY_LOW  ||
            calSession.leftMin  > CAL_MIN_SAFETY_HIGH ||
            calSession.rightMin < CAL_MIN_SAFETY_LOW  ||
            calSession.rightMin > CAL_MIN_SAFETY_HIGH) {
          Serial.println(F("  WARNING: min value outside expected range."));
          Serial.println(F("  Send  u  again to accept anyway, or  d  to redo."));
          calDrawScreen("MIN WARNING", "Out of range", nullptr, "u=accept  d=redo");
          calSession.prevState = CAL_ABORT;  // force re-prompt on next u
          break;
        }
        calSession.state = CAL_INFLATING;
        calSession.stateEntered = now;
        digitalWrite(INFLATE_LEFT_PIN,  HIGH);
        digitalWrite(INFLATE_RIGHT_PIN, HIGH);
        calDrawScreen("CALIBRATE 3/4", "Inflating...", nullptr, "u=done  d=cancel");
      } else if (down) {
        calSession.state = CAL_DEFLATING;
        calSession.stateEntered = now;
        digitalWrite(DEFLATE_LEFT_PIN,  HIGH);
        digitalWrite(DEFLATE_RIGHT_PIN, HIGH);
        calDrawScreen("CALIBRATE 1/4", "Deflating...", nullptr, "u=done  d=cancel");
      }
      break;

    // ── INFLATING ───────────────────────────────────────────────────────────
    case CAL_INFLATING: {
      static unsigned long lastRefresh2 = 0;
      if (now - lastRefresh2 > 500) {
        int lv = analogRead(SENSOR_LEFT_PIN);
        int rv = calibration.rightInverted
                 ? (ADC_MAX_RAW - analogRead(SENSOR_RIGHT_PIN))
                 :  analogRead(SENSOR_RIGHT_PIN);
        Serial.print(F("  ADC  L=")); Serial.print(lv);
        Serial.print(F("  R="));     Serial.println(rv);
        calDrawScreen("CALIBRATE 3/4", "Inflating...",
                      "Press UP at top", "u=done  d=cancel", lv, rv);
        lastRefresh2 = now;
      }
      if (up) {
        allValvesOff();
        Serial.println(F("  Valves off — settling 1 s..."));
        delay(1000);
        calSession.leftMax  = calAveragedRead(SENSOR_LEFT_PIN);
        calSession.rightMax = calReadRight();
        calSession.state    = CAL_CONFIRM_MAX;
        calSession.stateEntered = now;
        char buf[24];
        snprintf(buf, sizeof(buf), "L:%4d R:%4d", calSession.leftMax, calSession.rightMax);
        calDrawScreen("MAX RECORDED", buf, nullptr, "u=ok  d=redo");
      } else if (down) {
        allValvesOff();
        calSession.state = CAL_ABORT;
      }
      break;
    }

    // ── CONFIRM MAX ─────────────────────────────────────────────────────────
    case CAL_CONFIRM_MAX:
      // Manual ADC override via serial  l <val>  or  r <val>
      if (manualSide == 'L' || manualSide == 'R') {
        if (manualSide == 'L') {
          calSession.leftMax = manualVal;
          Serial.print(F("  Left max set to ")); Serial.println(manualVal);
        } else {
          calSession.rightMax = manualVal;
          Serial.print(F("  Right max set to ")); Serial.println(manualVal);
        }
        Serial.print(F("  Current max  L=")); Serial.print(calSession.leftMax);
        Serial.print(F("  R="));              Serial.println(calSession.rightMax);
        char buf[24];
        snprintf(buf, sizeof(buf), "L:%4d R:%4d", calSession.leftMax, calSession.rightMax);
        calDrawScreen("MAX UPDATED", buf, nullptr, "u=ok  d=redo");
        calSession.prevState = CAL_ABORT;   // force re-prompt on next state print
        break;
      }
      if (up) {
        calSession.valid = calValidate(calSession.leftMin, calSession.leftMax,
                                       calSession.rightMin, calSession.rightMax);
        calSession.state = CAL_VALIDATE;
        calSession.stateEntered = now;
        if (calSession.valid) {
          char buf[24];
          snprintf(buf, sizeof(buf), "Span L:%d R:%d",
                   calSession.leftMax - calSession.leftMin,
                   calSession.rightMax - calSession.rightMin);
          calDrawScreen("VALID OK", buf, nullptr, "u=save  d=restart");
        } else {
          calDrawScreen("INVALID!", "Check ranges", nullptr, "u=save anyway d=redo");
        }
      } else if (down) {
        calSession.state = CAL_INFLATING;
        calSession.stateEntered = now;
        digitalWrite(INFLATE_LEFT_PIN,  HIGH);
        digitalWrite(INFLATE_RIGHT_PIN, HIGH);
        calDrawScreen("CALIBRATE 3/4", "Inflating...", nullptr, "u=done  d=cancel");
      }
      break;

    // ── VALIDATE ────────────────────────────────────────────────────────────
    case CAL_VALIDATE:
      if (up) {
        calSession.state = CAL_SAVE;
        calSession.stateEntered = now;
      } else if (down) {
        calSession.state = CAL_INTRO;
        calSession.stateEntered = now;
        calDrawScreen("CALIBRATION", "Guided sensor", "calibration",
                      "u=start  d=cancel");
      }
      break;

    // ── SAVE ────────────────────────────────────────────────────────────────
    case CAL_SAVE:
      calibration.leftMinRaw  = calSession.leftMin;
      calibration.leftMaxRaw  = calSession.leftMax;
      calibration.rightMinRaw = calSession.rightMin;
      calibration.rightMaxRaw = calSession.rightMax;
      calibration.valid       = true;
      saveCalibration();
      calSession.state = CAL_DONE;
      calSession.stateEntered = now;
      calDrawScreen("SAVED!", "Calibration", "stored in EEPROM", nullptr);
      break;

    // ── DONE ────────────────────────────────────────────────────────────────
    case CAL_DONE:
      if (now - calSession.stateEntered > 2000) {
        calSession.active = false;
        allValvesOff();
        return false;
      }
      break;

    // ── ABORT ───────────────────────────────────────────────────────────────
    case CAL_ABORT:
      allValvesOff();
      calDrawScreen("ABORTED", "No changes", "were saved", nullptr);
      delay(1500);
      calSession.active = false;
      return false;
  }

  return true;
}

#endif // CALIBRATION_H