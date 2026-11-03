# Hardware

## Connections

### CAN transceiver, SN65HVD230

| Transceiver | Nucleo-L432KC | Notes |
|-------------|---------------|-------|
| 3V3 | 3V3 | |
| GND | GND | Not RST, which sits right beside GND on the headers. See the debugging notes in the top-level README |
| CTX | D2 (PA12) | CAN1_TX |
| CRX | D10 (PA11) | CAN1_RX |
| RS | GND | High-speed mode, if the breakout exposes it |

| Transceiver | OBD-II connector |
|-------------|------------------|
| CANH | Pin 6 |
| CANL | Pin 14 |
| (board GND) | Pin 4 or 5 |

Many SN65HVD230 breakouts ship with a 120 ohm termination resistor already fitted between CANH and CANL, often labeled R2 or RL. Remove it. The vehicle bus is already terminated at both ends, and a third terminator degrades signaling for every node. To check, measure resistance between CANH and CANL with the board unpowered and disconnected. Roughly 120 ohms means the resistor is present.

### Display, Lite-On LTD-4608JR

Common anode. Segment cathodes connect to the MCU through 150 ohm resistors, and the two digit commons are switched on the high side by PNP transistors.

| Display pin | Function | Through | Nucleo pin | STM32 |
|-------------|----------|---------|------------|-------|
| 10 | Segment a | 150 ohm | A0 | PA0 |
| 8 | Segment b | 150 ohm | A1 | PA1 |
| 1 | Segment c | 150 ohm | A2 | PA3 |
| 5 | Segment d | 150 ohm | A3 | PA4 |
| 3 | Segment e | 150 ohm | A4 | PA5 |
| 6 | Segment f | 150 ohm | A5 | PA6 |
| 7 | Segment g | 150 ohm | A6 | PA7 |
| 2 | Decimal point | | not connected | |
| 9 | Common anode, digit 1 (left) | PNP collector | D3, via base resistor | PB0 |
| 4 | Common anode, digit 2 (right) | PNP collector | D6, via base resistor | PB1 |

Note that the segment pins are not in alphabetical order around the package. Pin 1 is bottom left viewed from the front, and the numbering runs counterclockwise.

### Digit drivers

Two PNP transistors, 2N2907, 2N3906, or BC327.

| Transistor pin | Connects to |
|----------------|-------------|
| Emitter | 3V3 |
| Collector | Display pin 9 (left digit) or pin 4 (right digit) |
| Base | 1 kohm resistor to PB0 (left) or PB1 (right) |

Base low turns the digit on, which is why the firmware sets `COMMON_CATHODE` to `0`.

On the TO-92 package with the flat face toward you and the legs pointing down, the pinout is emitter, base, collector from left to right. Verify against the datasheet for the exact part you buy, since a swapped emitter and collector is the most common reason one digit stays dark.

## Component value reasoning

**Segment resistors, 150 ohm.** With a 3.3 V rail and roughly 2.0 V forward drop across an AlInGaP segment, 150 ohm sets about 9 mA. Seven segments lit at once totals 63 mA, comfortably inside the STM32's roughly 100 mA aggregate I/O budget. Dropping to 100 ohm raises this to about 91 mA, which works but leaves no margin. The LTD-4608JR is efficient enough that 9 mA is plenty bright: its luminous intensity is specified at a 1 mA test current, where most displays are specified at 10 or 20 mA.

**Base resistors, 1 kohm.** A base-emitter junction is a diode and will draw whatever the pin supplies if left unlimited. With the emitter at 3.3 V, the pin at 0 V, and about 0.7 V across the junction, 1 kohm gives roughly 2.6 mA of base current. Multiplied by a transistor gain above 100, that supports far more collector current than the 63 mA worst case.

**Why the transistors are not optional.** Multiplexing is done entirely in software, so the transistors add no logic. They exist because the common pin carries the sum of every lit segment current, and a GPIO is rated for 20 mA absolute maximum and only about 8 mA within specification. Driven directly the pin sags, and the display dims progressively as more segments light, which is invisible indoors and very visible reflected off glass in daylight.

**Why the segment resistors are not optional either.** The STM32's internal pull-ups are about 40 kohm and only bias inputs, so they cannot stand in for current limiting. And with the PNPs supplying a stiff 3.3 V, nothing but these resistors stops the segment current from running away.

## Schematic

`schematic.png` goes in this folder, exported from the KiCad project kept alongside it.
