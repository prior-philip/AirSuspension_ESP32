# FCU — Front Control Unit

Controller board of the air suspension system. It carries the ESP32-S3
SuperMini, four low-side MOSFETs for the pneumatic valves, a plug-in
step-down module for the 3.3 V rail, and the connectors for the OLED, the
two buttons and the ride-height sensors.

| | |
|---|---|
| KiCad project | `FCU.kicad_pro` (KiCad 10) |
| Revision | 0.01 (schematic title block) |
| Board | 80.0 × 62.2 mm, 2 layers, 1.6 mm |
| Firmware | [`AirSuspension/`](../../AirSuspension) |

## Functions

- **MCU:** ESP32-S3 SuperMini module (U3).
- **Valve drivers:** four IRLZ44N logic-level MOSFETs (Q1–Q4), low-side
  switching. Each gate has a 100 Ω series resistor and a 10 k pulldown to
  GND, so valves stay off while the MCU boots or sleeps. Each valve coil has
  a 1N4003 flyback diode to +12 V (D1–D4).
- **Power:** 12 V input (P1/P2), 470 µF and 470 nF bulk/decoupling on 12 V.
  A plug-in step-down module (P2–P5, marked "3.3V out") makes the 3.3 V rail.
  D5 (1N4003) connects the SuperMini's 5 V pin to the 12 V rail, so USB power also feeds that rail. The
  schematic note says some step-down modules need an input voltage when the
  board is USB-powered, otherwise their output shorts 3.3 V to GND.
- **Sensor inputs:** two analog inputs with 47 pF filter capacitors. In the
  vehicle the sensors are powered from the RCU, which also divides their
  signals down to 3.3 V (see [`../RCU`](../RCU)). Connector J3 breaks out
  the two signals together with 3.3 V and GND.
- **Buttons:** UP and DOWN with 10 k pullups to 3.3 V. The schematic notes
  that the signals are lifted during deep sleep to prevent unintended wake-ups.

## Connectors

| Ref | Name | Pins |
|-----|------|------|
| P1 | Power In | 2×3 header: 12 V and GND pairs |
| J1 | Valve Power 12V | 2 pins, +12 V |
| J2 | Valve Ctrl | 8 pins: 1 L_UP, 3 L_DN, 5 R_UP, 7 R_DN, 8 GND (2, 4, 6 unconnected) |
| J3 | Sensor | 1 +3.3 V, 2 GND, 3 Sensor_L, 4 Sensor_R |
| J4 | OLED Display | 1 GND, 2 +3.3 V, 3 SCL, 4 SDA |
| P6 | Buttons | 1 Button_Down, 2 GND, 3 Button_Up |

## ESP32-S3 pin assignment (from the PCB netlist)

| Signal | GPIO | Function |
|--------|------|----------|
| Sensor_L | 1 | Left ride-height sensor (ADC1) |
| Sensor_R | 2 | Right ride-height sensor (ADC1) |
| Left_Inflate | 4 | Gate Q1 (net L_UP) |
| Left_Deflate | 5 | Gate Q2 (net L_DN) |
| Right_Inflate | 6 | Gate Q3 (net R_UP) |
| Right_Deflate | 7 | Gate Q4 (net R_DN) |
| SDA / SCL | 8 / 9 | OLED I²C |
| Button_Up | 12 | see check below |
| Button_Down | 13 | see check below |

Unused: GPIO 3, 10, 11, TX, RX.

## Open items to check

- **Button GPIOs.** The PCB wires `Button_Up` to GPIO 12 and `Button_Down`
  to GPIO 13. The firmware (`Config.h`) defines `BUTTON_DOWN = 12` and
  `BUTTON_UP = 13`. Either the buttons are wired crosswise on the harness, or
  UP and DOWN are swapped in one of the two places.
- **`FCU.csv` is out of date.** It lists an Arduino Nano, LM7805 and IRLU120,
  which are not in the current schematic. Regenerate the BOM from KiCad.
- **Fabrication files are older than the board.** The Gerber and drill files
  in `Documents/VW-1234-10001` date from October 2025, the board file from
  June 2026. Regenerate them before ordering.
- **Stray files.** `AirSuspension.kicad_pcb` has no matching project, and
  `FCU.xml` is an old netlist export.

## Files

| Path | Content |
|------|---------|
| `FCU.kicad_pro/.kicad_sch/.kicad_pcb` | KiCad project |
| `Documents/VW-1234-10001/` | Gerber, drill, placement files and a zip of the fabrication data |
| `Documents/VW-1234-20001/` | Schematic and assembly PDFs, netlist, layer PDFs |
| `Documents/FCU.pdf` | Current schematic PDF |
| `Documents/schemtic.pdf` | Older schematic PDF |
