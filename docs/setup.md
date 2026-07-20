# Setup & Simulation Environment

*(Content moved from the report's "Implementation → Development Environment" and "Testing and Results → Simulation Environment" sections.)*

## Toolchain

| Tool | Role |
|---|---|
| **CodeVisionAVR** | C compiler / IDE used to build the firmware. Provides the `<mega32.h>` device header, the `interrupt[...]` ISR syntax, and the `#asm("sei")` intrinsic used to enable global interrupts. |
| **Proteus Design Suite** | Circuit-level simulation and microcontroller emulation used to validate the design end-to-end. |

## Target Configuration

- **MCU:** ATmega32
- **Clock:** 8 MHz (external crystal)
- **UART:** 9600 baud, 8N1

## Building the Firmware

[Add build instructions here — e.g. CodeVisionAVR project file name, compiler flags, and the exact steps to produce `final.hex` from `main.c`.]

## Running the Simulation

The Proteus project file `SysDig2AVRProject.pdsprj` wires together two ATmega32 instances:

1. **Display/controller board** — loaded with the compiled `final.hex` (this repository's firmware).
2. **Sensor board** — loaded with `SysDig2AVRProject.hex` (a separate, external firmware image that performs the analog measurements and answers UART requests).

Steps:

1. Open `SysDig2AVRProject.pdsprj` in Proteus.
2. Confirm `final.hex` is assigned to the controller ATmega32 and `SysDig2AVRProject.hex` to the sensor AVR.
3. Run the simulation.
4. Use the on-schematic push-buttons (INT0/INT1) and keypad to exercise screen cycling and clock editing.

[Add any additional environment-specific setup notes here, e.g. required Proteus library models for the 7SEG-MPX6-CA part.]

## Manual UART Injection (for isolated receive-path testing)

To test the receive/formatting path without depending on the sensor board's actual measurements, single bytes can be injected directly over a virtual COM port (e.g. using a Serial Port Utility) while the simulation is running. See [docs/experiments.md](experiments.md) for the full test procedure and results.
