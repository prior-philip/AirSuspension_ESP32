// Types.h - Data Structure Definitions
#ifndef TYPES_H
#define TYPES_H

#include <Arduino.h>
#include "Config.h"

// ============================================================================
// SENSOR DATA STRUCTURE
// ============================================================================
struct SensorData {
  int leftSamples[AVERAGE_WINDOW];
  int rightSamples[AVERAGE_WINDOW];
  byte sampleIndex;
  int avgLeft;
  int avgRight;
  byte leftPercent;
  byte rightPercent;
  // Fast estimate for regulation stop decisions (median of last 3 reads)
  int  fastRawL[3], fastRawR[3];
  byte fastIdx;
  byte leftFastPct;
  byte rightFastPct;
};

// ============================================================================
// CALIBRATION DATA STRUCTURE
// ============================================================================
struct CalibrationData {
  int leftMinRaw;
  int leftMaxRaw;
  int rightMinRaw;
  int rightMaxRaw;
  bool valid;
  bool rightInverted;
};

// ============================================================================
// REGULATION STATE STRUCTURE
// ============================================================================
struct RegulationState {
  bool isRegulating;
  bool leftShouldRegulate;
  bool rightShouldRegulate;
  bool errorState;
  unsigned long startTime;
  unsigned long leftOutSince;
  unsigned long rightOutSince;
};

// ============================================================================
// REGULATION VALVE STATE (continuous regulation — name kept for compatibility)
// ============================================================================
//   inValvePulse      = a correction is running, valves held open per side
//   inSettlingPeriod  = valves closed, single settle before re-evaluating
//   *Needs*           = which valve is open for each side
struct PulsedRegulation {
  bool inValvePulse;
  bool inSettlingPeriod;
  unsigned long valvePulseStartTime;
  unsigned long settlingStartTime;
  bool leftNeedsInflate;
  bool leftNeedsDeflate;
  bool rightNeedsInflate;
  bool rightNeedsDeflate;
};

// ============================================================================
// ANTI-HUNTING STATE STRUCTURE
// ============================================================================
struct AntiHuntingState {
  byte attempts;
  byte lastLeftPercent;
  byte lastRightPercent;
  unsigned long lastProgressCheck;
  unsigned long lockoutUntil;
  bool inLockout;  // TRUE when in lockout period
};

// ============================================================================
// MANUAL CONTROL STATE STRUCTURE
// ============================================================================
struct ManualControlState {
  bool active;                      // TRUE when in manual mode
  unsigned long activatedTime;      // When manual mode started
  unsigned long lastActivityTime;   // Last button action in manual mode
  byte selectedValve;               // 0=LeftInflate, 1=LeftDeflate, 2=RightInflate, 3=RightDeflate
  bool valveActive;                 // TRUE when valve is pulsing
  unsigned long valvePulseStart;    // When current pulse started
};

// ============================================================================
// POWER SAVE STATE STRUCTURE
// ============================================================================
struct PowerSaveState {
  bool active;
  bool displayOn;
  byte lastStableLeftPercent;
  byte lastStableRightPercent;
  byte stabilityCounter;
  unsigned long lastActivityTime;
  uint16_t sleepCycles;
  volatile bool wakeupFlag;
};

// ============================================================================
// BAG INIT (out-of-range detection / guided initialization) — see BagInit.h
// ============================================================================
enum BagInitPhase : byte {
  BI_IDLE          = 0,   // normal operation, watching for out-of-range
  BI_PROMPT        = 1,   // asking the user whether to run the init
  BI_DEFLATE       = 2,   // 5 s deflate probe, both sides
  BI_DEFLATE_EXTRA = 3,   // further 5 s deflate steps (bags were not empty)
  BI_INFLATE       = 4,   // 3 x 10 s inflate, both sides together
  BI_RETRY_PROMPT  = 5,   // asking the user whether to retry
  BI_FAULT         = 6    // blocked: regulation stopped until resolved
};

// Why the last prompt/fault was raised (display/web/serial text).
const byte BIR_NONE         = 0;
const byte BIR_OUT_OF_RANGE = 1;
const byte BIR_NO_RESPONSE  = 2;   // inflate: no raw change at all
const byte BIR_NOT_IN_RANGE = 3;   // inflate: moved, but not plausible yet
const byte BIR_NO_RECOVERY  = 4;   // extra deflates didn't bring both > 0%
const byte BIR_DECLINED     = 5;   // user said no / didn't answer
const byte BIR_CANCELLED    = 6;   // user aborted a running phase

struct BagInitState {
  BagInitPhase  phase;
  unsigned long phaseStart;       // start of current phase / period
  unsigned long conditionSince;   // trigger (IDLE) or clear (FAULT) timer
  byte          step;             // extra-deflate step or inflate period
  int           baseL, baseR;     // raw baselines for change detection
  bool          changedL, changedR;
  bool          inflL, inflR;     // inflate phase: side still being filled
  byte          reason;           // BIR_*
  bool          btnArmed;         // prompts: buttons released since entry
};

inline bool biIsRunning(const BagInitState& b) {
  return b.phase == BI_DEFLATE || b.phase == BI_DEFLATE_EXTRA || b.phase == BI_INFLATE;
}

#endif // TYPES_H