// ManualControl.h - Manual Valve Control Functions
#ifndef MANUAL_CONTROL_H
#define MANUAL_CONTROL_H

#include <Arduino.h>
#include "Config.h"
#include "Types.h"
#include "Valves.h"
#include "Buttons.h"

// External references to global state
extern ManualControlState manualControl;
extern RegulationState regulation;
extern PowerSaveState powerSave;
extern SensorData sensors;

// ============================================================================
// MANUAL CONTROL CONSTANTS
// ============================================================================
// const unsigned long VALVE_PULSE_DURATION = 500;  // 500ms pulse

// // Valve selection indices
// const byte VALVE_LEFT_INFLATE = 0;
// const byte VALVE_LEFT_DEFLATE = 1;
// const byte VALVE_RIGHT_INFLATE = 2;
// const byte VALVE_RIGHT_DEFLATE = 3;
// const byte VALVE_COUNT = 4;

// ============================================================================
// MANUAL CONTROL FUNCTIONS
// ============================================================================

// Get valve name for display
const char* getValveName(byte valveIndex) {
  switch(valveIndex) {
    case VALVE_LEFT_INFLATE:  return "L-UP";
    case VALVE_LEFT_DEFLATE:  return "L-DN";
    case VALVE_RIGHT_INFLATE: return "R-UP";
    case VALVE_RIGHT_DEFLATE: return "R-DN";
    default: return "???";
  }
}

// Enter manual control mode
void enterManualMode(unsigned long currentMillis) {
  if (manualControl.active) return;
  
  Serial.println(F(">>> MANUAL MODE ACTIVATED <<<"));
  Serial.println(F("UP = Select Valve | DOWN = Pulse Valve | Both = Exit"));
  
  manualControl.active = true;
  manualControl.activatedTime = currentMillis;
  manualControl.lastActivityTime = currentMillis;
  manualControl.selectedValve = VALVE_LEFT_INFLATE;  // Start at first valve
  manualControl.valveActive = false;
  manualControl.valvePulseStart = 0;
  
  // Disable auto-regulation — end any running correction completely, so no
  // stale valve decision resumes after manual mode is left.
  abortRegulationRun();
  
  // Wake display if sleeping
  powerSave.lastActivityTime = currentMillis;
  
  Serial.print(F("Selected: "));
  Serial.println(getValveName(manualControl.selectedValve));
}

// Exit manual control mode
void exitManualMode(unsigned long currentMillis) {
  if (!manualControl.active) return;
  
  Serial.println(F("<<< MANUAL MODE EXITED >>>"));
  
  manualControl.active = false;
  manualControl.activatedTime = 0;
  manualControl.valveActive = false;
  
  // Turn off all valves
  allValvesOff();
  
  // Reset activity time
  powerSave.lastActivityTime = currentMillis;

  // Both buttons are usually still held here — don't let the normal loop
  // read that as a new menu/reboot gesture.
  armButtonReleaseLock();
}

// // Check for manual mode timeouts
// void checkManualModeTimeout(unsigned long currentMillis) {
//   if (!manualControl.active) return;
//   /*
//   // Check idle timeout (no button presses)
//   if (currentMillis - manualControl.lastActivityTime > MANUAL_MODE_TIMEOUT) {
//     Serial.println(F("Manual mode: Idle timeout"));
//     // exitManualMode(currentMillis);
//     return;
//   }
//   */
  
//   // Check maximum duration
//   if (currentMillis - manualControl.activatedTime > MANUAL_MODE_MAX_DURATION) {
//     Serial.println(F("Manual mode: Max duration reached"));
//     // exitManualMode(currentMillis);
//     return;
//   }
// }

// Activate the currently selected valve
void activateSelectedValve(byte valveIndex) {
  // Turn off all valves first
  allValvesOff();
  
  // Activate the selected valve
  switch(valveIndex) {
    case VALVE_LEFT_INFLATE:
      digitalWrite(INFLATE_LEFT_PIN, HIGH);
      Serial.print(F("Puls: L UP | "));
      break;
      
    case VALVE_LEFT_DEFLATE:
      digitalWrite(DEFLATE_LEFT_PIN, HIGH);
      Serial.print(F("Puls: L DN | "));
      break;
      
    case VALVE_RIGHT_INFLATE:
      digitalWrite(INFLATE_RIGHT_PIN, HIGH);
      Serial.print(F("Puls: R UP | "));
      break;
      
    case VALVE_RIGHT_DEFLATE:
      digitalWrite(DEFLATE_RIGHT_PIN, HIGH);
      Serial.print(F("Puls: R DN | "));
      break;
  }
  
  Serial.print(F("L:"));
  Serial.print(sensors.leftPercent);
  Serial.print(F("% R:"));
  Serial.print(sensors.rightPercent);
  Serial.println(F("%"));
}

// Handle manual valve control based on button presses
void handleManualControl(unsigned long currentMillis) {
  if (!manualControl.active) return;
  
  // Check for exit (both buttons pressed again)
  if (bothButtonsPressed()) {
    delay(100);  // Debounce
    if (bothButtonsPressed()) {  // Still pressed
      exitManualMode(currentMillis);   // arms the button release lock
      return;
    }
  }
  
  // // Handle active valve pulse
  // if (manualControl.valveActive) {
  //   if (currentMillis - manualControl.valvePulseStart >= VALVE_PULSE_DURATION) {
  //     // Pulse complete - turn off valve
  //     allValvesOff();
  //     manualControl.valveActive = false;
  //     Serial.println(F("Pulse complete"));
  //   }
  //   return;  // Don't process buttons during pulse
  // }
  
  // Read current button states
  static bool upWasPressed = false;
  static bool downWasPressed = false;
  
  bool upPressed = digitalRead(BUTTON_UP) == LOW;
  bool downPressed = digitalRead(BUTTON_DOWN) == LOW;
  
  // UP button - cycle through valves (on release to avoid rapid cycling)
  if (!upPressed && upWasPressed) {
    // Button released - cycle to next valve
    manualControl.selectedValve = (manualControl.selectedValve + 1) % VALVE_COUNT;
    manualControl.lastActivityTime = currentMillis;
    
    Serial.print(F("Selected: "));
    Serial.println(getValveName(manualControl.selectedValve));
  }
  upWasPressed = upPressed;
  
  // DOWN button - activate valve while held (not just downWasPressed condition)
  if (downPressed) {
    if (!manualControl.valveActive) {
      // Button just pressed - activate valve
      manualControl.valveActive = true;
      manualControl.lastActivityTime = currentMillis;
      activateSelectedValve(manualControl.selectedValve);
    }
    // Valve stays on while button held
  } else if (manualControl.valveActive) {
    // Button released - turn off valve
    allValvesOff();
    manualControl.valveActive = false;
    Serial.println(F("Released"));
  }
  downWasPressed = downPressed;
}

#endif // MANUAL_CONTROL_H