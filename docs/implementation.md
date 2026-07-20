# Implementation Details

*(Content moved from the report's "Software Design", "Implementation", and "Timing Analysis" chapters.)*

## Program Structure

The firmware is a single C translation unit. State shared between the main loop and the ISRs lives in a small set of `volatile` globals:

- **Glyph table** (`seg_code[]`) — segment patterns for digits 0–9 plus blank, minus, and the letters `C`, `F`, `H`.
- **Clock state** — `hours`, `minutes`, `seconds`.
- **Multiplexing state** — `display_buf[6]`, `current_digit`.
- **Screen/edit state** — `mode`, `clock_edit`, `cursor` (0 = hours, 1 = minutes, 2 = seconds), `blink[6]`.
- **Sensor cache** — `current_temp`, `temp_request`.
- **ISRs** — `INT0`, `INT1`, `TIM1_COMPA`, `TIM2_COMP`, `USART_RXC`.
- **Helpers** — `uart_send`, `poll_temperature`, `apply_keys`, `scan_edit_keys`, `set_blink(i, on)`, `show_time`, `draw_env_value`, `show_temperature`, `show_fahr`, `show_humidity`, `update_display`.
- **`main()`** — initializes all ports/timers/peripherals/interrupts, then runs the super-loop (`poll_temperature()` + `update_display()`).

## Global Variables

| Variable | Type | Purpose |
|---|---|---|
| `seg_code[]` | `unsigned char[15]` | Segment glyph lookup table (read-only) |
| `display_buf[6]` | `volatile unsigned char[6]` | Glyph indices for each digit position |
| `current_digit` | `volatile unsigned char` | Currently active digit (0–5) |
| `mode` | `volatile unsigned char` | Active screen: TIME, TEMP, FAHR, or HUM |
| `clock_edit` | `volatile unsigned char` | Edit-mode flag (0 = off, 1 = on) |
| `cursor` | `volatile unsigned char` | Edit cursor: 0 = hours, 1 = minutes, 2 = seconds |
| `hours` | `volatile unsigned char` | Clock hours (0–23) |
| `minutes` | `volatile unsigned char` | Clock minutes (0–59) |
| `seconds` | `volatile unsigned char` | Clock seconds (0–59) |
| `temp_request` | `volatile unsigned char` | Flag set once per minute to trigger UART poll |
| `current_temp` | `volatile signed char` | Last received sensor value |
| `blink[6]` | `volatile uint8_t[6]` | Per-digit blink enable for edit cursor |
| `tick` | `volatile uint16_t` | Free-running counter for blink timing |

All single-byte variables (`seconds`, `minutes`, `hours`, `mode`, `clock_edit`, `cursor`, `temp_request`, `current_temp`) are atomic on the AVR architecture and require no critical section when shared between the main loop and ISRs. `tick` is 16-bit but is only ever touched inside the Timer2 ISR, so it needs no synchronization either.

## Timer Configuration

Both timers run in **CTC (Clear-Timer-on-Compare-Match)** mode.

**Timer1** (16-bit) — one-second timekeeping base. Prescaler /256, `OCR1A = 31250`:

```
f_T1 = 8,000,000 / (256 × 31250) = 1 Hz
```

**Timer2** (8-bit) — 2 ms multiplexing cadence. Prescaler /64, `OCR2 = 249` (250 counts):

```
f_T2 = 8,000,000 / (64 × 250) = 500 Hz  →  2 ms period
```

A private `heartbeat_tick` counter accumulates 500 of these 2 ms ticks to produce the 1 s heartbeat LED toggle.

```c
/* Timer1: CTC, prescaler /256, 1 s period */
TCCR1A = 0x00;
TCCR1B = (1<<WGM12) | (1<<CS12);
OCR1AH = 0x7A;
OCR1AL = 0x11;
TCNT1  = 0x0000;

/* Timer2: CTC, prescaler /64, 2 ms period */
TCCR2 = (1<<CTC2) | (1<<CS22);
OCR2  = 0xF9;
TCNT2 = 0x00;

TIMSK  = (1<<OCIE1A) | (1<<OCIE2);
```

## Peripheral Configuration

- **Port A:** input, internal pull-ups enabled on PA0–PA3 (active-low keypad).
- **Port B:** output, initial value `0xFF` (all segments off, common-anode = active-low).
- **Port C:** PC0–PC5 outputs for the six common-anode digit-enable lines.
- **Port D:** PD0 (TXD) and PD4 (LED) outputs; PD2/PD3 (INT0/INT1) inputs with pull-ups, falling-edge triggered.
- **USART:** 9600 baud, 8N1. With an 8 MHz clock:

```
UBRR = 8,000,000 / (16 × 9600) − 1 ≈ 51.08 → 51 (0x33)
```

giving ≈0.16% baud error, well inside the ±2% receiver tolerance. Reception is interrupt-driven (`RXCIE`); transmission is polled on the `UDRE` flag in `uart_send()`.

- **Unused peripherals** (ADC, analog comparator, SPI, TWI, Timer0) are explicitly disabled at start-up.

## Interrupt Architecture

![Firmware architecture](../assets/figures/FW_ARCHITECTURE.svg)

| Priority | Vector | Source | Function |
|---|---|---|---|
| 1 | 0x002 | RESET | Power-up / watchdog reset |
| 2 | 0x004 | EXT_INT0 | Screen cycle (mode switching) |
| 3 | 0x006 | EXT_INT1 | Edit-mode toggle |
| 4 | 0x014 | TIM1_COMPA | One-second timekeeping |
| 5 | 0x016 | TIM2_COMP | 2 ms multiplexing heartbeat |
| 6 | 0x01C | USART_RXC | Sensor data reception |

### INT0 — Screen Cycle
Triggered on the falling edge of PD2. Increments `mode` with wrap-around at `MODE_HUM`. Deliberately minimal — modifies a single `volatile` byte and returns.

### INT1 — Edit Mode Toggle
Triggered on the falling edge of PD3. Toggles `clock_edit`. On 0→1 transition, edit mode is entered and the cursor defaults to the hours field. On 1→0, editing ends and the blink array is cleared.

### TIM1_COMPA — One-Second Timekeeping

```c
interrupt [TIM1_COMPA] void timer1_compa_isr(void)
{
    if (++seconds >= 60)
    {
        seconds = 0;
        temp_request = 1;
        if (++minutes >= 60)
        {
            minutes = 0;
            if (++hours >= 24) hours = 0;
        }
    }
}
```

### TIM2_COMP — 2 ms Multiplexing Heartbeat

The busiest ISR. Each call:

1. Calls `scan_edit_keys()` to detect keypad events.
2. Turns off all digits to prevent ghosting.
3. Decides whether the current digit should be shown or blanked (edit-cursor blink).
4. Outputs the segment code to `PORTB`.
5. Applies colon decimal points for TIME mode.
6. Enables the current digit via `PORTC`.
7. Advances the digit counter.
8. Increments the heartbeat tick counter; toggles PD4 every 500 ticks.

```c
interrupt [TIM2_COMP] void timer2_comp_isr(void)
{
    static unsigned int heartbeat_tick = 0;

    scan_edit_keys();

    PORTC = 0x00;              /* all digits off */

    unsigned char show = 1;
    if (blink[current_digit] && ((tick >> 6) & 1))
        show = 0;
    tick++;

    if (show)
        PORTB = seg_code[display_buf[current_digit]];
    else
        PORTB = seg_code[BLANK];

    /* colon on digit 2 (HH:MM) and digit 4 (MM:SS),
       blinks once per second with seconds parity */
    if (mode == MODE_TIME)
        if ((current_digit == 2 || current_digit == 4) && (seconds & 1))
            PORTB &= 0x7F;   /* DP ON (common anode) */

    PORTC = (1 << current_digit);
    if (++current_digit >= NUM_DIGITS) current_digit = 0;

    if (++heartbeat_tick >= 500)
    {
        heartbeat_tick = 0;
        PORTD ^= 0x10;       /* heartbeat LED toggle */
    }
}
```

### USART_RXC — Sensor Data Reception
Triggered when a byte is received. Stores `UDR` into `current_temp`. Because a single-byte store is atomic on AVR, no critical section is needed.

## Display Buffer Mechanism

- `display_buf[6]` is `volatile unsigned char[]`, indexed by digit position (0 = leftmost, 5 = rightmost). Each element is an index into `seg_code[]`.
- The **main loop** writes to `display_buf[]` via `update_display()`, based on the active screen mode.
- The **Timer2 ISR** reads `display_buf[]` sequentially, one digit per 2 ms tick.

This separation means the multiplexing refresh is never blocked by main-loop work — even if `update_display()`'s runtime varies across screen modes.

The `blink[6]` array is an overlay on top of this: when `blink[i]` is set and bit 6 of the global `tick` counter is 1, digit `i` is blanked instead of shown — producing the 2 Hz edit-cursor blink **without** modifying `display_buf[]` itself.

### Edit-Cursor Blink Mapping

`cursor=0` (hours) blinks digits 0–1; `cursor=1` (minutes) blinks digits 2–3; `cursor=2` (seconds) blinks digits 4–5.

```c
void set_blink(unsigned char i, unsigned char on)
{
    blink[i] = on;
}

void update_display(void)
{
    unsigned char i;
    for (i = 0; i < NUM_DIGITS; i++) set_blink(i, 0);   // always start clean

    switch (mode)
    {
        case MODE_TIME:
            if (clock_edit)
            {
                if (cursor == 0)      { set_blink(0, 1); set_blink(1, 1); }
                else if (cursor == 1) { set_blink(2, 1); set_blink(3, 1); }
                else                  { set_blink(4, 1); set_blink(5, 1); }
            }
            show_time();
            break;

        case MODE_TEMP: show_temperature(); break;
        case MODE_FAHR: show_fahr();        break;
        case MODE_HUM:  show_humidity();    break;
    }
}
```

## Keypad Debouncing

Mechanical contacts bounce for several milliseconds on press/release; without debouncing this can register as multiple key events. `scan_edit_keys()` runs inside the Timer2 ISR every 2 ms and implements a periodic-sampling debouncer:

1. Read PA0–PA3 (`curr = PINA & 0x0F`).
2. Keep the previous sample in `prev`.
3. Compute falling edges: `falling = (prev ^ curr) & prev` — detects HIGH→LOW (released→pressed) transitions.
4. Update `prev = curr`.
5. If any falling edge is detected, call `apply_keys(falling)` immediately.

Since the keypad is sampled at 500 Hz and typical contact bounce lasts 5–20 ms, the bounce train spans multiple samples, but only the *first* sample after a genuine transition produces a new falling edge — rejecting both the bounce train and any shorter noise events.

## Environmental Value Formatting

`draw_env_value()` right-justifies the signed value into digits 0–2 with a leading blank and optional minus sign. The unit letter appears in the last digit; a separator glyph appears in digit 4 for C/F modes (blank for humidity).

```c
void draw_env_value(signed char val, unsigned char unit)
{
    if (val < 0)
    {
        display_buf[0] = MINUS;
        val = -val;
    }
    else
        display_buf[0] = (val >= 100) ? (val / 100) : BLANK;

    if (val >= 100)
    {
        display_buf[1] = (val / 10) % 10;
        display_buf[2] = val % 10;
    }
    else if (val >= 10)
    {
        display_buf[1] = val / 10;
        display_buf[2] = val % 10;
    }
    else
    {
        display_buf[1] = BLANK;
        display_buf[2] = val;
    }

    display_buf[3] = BLANK;
    display_buf[4] = BLANK;
    display_buf[5] = unit;
}
```

## Keypad Editing Logic

```c
void apply_keys(unsigned char pressed)
{
    if (!clock_edit) return;

    if (pressed & 0x04) { if (cursor < 2) cursor++; }   /* RIGHT */
    if (pressed & 0x08) { if (cursor > 0) cursor--; }   /* LEFT  */

    if (pressed & 0x01) {                               /* UP */
        if      (cursor == 0) { if (++hours   >= 24) hours   = 0; }
        else if (cursor == 1) { if (++minutes >= 60) minutes = 0; }
        else                  { if (++seconds >= 60) seconds = 0; }
    }
    if (pressed & 0x02) {                               /* DOWN */
        if      (cursor == 0) { if (hours   == 0) hours   = 23; else hours--;   }
        else if (cursor == 1) { if (minutes == 0) minutes = 59; else minutes--; }
        else                  { if (seconds == 0) seconds = 59; else seconds--; }
    }
}
```

## Timing Analysis

### Timer1 (Timekeeping)

![Timing diagram](../assets/figures/TIMING.svg)

```
t_count = 31250 / (8,000,000 / 256) = 31250 / 31250 = 1 s
```

### Timer2 (Multiplexing)

- Digit ON time: 2 ms
- Frame time (6 digits): 12 ms
- Refresh rate: ≈83.3 Hz

Blink timing, derived from the `tick` counter (incremented every 2 ms):

- Toggles every 64 ticks → 2 ms × 64 = 128 ms
- Full blink period: 256 ms → ≈3.9 Hz toggle (≈2 Hz perceived blink)

Heartbeat LED: toggles every 500 ticks → 2 ms × 500 = 1 s.

### UART Timing

At 9600 baud:

```
t_bit = 1 / 9600 ≈ 104.17 µs
```

A full 10-bit frame (1 start + 8 data + 1 stop):

```
t_frame = 10 × 104.17 µs ≈ 1.042 ms
```

A full sensor transaction (request byte out + reply byte in) takes ≈2.1 ms plus sensor processing latency — negligible against the 60-second polling interval.

## Design Iterations / Debugging Notes

- **Clock keep-running during edit** — early revisions risked freezing the clock while editing; the final design keeps Timer1's 1 s tick fully independent of edit state.
- **Dual-timer separation** — splitting timekeeping (Timer1) from multiplexing (Timer2) removed jitter that appeared in an earlier single-timer approach.
- **Blink rate** — the `tick >> 6` shift gives a readable ≈2 Hz blink that clearly signals the field under edit.
- **Cursor wrap limits** — explicit bounds checks in the LEFT/RIGHT handlers keep `0 ≤ cursor ≤ 2`.
- **Polling cadence** — only the currently displayed sensor screen is refreshed each minute; switching screens mid-cycle means waiting up to a minute for a fresh reading (acknowledged limitation, see README future work).
