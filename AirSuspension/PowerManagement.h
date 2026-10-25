// PowerManagement.h - Power Save and Watchdog Functions (ESP32-S3)
//
// DEEP SLEEP architecture:
//   esp_deep_sleep_start() fully resets the CPU on wake; setup() runs again
//   on every wake.  State that must survive sleep lives in RTC_DATA_ATTR
//   globals (defined in the .ino, declared extern here).  handleSleepWake()
//   is called from setup() to decide: full wake, or quick pressure-check then
//   back to sleep.  enterPowerSave() calls esp_deep_sleep_start() and never
//   returns.
//
//   WiFi is OFF during normal operation (started on demand from the button
//   menu), so sleep entry only needs to make sure it's off — there is no AP
//   to rebuild on wake.  The clean full-chip reset on wake also means USB CDC
//   serial re-enumerates normally.
//
#ifndef POWER_MANAGEMENT_H
#define POWER_MANAGEMENT_H

#include <Arduino.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <driver/rtc_io.h>   // rtc_gpio_pullup_en() — hold button pulls in sleep
#include <WiFi.h>
#include "Config.h"
#include "Types.h"
#include "Sensors.h"
#include "Valves.h"

extern SensorData     sensors;
extern CalibrationData calibration;
extern RegulationState regulation;
extern BagInitState     bagInit;
extern PowerSaveState  powerSave;
extern bool            displayAvailable;
extern bool            wifiEnabled;
extern ManualControlState manualControl;
extern bool            calibrationMode;
extern byte            targetPercent;
extern U8G2_SH1107_SEEED_128X128_F_HW_I2C u8g2;

// RTC memory — survives deep sleep (defined in .ino)
extern bool     rtc_wasInSleep;
extern uint16_t rtc_sleepCycles;
extern byte     rtc_stableLeft;
extern byte     rtc_stableRight;
extern byte     rtc_targetPercent;
extern bool     rtc_bagInitFault;   // bag-init FAULT latched across sleep
extern bool     rtc_regError;       // regulation timeout latched across sleep

// ============================================================================
// WATCHDOG FUNCTIONS
// ============================================================================

void reboot() {
  esp_restart();
}

inline void initWatchdog() {
#if WATCHDOG_ENABLED
  // Deinit any existing WDT config before applying ours
  esp_task_wdt_deinit();
  // Arduino ESP32 3.x uses config struct; 2.x uses (timeout_s, panic)
  // We use the 2.x-compatible call which also works on 3.x:
  esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms     = WATCHDOG_TIMEOUT_MS,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  esp_task_wdt_init(&wdt_cfg);
  esp_task_wdt_add(NULL);
  Serial.print(F("WDT: "));
  Serial.print(WATCHDOG_TIMEOUT_MS / 1000);
  Serial.println(F("s timeout"));
#else
  Serial.println(F("WDT: disabled"));
#endif
}

inline void resetWatchdog() {
#if WATCHDOG_ENABLED
  esp_task_wdt_reset();
#endif
}

inline void disableWatchdog() {
#if WATCHDOG_ENABLED
  esp_task_wdt_delete(NULL);
  esp_task_wdt_deinit();
#endif
}

inline void enableWatchdog() {
#if WATCHDOG_ENABLED
  esp_task_wdt_deinit();
  esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms     = WATCHDOG_TIMEOUT_MS,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  esp_task_wdt_init(&wdt_cfg);
  esp_task_wdt_add(NULL);
#endif
}

void printResetReason() {
  Serial.print(F("Reset: "));
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   Serial.println(F("Power-on"));    break;
    case ESP_RST_SW:        Serial.println(F("Software"));    break;
    case ESP_RST_PANIC:     Serial.println(F("Panic"));       break;
    case ESP_RST_INT_WDT:   Serial.println(F("Int WDT"));     break;
    case ESP_RST_TASK_WDT:  Serial.println(F("Task WDT"));    break;
    case ESP_RST_WDT:       Serial.println(F("WDT"));         break;
    case ESP_RST_DEEPSLEEP: Serial.println(F("Sleep wake"));  break;
    case ESP_RST_BROWNOUT:  Serial.println(F("Brown-out"));   break;
    default:                Serial.println(F("Unknown"));     break;
  }
}

// ============================================================================
// STABILITY / WAKE CONDITION HELPERS
// ============================================================================

bool sensorsChangedBy(byte leftCurrent,  byte rightCurrent,
                      byte leftBaseline, byte rightBaseline,
                      byte threshold) {
  bool leftChanged  = abs((int)leftCurrent  - (int)leftBaseline)  > threshold;
  bool rightChanged = abs((int)rightCurrent - (int)rightBaseline) > threshold;
  return leftChanged || rightChanged;
}

bool isVehicleStationary() {
  static unsigned long lastCheckTime = 0;
  unsigned long currentTime = millis();

  if (currentTime - lastCheckTime < 500) {
    return (powerSave.stabilityCounter >= STABILITY_SAMPLES);
  }
  lastCheckTime = currentTime;

  bool stable = !sensorsChangedBy(sensors.leftPercent, sensors.rightPercent,
                                  powerSave.lastStableLeftPercent,
                                  powerSave.lastStableRightPercent,
                                  STABILITY_THRESHOLD);
  if (stable) {
    powerSave.stabilityCounter++;
    if (powerSave.stabilityCounter >= STABILITY_SAMPLES) return true;
  } else {
    if (powerSave.stabilityCounter > 5) {
      powerSave.stabilityCounter -= 2;
    } else {
      powerSave.stabilityCounter = 0;
    }
    powerSave.lastStableLeftPercent  = sensors.leftPercent;
    powerSave.lastStableRightPercent = sensors.rightPercent;
  }
  return false;
}

enum WakeReason {
  WAKE_NONE       = 0,
  WAKE_MOVEMENT   = 1,
  WAKE_REGULATION = 2,
  WAKE_PERIODIC   = 4   // forced housekeeping wake after SLEEP_CYCLES
};

// ============================================================================
// WAKE REASON CAPTURE (for reliable serial reporting)
// ============================================================================
// USB-CDC drops on the deep-sleep reset and the host re-enumerates ~150 ms
// later, so the "Wake: ..." lines printed early in handleSleepWake() are
// usually lost.  We latch the cause/reason here and re-print them later in
// setup() (the "Sys ready" block reliably survives), via printWakeSummary().
extern esp_sleep_wakeup_cause_t lastWakeCause;   // defined in the .ino
extern WakeReason               lastWakeReason;  // defined in the .ino
extern uint16_t                 lastWakeCycles;  // defined in the .ino

inline const char* wakeCauseStr(esp_sleep_wakeup_cause_t c) {
  switch (c) {
    case ESP_SLEEP_WAKEUP_EXT1:      return "Button (EXT1)";
    case ESP_SLEEP_WAKEUP_TIMER:     return "Timer";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "Power-on / reset (not from sleep)";
    default:                         return "Other";
  }
}

inline const char* wakeReasonStr(WakeReason r) {
  switch (r) {
    case WAKE_MOVEMENT:   return "movement detected";
    case WAKE_REGULATION: return "regulation needed";
    case WAKE_PERIODIC:   return "periodic housekeeping";
    case WAKE_NONE:       return "none";
    default:              return "?";
  }
}

// Print the latched wake cause/reason.  Call from setup() AFTER USB-CDC has
// re-enumerated (e.g. in the "Sys ready" block) so it actually reaches the host.
inline void printWakeSummary() {
  Serial.print(F("Wake cause: "));
  Serial.print(wakeCauseStr(lastWakeCause));
  if (lastWakeCause == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.print(F("  ("));
    Serial.print(wakeReasonStr(lastWakeReason));
    Serial.print(F(", after "));
    Serial.print(lastWakeCycles);
    Serial.print(F(" sleep cycles)"));
  }
  Serial.println();
}

WakeReason checkWakeConditions(bool checkRegulation) {
  SensorData quick = quickSensorRead();

  bool moved = sensorsChangedBy(quick.leftPercent, quick.rightPercent,
                                powerSave.lastStableLeftPercent,
                                powerSave.lastStableRightPercent,
                                STABILITY_THRESHOLD);
  if (moved) {
    Serial.print(F("Movement: L:"));
    Serial.print(quick.leftPercent);
    Serial.print(F("%(was "));
    Serial.print(powerSave.lastStableLeftPercent);
    Serial.print(F(") R:"));
    Serial.print(quick.rightPercent);
    Serial.print(F("%(was "));
    Serial.print(powerSave.lastStableRightPercent);
    Serial.println(F(")"));
    return WAKE_MOVEMENT;
  }

  if (!checkRegulation) return WAKE_NONE;

  // OFF (standby): nothing regulates, so never wake just to regulate —
  // otherwise heights away from 45% cause a full wake every 8 s tick.
  if (isSetpointOff(targetPercent)) return WAKE_NONE;

  // Bag-init FAULT: regulation is blocked until the user acts, so a
  // "regulation needed" wake would only burn power.  Movement (above) and
  // buttons still wake as usual.
  if (rtc_bagInitFault) return WAKE_NONE;

  // Regulation timeout (ERR) latched: regulation stays off until the user
  // changes the setpoint, so don't wake just to "regulate" and retry.
  if (rtc_regError) return WAKE_NONE;

  bool needsReg = sensorsChangedBy(quick.leftPercent, quick.rightPercent,
                                   targetPercent, targetPercent,
                                   STATIONARY_WINDOW + 5);
  if (needsReg) {
    Serial.print(F("Wake: regulation needed L:"));
    Serial.print(quick.leftPercent);
    Serial.print(F("% R:"));
    Serial.print(quick.rightPercent);
    Serial.println(F("%"));
    return WAKE_REGULATION;
  }

  return WAKE_NONE;
}

// ============================================================================
// DEEP SLEEP WAKE SOURCE SETUP
// ============================================================================
// Arms the EXT1 (button) + periodic-timer wake sources AND holds the internal
// pull-ups on the button pins through deep sleep.
//
// Why the pull-up hold is required: pinMode(BUTTON_*, INPUT_PULLUP) in setup()
// configures a pull-up in the *digital* IO domain, which is powered down during
// deep sleep.  With ESP_EXT1_WAKEUP_ANY_LOW the pins then float, drift low and
// trigger a spurious "button" wake (field log: woke ~2 min after sleep, no
// press).  Re-asserting the pulls as RTC-domain pull-ups and keeping the RTC
// peripheral domain powered keeps GPIO 12/13 firmly HIGH while asleep.
//
// NOTE: external 10k pull-ups from each button pin to 3V3 are the bulletproof
// fix (immune to any power-domain behaviour and to in-cabin EMI).  This is the
// software-only mitigation for when external resistors aren't fitted.
inline void armDeepSleepWakeSources() {
  rtc_gpio_pullup_en((gpio_num_t)BUTTON_UP);
  rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_UP);
  rtc_gpio_pullup_en((gpio_num_t)BUTTON_DOWN);
  rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_DOWN);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  uint64_t wakeMask = (1ULL << BUTTON_UP) | (1ULL << BUTTON_DOWN);
  esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup(SLEEP_CHECK_INTERVAL_US);
}

// ============================================================================
// DEEP SLEEP ENTRY (called from normal operation — never returns)
// ============================================================================
void enterPowerSave() {
  if (powerSave.active) return;
  powerSave.active = true;

  if (displayAvailable) {
    u8g2.setPowerSave(1);
    powerSave.displayOn = false;
    Serial.println(F("Display OFF"));
  }

  allValvesOff();
  Serial.println(F("Entering deep sleep"));
  Serial.flush();

  // Make sure WiFi is off before sleeping.  In normal use it's already off
  // (started on demand from the menu), but if the user left it on we shut it
  // down cleanly here.  On wake the chip resets and WiFi stays off until the
  // user re-enables it from the menu.
  if (wifiEnabled) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiEnabled = false;
  }

  // Save state to RTC memory so it survives the reset on wake
  rtc_wasInSleep    = true;
  rtc_sleepCycles   = 0;
  rtc_stableLeft    = powerSave.lastStableLeftPercent;
  rtc_stableRight   = powerSave.lastStableRightPercent;
  rtc_targetPercent = targetPercent;

  // Arm wake sources (EXT1 buttons + periodic timer) WITH held pull-ups, so
  // the button pins don't float low and cause spurious wakes — see
  // armDeepSleepWakeSources() for the full rationale.
  armDeepSleepWakeSources();

  disableWatchdog();
  esp_deep_sleep_start();
  // Execution never reaches here — CPU resets on wake
}

// ============================================================================
// SLEEP WAKE HANDLER (called from setup() when rtc_wasInSleep is true)
//
// Returns true  → proceed with full setup() and run loop().
// Returns false → went back to sleep (esp_deep_sleep_start called); unreachable.
// ============================================================================
bool handleSleepWake() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  lastWakeCause = cause;          // latch for printWakeSummary() in setup()

  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println(F("Wake: button"));
    lastWakeReason = WAKE_NONE;   // EXT1 = physical button, no sensor reason
    lastWakeCycles = rtc_sleepCycles;
    rtc_wasInSleep  = false;
    rtc_sleepCycles = 0;
    powerSave.wakeupFlag = true;
    powerSave.lastStableLeftPercent  = rtc_stableLeft;
    powerSave.lastStableRightPercent = rtc_stableRight;
    targetPercent = rtc_targetPercent;
    return true;
  }

  // Timer wake — quick pressure check
  rtc_sleepCycles++;
  powerSave.lastStableLeftPercent  = rtc_stableLeft;
  powerSave.lastStableRightPercent = rtc_stableRight;
  targetPercent = rtc_targetPercent;

  WakeReason reason = checkWakeConditions(true);
  bool doFullWake = (reason == WAKE_MOVEMENT || reason == WAKE_REGULATION);

  if (!doFullWake && rtc_sleepCycles >= SLEEP_CYCLES) {
    Serial.println(F("[Periodic check]"));
    reason     = WAKE_PERIODIC;
    doFullWake = true;
  }

  if (doFullWake) {
    Serial.print(F("Full wake after "));
    Serial.print(rtc_sleepCycles);
    Serial.println(F(" cycles"));
    lastWakeReason = reason;            // movement / regulation / periodic
    lastWakeCycles = rtc_sleepCycles;
    rtc_wasInSleep  = false;
    rtc_sleepCycles = 0;
    return true;
  }

  // Still OK to sleep — go back immediately (same held-pull-up wake config).
  armDeepSleepWakeSources();
  esp_deep_sleep_start();
  return false;  // Never reached
}

// ============================================================================
// LOOP INTEGRATION
// ============================================================================
// Deep sleep is entered DIRECTLY by enterPowerSave() from checkForIdleTimeout()
// (and the menu/manual paths).  enterPowerSave() calls esp_deep_sleep_start()
// and never returns; the CPU resets on wake, which clears powerSave.active.
// There is no deferred sleep-flag shim — powerSave.active only marks the brief
// in-function transition.  (The previous handlePowerSaveMode() was dead logic:
// enterPowerSave()'s `if (active) return;` guard meant the shim could never
// actually sleep, and the loop-top check on active could wedge the loop.)

void checkForIdleTimeout(unsigned long currentMillis) {
  // Never idle-sleep while the web UI is up: deep sleep shuts WiFi off, which
  // is why "WiFi turns off after a while" — the whole board was sleeping.
  // While WiFi is enabled the user wants the controller awake to serve pages.
  if (wifiEnabled) return;
  // Likewise never sleep mid-mode (manual/calibration are interactive).
  if (manualControl.active || calibrationMode) return;

  // Never sleep during a bag-init prompt or running phase (FAULT may sleep).
  if (bagInit.phase != BI_IDLE && bagInit.phase != BI_FAULT) return;

  if (regulation.isRegulating) return;

  bool stationary = isVehicleStationary();

  if (stationary) {
    unsigned long idleTime = currentMillis - powerSave.lastActivityTime;
    if (idleTime >= IDLE_TIMEOUT) {
      Serial.println(F("Idle timeout — entering deep sleep"));
      enterPowerSave();  // Never returns
    }
  }
}

#endif // POWER_MANAGEMENT_H