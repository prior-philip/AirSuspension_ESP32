// BagInit.h - Out-of-range detection and guided bag initialization
//
// WHY: the height sensors wrap to the opposite end of their range when a bag
// is completely empty (or overfull).  Typical signature with both bags empty:
// L = 100% / R = 0% — physically impossible for a parked van.  Regulating on
// such readings either never refills an empty bag or keeps inflating a full
// one.  This module detects the state, stops regulation, and (with the user's
// consent) runs a short init sequence that tells the two cases apart:
//
//   1. Deflate both sides 5 s   (deflating is safe in every case)
//        - no raw change  → bags are empty, one sensor wrapped  → step 2
//        - raw change     → bags were NOT empty (likely overfull, one sensor
//                           wrapped low) → keep deflating in 5 s steps until
//                           both sides read > 0%, then hand back to regulation
//   2. Inflate both sides together, 3 × 10 s  (30 s max)
//        - each side's valve closes as soon as that side reads OK
//        - stops early as soon as both sides read plausibly
//        - no success → ask the user whether to retry 3×10 s
//
// Change is judged on RAW ADC (fresh 8-sample average), not on percent:
// a wrapped side sits beyond the calibrated range, so its percentage stays
// pinned at 0 or 100 while the raw value is actually moving.
//
// Declined / unanswered / cancelled / failed → FAULT: regulation blocked,
// shown on OLED/web/serial, survives deep sleep (RTC).  Leave FAULT via the
// "Bag init" menu entry, the web button, or automatically once both sides
// read plausibly for BAGINIT_TRIGGER_MS (e.g. fixed in manual mode).
//
// Valve ownership: while a phase is running this module drives the valves
// (re-asserted every loop, so an unrelated allValvesOff() can't silently end
// a phase); regulation and serial valve commands stand off.
//
#ifndef BAG_INIT_H
#define BAG_INIT_H

#include <Arduino.h>
#include "Config.h"
#include "Types.h"
#include "Sensors.h"
#include "Valves.h"
#include "Regulation.h"
#include "Buttons.h"      // armButtonReleaseLock()

extern BagInitState       bagInit;
extern SensorData         sensors;
extern CalibrationData    calibration;
extern ManualControlState manualControl;
extern PowerSaveState     powerSave;
extern bool               calibrationMode;
extern byte               targetPercent;
extern bool               rtc_bagInitFault;   // RTC: survives deep sleep

// ============================================================================
// HELPERS
// ============================================================================

// Fresh raw reads, averaged over 8 samples.  Right side is returned in the same
// (inverted) domain the rest of the firmware uses.
static int biRawLeft() {
  long s = 0;
  for (byte i = 0; i < 8; i++) s += analogRead(SENSOR_LEFT_PIN);
  return (int)(s / 8);
}
static int biRawRight() {
  long s = 0;
  for (byte i = 0; i < 8; i++) s += readRightSensor();
  return (int)(s / 8);
}

// Map a raw value to percent with the same calibration/fallback as Sensors.h.
static byte biPct(int raw, bool left) {
  long lo = calibration.valid ? (left ? calibration.leftMinRaw  : calibration.rightMinRaw)  : 200;
  long hi = calibration.valid ? (left ? calibration.leftMaxRaw  : calibration.rightMaxRaw)  : 3800;
  return (byte)constrain(map(raw, lo, hi, 0, 100), 0, 100);
}

static inline bool biSideOk(byte pct) {
  return pct >= BAGINIT_OK_MIN && pct <= BAGINIT_OK_MAX;
}

static inline bool biPairOk(byte l, byte r) {
  return biSideOk(l) && biSideOk(r) && abs((int)l - (int)r) <= BAGINIT_OK_MAX_DIFF;
}

static inline bool biIsExtreme(byte l, byte r) {
  return (l >= BAGINIT_EXTREME_HIGH && r <= BAGINIT_EXTREME_LOW) ||
         (l <= BAGINIT_EXTREME_LOW  && r >= BAGINIT_EXTREME_HIGH);
}

// Buttons belong to this module during prompts and running phases.
inline bool bagInitOwnsButtons() {
  return bagInit.phase == BI_PROMPT || bagInit.phase == BI_RETRY_PROMPT ||
         biIsRunning(bagInit);
}

const char* biPhaseName() {
  switch (bagInit.phase) {
    case BI_IDLE:          return "idle";
    case BI_PROMPT:        return "prompt";
    case BI_DEFLATE:       return "deflate probe";
    case BI_DEFLATE_EXTRA: return "deflate extra";
    case BI_INFLATE:       return "inflate";
    case BI_RETRY_PROMPT:  return "retry prompt";
    case BI_FAULT:         return "FAULT";
    default:               return "?";
  }
}

const char* biReasonText() {
  switch (bagInit.reason) {
    case BIR_OUT_OF_RANGE: return "Out of range";
    case BIR_NO_RESPONSE:  return "No response";
    case BIR_NOT_IN_RANGE: return "Not in range";
    case BIR_NO_RECOVERY:  return "No recovery";
    case BIR_DECLINED:     return "Declined";
    case BIR_CANCELLED:    return "Cancelled";
    default:               return "";
  }
}

// Seconds left in the current timed phase (for display/web), 0 if not timed.
unsigned int biSecondsLeft(unsigned long now) {
  unsigned long len = 0;
  switch (bagInit.phase) {
    case BI_DEFLATE:
    case BI_DEFLATE_EXTRA: len = BAGINIT_DEFLATE_MS;       break;
    case BI_INFLATE:       len = BAGINIT_INFLATE_MS;       break;
    case BI_PROMPT:
    case BI_RETRY_PROMPT:  len = BAGINIT_PROMPT_TIMEOUT;   break;
    default:               return 0;
  }
  unsigned long el = now - bagInit.phaseStart;
  return (el >= len) ? 0 : (unsigned int)((len - el + 999) / 1000);
}

// Drive the valves for the current phase.  Called every loop while running.
static void biApplyValves() {
  bool li = false, ld = false, ri = false, rd = false;
  switch (bagInit.phase) {
    case BI_DEFLATE:
    case BI_DEFLATE_EXTRA: ld = rd = true; break;
    case BI_INFLATE: li = bagInit.inflL; ri = bagInit.inflR; break;
    default: break;
  }
  digitalWrite(INFLATE_LEFT_PIN,  li ? HIGH : LOW);
  digitalWrite(DEFLATE_LEFT_PIN,  ld ? HIGH : LOW);
  digitalWrite(INFLATE_RIGHT_PIN, ri ? HIGH : LOW);
  digitalWrite(DEFLATE_RIGHT_PIN, rd ? HIGH : LOW);
}

static void biEnter(BagInitPhase p, unsigned long now) {
  bagInit.phase      = p;
  bagInit.phaseStart = now;
  bagInit.btnArmed   = false;   // prompts: ignore buttons until all released
  powerSave.lastActivityTime = now;
  rtc_bagInitFault   = (p == BI_FAULT);
  Serial.print(F("[BagInit] -> ")); Serial.print(biPhaseName());
  if (bagInit.reason != BIR_NONE && (p == BI_PROMPT || p == BI_RETRY_PROMPT || p == BI_FAULT)) {
    Serial.print(F(" (")); Serial.print(biReasonText()); Serial.print(')');
  }
  Serial.println();
}

static void biFault(byte reason, unsigned long now) {
  allValvesOff();
  bagInit.reason = reason;
  bagInit.conditionSince = 0;
  biEnter(BI_FAULT, now);
}

static void biFinish(unsigned long now) {
  allValvesOff();
  resetRegulationState();          // clean start for normal regulation
  bagInit.reason = BIR_NONE;
  bagInit.conditionSince = 0;
  Serial.print(F("[BagInit] done — L:")); Serial.print(biPct(biRawLeft(), true));
  Serial.print(F("% R:"));               Serial.print(biPct(biRawRight(), false));
  Serial.println(F("% — back to regulation"));
  biEnter(BI_IDLE, now);
}

// Begin the inflate phase (first run or user retry): baselines, then open the
// inflate valve of every side that does not read OK yet — both at once.
static void biStartInflate(unsigned long now) {
  bagInit.baseL    = biRawLeft();
  bagInit.baseR    = biRawRight();
  bagInit.changedL = false;
  bagInit.changedR = false;
  bagInit.inflL    = !biSideOk(biPct(bagInit.baseL, true));
  bagInit.inflR    = !biSideOk(biPct(bagInit.baseR, false));
  bagInit.step     = 0;

  if (!bagInit.inflL && !bagInit.inflR) {
    // Both sides already individually OK: done if the pair is plausible.
    if (biPairOk(biPct(bagInit.baseL, true), biPct(bagInit.baseR, false))) {
      biFinish(now);
    } else {
      bagInit.reason = BIR_NOT_IN_RANGE;
      biEnter(BI_RETRY_PROMPT, now);
    }
    return;
  }
  biEnter(BI_INFLATE, now);
  Serial.print(F("[BagInit] inflate "));
  if (bagInit.inflL) Serial.print('L');
  if (bagInit.inflR) Serial.print('R');
  Serial.print(F(" 1/")); Serial.println(BAGINIT_INFLATE_PERIODS);
}

// ============================================================================
// PUBLIC ENTRY POINTS (buttons, menu, web)
// ============================================================================

// Start the full sequence (prompt "yes", menu "Bag init", web "start").
void bagInitStart(unsigned long now) {
  resetRegulationState();          // stop any regulation pulse, valves off
  bagInit.reason = BIR_NONE;
  bagInit.step   = 0;
  bagInit.baseL  = biRawLeft();
  bagInit.baseR  = biRawRight();
  Serial.print(F("[BagInit] start — raw L:")); Serial.print(bagInit.baseL);
  Serial.print(F(" R:")); Serial.println(bagInit.baseR);
  biEnter(BI_DEFLATE, now);
}

// Answer to a prompt (UP / web "yes" = true, DOWN / web "no" = false).
void bagInitAnswer(bool yes, unsigned long now) {
  if (bagInit.phase == BI_PROMPT) {
    if (yes) bagInitStart(now);
    else     biFault(BIR_DECLINED, now);
  } else if (bagInit.phase == BI_RETRY_PROMPT) {
    if (!yes)                                   biFault(bagInit.reason, now);
    else if (bagInit.reason == BIR_NO_RECOVERY) bagInitStart(now);   // redo all
    else                                        biStartInflate(now); // redo inflate
  }
}

// Abort a running phase (both buttons / web cancel).
void bagInitCancel(unsigned long now) {
  if (biIsRunning(bagInit)) {
    Serial.println(F("[BagInit] cancelled by user"));
    biFault(BIR_CANCELLED, now);
    armButtonReleaseLock();   // both buttons still held after the cancel
  }
}

// Restore FAULT after deep sleep (call once in setup()).
void bagInitRestore() {
  if (rtc_bagInitFault) {
    bagInit.phase  = BI_FAULT;
    bagInit.reason = BIR_OUT_OF_RANGE;
    Serial.println(F("[BagInit] FAULT restored from RTC — regulation blocked"));
  }
}

// ============================================================================
// STATE MACHINE — call every loop after the sensor update
// ============================================================================
void handleBagInit(unsigned long now) {
  if (!BAGINIT_ENABLED) return;

  switch (bagInit.phase) {

    // ── Watch for the out-of-range signature ──────────────────────────────
    case BI_IDLE: {
      if (manualControl.active || calibrationMode || isSetpointOff(targetPercent) ||
          !biIsExtreme(sensors.leftPercent, sensors.rightPercent)) {
        bagInit.conditionSince = 0;
        return;
      }
      if (bagInit.conditionSince == 0) { bagInit.conditionSince = now; return; }
      if (now - bagInit.conditionSince >= BAGINIT_TRIGGER_MS) {
        resetRegulationState();                  // stop regulating on bad data
        bagInit.reason = BIR_OUT_OF_RANGE;
        Serial.print(F("[BagInit] out-of-range state L:"));
        Serial.print(sensors.leftPercent); Serial.print(F("% R:"));
        Serial.print(sensors.rightPercent); Serial.println('%');
        biEnter(BI_PROMPT, now);
      }
      return;
    }

    // ── Prompts: UP = yes, DOWN = no (on release), timeout = no ──────────
    case BI_PROMPT:
    case BI_RETRY_PROMPT: {
      static bool upWas = false, dnWas = false;
      bool up = (digitalRead(BUTTON_UP)   == LOW);
      bool dn = (digitalRead(BUTTON_DOWN) == LOW);
      if (!bagInit.btnArmed) {                   // wait until all released
        if (!up && !dn) { bagInit.btnArmed = true; upWas = dnWas = false; }
        return;
      }
      if (now - bagInit.phaseStart >= BAGINIT_PROMPT_TIMEOUT) {
        Serial.println(F("[BagInit] no answer"));
        bagInitAnswer(false, now);
        return;
      }
      if (!up && upWas && !dnWas) { upWas = false; bagInitAnswer(true,  now); return; }
      if (!dn && dnWas && !upWas) { dnWas = false; bagInitAnswer(false, now); return; }
      if (up && dn) { upWas = dnWas = false; return; }   // both = ignore
      upWas = up; dnWas = dn;
      return;
    }

    // ── Running phases: both buttons = cancel ─────────────────────────────
    case BI_DEFLATE:
    case BI_DEFLATE_EXTRA:
    case BI_INFLATE: {
      if (digitalRead(BUTTON_UP) == LOW && digitalRead(BUTTON_DOWN) == LOW) {
        delay(50);
        if (digitalRead(BUTTON_UP) == LOW && digitalRead(BUTTON_DOWN) == LOW) {
          bagInitCancel(now);
          return;
        }
      }
      powerSave.lastActivityTime = now;
      biApplyValves();
      break;   // phase logic below
    }

    // ── Blocked: leave automatically once readings are plausible again ───
    case BI_FAULT: {
      if (biPairOk(sensors.leftPercent, sensors.rightPercent)) {
        if (bagInit.conditionSince == 0) bagInit.conditionSince = now;
        else if (now - bagInit.conditionSince >= BAGINIT_TRIGGER_MS) {
          Serial.println(F("[BagInit] readings plausible again — fault cleared"));
          biFinish(now);
        }
      } else {
        bagInit.conditionSince = 0;
      }
      return;
    }

    default: return;
  }

  // ── Timed phase logic (running phases only) ─────────────────────────────
  unsigned long el = now - bagInit.phaseStart;

  if (bagInit.phase == BI_DEFLATE && el >= BAGINIT_DEFLATE_MS) {
    allValvesOff();
    int rl = biRawLeft(), rr = biRawRight();
    int dl = abs(rl - bagInit.baseL), dr = abs(rr - bagInit.baseR);
    Serial.print(F("[BagInit] deflate probe: dL=")); Serial.print(dl);
    Serial.print(F(" dR=")); Serial.println(dr);

    if (dl <= BAGINIT_RAW_CHANGE && dr <= BAGINIT_RAW_CHANGE) {
      Serial.println(F("[BagInit] no change → bags empty, inflating"));
      biStartInflate(now);
    } else if (biPct(rl, true) > 0 && biPct(rr, false) > 0) {
      Serial.println(F("[BagInit] bags not empty, both > 0% → regulation"));
      biFinish(now);
    } else {
      Serial.println(F("[BagInit] bags not empty, deflating further"));
      bagInit.step = 1;
      biEnter(BI_DEFLATE_EXTRA, now);
    }
    return;
  }

  if (bagInit.phase == BI_DEFLATE_EXTRA && el >= BAGINIT_DEFLATE_MS) {
    allValvesOff();
    byte pl = biPct(biRawLeft(), true), pr = biPct(biRawRight(), false);
    Serial.print(F("[BagInit] deflate +")); Serial.print(bagInit.step);
    Serial.print(F(": L:")); Serial.print(pl); Serial.print(F("% R:"));
    Serial.print(pr); Serial.println('%');
    if (pl > 0 && pr > 0) { biFinish(now); return; }
    if (bagInit.step >= BAGINIT_EXTRA_DEFLATES) {
      bagInit.reason = BIR_NO_RECOVERY;
      biEnter(BI_RETRY_PROMPT, now);
      return;
    }
    bagInit.step++;
    bagInit.phaseStart = now;
    return;
  }

  if (bagInit.phase == BI_INFLATE) {
    // Check a few times per second: record change, stop early when OK.
    static unsigned long lastCheck = 0;
    if (now - lastCheck < 250 && el < BAGINIT_INFLATE_MS) return;
    lastCheck = now;

    int  rl = biRawLeft(), rr = biRawRight();
    byte pl = biPct(rl, true), pr = biPct(rr, false);
    if (abs(rl - bagInit.baseL) > BAGINIT_RAW_CHANGE) bagInit.changedL = true;
    if (abs(rr - bagInit.baseR) > BAGINIT_RAW_CHANGE) bagInit.changedR = true;

    if (biPairOk(pl, pr)) { biFinish(now); return; }

    // Close each side's valve as soon as that side reads OK (latched for
    // the rest of this phase, so noise around the threshold can't chatter).
    if (bagInit.inflL && biSideOk(pl)) {
      bagInit.inflL = false;
      Serial.print(F("[BagInit] L reached ")); Serial.print(pl); Serial.println('%');
    }
    if (bagInit.inflR && biSideOk(pr)) {
      bagInit.inflR = false;
      Serial.print(F("[BagInit] R reached ")); Serial.print(pr); Serial.println('%');
    }
    biApplyValves();

    bool allClosed = !bagInit.inflL && !bagInit.inflR;   // both OK, pair not
    if (el >= BAGINIT_INFLATE_MS || allClosed) {
      if (!allClosed && ++bagInit.step < BAGINIT_INFLATE_PERIODS) {
        bagInit.phaseStart = now;                        // next 10 s period
        Serial.print(F("[BagInit] inflate ")); Serial.print(bagInit.step + 1);
        Serial.print('/'); Serial.println(BAGINIT_INFLATE_PERIODS);
        return;
      }
      allValvesOff();
      bagInit.reason = (!bagInit.changedL && !bagInit.changedR)
                       ? BIR_NO_RESPONSE : BIR_NOT_IN_RANGE;
      Serial.print(F("[BagInit] after inflate: L:")); Serial.print(pl);
      Serial.print(F("% R:")); Serial.print(pr); Serial.println('%');
      biEnter(BI_RETRY_PROMPT, now);
    }
  }
}

#endif // BAG_INIT_H
