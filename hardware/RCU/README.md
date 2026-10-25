# RCU — Rear Control Unit

Small distribution board at the rear axle. It brings the valves and the two
ride-height sensors together on one 12-pin connector (J4) that leads to the FCU. It also makes
the 5 V sensor supply and divides the sensor signals down to a level the
ESP32-S3 can read.

| | |
|---|---|
| KiCad project | `RCU.kicad_pro` (KiCad 10) |
| Revision | not set (the schematic has no title block) |
| Board | 55.9 × 45.7 mm, 2 layers, 1.6 mm |
| Counterpart | [`hardware/FCU`](../FCU) |

## Functions

- **Connector distribution:** 12-pin input connector J4 (marked IP68) to the
  valve header J3 and the sensor header J1.
- **Sensor supply:** a plug-in step-down module (P2–P5, marked "5V out")
  makes +VSens (5 V) from the 12 V line. Capacitors C1–C6 decouple the
  12 V and 5 V rails.
- **Level shifter:** each sensor signal passes a 10 k series resistor and a
  20 k resistor to GND (R3/R1 for right, R4/R2 for left). A 5 V sensor
  output becomes about 3.3 V (5 V × 20 k / 30 k) at the FCU input.
- **Spare line:** J5 and J4 pin 11 carry an unused `SPARE` wire.

## Connectors

| Ref | Name | Pins |
|-----|------|------|
| J4 | Input connector IP68 (12 pin) | 1 +12 V, 2 L_INFLATE, 3 L_DEFLATE, 4 R_INFLATE, 5 R_DEFLATE, 6 +12 V, 7 Sensor_R, 8 GND, 9 Sensor_L, 10 GND, 11 SPARE, 12 not connected |
| J3 | Valves (8 pin) | 1 +12 V, 2 L_INFLATE, 3 +12 V, 4 L_DEFLATE, 5 +12 V, 6 R_INFLATE, 7 +12 V, 8 R_DEFLATE |
| J1 | Sensor (6 pin) | Two 3-pin sensors: 1 +VSens, 2 signal (to Sensor_R), 3 GND; 4 +VSens, 5 signal (to Sensor_L), 6 GND |
| J5 | Spare | SPARE |
| J2 | DE9 | in the schematic only: marked DNP, not placed on the board |

Valve coils sit between +12 V and the switched inflate/deflate lines, so the
FCU's low-side MOSFETs switch them.

## Signal names between the boards

| FCU net | RCU net | Meaning |
|---------|---------|---------|
| L_UP | L_INFLATE | Left valve, inflate |
| L_DN | L_DEFLATE | Left valve, deflate |
| R_UP | R_INFLATE | Right valve, inflate |
| R_DN | R_DEFLATE | Right valve, deflate |
| Sensor_L / Sensor_R | Sensor_L / Sensor_R | Sensor signals after the divider |

The cable between the boards is not part of this repository. Use the table
above when you build it.

## Open items to check

- **No revision.** The schematic has no title block, revision or date.
  Add them before the first release tag.

## Files

| Path | Content |
|------|---------|
| `RCU.kicad_pro/.kicad_sch/.kicad_pcb` | KiCad project |
| `Output/RCU.pdf` | Schematic PDF |
