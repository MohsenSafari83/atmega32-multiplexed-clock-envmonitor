# Experiments & Test Log

*(Content moved from the report's "Testing and Results" chapter.)*

## Simulation Environment

- **Compiler/IDE:** CodeVisionAVR
- **Simulator:** Proteus Design Suite
- **Clock frequency:** 8 MHz
- **Display model:** 7SEG-MPX6-CA (6-digit common-anode)

[Insert figure here — `SSS.png`, Proteus simulation screenshot of the ATmega32 driving the 7SEG-MPX6-CA display.]

## Functional Test Results

| Test Case | Expected Result | Observed |
|---|---|---|
| Power-on display | TIME screen showing 12:34:00 | Confirmed |
| INT0 cycling | TIME → TEMP → FAHR → HUM → TIME | Confirmed |
| Colon blink rate | Once per second on both colons | Confirmed |
| Heartbeat LED | PD4 toggles every 1 s | Confirmed |
| Edit-mode entry | Press INT1 on TIME | Cursor on hours blinks at ≈2 Hz |
| RIGHT/LEFT cursor | Moves H→M→S and back | Confirmed |
| UP/DOWN adjust | Wrap at 24/60/60 for H/M/S | Confirmed |
| Clock while editing | Seconds keep counting | Confirmed |
| Sensor poll (C) | UART sends 'C', value updates | Confirmed |
| Sensor poll (F) | UART sends 'F', value updates | Confirmed |
| Sensor poll (H) | UART sends 'H', value updates | Confirmed |
| Sensor screen layout | Value in digits 0–2, unit in digit 5 | Confirmed |
| Separator glyph | Shown in digit 4 for C/F modes | Confirmed |

## UART Manual Reception Verification

To confirm that the USART receive path correctly updates `current_temp` and that the display logic renders it exactly as intended, the sensor board was bypassed and single bytes were injected manually through a virtual COM port using a Serial Port Utility, simulating the sensor's reply for each mode.

### Celsius Mode

[Insert figure here — `c.png`]

Request byte `'C'` observed; reply `0x19` (25 decimal) injected. Display showed `25` right-justified in digits 1–2 with the `C` unit glyph in digit 5 — matching `draw_env_value(25, CHAR_C)` exactly.

### Fahrenheit Mode

[Insert figure here — `f.png`]

Request byte `'F'` observed; reply `0x4D` (77 decimal, the Fahrenheit equivalent of 25 °C) injected. Display showed `77` with the `F` unit glyph.

### Humidity Mode

[Insert figure here — `h.png`]

Request byte `'H'` observed; reply `0x3C` (60 decimal) injected, representing 60% relative humidity. Display showed `60` with the `H` unit glyph in digit 5.

### Negative Value Handling

[Insert figure here — `manfi.png`]

To verify correct handling of negative `signed char` values, reply byte `0xFB` (two's-complement encoding of −5) was injected while in Humidity mode. Display correctly showed a minus sign in digit 0 followed by `5` in digit 2, confirming the sign-and-magnitude branch of `draw_env_value()` for out-of-range or below-zero readings.

### Direct Variable Inspection

[Insert figure here — `dr.png`]

To rule out ambiguity from the multiplexed display (only one digit is physically lit per instant), `current_temp` and `display_buf` were inspected directly in Proteus's variable viewer immediately after the Celsius test:

- `current_temp` held `0x19` (25 decimal)
- `display_buf` held `{0x0A, 0x02, 0x05, 0x0A, 0x0A, 0x0C}` — an exact match to `{BLANK, 2, 5, BLANK, BLANK, CHAR_C}`

This confirms, independent of physical display rendering, that UART reception and buffer computation function correctly end to end.

### Summary Table

| Test Case | Byte Sent | Expected Result | Observed |
|---|---|---|---|
| Celsius reply | `0x19` (25) | `25` with `C` glyph | Confirmed |
| Fahrenheit reply | `0x4D` (77) | `77` with `F` glyph | Confirmed |
| Humidity reply | `0x3C` (60) | `60` with `H` glyph | Confirmed |
| Negative reply | `0xFB` (−5) | Minus sign, `5`, unit glyph | Confirmed |
| Variable check | `0x19` (25) | `current_temp == 0x19` | Confirmed |
