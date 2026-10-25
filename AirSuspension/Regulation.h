// Regulation.h - Main Regulation Control Logic
#ifndef REGULATION_H
#define REGULATION_H

#include <Arduino.h>
#include "Config.h"
#include "Types.h"
#include "Valves.h"

// External references to global state
extern SensorData sensors;
extern RegulationState regulation;
extern PulsedRegulation pulse;
extern BagInitState bagInit;      // BagInit.h owns valves while running
extern AntiHuntingState antiHunt;
extern PowerSaveState powerSave;
extern byte targetPercent;
// extern bool DrivingFlag = false;
extern unsigned long lastUserChange;  // Track when user last changed target
extern bool rtc_regError;             // regulation timeout latched across sleep
// USER_CHANGE_OVERRIDE is defined in Config.h


// ============================================================================
// DRIVING DETECTION - Simple movement detection
// ============================================================================
// More memory-efficient version using running min/max
bool isVehicleDriving() {
  
  // ADD: Timer variables for 60-second latch
  static unsigned long movementDetectedTime = 0;
  
  // Existing min/max tracking variables
  static byte leftMin = 255, leftMax = 0;
  static byte rightMin = 255, rightMax = 0;
  static byte cycleCounter = 0;
  
  // Current readings
  byte currentLeft = sensors.leftPercent;
  byte currentRight = sensors.rightPercent;

  // Reset min/max every WINDOW_SIZE cycles
  if (cycleCounter == 0) {
    leftMin = currentLeft;
    leftMax = currentLeft;
    rightMin = currentRight;
    rightMax = currentRight;
  } else {
    // Update min/max
    if (currentLeft < leftMin) leftMin = currentLeft;
    if (currentLeft > leftMax) leftMax = currentLeft;
    if (currentRight < rightMin) rightMin = currentRight;
    if (currentRight > rightMax) rightMax = currentRight;
  }
  
  // Increment counter (circular)
  cycleCounter = (cycleCounter + 1) % WINDOW_SIZE;
  
  // Calculate ranges
  byte leftRange = leftMax - leftMin;
  byte rightRange = rightMax - rightMin;
  
  // CHANGE: Check for movement and update timer
  bool currentMovement = (leftRange >= STATIONARY_WINDOW || rightRange >= STATIONARY_WINDOW);  // Detect movement, same value as Stationary window 
  if (currentMovement) {
    // Movement detected - update/set timer
    movementDetectedTime = millis();
  }
  
  // CHANGE: Return true if movement detected OR within latch period
  if (movementDetectedTime > 0) {
    unsigned long timeSinceMovement = millis() - movementDetectedTime;
    
    // Handle millis() overflow
    if (timeSinceMovement < REGULATION_TIMEOUT) {   // Set timeout to whatever REGULATION_TIMEOUT is (60sec in this case)
      return true;  // Still within 60-second window
    } else {
      movementDetectedTime = 0;  // Reset after 60 seconds
      return false;
    }
  }
  return false;  // No movement detected yet
}


// ============================================================================
// REGULATION CONTROL FUNCTIONS
// ============================================================================

// Update lockout status
void updateLockoutStatus(unsigned long currentMillis) {
  antiHunt.inLockout = (antiHunt.lockoutUntil > 0) && (currentMillis < antiHunt.lockoutUntil);
}




// Update which sides need regulation - WITH DYNAMIC TOLERANCE AND CURVE HANDLING
void updateRegulationNeeds(unsigned long currentMillis) {
  updateLockoutStatus(currentMillis);

  // "OFF" standby setpoint — no regulation at all, valves held closed.
  // (Callers that change the setpoint also call resetRegulationState(), which
  // closes the valves immediately; this gate keeps them closed thereafter.)
  if (isSetpointOff(targetPercent)) {
    regulation.leftShouldRegulate  = false;
    regulation.rightShouldRegulate = false;
    regulation.leftOutSince  = 0;
    regulation.rightOutSince = 0;
    regulation.isRegulating  = false;
    return;
  }

  // Bag init active (prompt, running, retry or FAULT) — sensor data is not
  // trustworthy or the init owns the valves.  No regulation.
  if (bagInit.phase != BI_IDLE) {
    regulation.leftShouldRegulate  = false;
    regulation.rightShouldRegulate = false;
    regulation.leftOutSince  = 0;
    regulation.rightOutSince = 0;
    regulation.isRegulating  = false;
    return;
  }

  if (antiHunt.inLockout || regulation.errorState) {
    regulation.leftShouldRegulate = false;
    regulation.rightShouldRegulate = false;
    regulation.leftOutSince = 0;
    regulation.rightOutSince = 0;
    regulation.isRegulating = false;
    return;
  }
  
  byte currentTolerance;
  bool leftOut, rightOut;
    // IMPROVED: Check if user recently changed target
  bool userChangeActive = (currentMillis - lastUserChange) < USER_CHANGE_OVERRIDE;
  
  if (isVehicleDriving() && !userChangeActive) {  // <-- KEY CHANGE
  // if (isVehicleDriving()) {
    currentTolerance = DRIVING_TOLERANCE;
    
    // KEY CHANGE: When driving, use average height to allow natural lean in curves
    byte avgHeight = (sensors.leftPercent + sensors.rightPercent) / 2;
    bool avgOut = abs((int)avgHeight - (int)targetPercent) > currentTolerance;
    
    // Only regulate individual sides if average is out AND that side needs adjustment
    leftOut = avgOut && (abs((int)sensors.leftPercent - (int)targetPercent) > currentTolerance);
    rightOut = avgOut && (abs((int)sensors.rightPercent - (int)targetPercent) > currentTolerance);
  } else {
    // Stationary - normal individual side regulation
    currentTolerance = STATIONARY_WINDOW;
    leftOut = abs((int)sensors.leftPercent - (int)targetPercent) > currentTolerance;
    rightOut = abs((int)sensors.rightPercent - (int)targetPercent) > currentTolerance;
  }
  
  // Track time out of tolerance (unchanged)
  if (leftOut) {
    if (regulation.leftOutSince == 0) regulation.leftOutSince = currentMillis;
  } else {
    regulation.leftOutSince = 0;
  }
  
  if (rightOut) {
    if (regulation.rightOutSince == 0) regulation.rightOutSince = currentMillis;
  } else {
    regulation.rightOutSince = 0;
  }
  
  // Only regulate after delay
  regulation.leftShouldRegulate = leftOut && 
                                  (currentMillis - regulation.leftOutSince >= DELAY_BEFORE_REGULATE);
  regulation.rightShouldRegulate = rightOut && 
                                   (currentMillis - regulation.rightOutSince >= DELAY_BEFORE_REGULATE);
}

// ADD THIS FUNCTION:
void notifyUserChange(unsigned long currentMillis) {
  lastUserChange = currentMillis;
}

// Check regulation progress for anti-hunting
void checkRegulationProgress(unsigned long currentMillis) {
  if (!regulation.isRegulating) return;
  if (currentMillis - antiHunt.lastProgressCheck < PROGRESS_CHECK_INTERVAL) return;
  
  // Check for measurable progress
  bool leftProgress = abs((int)sensors.leftPercent - antiHunt.lastLeftPercent) > STATIONARY_WINDOW;
  bool rightProgress = abs((int)sensors.rightPercent - antiHunt.lastRightPercent) > STATIONARY_WINDOW;
  
  // If should be regulating but no progress detected
  if ((regulation.leftShouldRegulate && !leftProgress) || 
      (regulation.rightShouldRegulate && !rightProgress)) {
    antiHunt.attempts++;
    
    if (antiHunt.attempts >= MAX_REGULATION_ATTEMPTS) {
      // Give up and enter lockout
      Serial.println(F("Anti-hunt: Stopping reg..."));
      antiHunt.lockoutUntil = currentMillis + LOCKOUT_DURATION;
      antiHunt.inLockout = true;  // Set lockout flag
      Serial.print(F("Locked for "));
      Serial.print(LOCKOUT_DURATION / 60000);
      Serial.println(F(" min"));
      
      // Clear regulation state
      regulation.leftShouldRegulate = false;
      regulation.rightShouldRegulate = false;
      regulation.leftOutSince = 0;
      regulation.rightOutSince = 0;
      antiHunt.attempts = 0;
      regulation.isRegulating = false;
      regulation.startTime = 0;
      
      // Clear pulsed regulation state
      pulse.inValvePulse = false;
      pulse.inSettlingPeriod = false;
      pulse.valvePulseStartTime = 0;
      pulse.settlingStartTime = 0;
      allValvesOff();
    }
  } else if (leftProgress || rightProgress) {
    antiHunt.attempts = 0;  // Reset on progress
  }
  
  // Update tracking
  antiHunt.lastLeftPercent = sensors.leftPercent;
  antiHunt.lastRightPercent = sensors.rightPercent;
  antiHunt.lastProgressCheck = currentMillis;
}

// End any running correction: valves closed, state machine back to idle and
// isRegulating cleared.  Used on errors, on OFF/bag-init/lockout, and when
// manual mode takes over.  (Previously the error branch left isRegulating
// true, which blocked deep sleep forever and made the web UI show
// "Regulating".)
void abortRegulationRun() {
  allValvesOff();
  regulation.isRegulating        = false;
  regulation.leftShouldRegulate  = false;
  regulation.rightShouldRegulate = false;
  regulation.startTime           = 0;
  antiHunt.attempts              = 0;
  pulse.inValvePulse             = false;
  pulse.inSettlingPeriod         = false;
  pulse.valvePulseStartTime      = 0;
  pulse.settlingStartTime        = 0;
  pulse.leftNeedsInflate  = pulse.leftNeedsDeflate  = false;
  pulse.rightNeedsInflate = pulse.rightNeedsDeflate = false;
}
static inline void stopRegulationForError() { abortRegulationRun(); }

// Re-latch a regulation timeout after deep sleep (call once in setup()).
void regulationRestore() {
  if (rtc_regError) {
    regulation.errorState = true;
    Serial.println(F("Regulation ERR restored from RTC — change setpoint to retry"));
  }
}

// Regulation valve control (continuous since Phase 2; name kept so the .ino
// and other callers stay unchanged).
void performPulsedRegulation(unsigned long currentMillis) {
  // While a bag-init phase is running it drives the valves — hands off.
  if (biIsRunning(bagInit)) return;

  if (regulation.errorState) {
    stopRegulationForError();
    return;
  }

  // Defensive: OFF, any bag-init state or an anti-hunt lockout always end a
  // running correction (the normal paths already reset it; this guarantees
  // no stale valve decision can survive a mode change).
  if (isSetpointOff(targetPercent) || bagInit.phase != BI_IDLE || antiHunt.inLockout) {
    if (pulse.inValvePulse || pulse.inSettlingPeriod || regulation.isRegulating) abortRegulationRun();
    allValvesOff();
    return;
  }

  // ── Continuous regulation state machine ─────────────────────────────────
  //   idle    : a side is out of band (slow average, after the entry delay)
  //             → open its valve, provided the fast estimate agrees
  //   running : keep valves open; each side closes on its own once its fast
  //             estimate is within STOP_MARGIN of the target
  //   settle  : all closed → one SETTLING_TIME wait, then re-evaluate
  bool anyShould = regulation.leftShouldRegulate || regulation.rightShouldRegulate;

  if (pulse.inValvePulse) {                                  // ── running ──
    if (currentMillis - regulation.startTime > REGULATION_TIMEOUT) {
      regulation.errorState = true;
      rtc_regError          = true;
      Serial.println(F("Regulation timeout -> ERR (change setpoint to retry)"));
      stopRegulationForError();
      return;
    }
    if (!updateValveStops()) {                               // all sides reached
      allValvesOff();
      pulse.inValvePulse      = false;
      pulse.inSettlingPeriod  = true;
      pulse.settlingStartTime = currentMillis;
    }
    return;
  }

  if (pulse.inSettlingPeriod) {                              // ── settle ──
    if (currentMillis - pulse.settlingStartTime < SETTLING_TIME) return;
    pulse.inSettlingPeriod = false;
    // fall through: re-evaluate below with settled readings
  }

  if (anyShould) {                                           // ── idle → start ──
    cacheValveDecisions();
    bool anyOpen = pulse.leftNeedsInflate || pulse.leftNeedsDeflate ||
                   pulse.rightNeedsInflate || pulse.rightNeedsDeflate;
    if (anyOpen) {
      if (regulation.startTime == 0) {                       // new correction
        regulation.startTime       = currentMillis;
        antiHunt.lastLeftPercent   = sensors.leftPercent;
        antiHunt.lastRightPercent  = sensors.rightPercent;
        antiHunt.lastProgressCheck = currentMillis;
      }
      powerSave.lastActivityTime = currentMillis;
      regulation.isRegulating    = true;
      pulse.inValvePulse         = true;
      pulse.valvePulseStartTime  = currentMillis;
      updateValveStates();
      return;
    }
    // Slow average still says "out", but the fast estimate is already within
    // STOP_MARGIN: the average is just catching up — nothing to do.
  }

  // Nothing to correct: idle
  regulation.isRegulating = false;
  regulation.startTime    = 0;
  antiHunt.attempts       = 0;
  allValvesOff();
}

// Reset all regulation state
void resetRegulationState() {
  regulation.errorState = false;
  rtc_regError          = false;   // user action clears the latched error
  // digitalWrite(LED_PIN, LOW);
  regulation.startTime = 0;
  regulation.leftOutSince = 0;
  regulation.rightOutSince = 0;
  antiHunt.attempts = 0;
  antiHunt.lastLeftPercent = 0;
  antiHunt.lastRightPercent = 0;
  antiHunt.lastProgressCheck = 0;
  antiHunt.lockoutUntil = 0;
  antiHunt.inLockout = false;  // Clear lockout flag
  pulse.inValvePulse = false;
  pulse.inSettlingPeriod = false;
  pulse.valvePulseStartTime = 0;
  pulse.settlingStartTime = 0;
  allValvesOff();
}

#endif // REGULATION_H