// AirSuspensionController.ino - Main Program (ESP32-S3 SuperMini)
// Modular air suspension control with pulsed regulation and power saving.
//
// Target board: ESP32-S3 SuperMini (WeAct/clone, USB-C → native USB,
// 13 GPIOs broken out: GPIO 1–13).  Pin assignments match the PCB
// schematic v1 (see Config.h for the full pinout table).
//
// ── ARDUINO IDE BOARD SETTINGS (easy to get wrong) ─────────────────────
//   Board:                "ESP32S3 Dev Module"
//   USB CDC On Boot:      "Enabled"        ← required for Serial output
//   USB Mode:             "Hardware CDC and JTAG"
//   Upload Mode:          "UART0 / Hardware CDC"
//
//   Without "USB CDC On Boot: Enabled", Serial.print() goes nowhere — the
//   Type-C connector wires straight to GPIO 19/20 (native USB), there is
//   no USB-UART bridge chip.
//
// ── KEY ESP32-S3 NOTES ─────────────────────────────────────────────────
//   - Dual-core Xtensa @ 240 MHz.  Arduino runs on core 1; no pinning.
//   - LP_IO range is GPIO 0–21, so buttons on GPIO 12/13 wake the chip
//     from deep sleep via EXT1.
//   - ADC1 (GPIO 1–10) is safe with WiFi; ADC2 (GPIO 11–20) is hijacked
//     by the radio peripheral.  Sensors are on ADC1.  GPIO 12/13 are
//     ADC2 but used digitally for buttons — no conflict.
//   - DEEP sleep: esp_deep_sleep_start() resets the CPU on wake; setup()
//     reruns and RTC_DATA_ATTR globals carry state across the reset.
//     The clean reset is also why WiFi + USB-CDC serial come back reliably
//     on wake.  See PowerManagement.h.
//   - EEPROM emulated in flash; requires begin() + commit().
//   - No external LED on this board (LED_PIN = 255 disables LED logic).
//
// ── WIFI ON DEMAND ─────────────────────────────────────────────────────
//   WiFi/web is OFF by default (no radio in normal use → low power, no ADC
//   or OLED disturbance).  Turn it on from the button menu (both-buttons
//   short press → "WiFi: OFF/ON" entry).  It stays on until toggled off,
//   a reboot, or deep sleep.  Because the radio is off at boot, none of the
//   radio-vs-ADC / radio-vs-OLED startup workarounds are needed in setup().
//   When WiFi is switched on we re-apply analogSetAttenuation() once, since
//   RF calibration can disturb the ADC range.
//
//   I²C is on GPIO 8/9 (no conflicts on the SuperMini).  Do NOT call
//   pinMode() on these pins anywhere outside Wire.begin().
//
// ============================================================================
// LIBRARIES
// ============================================================================
#include <Wire.h>
#include <U8g2lib.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>

// ============================================================================
// MODULE HEADERS (order matters for dependencies)
// ============================================================================
#include "Config.h"
#include "Types.h"
#include "Sensors.h"
#include "Valves.h"
#include "EEPROM_Manager.h"
#include "Regulation.h"
#include "Buttons.h"
#include "ManualControl.h"
#include "PowerManagement.h"
#include "SerialControl.h"
#include "BagInit.h"          // out-of-range detection / guided bag init
#include "Display.h"
#include "Calibration.h"
#include "WebInterface.h"
#include "Debug.h"

// ============================================================================
// LED HELPER (handles LED_PIN = 255 "disabled" sentinel cleanly)
// ============================================================================
// All status-LED writes from this file go through this helper so the rest
// of the code does not need conditional logic.  If LED_PIN is 255 (the
// "disabled" sentinel), calls become no-ops.  To enable an LED later,
// just change LED_PIN in Config.h — no code edits required here.
//
// NOTE: Valves.h and a few other headers still call digitalWrite(LED_PIN,
// LOW) directly when shutting valves off.  Arduino-ESP32 silently ignores
// writes to invalid pin numbers, so those calls are also safe with
// LED_PIN = 255 — they just don't do anything.

static inline void ledWrite(uint8_t state) {
  if (LED_PIN != 255) digitalWrite(LED_PIN, state);
}

// ============================================================================
// GLOBAL OBJECTS
// ============================================================================
U8G2_SH1107_SEEED_128X128_F_HW_I2C u8g2(U8G2_R1);

// ============================================================================
// RTC MEMORY — survives deep sleep (zeroed only on power-on reset)
// ============================================================================
RTC_DATA_ATTR bool     rtc_wasInSleep    = false;
RTC_DATA_ATTR uint16_t rtc_sleepCycles   = 0;
RTC_DATA_ATTR byte     rtc_stableLeft    = 50;
RTC_DATA_ATTR byte     rtc_stableRight   = 50;
RTC_DATA_ATTR byte     rtc_targetPercent = 50;
RTC_DATA_ATTR bool     rtc_bagInitFault  = false;   // bag-init FAULT survives sleep
RTC_DATA_ATTR bool     rtc_regError      = false;   // regulation timeout survives sleep

// ============================================================================
// GLOBAL STATE VARIABLES
// ============================================================================
SensorData        sensors     = {};
CalibrationData   calibration = {0, 4095, 0, 4095, false, true};
RegulationState   regulation  = {};
PulsedRegulation  pulse       = {};
BagInitState      bagInit     = {};
AntiHuntingState  antiHunt    = {};
ManualControlState manualControl = {};
PowerSaveState    powerSave   = {false, true, 50, 50, 0, 0, 0, false};

unsigned long lastUserChange = 0;
SerialControlState serialControl = {false, 0};

byte targetPercent      = 50;
byte lastActiveSetpoint = 50;   // resume target for OFF → long-UP; loaded from EEPROM in setup()
bool displayAvailable = false;
bool calibrationMode  = false;
bool wifiEnabled      = false;   // WiFi/web off by default; toggled from button menu
unsigned long lastDebug = 0;
bool          wifiAutoStarted   = false;   // AP was started by power-on auto-start
unsigned long wifiAutoStartedAt = 0;       // millis() of that start (for timeout)

// Latched at wake (see PowerManagement.h) so the reason can be printed reliably
// once USB-CDC has re-enumerated, instead of being lost in the early boot prints.
esp_sleep_wakeup_cause_t lastWakeCause  = ESP_SLEEP_WAKEUP_UNDEFINED;
WakeReason               lastWakeReason = WAKE_NONE;
uint16_t                 lastWakeCycles = 0;

// ============================================================================
// GLOBAL DEFINITIONS (declared extern in headers)
// ============================================================================
const char* VERSION = "Version v261025_0828";
const char* DESCR   = "ESP32-S3 SuperMini | CAL | ManualMode | WiFi | OTA | flatBagInit | noPulsing";



// SH1107 I²C address (Seeed 128×128 OLED) — used for liveness probing

// Display health watchdog cadence (ms)
static const unsigned long DISPLAY_PROBE_INTERVAL  = 5000;
static const unsigned long DISPLAY_FORCED_REINIT   = 60000;

// ============================================================================
// HELPER: apply ADC config (called from setup AND after WiFi init)
// ============================================================================
static inline void applyAdcConfig() {
  analogSetAttenuation(ADC_ATTEN);
  analogSetPinAttenuation(SENSOR_LEFT_PIN,  ADC_ATTEN);
  analogSetPinAttenuation(SENSOR_RIGHT_PIN, ADC_ATTEN);
}

// Toggle WiFi / web UI on demand — shared by the button menu and the serial
// 'w' command so both always behave identically.  startWebInterface() /
// stopWebInterface() print the AP state (SSID and IP when switching on).
void toggleWiFi() {
  wifiAutoStarted = false;        // any manual switch cancels the auto-off timer
  if (wifiEnabled) {
    stopWebInterface();
  } else {
    startWebInterface();
    // WiFi RF cal can disturb the ADC — re-apply attenuation after
    applyAdcConfig();
  }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {

   setCpuFrequencyMhz(160); //Reduce CPU freq to reduce power consumption and heat dissapation
   
  // ── SAFETY FIRST: valves LOW before anything else ──────────────────────
  pinMode(INFLATE_LEFT_PIN,  OUTPUT); digitalWrite(INFLATE_LEFT_PIN,  LOW);
  pinMode(DEFLATE_LEFT_PIN,  OUTPUT); digitalWrite(DEFLATE_LEFT_PIN,  LOW);
  pinMode(INFLATE_RIGHT_PIN, OUTPUT); digitalWrite(INFLATE_RIGHT_PIN, LOW);
  pinMode(DEFLATE_RIGHT_PIN, OUTPUT); digitalWrite(DEFLATE_RIGHT_PIN, LOW);

  // LED: only configure if enabled (LED_PIN != 255)
  if (LED_PIN != 255) {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
  }

  // ── OTHER PINS ──────────────────────────────────────────────────────────
  pinMode(SENSOR_LEFT_PIN,  INPUT);
  pinMode(SENSOR_RIGHT_PIN, INPUT);
  pinMode(BUTTON_UP,   INPUT_PULLUP);
  pinMode(BUTTON_DOWN, INPUT_PULLUP);
  // NOTE: do NOT call pinMode() on I2C_SDA_PIN (GPIO 8) or I2C_SCL_PIN
  // (GPIO 9).  Wire.begin() owns them.

  // ── ADC ATTENUATION (first application) ────────────────────────────────
  applyAdcConfig();

  // ── SERIAL ─────────────────────────────────────────────────────────────
  // Goes via USB CDC on this board — requires "USB CDC On Boot: Enabled"
  // in Arduino IDE board settings.
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { delay(10); }   // wait up to 3s for host
  delay(100);
  Serial.println("\n\n=== BOOT ===");  Serial.println();
  Serial.println("Running at: 160MHZ");
  Serial.println("\n\n");
  Serial.println(VERSION);
  Serial.println(F("Air Suspension Controller"));
  Serial.println(DESCR);
  Serial.println();

  printResetReason();

  // ── EEPROM + CALIBRATION + SETPOINT ────────────────────────────────────
  // MUST run BEFORE handleSleepWake().  The timer-wake check calls
  // quickSensorRead(), which converts raw ADC → percent using the
  // calibration struct (and its rightInverted flag).  If calibration isn't
  // loaded yet, the wake decision is made on the compile-time default values
  // {200,3800,200,3800,...} with the right channel inverted, so the computed
  // percentages almost never match the target/baseline — forcing a needless
  // full wake on every 8 s timer tick.  Loading calibration first fixes the
  // "announces deep sleep but stays awake and regulates" loop.
  EEPROM.begin(EEPROM_SIZE);

  loadCalibration();
  if (calibration.valid) {
    Serial.println(F("Calibration loaded"));
    Serial.print(F("  L: ")); Serial.print(calibration.leftMinRaw);
    Serial.print(F("-"));     Serial.println(calibration.leftMaxRaw);
    Serial.print(F("  R: ")); Serial.print(calibration.rightMinRaw);
    Serial.print(F("-"));     Serial.println(calibration.rightMaxRaw);
  } else {
    Serial.println(F("No cal — using defaults"));
  }

  loadSetpoint();
  loadLastActiveSetpoint();   // resume target for OFF → long-UP (survives sleep/power-off)
  Serial.print(F("Target: ")); Serial.print(targetPercent); Serial.println(F("%"));

  // ── WAKE FROM DEEP SLEEP (now calibration-aware) ───────────────────────
  // If we woke from deep sleep, handleSleepWake() either returns true (full
  // wake → finish setup and run loop) or re-enters deep sleep (never returns).
  // On a button/full wake it also restores targetPercent from RTC, overriding
  // the loadSetpoint() value above (RTC is the freshest copy, and equal to it).
  if (rtc_wasInSleep) {
    Serial.println(F("Resuming from deep sleep..."));
    initializeSensorSamples();
    if (!handleSleepWake()) {
      return;  // Unreachable — deep sleep was re-entered
    }
  }
  bagInitRestore();     // re-latch a bag-init FAULT that was active before sleep
  regulationRestore();  // re-latch a regulation timeout (ERR) likewise

  // ── I²C BUS (display init deferred until after WiFi is up) ─────────────
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // ── BUTTON INTERRUPTS (wake from sleep) ────────────────────────────────
  attachInterrupt(digitalPinToInterrupt(BUTTON_UP),   wakeFromButton1, FALLING);
  attachInterrupt(digitalPinToInterrupt(BUTTON_DOWN), wakeFromButton2, FALLING);

  // ── SENSORS (first read; will be re-primed after WiFi) ─────────────────
  initializeSensorSamples();
  updateSensorReadings();

  // ── POWER SAVE BASELINE ────────────────────────────────────────────────
  powerSave.lastActivityTime        = millis();
  powerSave.lastStableLeftPercent   = sensors.leftPercent;
  powerSave.lastStableRightPercent  = sensors.rightPercent;

  // ── VALVES OFF (redundant safety check) ────────────────────────────────
  allValvesOff();

  // ── WATCHDOG ───────────────────────────────────────────────────────────
  initWatchdog();

  // ── WEB ROUTES (radio stays OFF — started on demand from button menu) ──
  initWebInterface();   // registers routes only; does not power the radio

  // ── DISPLAY ─────────────────────────────────────────────────────────────
  // No WiFi running at boot, so none of the radio-vs-ADC / radio-vs-OLED
  // workarounds are needed here — just init the display normally.
  recoverI2CBus();
  displayAvailable = initDisplay();
  Serial.println(displayAvailable ? F("Display: OK") : F("Display: FAILED (headless)"));

  // ── WIFI AUTO-START ON POWER-ON ────────────────────────────────────────
  // Only a true power-on (not a deep-sleep wake, reboot or crash reset).
  // Done last, after display init (OLED init runs with the radio still off),
  // then the sensor buffers are re-primed so no sample taken during the
  // radio's RF calibration enters the averages.
  if (WIFI_AUTOSTART_ON_POWERON && esp_reset_reason() == ESP_RST_POWERON && !wifiEnabled) {
    Serial.println(F("[WiFi] Power-on → starting access point"));
    resetWatchdog();
    toggleWiFi();                 // same path as Menu → WiFi / serial 'w'
    wifiAutoStarted   = true;     // set AFTER toggleWiFi(), which clears it
    wifiAutoStartedAt = millis();
    if (WIFI_AUTOSTART_TIMEOUT_MS > 0) {
      Serial.print(F("[WiFi] Auto-off in "));
      Serial.print(WIFI_AUTOSTART_TIMEOUT_MS / 60000UL);
      Serial.println(F(" min (switch WiFi manually to keep it on)"));
    }
    resetWatchdog();
    initializeSensorSamples();    // re-prime after RF calibration
    updateSensorReadings();
  }

  Serial.println(F("============================================"));
  Serial.println(F("Sys ready"));
  printWakeSummary();   // reliable wake-reason report (USB-CDC is up by now)
  Serial.println(F("Buttons: UP/DN=Adjust"));
  Serial.println(F("         Both short=Menu  long=Reset"));
  printSerialControlInfo();
  Serial.println(F("============================================"));
  Serial.println();
}

// ============================================================================
// DISPLAY HEALTH WATCHDOG
// ============================================================================
void displayHealthWatchdog(unsigned long currentMillis) {
  static unsigned long lastProbe        = 0;
  static unsigned long lastForcedReinit = 0;

  if (!displayAvailable) {
    if (currentMillis - lastForcedReinit > DISPLAY_FORCED_REINIT) {
      Serial.println(F("Display: retry init"));
      displayAvailable  = initDisplay();
      lastForcedReinit  = currentMillis;
      lastProbe         = currentMillis;
    }
    return;
  }

  if (currentMillis - lastProbe >= DISPLAY_PROBE_INTERVAL) {
    Wire.beginTransmission(SH1107_I2C_ADDR);
    byte err = Wire.endTransmission();
    if (err != 0) {
      Serial.print(F("Display watchdog: I2C err "));
      Serial.print(err);
      Serial.println(F(" — re-init"));
      displayAvailable  = initDisplay();
      lastForcedReinit  = currentMillis;
    }
    lastProbe = currentMillis;
  }

  if (currentMillis - lastForcedReinit >= DISPLAY_FORCED_REINIT) {
    displayAvailable  = initDisplay();
    lastForcedReinit  = currentMillis;
  }
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  unsigned long currentMillis = millis();

  handleWebInterface();

  // Auto-started WiFi (power-on) times out; manually switched WiFi does not.
  // An OTA upload can't be cut off: it runs entirely inside
  // handleWebInterface(), so this check never runs during an upload.
  if (wifiAutoStarted && wifiEnabled && WIFI_AUTOSTART_TIMEOUT_MS > 0 &&
      currentMillis - wifiAutoStartedAt >= WIFI_AUTOSTART_TIMEOUT_MS) {
    Serial.println(F("[WiFi] Auto-start timeout → switching WiFi off"));
    toggleWiFi();                 // also clears wifiAutoStarted
    powerSave.lastActivityTime = currentMillis;   // normal 120 s idle before sleep
  }
  resetWatchdog();

  // Deep-sleep entry is triggered directly by checkForIdleTimeout() (and the
  // menu/manual paths) calling enterPowerSave(), which calls
  // esp_deep_sleep_start() and never returns — the CPU resets on wake.  There
  // is no deferred "sleep request" flag to service here, so we do NOT gate the
  // loop on powerSave.active.  (The old shim wedged the loop: it returned at
  // the top whenever active was true, but enterPowerSave() bails when active is
  // already true, so it could never actually sleep from that path.)

  // Serial valve commands stand off while a bag-init phase drives the valves.
  if (!manualControl.active && !biIsRunning(bagInit)) {
    handleSerialControl(currentMillis);
  }

  if (manualControl.active) {
    handleManualControl(currentMillis);
    resetWatchdog();
  }

  if (calibrationMode) {
    calibrationMode = handleCalibrationMode(currentMillis);
    if (!calibrationMode) armButtonReleaseLock();   // abort = both buttons held
    return;
  }

  // Buttons belong to the bag init during its prompts and running phases.
  if (!manualControl.active && !serialControl.valveActive && !bagInitOwnsButtons()) {
    byte bothResult = checkBothButtons(currentMillis);
    if (bothResult == 2) {
      allValvesOff();
      Serial.println(F("RESET"));
      reboot();
    } else if (bothResult == 1) {
      byte choice = displayAvailable ? showModeSelect() : 1;
      armButtonReleaseLock();   // menu may return (cancel) with both buttons held
      if (choice == 1) {
        enterManualMode(currentMillis);
      } else if (choice == 2) {
        enterCalibrationMode();
        calibrationMode = true;
      } else if (choice == 3) {
        toggleWiFi();             // WiFi / web UI on demand
        powerSave.lastActivityTime = currentMillis;
      } else if (choice == 4) {
        // Guided bag init: deflate probe, then inflate if the bags are empty
        bagInitStart(currentMillis);
      }
    } else if (handleButtons()) {
      powerSave.lastActivityTime = currentMillis;
    }
  }

  // ── "Active" = regulating, manual, calibration, or serial valve control.
  //    In any of these we want maximum-rate sensor reads and a snappy
  //    display.  Calibration is handled in its own branch above and never
  //    reaches here, but we include the flag for completeness.
  bool active = regulation.isRegulating || manualControl.active ||
                serialControl.valveActive || calibrationMode ||
                (bagInit.phase != BI_IDLE && bagInit.phase != BI_FAULT);

  // Sensor read: every loop when active OR when the vehicle is not settled
  // (so driving-detection still gets fast samples).  Throttle to
  // SENSOR_IDLE_INTERVAL only when parked, settled and idle.
  static unsigned long lastSensorRead = 0;
  bool settled = isVehicleStationary();   // also drives the sleep timer; cheap to call
  if (active || !settled ||
      (currentMillis - lastSensorRead >= SENSOR_IDLE_INTERVAL)) {
    updateSensorReadings();
    lastSensorRead = currentMillis;
  }
  resetWatchdog();

  // Out-of-range detection / guided bag init (owns valves while running)
  handleBagInit(currentMillis);

  if (!serialControl.valveActive) {
    updateRegulationNeeds(currentMillis);
  }

  checkRegulationProgress(currentMillis);

  if (!manualControl.active && !serialControl.valveActive) {
    performPulsedRegulation(currentMillis);
  }

  resetWatchdog();

  // ── Display update (adaptive rate) + health watchdog ───────────────────
  static unsigned long lastDisplayUpdate = 0;
  // While a regulation valve is open, refresh the OLED less often: each update
  // blocks the loop for the I2C transfer and would delay the stop decision.
  unsigned long displayInterval = pulse.inValvePulse ? DISPLAY_UPDATE_REGULATING
                                : active             ? DISPLAY_UPDATE_ACTIVE
                                                     : DISPLAY_UPDATE_INTERVAL;
  if (displayAvailable && powerSave.displayOn) {
    if (currentMillis - lastDisplayUpdate >= displayInterval) {
      updateDisplay(currentMillis);
      lastDisplayUpdate = currentMillis;
    }
  }
  displayHealthWatchdog(currentMillis);

  printDebugInfo(currentMillis);

  if (!manualControl.active && !serialControl.valveActive && !calibrationMode) {
    checkForIdleTimeout(currentMillis);
  }

  // ── Status LED (no-op when LED_PIN == 255) ─────────────────────────────
  if (regulation.errorState) {
    ledWrite(HIGH);
  } else if (regulation.isRegulating) {
    if (pulse.inSettlingPeriod) {
      ledWrite((currentMillis / 250) % 2);
    } else if (pulse.inValvePulse) {
      ledWrite(HIGH);
    }
  } else if (manualControl.active && manualControl.valveActive) {
    ledWrite(HIGH);
  } else {
    ledWrite(LOW);
  }

  resetWatchdog();
}
