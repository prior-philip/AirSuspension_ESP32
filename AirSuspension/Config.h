// Config.h - Configuration Constants and Pin Definitions (ESP32-S3 SuperMini)
//
// Target board: ESP32-S3 SuperMini — the WeAct/clone variant with USB-C
// going straight to native USB; 13 GPIOs broken out on the header
// (GPIO 1–13), plus TX/RX (GPIO 43/44).  NOT the SuperMini Zero.
//
// Pin assignments below match the PCB schematic v1.  All four valves are
// contiguous on GPIO 3–7 along the left edge of the chip for clean PCB
// routing; buttons are next to each other on the right edge (GPIO 12/13);
// I²C and sensors are on their natural ADC-capable pins.
//
// ── ARDUINO IDE BOARD SETTINGS ─────────────────────────────────────────
//   Board:                "ESP32S3 Dev Module"
//   USB CDC On Boot:      "Enabled"        ← required for Serial.print
//   USB Mode:             "Hardware CDC and JTAG"
//   Upload Mode:          "UART0 / Hardware CDC"
//   Flash Size:           match your board (usually 8MB)
//   PSRAM:                Disabled (unless N8R8 variant)
//
// ── KEY ESP32-S3 NOTES ─────────────────────────────────────────────────
//   - LP_IO range is GPIO 0–21 (vs 0–7 on C6), so buttons on GPIO 12/13
//     CAN wake the chip from deep sleep via EXT1.
//   - ADC1 = GPIO 1–10 (safe with WiFi).  ADC2 = GPIO 11–20 (hijacked by
//     WiFi).  Sensors must be on ADC1.  GPIO 12/13 are ADC2 but used here
//     for digital input only — no analogRead(), so the ADC2/WiFi conflict
//     does not apply.
//   - Native USB CDC for serial — no FTDI/CP2102 chip.
//   - Onboard LED is a WS2812 on GPIO 48, not exposed on the header.
//     This build runs without a status LED; LED_PIN logic is stubbed out
//     (see "STATUS LED" section below).
//
#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================================
// VERSION
// ============================================================================
extern const char* VERSION;
extern const char* DESCR;

// ============================================================================
// PLATFORM
// ============================================================================
#define PLATFORM_ESP32S3
#define BOARD_S3_SUPERMINI

// ============================================================================
// WIFI ACCESS POINT
// ============================================================================
#define WIFI_AP_SSID  "AirSuspension"
// WIFI_AP_PASS (default AP password) lives in secrets.h (git-ignored). Copy
// secrets.h.example to secrets.h and set your own default.  A password set over
// serial ("p") is stored in NVS and takes precedence over this default.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Missing AirSuspension/secrets.h - copy secrets.h.example to secrets.h and set WIFI_AP_PASS"
#endif

// Lower TX power → smaller current spike at AP start-up + less EMI on
// I²C/ADC.  8 dBm ≈ 6 mW, plenty for in-cabin range.
// WiFi.setTxPower() takes 0.25 dBm units; 32 = 8 dBm.
#define WIFI_TX_POWER_QDBM  32

// Start WiFi / web UI automatically after a POWER-ON (battery connected /
// cold start) only — never after a deep-sleep wake, button/serial/OTA reboot
// or crash reset.  An auto-started AP switches itself off after
// WIFI_AUTOSTART_TIMEOUT_MS (0 = never).  Switching WiFi manually (Menu →
// WiFi, serial 'w') cancels that timeout: manual WiFi stays on until switched
// off.  NOTE: while WiFi is on the controller does not deep-sleep.
#define WIFI_AUTOSTART_ON_POWERON  true
const unsigned long WIFI_AUTOSTART_TIMEOUT_MS = 20UL * 60UL * 1000UL;   // 20 min

// ============================================================================
// WATCHDOG CONFIGURATION
// ============================================================================
#define WATCHDOG_ENABLED true
#define WATCHDOG_TIMEOUT_MS 4000

// ============================================================================
// ADC CONFIGURATION
// ============================================================================
// ESP32-S3:
//   ADC1: GPIO 1–10  — safe with WiFi active
//   ADC2: GPIO 11–20 — hijacked by the radio peripheral.  Do not use for
//                      analogRead() while WiFi is up.
#define ADC_MAX_RAW  4095
#define ADC_ATTEN    ADC_11db   // 0–3.1V input range


// ============================================================================
// HARDWARE PIN DEFINITIONS
// ============================================================================
//
//  Function          GPIO   ADC?    LP_IO  Notes
//  ────────────────────────────────────────────────────────────────────────
//  Sensor left       1      ADC1_CH0  ✓
//  Sensor right      2      ADC1_CH1  ✓
//  Inflate left      4      ADC1_CH3  ✓    Strapping (USB-JTAG sel) — see below
//  Deflate left      5      ADC1_CH4  ✓
//  Inflate right     6      ADC1_CH5  ✓
//  Deflate right     7      ADC1_CH6  ✓
//  I2C SDA           8      ADC1_CH7  ✓
//  I2C SCL           9      ADC1_CH8  ✓
//  Button DOWN       12     ADC2_CH1  ✓    Digital input — ADC2 conflict N/A
//  Button UP         13     ADC2_CH2  ✓    Digital input — ADC2 conflict N/A
//  Free              8, 10, 11
//  LED (disabled)    255                   No external LED on this board
// ────────────────────────────────────────────────────────────────────────
//
// GPIO 3 strapping-pin note:
//   GPIO 3 is sampled at reset to select USB/UART download mode (USB-JTAG
//   chip default).  For an OUTPUT driving a relay/MOSFET gate, this is
//   only a concern if the load actively pulls GPIO 3 LOW *during reset*.
//   With a normal high-impedance gate driver (MOSFET, ULN2003 input,
//   optocoupler LED through a resistor) the pin floats until the MCU
//   drives it, and the chip's internal pull resolves the strapping
//   sample correctly.  No action required for standard relay drivers.

const byte SENSOR_LEFT_PIN   = 1;
const byte SENSOR_RIGHT_PIN  = 2;
const byte INFLATE_LEFT_PIN  = 4;     // Strapping pin — see note above
const byte DEFLATE_LEFT_PIN  = 5;
const byte INFLATE_RIGHT_PIN = 6;
const byte DEFLATE_RIGHT_PIN = 7;
const byte BUTTON_DOWN       = 12;    // LP_IO — wakes from deep sleep
const byte BUTTON_UP         = 13;    // LP_IO — wakes from deep sleep

// ============================================================================
// I2C PIN DEFINITIONS
// ============================================================================
// GPIO 8 = SDA, GPIO 9 = SCL.  Neither pin has a strapping role or onboard
// peripheral on the SuperMini, so they're free for I²C.
const byte I2C_SDA_PIN = 8;
const byte I2C_SCL_PIN = 9;
const uint8_t  SH1107_I2C_ADDR = 0x3C;   // OLED 7-bit address
// Max time per I2C transaction.  Bounds how long a stuck or noisy bus can
// block the loop: a display init is ~100 transactions, so the old 50 ms
// default could add up to > 4 s and trip the task watchdog (boot loop).
const uint16_t I2C_TIMEOUT_MS  = 20;

// ============================================================================
// STATUS LED (disabled on this board — no external LED, onboard is WS2812)
// ============================================================================
//
// LED_PIN = 255 disables status-LED logic across the codebase.  All
// digitalWrite(LED_PIN, ...) calls become no-ops (Arduino-ESP32 silently
// ignores writes to invalid pin numbers).
//
// To re-enable a status LED later:
//   • External LED on header: set LED_PIN to any of GPIO 6, 10, or 11.
//   • Onboard WS2812 on GPIO 48: set LED_PIN to 48 AND replace the
//     digitalWrite() calls in the .ino's LED section with neopixelWrite().
//
const byte LED_PIN = 48;             // 255 = no LED (Arduino treats as invalid)

// ============================================================================
// CONTROL PARAMETERS
// ============================================================================
const byte STATIONARY_WINDOW   = 5;
const byte DRIVING_TOLERANCE   = 15;
const byte STEP_SIZE           = 10;
const byte AVERAGE_WINDOW      = 15;

// ── "OFF" SETPOINT ─────────────────────────────────────────────────────────
// A dedicated standby stop that sits BETWEEN 40% and 50% in the stepping order:
//   ... 30 → 40 → OFF → 50 → 60 ...
// 40 and 50 remain normal regulating setpoints; only OFF disables regulation
// and closes the valves.  OFF is represented by the sentinel value 45, which
// normal 10%-stepping can never otherwise land on, so it's unambiguous and
// persists through EEPROM (loadSetpoint accepts it, <=100).
const byte SETPOINT_OFF = 45;
inline bool isSetpointOff(byte sp) { return sp == SETPOINT_OFF; }

const unsigned int  DELAY_BEFORE_REGULATE = 10000;
const unsigned long REGULATION_TIMEOUT   = 60000;
const unsigned long VALVE_PULSE_TIME     = 500;    // serial/debug pulses only

// ── CONTINUOUS REGULATION (Phase 2) ──────────────────────────────────────
// A side that leaves the band (STATIONARY_WINDOW / DRIVING_TOLERANCE, for
// DELAY_BEFORE_REGULATE) gets its valve held OPEN until the FAST height
// estimate (median of the last 3 oversampled reads) is within STOP_MARGIN of
// the target.  Then one SETTLING_TIME wait lets coast and the slow average
// catch up before the next decision.  No pulse train, no per-pulse waits.
// STOP_MARGIN absorbs the coast after closing — tune from the coast
// measurement (how far a bag keeps moving after its valve closes).
const byte          STOP_MARGIN          = 2;      // % short of target to close
const unsigned long SETTLING_TIME        = 500;    // single settle after closing
const unsigned long USER_CHANGE_OVERRIDE = 30000;

// ============================================================================
// SENSOR OUTLIER RE-PRIME
// ============================================================================
// updateSensorReadings() rejects single samples that deviate from the rolling
// average by more than OUTLIER_THRESHOLD (in Sensors.h) — good against ADC
// glitches.  But a genuine large, *sustained* step (bench voltage change, fast
// suspension move) also exceeds that threshold and would otherwise be rejected
// forever, freezing the percentage.  After this many CONSECUTIVE rejections on
// a channel, the reading is treated as a real step and the buffer is re-primed
// to the new value.  Keep it low enough to feel responsive, high enough that a
// brief glitch burst doesn't trip it.
const byte OUTLIER_REPRIME_COUNT = 4;

// ============================================================================
// BAG INIT — out-of-range detection and guided initialization (BagInit.h)
// ============================================================================
// Sensors wrap to the opposite end when a bag is completely empty/overfull
// (e.g. both empty → L=100% / R=0%).  Trigger: one side >= HIGH and the other
// <= LOW for TRIGGER_MS.  Set BAGINIT_ENABLED false for bench work with
// floating/unconnected sensor inputs.
const bool          BAGINIT_ENABLED        = true;
const byte          BAGINIT_EXTREME_HIGH   = 95;     // %
const byte          BAGINIT_EXTREME_LOW    = 5;      // %
const unsigned long BAGINIT_TRIGGER_MS     = 5000;   // also: FAULT auto-clear time
const unsigned long BAGINIT_PROMPT_TIMEOUT = 60000;  // no answer → FAULT
const unsigned long BAGINIT_DEFLATE_MS     = 5000;   // deflate probe / extra step
const byte          BAGINIT_EXTRA_DEFLATES = 3;      // max extra 5 s deflate steps
const unsigned long BAGINIT_INFLATE_MS     = 10000;  // per inflate period
const byte          BAGINIT_INFLATE_PERIODS = 3;     // 3 x 10 s, both sides together
const int           BAGINIT_RAW_CHANGE     = 100;    // raw counts = "value changed"
const byte          BAGINIT_OK_MIN         = 10;     // plausible range for a side (%)
const byte          BAGINIT_OK_MAX         = 90;
const byte          BAGINIT_OK_MAX_DIFF    = 40;     // plausible max L/R difference (%)

// ============================================================================
// POWER SAVE CONFIGURATION
// ============================================================================
const unsigned long IDLE_TIMEOUT    = 120000;  // 120 seconds before sleep
const byte STABILITY_THRESHOLD      = 3;
const byte STABILITY_SAMPLES        = 25;
// Deep-sleep timer wake cadence: the chip wakes this often to sniff the
// air-bag pressures, then goes straight back to sleep if nothing changed.
// Expressed in microseconds for esp_sleep_enable_timer_wakeup().
const uint64_t SLEEP_CHECK_INTERVAL_US = 8000000;   // 8 s

// Number of timer-wake cycles before a forced full wake (housekeeping).
// Total ≈ SLEEP_CYCLES × (SLEEP_CHECK_INTERVAL_US / 1e6) seconds.
const uint16_t SLEEP_CYCLES         = 225;     // 225 × 8 s ≈ 30 min

// ============================================================================
// MANUAL CONTROL CONFIGURATION
// ============================================================================
const unsigned long MANUAL_MODE_BUTTON_HOLD = 1000;  // <1s=manual  >=1s=reset

// ============================================================================
// ANTI-HUNTING CONFIGURATION
// ============================================================================
const byte MAX_REGULATION_ATTEMPTS   = 3;
const unsigned long PROGRESS_CHECK_INTERVAL = 10000;
const unsigned long LOCKOUT_DURATION = 300000;

// ============================================================================
// DISPLAY CONFIGURATION
// ============================================================================
const byte MAX_BAR_HEIGHT          = 80;
const byte DISPLAY_UPDATE_INTERVAL = 200;   // idle refresh (ms)
const byte DISPLAY_UPDATE_ACTIVE   = 80;    // refresh while manual/cal/bag init (ms)
// While a valve is open for regulation the display is refreshed LESS often:
// each full OLED update blocks the loop for the I2C transfer (~50-100 ms),
// which would delay the stop decision.  Sensor reads get the time instead.
const unsigned int DISPLAY_UPDATE_REGULATING = 250;

// Sensor-read throttle: when the vehicle is parked, settled AND regulation
// is idle, reading the ADC every loop is wasteful and adds noise to the
// rolling average.  In that state we read once per SENSOR_IDLE_INTERVAL.
// In every other state (regulating, manual, calibration, OR any sign of
// movement) we read every loop for maximum precision.
//
// IMPORTANT: the throttle must NOT engage merely because regulation is
// idle — driving-detection and oscillation-detection need fast samples to
// notice movement in the first place.  It only engages when isVehicle
// Stationary() also reports settled, so a moving van is always read fast.
const unsigned long SENSOR_IDLE_INTERVAL = 1000;  // ms between reads when parked+idle

// ============================================================================
// EEPROM ADDRESSES
// ============================================================================
const byte EEPROM_LEFT_MIN_ADDR  = 0;   // 4 bytes
const byte EEPROM_LEFT_MAX_ADDR  = 4;   // 4 bytes
const byte EEPROM_RIGHT_MIN_ADDR = 8;   // 4 bytes
const byte EEPROM_RIGHT_MAX_ADDR = 12;  // 4 bytes
const byte EEPROM_CHECKSUM_ADDR  = 16;  // 1 byte
const byte EEPROM_VERSION_ADDR   = 17;  // 1 byte
const byte EEPROM_SETPOINT_ADDR  = 18;  // 1 byte
const byte EEPROM_LAST_ACTIVE_ADDR = 19; // 1 byte — setpoint to resume from OFF
const byte EEPROM_VERSION        = 3;
const byte EEPROM_SIZE           = 64;

// ============================================================================
// MISC
// ============================================================================
static const byte WINDOW_SIZE = 10;

const byte VALVE_LEFT_INFLATE  = 0;
const byte VALVE_LEFT_DEFLATE  = 1;
const byte VALVE_RIGHT_INFLATE = 2;
const byte VALVE_RIGHT_DEFLATE = 3;
const byte VALVE_COUNT         = 4;

#endif // CONFIG_H
