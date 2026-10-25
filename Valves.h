// Valves.h - Valve Control Functions
#ifndef VALVES_H
#define VALVES_H

#include <Arduino.h>
#include "Config.h"
#include "Types.h"

// External references to global state
extern SensorData sensors;
extern RegulationState regulation;
extern PulsedRegulation pulse;
extern byte targetPercent;

// ============================================================================
// VALVE CONTROL FUNCTIONS
// ============================================================================

// Turn off all valve outputs
void allValvesOff() {
  digitalWrite(INFLATE_LEFT_PIN, LOW);
  digitalWrite(DEFLATE_LEFT_PIN, LOW);
  digitalWrite(INFLATE_RIGHT_PIN, LOW);
  digitalWrite(DEFLATE_RIGHT_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
}

// Update valve states based on cached decisions
void updateValveStates() {
  digitalWrite(INFLATE_LEFT_PIN, pulse.leftNeedsInflate ? HIGH : LOW);
  digitalWrite(DEFLATE_LEFT_PIN, pulse.leftNeedsDeflate ? HIGH : LOW);
  digitalWrite(INFLATE_RIGHT_PIN, pulse.rightNeedsInflate ? HIGH : LOW);
  digitalWrite(DEFLATE_RIGHT_PIN, pulse.rightNeedsDeflate ? HIGH : LOW);
  if (digitalRead(INFLATE_LEFT_PIN) == HIGH) digitalWrite(LED_PIN, HIGH);
  if (digitalRead(DEFLATE_LEFT_PIN) == HIGH) digitalWrite(LED_PIN, HIGH);
  if (digitalRead(INFLATE_RIGHT_PIN) == HIGH) digitalWrite(LED_PIN, HIGH);
  if (digitalRead(DEFLATE_RIGHT_PIN) == HIGH) digitalWrite(LED_PIN, HIGH);
  
}

// Decide which valves to open at the start of a correction.
// Entry is decided on the slow average (shouldRegulate), but a side only
// opens if the FAST estimate is still beyond STOP_MARGIN in that direction —
// otherwise a lagging slow average right after a correction would re-open a
// valve that would have to close again immediately (chatter).
void cacheValveDecisions() {
  int t = targetPercent;
  pulse.leftNeedsInflate  = regulation.leftShouldRegulate  && ((int)sensors.leftFastPct  < t - STOP_MARGIN);
  pulse.leftNeedsDeflate  = regulation.leftShouldRegulate  && ((int)sensors.leftFastPct  > t + STOP_MARGIN);
  pulse.rightNeedsInflate = regulation.rightShouldRegulate && ((int)sensors.rightFastPct < t - STOP_MARGIN);
  pulse.rightNeedsDeflate = regulation.rightShouldRegulate && ((int)sensors.rightFastPct > t + STOP_MARGIN);
}

// Close each side's valve individually once its fast estimate is within
// STOP_MARGIN of the target.  Returns true while any valve is still open.
bool updateValveStops() {
  int t = targetPercent;
  if (pulse.leftNeedsInflate  && (int)sensors.leftFastPct  >= t - STOP_MARGIN) pulse.leftNeedsInflate  = false;
  if (pulse.leftNeedsDeflate  && (int)sensors.leftFastPct  <= t + STOP_MARGIN) pulse.leftNeedsDeflate  = false;
  if (pulse.rightNeedsInflate && (int)sensors.rightFastPct >= t - STOP_MARGIN) pulse.rightNeedsInflate = false;
  if (pulse.rightNeedsDeflate && (int)sensors.rightFastPct <= t + STOP_MARGIN) pulse.rightNeedsDeflate = false;
  updateValveStates();
  return pulse.leftNeedsInflate || pulse.leftNeedsDeflate ||
         pulse.rightNeedsInflate || pulse.rightNeedsDeflate;
}

#endif // VALVES_H
