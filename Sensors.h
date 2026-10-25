// Sensors.h - Sensor Reading and Processing Functions (ESP32-C6)
//
// Robustness notes (ESP32-C6 + active WiFi):
//   The internal ADC occasionally returns wild values during WiFi TX bursts
//   — typically near 0 or near full-scale, not just the ±50-count noise the
//   datasheet quotes.  Without filtering, a single bad sample drags the
//   rolling average down for AVERAGE_WINDOW iterations, which the web UI
//   exposes as "raw 2288 but percent 9%" — fresh instantaneous read is fine,
//   but the average is poisoned by old zero-samples that haven't rotated out.
//
//   We defend at two layers before a sample is added to the rolling buffer:
//     1) Floor/ceiling rejection — values outside [RAW_FLOOR, RAW_CEILING]
//        are physically impossible for a working pressure transducer on this
//        ADC range, so they are dropped outright.
//     2) Outlier rejection — once the buffer is primed, samples that differ
//        from the current average by more than OUTLIER_THRESHOLD are also
//        dropped.  500 counts ≈ 12% of full scale; real pressure changes
//        between two loop iterations (~ms apart) cannot move that fast.
//   Dropped samples leave the previous entry at that buffer slot intact, so
//   the average stays based on the last known-good readings.
//
#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include "Config.h"
#include "Types.h"

extern SensorData sensors;
extern CalibrationData calibration;

// ============================================================================
// SAMPLE FILTERING THRESHOLDS
// ============================================================================
// Floor/ceiling: a working sensor on ADC_11db should never read this extreme.
// Below 50: pin pulled low (WiFi glitch, disconnected sensor, brownout)
// Above 4045: pin pulled high (WiFi glitch, sensor stuck, ground fault)
static const int RAW_FLOOR   = 20;
static const int RAW_CEILING = 4075;

// Max sample-to-average deviation before a sample is considered an outlier.
// 500 counts ≈ 12% of full ADC scale.  Increase if real signal changes
// faster than this between loop iterations (it normally cannot — pulsed
// valve regulation moves the bags slowly).
static const int OUTLIER_THRESHOLD = 1000;

// ============================================================================
// SENSOR FUNCTIONS
// ============================================================================

// Read right sensor with optional inversion.
// ESP32-C6 ADC: 12-bit (0–4095).  Inversion flips around full-scale.
int readRightSensor() {
  int rawValue = analogRead(SENSOR_RIGHT_PIN);
  return calibration.rightInverted ? (ADC_MAX_RAW - rawValue) : rawValue;
}

// Oversampled read: 4 conversions, drop min and max, average the middle two.
// Lowers ADC noise for every consumer and rejects a single wild conversion
// (e.g. a WiFi TX glitch) without diluting it into the result.
static int readTrimmed4(uint8_t pin) {
  int s[4];
  for (byte i = 0; i < 4; i++) s[i] = analogRead(pin);
  // tiny sort network for 4 values
  auto sw = [&](byte a, byte b){ if (s[a] > s[b]) { int t = s[a]; s[a] = s[b]; s[b] = t; } };
  sw(0,1); sw(2,3); sw(0,2); sw(1,3); sw(1,2);
  return (s[1] + s[2]) / 2;
}
static inline int readLeftOversampled()  { return readTrimmed4(SENSOR_LEFT_PIN); }
static inline int readRightOversampled() {
  int raw = readTrimmed4(SENSOR_RIGHT_PIN);
  return calibration.rightInverted ? (ADC_MAX_RAW - raw) : raw;
}

// Raw → percent with calibration (or the documented fallback range).
static inline byte rawToPercent(int raw, bool left) {
  if (calibration.valid) {
    return left ? constrain(map(raw, calibration.leftMinRaw,  calibration.leftMaxRaw,  0, 100), 0, 100)
                : constrain(map(raw, calibration.rightMinRaw, calibration.rightMaxRaw, 0, 100), 0, 100);
  }
  // Fallback when no calibration is stored — adjust, then run calibration.
  return constrain(map(raw, 200, 3800, 0, 100), 0, 100);
}

static inline int median3(int a, int b, int c) {
  return max(min(a, b), min(max(a, b), c));
}

// Returns true if a raw sample is acceptable for the rolling buffer.
//   primed = false → only floor/ceiling check (no average yet)
//   primed = true  → floor/ceiling AND deviation-from-average check
static inline bool isSampleAcceptable(int raw, int currentAvg, bool primed) {
  if (raw < RAW_FLOOR || raw > RAW_CEILING) return false;
  if (primed && abs(raw - currentAvg) > OUTLIER_THRESHOLD) return false;
  return true;
}

// Initialize rolling sample buffers on startup.
// analogSetAttenuation() must be called before this in setup().
// Retries individual reads up to 3 times if the first reading is wild —
// gives the ADC a moment to stabilise after WiFi init.
void initializeSensorSamples() {
  for (byte i = 0; i < AVERAGE_WINDOW; i++) {
    int leftRaw  = analogRead(SENSOR_LEFT_PIN);
    int rightRaw = readRightSensor();

    // Retry if either reads is obviously bad (likely during WiFi settling).
    for (byte retry = 0; retry < 3; retry++) {
      bool leftOk  = (leftRaw  >= RAW_FLOOR && leftRaw  <= RAW_CEILING);
      bool rightOk = (rightRaw >= RAW_FLOOR && rightRaw <= RAW_CEILING);
      if (leftOk && rightOk) break;
      delay(5);
      if (!leftOk)  leftRaw  = analogRead(SENSOR_LEFT_PIN);
      if (!rightOk) rightRaw = readRightSensor();
    }

    sensors.leftSamples[i]  = leftRaw;
    sensors.rightSamples[i] = rightRaw;
    delay(10);
  }
  sensors.sampleIndex = 0;

  // Seed the fast estimate with the last initial reads
  for (byte i = 0; i < 3; i++) {
    sensors.fastRawL[i] = sensors.leftSamples[AVERAGE_WINDOW - 1];
    sensors.fastRawR[i] = sensors.rightSamples[AVERAGE_WINDOW - 1];
  }
  sensors.fastIdx      = 0;
  sensors.leftFastPct  = rawToPercent(sensors.fastRawL[0], true);
  sensors.rightFastPct = rawToPercent(sensors.fastRawR[0], false);
}

// Update sensor readings with rolling average + outlier rejection.
void updateSensorReadings() {
  // Tracks whether the rolling buffer has at least one full pass of good
  // data — only then is outlier-vs-average rejection trustworthy.
  static bool primed = false;

  // Consecutive-rejection tracking.  Outlier rejection guards against single
  // ADC glitches, but a genuine large *sustained* step also exceeds
  // OUTLIER_THRESHOLD and would be rejected forever, freezing the average.
  // The escape hatch must distinguish a real step (a run of rejected samples
  // that AGREE with each other, clustered at a new level) from a lone glitch
  // followed by normal reads (rejected samples that DON'T agree).  We only
  // re-prime when OUTLIER_REPRIME_COUNT rejected-in-a-row samples all fall
  // within OUTLIER_THRESHOLD of the FIRST one in the run; any disagreeing
  // sample resets the run.  This kills the phantom 1% dips caused by a single
  // low glitch re-priming the buffer.
  static byte leftReject   = 0,  rightReject   = 0;
  static int  leftRejFirst = 0,  rightRejFirst = 0;  // value that opened the run

  int rawLeft  = readLeftOversampled();
  int rawRight = readRightOversampled();

  // ── FAST ESTIMATE (regulation stop decisions) ───────────────────────────
  // Median of the last 3 in-range reads: reacts within ~3 loops, but a single
  // bad read can never move it.  Hard floor/ceiling glitches are skipped.
  bool okL = (rawLeft  >= RAW_FLOOR && rawLeft  <= RAW_CEILING);
  bool okR = (rawRight >= RAW_FLOOR && rawRight <= RAW_CEILING);
  if (okL) sensors.fastRawL[sensors.fastIdx] = rawLeft;
  if (okR) sensors.fastRawR[sensors.fastIdx] = rawRight;
  sensors.fastIdx = (sensors.fastIdx + 1) % 3;
  sensors.leftFastPct  = rawToPercent(median3(sensors.fastRawL[0], sensors.fastRawL[1], sensors.fastRawL[2]), true);
  sensors.rightFastPct = rawToPercent(median3(sensors.fastRawR[0], sensors.fastRawR[1], sensors.fastRawR[2]), false);

  // ── LEFT CHANNEL ────────────────────────────────────────────────────────
  if (isSampleAcceptable(rawLeft, sensors.avgLeft, primed)) {
    sensors.leftSamples[sensors.sampleIndex] = rawLeft;
    leftReject = 0;
  } else if (rawLeft < RAW_FLOOR || rawLeft > RAW_CEILING) {
    // Hard floor/ceiling glitch — never a real step; drop and reset the run.
    leftReject = 0;
  } else {
    // In-band rejection.  Start or continue a run only while samples agree.
    if (leftReject == 0 || abs(rawLeft - leftRejFirst) <= OUTLIER_THRESHOLD) {
      if (leftReject == 0) leftRejFirst = rawLeft;
      leftReject++;
    } else {
      // Disagrees with the run (e.g. signal returned to normal) — restart.
      leftRejFirst = rawLeft;
      leftReject   = 1;
    }
    if (leftReject >= OUTLIER_REPRIME_COUNT) {     // sustained, agreeing step
      for (byte i = 0; i < AVERAGE_WINDOW; i++) sensors.leftSamples[i] = rawLeft;
      leftReject = 0;
    }
  }

  // ── RIGHT CHANNEL ───────────────────────────────────────────────────────
  if (isSampleAcceptable(rawRight, sensors.avgRight, primed)) {
    sensors.rightSamples[sensors.sampleIndex] = rawRight;
    rightReject = 0;
  } else if (rawRight < RAW_FLOOR || rawRight > RAW_CEILING) {
    rightReject = 0;
  } else {
    if (rightReject == 0 || abs(rawRight - rightRejFirst) <= OUTLIER_THRESHOLD) {
      if (rightReject == 0) rightRejFirst = rawRight;
      rightReject++;
    } else {
      rightRejFirst = rawRight;
      rightReject   = 1;
    }
    if (rightReject >= OUTLIER_REPRIME_COUNT) {
      for (byte i = 0; i < AVERAGE_WINDOW; i++) sensors.rightSamples[i] = rawRight;
      rightReject = 0;
    }
  }

  sensors.sampleIndex = (sensors.sampleIndex + 1) % AVERAGE_WINDOW;
  if (sensors.sampleIndex == 0) primed = true;   // completed one full pass

  long sumLeft = 0, sumRight = 0;
  for (byte i = 0; i < AVERAGE_WINDOW; i++) {
    sumLeft  += sensors.leftSamples[i];
    sumRight += sensors.rightSamples[i];
  }
  sensors.avgLeft  = sumLeft  / AVERAGE_WINDOW;
  sensors.avgRight = sumRight / AVERAGE_WINDOW;

  sensors.leftPercent  = rawToPercent(sensors.avgLeft,  true);
  sensors.rightPercent = rawToPercent(sensors.avgRight, false);
}

// Quick 5-sample read used by wake-from-sleep check (minimal averaging).
// Skips outlier rejection — by design we want a fast snapshot here.
SensorData quickSensorRead() {
  SensorData quick;
  long sumLeft = 0, sumRight = 0;

  for (byte i = 0; i < 5; i++) {
    sumLeft  += analogRead(SENSOR_LEFT_PIN);
    sumRight += readRightSensor();
    delay(50);
  }

  quick.avgLeft  = sumLeft  / 5;
  quick.avgRight = sumRight / 5;
  quick.leftPercent  = constrain(map(quick.avgLeft,  calibration.leftMinRaw,
                                     calibration.leftMaxRaw,  0, 100), 0, 100);
  quick.rightPercent = constrain(map(quick.avgRight, calibration.rightMinRaw,
                                     calibration.rightMaxRaw, 0, 100), 0, 100);
  return quick;
}

#endif // SENSORS_H
