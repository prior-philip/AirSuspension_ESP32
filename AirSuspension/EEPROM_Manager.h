// EEPROM_Manager.h - EEPROM Read/Write Operations (ESP32-S3)
//
// On ESP32-S3, EEPROM is emulated in flash.
// Differences from AVR:
//   - EEPROM.begin(size) must be called once in setup() before any access.
//   - EEPROM.commit() must be called after any write to persist to flash.
//   - EEPROM.update() still works (only writes if value changed) but still
//     requires commit() to flush.
//   - int is 4 bytes on ESP32-S3 — the 4-byte address spacing already in the
//     layout is now an exact match (no wasted bytes).
//   - Checksum now covers all 4 bytes of each int field.
//   - EEPROM_VERSION bumped to 3 — calibration from a Nano is incompatible
//     (different ADC range and int size).
//
#ifndef EEPROM_MANAGER_H
#define EEPROM_MANAGER_H

#include <Arduino.h>
#include <EEPROM.h>
#include "Config.h"
#include "Types.h"

extern CalibrationData calibration;
extern byte targetPercent;
extern byte lastActiveSetpoint;

// ============================================================================
// CHECKSUM HELPER
// ============================================================================

// XOR all 4 bytes of a 32-bit int into the running checksum.
static inline void checksumAccum(byte& csum, int val) {
  csum ^= (byte)(val);
  csum ^= (byte)(val >> 8);
  csum ^= (byte)(val >> 16);
  csum ^= (byte)(val >> 24);
}

// ============================================================================
// EEPROM FUNCTIONS
// ============================================================================

// Load calibration from EEPROM.
// Call EEPROM.begin(EEPROM_SIZE) in setup() before calling this.
void loadCalibration() {
  byte storedVersion;
  EEPROM.get(EEPROM_VERSION_ADDR, storedVersion);

  if (storedVersion != EEPROM_VERSION) {
    calibration.valid = false;
    return;
  }

  int tempLeftMin, tempLeftMax, tempRightMin, tempRightMax;
  EEPROM.get(EEPROM_LEFT_MIN_ADDR,  tempLeftMin);
  EEPROM.get(EEPROM_LEFT_MAX_ADDR,  tempLeftMax);
  EEPROM.get(EEPROM_RIGHT_MIN_ADDR, tempRightMin);
  EEPROM.get(EEPROM_RIGHT_MAX_ADDR, tempRightMax);

  byte storedChecksum;
  EEPROM.get(EEPROM_CHECKSUM_ADDR, storedChecksum);

  byte calculatedChecksum = 0;
  checksumAccum(calculatedChecksum, tempLeftMin);
  checksumAccum(calculatedChecksum, tempLeftMax);
  checksumAccum(calculatedChecksum, tempRightMin);
  checksumAccum(calculatedChecksum, tempRightMax);

  if (storedChecksum != calculatedChecksum) {
    calibration.valid = false;
    return;
  }

  // Minimum span check — 12-bit ADC: meaningful range needs at least ~200 counts
  if (tempLeftMax  - tempLeftMin  < 200 ||
      tempRightMax - tempRightMin < 200) {
    calibration.valid = false;
    return;
  }

  calibration.leftMinRaw  = tempLeftMin;
  calibration.leftMaxRaw  = tempLeftMax;
  calibration.rightMinRaw = tempRightMin;
  calibration.rightMaxRaw = tempRightMax;
  calibration.valid = true;
}

// Save calibration to EEPROM.
// Call after a calibration procedure (write min/max into calibration struct first).
void saveCalibration() {
  byte checksum = 0;
  checksumAccum(checksum, calibration.leftMinRaw);
  checksumAccum(checksum, calibration.leftMaxRaw);
  checksumAccum(checksum, calibration.rightMinRaw);
  checksumAccum(checksum, calibration.rightMaxRaw);

  EEPROM.put(EEPROM_LEFT_MIN_ADDR,  calibration.leftMinRaw);
  EEPROM.put(EEPROM_LEFT_MAX_ADDR,  calibration.leftMaxRaw);
  EEPROM.put(EEPROM_RIGHT_MIN_ADDR, calibration.rightMinRaw);
  EEPROM.put(EEPROM_RIGHT_MAX_ADDR, calibration.rightMaxRaw);
  EEPROM.put(EEPROM_CHECKSUM_ADDR,  checksum);
  EEPROM.put(EEPROM_VERSION_ADDR,   (byte)EEPROM_VERSION);
  EEPROM.commit();
}

// Load setpoint from EEPROM.
void loadSetpoint() {
  byte stored;
  EEPROM.get(EEPROM_SETPOINT_ADDR, stored);

  if (stored != 255 && stored <= 100) {
    targetPercent = stored;
  }
  Serial.print(F("Setpoint: "));
  Serial.print(EEPROM_SETPOINT_ADDR);
  Serial.print(F(" = "));
  Serial.println(stored);
}

// Save setpoint to EEPROM.
void saveSetpoint() {
  EEPROM.put(EEPROM_SETPOINT_ADDR, targetPercent);
  EEPROM.commit();
}

// ── Setpoint to resume from OFF ──────────────────────────────────────────
// Stored in EEPROM (not just RAM) so a long-UP resume returns to the right
// value after deep sleep or a power cycle.  Outside the calibration block,
// so the calibration checksum is unaffected.  Invalid content (fresh flash
// 0xFF, >100, or the OFF sentinel itself) falls back to 50 %.
void loadLastActiveSetpoint() {
  byte stored;
  EEPROM.get(EEPROM_LAST_ACTIVE_ADDR, stored);
  lastActiveSetpoint = (stored <= 100 && !isSetpointOff(stored)) ? stored : 50;
}

// Remember the current setpoint before switching to OFF.  Written only on
// that transition, and only if the value actually changed (flash wear).
void rememberSetpointForResume() {
  if (isSetpointOff(targetPercent)) return;          // never store OFF itself
  if (lastActiveSetpoint == targetPercent) return;   // unchanged → no write
  lastActiveSetpoint = targetPercent;
  EEPROM.put(EEPROM_LAST_ACTIVE_ADDR, lastActiveSetpoint);
  EEPROM.commit();
}

#endif // EEPROM_MANAGER_H
