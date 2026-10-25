// Debug.h - Serial Debug Output Functions (OPTIMIZED)
#ifndef DEBUG_H
#define DEBUG_H

#include <Arduino.h>
#include "Config.h"
#include "Types.h"

// External references to global state
extern SensorData sensors;
extern CalibrationData calibration;
extern RegulationState regulation;
extern AntiHuntingState antiHunt;
extern PowerSaveState powerSave;
extern byte targetPercent;
extern unsigned long lastDebug;

// ============================================================================
// DEBUG OUTPUT FUNCTIONS
// ============================================================================

// Print debug information to serial (OPTIMIZED - shorter strings)
void printDebugInfo(unsigned long currentMillis) {
  if (currentMillis - lastDebug < 2000) return;
  
  if (regulation.errorState) Serial.print(F("ERR  |  "));
  if (bagInit.phase != BI_IDLE) {
    Serial.print(F("BAGINIT:")); Serial.print(biPhaseName());
    if (bagInit.reason != BIR_NONE) { Serial.print(F(" (")); Serial.print(biReasonText()); Serial.print(')'); }
    Serial.print(F("  |  "));
  }

  Serial.print("SET:");
  if (isSetpointOff(targetPercent)) { Serial.print(F("OFF ")); }
  else { Serial.print(targetPercent); Serial.print("% "); }
  Serial.print("L:");
  Serial.print(sensors.leftPercent);
  Serial.print("%/R:");
   Serial.print(sensors.rightPercent);
  Serial.print("%");
      if (!calibration.valid) Serial.print(F(" (NoCal)"));

  Serial.print("  |  L_ADC:");
  Serial.print(analogRead(SENSOR_LEFT_PIN));
  Serial.print(" / ");
  Serial.print("R_ADC:");
  Serial.print(analogRead(SENSOR_RIGHT_PIN));
  Serial.println("");

  if (regulation.isRegulating) {
    Serial.print(F("Regulating: "));
    if (digitalRead(INFLATE_LEFT_PIN) == HIGH)  Serial.print(F("L↑ "));
    if (digitalRead(DEFLATE_LEFT_PIN) == HIGH)  Serial.print(F("L↓ "));
    if (digitalRead(INFLATE_RIGHT_PIN) == HIGH) Serial.print(F("R↑ "));
    if (digitalRead(DEFLATE_RIGHT_PIN) == HIGH) Serial.print(F("R↓ "));
  }
  
  if (isVehicleDriving()) {
        Serial.print(F("(Driving: Tol 15%)"));
      }
  
  if (antiHunt.attempts > 0) {
    Serial.print(F("Attempt:"));
    Serial.print(antiHunt.attempts);
  }
  
  if (antiHunt.inLockout) {
    Serial.print(F("Locked - remaining: "));
    Serial.print((antiHunt.lockoutUntil - currentMillis) / 1000);
    Serial.print(F("sec "));
  }
  
  Serial.println();
  lastDebug = currentMillis;
}

#endif // DEBUG_H