# AirSuspension Controller — ESP32-S3 SuperMini

[![Build](https://github.com/prior-philip/AirSuspension_ESP32/actions/workflows/build.yml/badge.svg?branch=S3)](https://github.com/prior-philip/AirSuspension_ESP32/actions/workflows/build.yml)

Two-channel air suspension levelling controller for a campervan rear axle.
Reads two analog ride-height sensors, drives four valves (inflate/deflate per
side) from a tank with pressure-switched compressor, and shows state on a
128×128 SH1107 OLED and an on-demand WiFi web dashboard.

**Current release:** `Version v261026_1151`
`ESP32-S3 SuperMini | CAL | ManualMode | WiFi | OTA | flatBagInit | noPulsing`

---

## System overview

The controller replaces the vehicle's complete original air-suspension
control equipment. It consists of two boards connected by one shielded
cable, plus off-the-shelf parts:

```
  CABIN (left of steering wheel)                            REAR AXLE

  Fuse box 12 V ──► ┌───────────┐       shielded 12-core       ┌───────────┐
                    │    FCU    │ DE-9 #2 ───────────────────► │    RCU    │──► 4 valves (12 V)
  OLED SH1107 ──┐   │ ESP32-S3  │                  12-pin IP68 │  5 V sup. │──► 2 ride-height
  OEM buttons ──┴─► │ 4 MOSFETs │ DE-9 #1                      │  dividers │    sensors (5 V)
                    └───────────┘                              └───────────┘
                                                    Air springs (L/R) ◄── valves ◄── tank
  Compressor + tank with its own pressure switch:
  independent 12 V circuit, not controlled by this firmware.
```

### Components

| Component | Location | Part / specification | Connects to | Details |
|-----------|----------|----------------------|-------------|---------|
| **FCU** (Front Control Unit) | Cabin, next to the fuse box left of the steering wheel | Own PCB: ESP32-S3 SuperMini, 4 × IRLZ44N low-side valve MOSFETs, 12 V → 3.3 V step-down | 12 V from the fuse box; DE-9 #1 (display/buttons); DE-9 #2 (cable to RCU) | [`hardware/FCU`](hardware/FCU/README.md) |
| OLED display | Dashboard | I²C OLED 128 × 128, SH1107 controller, module MC01506 | FCU via DE-9 #1 | — |
| Buttons UP / DOWN | Dashboard | The vehicle's existing air-suspension buttons, reused | FCU via DE-9 #1 | — |
| Interconnect cable | Cabin → rear axle | Shielded, 12-core. DE-9 at the FCU, 12-pin IP68 connector (J4) at the RCU | FCU DE-9 #2 ↔ RCU J4 | Pinout: [RCU J4](hardware/RCU/README.md#connectors) |
| **RCU** (Rear Control Unit) | Rear axle | Own PCB: connector distribution, 12 V → 5 V sensor supply, 5 V → 3.3 V signal dividers | Cable (J4), valves (J3), sensors (J1) | [`hardware/RCU`](hardware/RCU/README.md) |
| Ride-height sensors (2) | Rear axle, left and right | Analog, 5 V supply. Original vehicle sensors, or BMW 37146870200 as replacement (see below) | RCU J1 | — |
| Valves (4) | Rear axle | 12 V solenoid valves, one inflate + one deflate per side; low-power coils, all four may be on together | RCU J3 (coil between +12 V and the switched line) | — |
| Compressor + tank | Vehicle | Any generic 12 V compressor with IPX water protection, ~6–8 bar, with its own pressure switch | Own fused 12 V circuit; air to the valves | — |
| Air springs (2) | Rear axle | Vehicle's air springs; inlet tubes restricted for slow, controllable flow | Valves | — |

### Ride-height sensors

- **Original sensors** connect directly to the RCU. They have not been tested
  with this controller yet but are expected to work.
- **Replacement:** **BMW 37146870200** is a good and cheaper fit (used on the
  2 Series F45, X5 F15/F85 and X6 F16/F86). Pinning and connector are the
  same as the original sensor.
- With the BMW sensor, the **sensor arm must be extended by 7 cm**. The longer
  arm reduces the angle the sensor turns over the full suspension travel, so
  it stays within its electrical range. Without it the sensor runs past its
  end stop with fully deflated or fully inflated air springs, and the reading
  wraps to the opposite end (see *Bag init* under Key behaviour).
- **Calibrate after fitting** any sensor or changing the arm (Menu →
  Calibrate). The right sensor is mounted mirrored, so its signal is
  inverted in firmware.

### Compressor

The compressor is not switched by the firmware: it fills the tank under the
control of its own pressure switch, and the controller only opens and closes
the valves between tank and air springs. Any generic 12 V compressor with
IPX water protection that reaches about 6–8 bar works. Fuse it separately.

## Pin assignment (firmware)

| Function      | GPIO | Notes                                        |
|---------------|------|----------------------------------------------|
| Sensor left   | 1    | ADC1                                         |
| Sensor right  | 2    | ADC1, sensor mounted inverted (always)       |
| Inflate left  | 4    |                                              |
| Deflate left  | 5    |                                              |
| Inflate right | 6    |                                              |
| Deflate right | 7    |                                              |
| I²C SDA / SCL | 8 / 9| SH1107 128×128 OLED                          |
| Button DOWN   | 12   | EXT1 deep-sleep wake, RTC pull-up held       |
| Button UP     | 13   | EXT1 deep-sleep wake, RTC pull-up held       |
| Status LED    | 48   | Shared red LED / WS2812 on the SuperMini     |

Low-power valves: all four valves may be energised together (no interlock).

## Repository layout

| Folder              | Content                                                  |
|---------------------|----------------------------------------------------------|
| `AirSuspension/`    | Arduino sketch (firmware)                                |
| `hardware/FCU/`     | KiCad project: Front Control Unit, see [its README](hardware/FCU/README.md) |
| `hardware/RCU/`     | KiCad project: Rear Control Unit, see [its README](hardware/RCU/README.md) |

Firmware releases are tagged `v*`; board revisions use `fcu-*` / `rcu-*`
tags (for example `fcu-rev2`) and do not trigger a firmware build. KiCad
backups, lock files and caches are git-ignored.

## Hardware designs

Two boards, both designed in KiCad 10, 2 copper layers, through-hole parts.

| Board | Role | Size | Revision |
|-------|------|------|----------|
| **FCU** (Front Control Unit) | ESP32-S3 SuperMini, four valve MOSFETs, 12 V to 3.3 V step-down, OLED / button / sensor connectors | 80.0 × 62.2 mm | 0.01 |
| **RCU** (Rear Control Unit) | Connector distribution at the rear axle: valves, ride-height sensors, 12 V to 5 V sensor supply, 5 V to 3.3 V signal dividers | 55.9 × 45.7 mm | not set in the title block |

```
 12 V ──► FCU ──► valves (low-side MOSFETs) ─┐
          ESP32-S3                            ├── cable ──► RCU ──► 4 valves
          OLED, 2 buttons       sensor signals ◄┘                         2 ride-height sensors
```

The schematics are the source of truth for pin assignments. The GPIO table
in the *Pin assignment (firmware)* section above is what the firmware uses; `hardware/FCU/README.md`
lists what the board wires up, and the two should match.

## Setup

1. Clone the repo and open `AirSuspension/AirSuspension.ino` in the Arduino IDE 2.x.
2. Install the ESP32 board package (core 3.3.x) and the **U8g2** library.
3. **Create your secrets file:** copy `AirSuspension/secrets.h.example` to
   `AirSuspension/secrets.h` and set your own `WIFI_AP_PASS` (at least 8
   characters). `secrets.h` is git-ignored and never committed. Without it
   the build stops with an error.
4. Select the board settings below, then compile and upload.

The example file contains a shared default password. Always set your own
before flashing a board that goes into a vehicle.

### Automatic builds (GitHub Actions)

Every push to `S3` or `main`, and every pull request, is compiled by the
**Build** workflow (badge above). The `.bin` files are attached to each run
as artifacts. Pushing a tag such as `v1.0.0` also attaches them to a GitHub
Release.

CI builds use `secrets.h.example`, so the downloaded `.bin` files carry the
default password. For your own device, compile locally with your own
`secrets.h`.

## Build settings (Arduino IDE 2.x, core 3.3.x)

Board `ESP32S3 Dev Module` · USB CDC On Boot `Enabled` · USB Mode
`Hardware CDC and JTAG` · Upload Mode `UART0 / Hardware CDC`. Do not debug
deep sleep with JTAG/gdb attached.

## Operation

| Input                      | Action                                        |
|----------------------------|-----------------------------------------------|
| Tap UP / DOWN              | Setpoint ±10 % (acts on release)              |
| Hold DOWN ≥ 1 s            | **OFF** standby (setpoint 45 = sentinel)      |
| Hold UP ≥ 1 s (while OFF)  | Resume last active setpoint                   |
| Both short                 | Menu: Manual · Calibrate · WiFi · Bag init    |
| Both ≥ 1 s                 | Reboot                                        |
| Serial `7 8 9 / 1 2 3`     | Valve pulses L↑ ↑↑ R↑ / L↓ ↓↓ R↓              |
| Serial `4 / 6`             | Setpoint −/+10 %                              |
| Serial `c`, `5`, `?`       | Calibration, reboot, help                     |
| Serial `w`                 | WiFi / web UI on / off                        |

Web UI (auto-on for 20 min after power-on; menu → WiFi or serial `w`): dashboard with axle view, target slider (10 % steps),
dedicated OFF button, bag-init prompt/buttons, OTA update (Sketch → Export
Compiled Binary → Update tab).

## Key behaviour

- **Continuous regulation (ContReg).** A side out of band (±5 % parked,
  ±15 % driving, for 10 s) gets its valve held open until the fast estimate
  (median of last 3 oversampled reads) is within `STOP_MARGIN` of target;
  one `SETTLING_TIME` settle, then re-check.
- **Driving / rough terrain.** Deliberately *no* correction while the road
  keeps the signal bouncing through the band — level is restored after the
  van stops (60 s driving latch + 10 s entry delay).
- **Bag init (flatBagInit).** Detects the sensor-wrap signature
  (one side ≥ 95 %, other ≤ 5 % for 5 s), stops regulation and asks the user.
  Deflate probe 5 s → no change: inflate both sides 3 × 10 s (each side closes
  when OK) → change: extra 5 s deflates until both > 0 %. Failure/decline →
  FAULT (regulation blocked, survives sleep, auto-clears when readings are
  plausible, restart via menu/web).
- **Protections.** Regulation timeout 60 s → ERR (latched in RTC, cleared by
  setpoint change); anti-hunt lockout after 3 × no progress (5 %/10 s);
  both-button release lock after manual exit / menu / calibration / bag-init
  cancel.
- **Power.** WiFi auto-starts on power-on and switches off after 20 min
  (manual WiFi stays on until switched off).
  Deep sleep after 120 s idle & stationary; 8 s timer sniff,
  30 min housekeeping wake; never sleeps while WiFi is on, in manual,
  calibration or a bag-init prompt/phase.

## Main tunables (`Config.h`)

| Constant                     | Value  | Meaning                                   |
|------------------------------|--------|-------------------------------------------|
| `STATIONARY_WINDOW`          | 5 %    | Parked tolerance band                     |
| `DRIVING_TOLERANCE`          | 15 %   | Driving tolerance band                    |
| `DELAY_BEFORE_REGULATE`      | 10 s   | Continuous out-of-band time before acting |
| `STOP_MARGIN`                | 2 %    | Close valve this far before target — set ≈ measured coast |
| `SETTLING_TIME`              | 500 ms | Single settle after a correction          |
| `REGULATION_TIMEOUT`         | 60 s   | Max correction time → ERR                 |
| `DISPLAY_UPDATE_REGULATING`  | 250 ms | OLED refresh while a valve is open        |
| `AVERAGE_WINDOW`             | 15     | Slow average (display, driving, sleep)    |
| `BAGINIT_ENABLED`            | true   | Set false on the bench with floating inputs |

---

## Changelog

Newest first. Each release updates `VERSION` (`Version vYYMMDD_HHMM`) and the
one-line `DESCR` in the `.ino`.

### v261026_1151 — Display retry only when missing, faster boot, WiFi password in `secrets.h`
- **Display:** the forced full re-init every 60 s while the display was
  healthy is removed. The display is now re-initialised only when the 5 s
  I²C probe fails, and retried every 60 s while it is missing
  (`DISPLAY_FORCED_REINIT` → `DISPLAY_RETRY_INTERVAL`). Saves a blocking
  re-init and a possible flicker every minute.
- **Boot:** waiting for a USB serial host shortened from 3 s to 0.5 s — there
  is no host in the car, so every boot was 2.5 s slower than needed.
- **WiFi password** moved out of `Config.h` into the git-ignored `secrets.h`
  (template `secrets.h.example`); the build stops with an error if it is
  missing. See *Setup*.

### v261025_0828 — Auto-started WiFi switches off after 20 min
- WiFi auto-started at power-on now switches itself off after
  `WIFI_AUTOSTART_TIMEOUT_MS` (20 min, `Config.h`; 0 = never), then the
  normal 120 s idle rule applies before deep sleep.
- Switching WiFi manually (Menu → WiFi, serial `w`) cancels the timeout —
  manually enabled WiFi stays on until switched off.
- An OTA upload cannot be interrupted by the timeout (the upload runs inside
  the web handler, the timeout check is outside it).

### v261024_1153 — WiFi auto-start on power-on
- WiFi / web UI start automatically after a **power-on** (battery connected,
  cold start) — not after deep-sleep wakes, reboots or crash resets.
- Stays on until switched off (Menu → WiFi or serial `w`); while on, the
  controller does not deep-sleep. Switch: `WIFI_AUTOSTART_ON_POWERON` in
  `Config.h`.
- Started at the end of `setup()` after display init; sensor buffers are
  re-primed afterwards so no RF-calibration sample enters the averages.

### v261024_1151 — WiFi toggle over serial
- New serial command `w` (or `W`): toggles WiFi / web UI on and off, same
  code path as Menu → WiFi (shared `toggleWiFi()` in the `.ino`). Prints
  `[Web] AP ON: AirSuspension IP: 192.168.4.1` / `[Web] AP OFF`.
- `?` help text lists `w=WiFi on/off`.

### v261022_2032 — Display/I²C can no longer cause a watchdog boot loop (fix)
- **Fix:** with the I²C bus stuck low (loose/shorted SDA/SCL, missing
  pull-up, hung OLED) `initDisplay()` blocked > 4 s — `u8g2.begin()` reports
  success unconditionally and runs ~100 transactions, each waiting for the
  50 ms default timeout — and the task watchdog reset the board endlessly
  (`E (4275) task_wdt`). Same at runtime via the display health watchdog.
- `recoverI2CBus()` now checks that SDA and SCL idle HIGH and reports a stuck
  bus; `initDisplay()` probes the display address with one transaction before
  `u8g2.begin()`. No ACK or stuck bus → controller runs **headless**
  (regulation, buttons, web UI unaffected) and retries every 60 s.
- I²C transaction timeout bounded to `I2C_TIMEOUT_MS` = 20 ms; watchdog fed
  between init attempts.
- Boot log no longer claims "Display: OK" when the OLED is absent.
- `SH1107_I2C_ADDR` moved from the `.ino` to `Config.h`.

### v260930_1343 — OTA hardening
- **Fix:** an aborted upload (browser closed, WiFi lost) left the watchdog
  disabled until the next reboot. Every OTA exit path now re-enables it.
- Upload start ends any regulation run and cancels a running bag init, so no
  stale activity resumes after a failed upload.
- Wrong-chip images rejected on the first chunk with a readable message
  (chip ID from the image header vs. this board).
- Update tab shows running version/partition and image vs. OTA-slot size;
  oversized images are refused in the browser before uploading.
- Real error text (e.g. "Not Enough Space") returned instead of "Update failed".
- New endpoint `GET /api/fwinfo`.

### v260930_1004 — Resume setpoint persists (fix)
- `lastActiveSetpoint` (target for hold-UP resume from OFF) is now stored in
  EEPROM (`EEPROM_LAST_ACTIVE_ADDR` = 19) and loaded on every boot/wake.
  Previously RAM-only → after deep sleep or power-off, resume went to 50 %.
- Web OFF button now stores the current setpoint for resume as well.
- Written only when entering OFF and only if changed; invalid content
  (fresh 0xFF, > 100, OFF sentinel) falls back to 50 %. Calibration block and
  checksum unaffected.

### v260930_0936 — Continuous regulation (Phase 2) + reliability fixes
**Regulation**
- Pulse train removed: valve held open per side until fast estimate is within
  `STOP_MARGIN` (2 %); single 500 ms settle, then re-check.
- New fast height estimate: median of last 3 reads (`leftFastPct/rightFastPct`).
- Every sensor read oversampled: 4 conversions, min/max dropped.
- A side only opens if the fast estimate confirms it is still off target
  (prevents re-open chatter while the slow average catches up).
- OLED refresh slowed to 250 ms while a valve is open (I²C blocking).
- OFF, bag init, lockout and manual-mode entry always abort a running
  correction (`abortRegulationRun()`).
- `REG_PULSE_TIME` removed; display shows "ADJ" instead of "PULSE".
- Simulated: 1 valve opening per correction (was 6–9), final error
  1.1–1.5 % (was 3.4–3.7 %), no overshoot/chatter with ±40-count noise.

**Fixes**
- Regulation timeout left `isRegulating` stuck → board never slept.
  Now stops cleanly; ERR latched in RTC (`rtc_regError`) so sleep/wake does
  not retry; cleared by setpoint change.
- Both-button release: exiting manual mode, cancelling the menu, leaving
  calibration or cancelling bag init with buttons still held reopened the
  menu or **rebooted**. Shared release lock (`armButtonReleaseLock()`).
  Blocking `delay(300)` on manual exit removed.

### Earlier changes (reconstructed, pre-changelog)
- **Bag init v2:** inflate both sides simultaneously 3 × 10 s with per-side
  cutoff (was alternating L,R,L,R,L,R).
- **Bag init v1:** out-of-range detection, prompt, deflate probe, inflate
  phase, retry prompt, FAULT with RTC persistence; menu entry + web card.
- Suppression (slope/rough/safe) code removed incl. the live slope rule in
  the sleep wake check; OFF mode no longer wakes to "regulate".
- Single-valve interlock added then removed (low-power valves fitted).
- OFF standby via long-press (hold DOWN = OFF, hold UP = resume); web OFF
  button; slider snaps to 10 %.
- Phase 1 settling: 1.5 s regulation pulse / 0.3 s settle for restricted tubes.
- WiFi enabled keeps the board awake (idle sleep was switching WiFi off).
- Sensor filter: outlier re-prime requires agreeing samples (fixes phantom
  1 % dips).
- Serial input: CR/LF skipped, no blind flush (keys no longer dropped).
- Menu loop feeds the watchdog.
- Deep sleep: calibration loaded before the wake check; button pull-ups held
  via RTC domain; wake reason reported after USB-CDC re-enumeration.

---

## Open items

**Bugs / inconsistencies (from code review)**
- Uncalibrated: wake check (`quickSensorRead`) maps 0–4095, normal operation
  200–3800 → false movement wakes.
- Manual mode does not pause the out-of-band timers → regulation restarts
  immediately after leaving manual mode.
- `isVehicleDriving()` has side effects and is called from Regulation,
  Display and Debug → detection depends on display/debug cadence.
- Debug prints right raw ADC before inversion; "Tol 15%" hardcoded.
- Calibration accepts a floating pin (`CAL_MIN_SAFETY_LOW = 0`); span check
  200 (load) vs 1000 (validate).
- LED writes in `Valves.h`/`SerialControl.h` are overridden by the loop.
- `REGULATION_TIMEOUT` also used as the driving latch.
- Anti-hunt threshold (5 %/10 s) not yet re-sized for restricted tubes.
- `bagInitRestore()` always restores reason "Out of range".
- Dead code: button ISRs + `wakeupFlag`, `powerSave.sleepCycles`, unused
  manual-mode fields/`downWasPressed`, commented blocks, stale comments
  (pin table, LED section, OFF stepping, `Sensors.h` header/thresholds).

- Serial `w` (WiFi on/off, v261024_1151) is missing on the `S3` branch: an
  older `SerialControl.h` was committed. `toggleWiFi()` in the `.ino` is
  present, only the serial command and help text are gone.

**Hardware (details in the board READMEs)**
- Button GPIOs: the FCU PCB routes `Button_Up` to GPIO 12 and `Button_Down`
  to GPIO 13; the firmware defines `BUTTON_DOWN = 12`, `BUTTON_UP = 13`.
  Check the harness, or swap in one place ([FCU open items](hardware/FCU/README.md#open-items-to-check)).
- DE-9 #1 (display/buttons) and DE-9 #2 (FCU side of the RCU cable) pin
  assignments are not documented in the repository yet.
- FCU: BOM (`FCU.csv`) and Gerber/drill files are older than the board —
  regenerate before ordering. RCU: schematic has no title block/revision.

**Planned**
- OTA follow-ups (not built): rollback safety, LAN join + mDNS + ArduinoOTA,
  NVS settings tab, persistent log. Check partition scheme vs. sketch size.
- Measure flow rate and coast → tune `STOP_MARGIN`; measure loop time.
- Fit the 7 cm sensor arm extension (BMW 37146870200) so the electrical
  range covers the full air-spring travel; recalibrate afterwards.
- Broken bag / tube detection + pump-overheat protection (duty budget,
  per-side faults, "Clear fault" in menu/web, healthy side keeps working).

## Design decisions

- Right height sensor is always inverted (hardware); inversion not calibrated.
- No correction on rough terrain; level restored after stopping.
- OFF persists through reboot and deep sleep.
- Faults/errors are cleared by explicit user action, never silently by sleep.
