// Buttons.h - Button Input Handling with Improved Two-Button Detection
#ifndef BUTTONS_H
#define BUTTONS_H

#include <Arduino.h>
#include "Config.h"
#include "Types.h"
#include "Regulation.h"
#include "EEPROM_Manager.h"

// External references to global state
extern PowerSaveState powerSave;
extern ManualControlState manualControl;
extern byte targetPercent;
extern byte lastActiveSetpoint;            // last non-OFF setpoint, for resume
extern void notifyUserChange(unsigned long);
// ============================================================================
// INTERRUPT HANDLERS
// ============================================================================

// Wake from sleep on button 1 (up)
void wakeFromButton1() {
  powerSave.wakeupFlag = true;
}

// Wake from sleep on button 2 (down)
void wakeFromButton2() {
  powerSave.wakeupFlag = true;
}

// ============================================================================
// RELEASE LOCK — ignore buttons until both are released
// ============================================================================
// Any mode that ends with a both-button gesture (manual exit, menu cancel,
// calibration abort, bag-init cancel) hands control back to the normal loop
// while the buttons may still be held.  Without this lock, checkBothButtons()
// then sees "both pressed": releasing opens the menu again, holding >1 s
// REBOOTS.  Arm the lock at those hand-over points; normal button handling
// resumes once both buttons have been released for BTN_LOCK_RELEASE_MS.
static const unsigned long BTN_LOCK_RELEASE_MS = 50;
static bool          btnLock         = false;
static bool          btnLockReleased = false;
static unsigned long btnLockFreeAt   = 0;

void armButtonReleaseLock() {
  btnLock = true;
  btnLockReleased = false;
}

// True while the lock is active.  Called at the top of the normal handlers.
static bool buttonLockActive() {
  if (!btnLock) return false;
  bool anyPressed = (digitalRead(BUTTON_UP) == LOW || digitalRead(BUTTON_DOWN) == LOW);
  if (anyPressed) { btnLockReleased = false; return true; }
  if (!btnLockReleased) { btnLockReleased = true; btnLockFreeAt = millis(); return true; }
  if (millis() - btnLockFreeAt < BTN_LOCK_RELEASE_MS) return true;   // debounce
  btnLock = false;
  return false;
}

// ============================================================================
// BOTH-BUTTON DETECTION
// ============================================================================

// Check if both buttons pressed simultaneously
bool bothButtonsPressed() {
  return (digitalRead(BUTTON_UP) == LOW && digitalRead(BUTTON_DOWN) == LOW);
}

// Unified both-button handler:
//   Short press (released before MANUAL_MODE_BUTTON_HOLD) → returns 1 → open menu
//   Long press  (held >= MANUAL_MODE_BUTTON_HOLD)         → returns 2 → system reset
//   No action                                             → returns 0
byte checkBothButtons(unsigned long currentMillis) {
  static unsigned long bothPressedStart = 0;
  static bool bothWerePressed = false;

  if (buttonLockActive()) {            // still held from a previous gesture
    bothPressedStart = 0;
    bothWerePressed  = false;
    return 0;
  }

  bool bothNow = bothButtonsPressed();

  if (bothNow) {
    if (!bothWerePressed) {
      bothPressedStart = currentMillis;   // Just pressed — start timer
      bothWerePressed = true;
    } else if (currentMillis - bothPressedStart >= MANUAL_MODE_BUTTON_HOLD) {
      // Held long enough — trigger reset immediately, don't wait for release
      bothPressedStart = 0;
      bothWerePressed = false;
      return 2;
    }
  } else if (bothWerePressed) {
    // Released before long-press threshold → short press → manual mode
    bothPressedStart = 0;
    bothWerePressed = false;
    return 1;
  }

  return 0;
}

// ============================================================================
// BUTTON PROCESSING WITH IMPROVED TWO-BUTTON DETECTION
// ============================================================================

// Handle button presses (normal mode).
//   Short tap UP   → +STEP_SIZE     Short tap DOWN → -STEP_SIZE
//   Hold  UP ≥1s   → resume from OFF (to last active setpoint)
//   Hold  DOWN ≥1s → OFF (standby: stop regulating, close valves)
// Short taps are IGNORED while OFF — resume is the deliberate long-UP gesture.
// Both buttons together are handled by checkBothButtons(); ignored here.
// NOTE: because long-press must be distinguished from a tap, a tap now acts on
// RELEASE (not on press) — this is required for hold detection to work.
bool handleButtons() {
  static unsigned long pressStart = 0;
  static byte heldBtn   = 0;     // 0=none, 1=UP, 2=DOWN
  static bool longFired = false;

  if (buttonLockActive()) {            // still held from a previous gesture
    heldBtn = 0; pressStart = 0; longFired = false;
    return false;
  }

  unsigned long now = millis();
  bool up = (digitalRead(BUTTON_UP)   == LOW);
  bool dn = (digitalRead(BUTTON_DOWN) == LOW);

  // Both buttons → checkBothButtons() owns this; clear our state.
  if (up && dn) {
    heldBtn = 0; pressStart = 0; longFired = false;
    return false;
  }

  // ── Release of a prior single press ─────────────────────────────────────
  if (!up && !dn) {
    bool acted = false;
    if (heldBtn != 0) {
      delay(15);   // debounce the release edge
      if (digitalRead(BUTTON_UP) == HIGH && digitalRead(BUTTON_DOWN) == HIGH) {
        if (!longFired && !isSetpointOff(targetPercent)) {
          // Short tap → one 10% step (only when not in OFF/standby).
          if (heldBtn == 1 && targetPercent < 100) {
            targetPercent = min(targetPercent + STEP_SIZE, 100);
            acted = true; Serial.print(F("UP->"));   Serial.println(targetPercent);
          } else if (heldBtn == 2 && targetPercent > 0) {
            targetPercent = max(targetPercent - STEP_SIZE, 0);
            acted = true; Serial.print(F("DOWN->")); Serial.println(targetPercent);
          }
          if (acted) { saveSetpoint(); resetRegulationState(); notifyUserChange(now); }
        }
        heldBtn = 0; pressStart = 0; longFired = false;
      }
      // else: contact bounce, still pressed — keep waiting
    }
    return acted;
  }

  // ── Exactly one button held ─────────────────────────────────────────────
  byte cur = up ? 1 : 2;
  if (heldBtn != cur) {            // new press begins — start the hold timer
    heldBtn    = cur;
    pressStart = now;
    longFired  = false;
    return false;
  }

  // Same button still held — fire once when the long-press threshold is met.
  if (!longFired && (now - pressStart >= MANUAL_MODE_BUTTON_HOLD)) {
    longFired = true;             // suppresses the tap action on release

    if (cur == 2) {               // ── Hold DOWN → OFF ──
      if (!isSetpointOff(targetPercent)) {
        rememberSetpointForResume();            // RAM + EEPROM, for resume
        targetPercent = SETPOINT_OFF;
        saveSetpoint(); resetRegulationState(); notifyUserChange(now);
        Serial.println(F("HOLD DOWN -> OFF (standby)"));
        return true;
      }
    } else {                      // ── Hold UP → resume ──
      if (isSetpointOff(targetPercent)) {
        targetPercent = isSetpointOff(lastActiveSetpoint) ? 50 : lastActiveSetpoint;
        saveSetpoint(); resetRegulationState(); notifyUserChange(now);
        Serial.print(F("HOLD UP -> resume ")); Serial.print(targetPercent); Serial.println('%');
        return true;
      }
    }
  }
  return false;
}

// ============================================================================
// OPTIONAL: More robust two-button detection if needed
// ============================================================================

// Alternative function with multi-sampling for noisy buttons
// Only use if simple method above doesn't work reliably
// bool handleButtonsWithSampling() {
//   bool buttonPressed = false;
  
//   // Take multiple samples for noise immunity
//   byte upCount = 0;
//   byte downCount = 0;
//   const byte samples = 5;
  
//   for (byte i = 0; i < samples; i++) {
//     if (digitalRead(BUTTON_UP) == LOW) upCount++;
//     if (digitalRead(BUTTON_DOWN) == LOW) downCount++;
//     delay(3);
//   }
  
//   // Require majority of samples to confirm press
//   bool upConfirmed = (upCount >= 3);
//   bool downConfirmed = (downCount >= 3);
  
//   // Check both first, then individuals
//   if (upConfirmed && downConfirmed) {
//     targetPercent = 50;
//     saveSetpoint();
//     resetRegulationState();
//     Serial.println(F("Both(sampled)->50%"));
//     delay(200);
//     buttonPressed = true;
//   }
//   else if (upConfirmed && targetPercent < 100) {
//     targetPercent = min(targetPercent + STEP_SIZE, 100);
//     saveSetpoint();
//     resetRegulationState();
//     delay(100);
//     buttonPressed = true;
//   }
//   else if (downConfirmed && targetPercent > 0) {
//     targetPercent = max(targetPercent - STEP_SIZE, 0);
//     saveSetpoint();
//     resetRegulationState();
//     delay(100);
//     buttonPressed = true;
//   }
  
//   return buttonPressed;
// }

#endif // BUTTONS_H