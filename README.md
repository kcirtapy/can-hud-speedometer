# CAN Bus Heads-Up Speedometer

A windshield heads-up display that reads vehicle speed **directly from the car's CAN bus**, with no ELM327 dongle and no GPS. An STM32L432KC talks the OBD-II protocol natively through a CAN transceiver, converts to mph, and drives a two-digit seven-segment display that is mounted face-up on the dashboard and read as a reflection in the windshield.

<p align="center">
  <img src="images/hero.jpg" width="600" alt="The HUD reflected in the windshield at night, showing current speed">
</p>

<p align="center">
  <img src="images/demo.gif" width="600" alt="Digits updating as the car accelerates">
</p>

---

## Why this is not another OBD dongle project

Most hobby OBD projects use an ELM327 adapter, which is a CAN transceiver plus a microcontroller that translates the bus into AT commands over serial. That works, but it adds latency, costs more than the rest of the build combined, and hides the interesting part.

This project skips it. The STM32L432KC has a bxCAN controller built into the silicon, so all it needs is a $2 transceiver chip to reach the physical bus. The firmware speaks ISO 15765-4 itself: it builds the request frames, sets up the acceptance filter, and parses the ECU's replies. Speed updates ten times a second and comes from the same source the dashboard speedometer uses, so there is no GPS lag and it keeps working in tunnels and parking garages.

Total cost of parts beyond the dev board is roughly $25.

---

## How it works

### Reading speed from the bus

The car's diagnostic bus runs ISO 15765-4 at 500 kbit/s with 11-bit identifiers on any US vehicle from 2008 onward, and most from around 2004. Every 100 ms the firmware transmits a request frame:

| Field | Value | Meaning |
|-------|-------|---------|
| CAN ID | `0x7DF` | OBD-II functional broadcast, so whichever ECU owns the PID answers |
| Byte 0 | `0x02` | Two payload bytes follow |
| Byte 1 | `0x01` | Service 01, show current data |
| Byte 2 | `0x0D` | PID 0x0D, vehicle speed |

The responding ECU replies on an ID in the range `0x7E8` to `0x7EF`:

| Field | Value | Meaning |
|-------|-------|---------|
| Byte 0 | `0x03` | Three payload bytes follow |
| Byte 1 | `0x41` | Positive response to service 01 |
| Byte 2 | `0x0D` | Echoes the PID being answered |
| Byte 3 | `0x00` to `0xFF` | Speed in km/h |

A hardware acceptance filter admits only `0x7E8` through `0x7EF`, which matters because a running car floods the bus with traffic that would otherwise swamp the receive FIFO. Conversion to mph is integer only, `(kmh * 621 + 500) / 1000`, which rounds to nearest without pulling in floating point.

### Bit timing

APB1 runs at 80 MHz. A prescaler of 10 gives an 8 MHz time quantum clock, and 1 + 13 + 2 = 16 tq per bit produces exactly 500 kbit/s with the sample point at 87.5%, the value automotive CAN expects.

CAN is unforgiving about oscillator accuracy. With these settings the tolerance budget is roughly 0.5%, which rules out running from an untrimmed internal RC oscillator. The firmware therefore locks the MSI oscillator to the board's 32.768 kHz crystal using MSI PLL mode, giving accuracy far inside the budget. If the crystal fails to start, it falls back to HSI16 and reports which source it ended up on over the debug serial port, rather than silently running out of spec.

### Mirroring for the windshield

The display sits face-up on the dashboard and is read as a reflection, which flips the image left to right. The firmware compensates in two ways at once. Each glyph is mirrored by swapping segments b with f and c with e, since a, d, and g are symmetric about the vertical axis. Then the digit order is reversed, so the tens digit is painted on the physical right in order to appear on the visual left. At 47 mph:

```
  on the dashboard          seen in the windshield
   _                                _
  |    |_|                  |_|    |
  |    |                      |    |
```

### Driving the display

The Lite-On LTD-4608JR is a common-anode part, so the two digit commons are switched on the high side by PNP transistors while the STM32 sinks the segment currents. A common pin carries the sum of every lit segment, up to about 63 mA when displaying an 8, which is well past the 20 mA a GPIO is rated to handle. Driving it directly makes the pin voltage sag, and the display dims progressively as more segments light. The transistors keep every digit at full, even brightness.

Both digits share one set of seven segment lines and are multiplexed at 500 Hz each from a timer interrupt, which is above the flicker threshold and above the frame rate of most phone cameras. The segment lines are blanked while the pattern changes, otherwise the outgoing digit briefly shows the incoming digit's pattern.

---

## Bill of materials

| Part | Qty | Approx. cost | Notes |
|------|-----|--------------|-------|
| Nucleo-L432KC | 1 | $15 | bxCAN controller on chip, 32.768 kHz crystal fitted |
| Lite-On LTD-4608JR display | 1 | $3 | 0.4 inch, common anode, AlInGaP super red |
| SN65HVD230 CAN transceiver | 1 | $3 | 3.3 V part, matches the MCU. Do not substitute the 5 V MCP2551 |
| OBD-II male pigtail or splitter | 1 | $6 | Only three conductors are used |
| PNP transistors, 2N2907 or 2N3906 | 2 | $1 | High-side digit switching |
| Resistors, 150 ohm | 7 | $1 | Segment current limiting, about 9 mA each |
| Resistors, 1 kohm | 2 | $1 | Transistor base current |
| USB car charger and micro-USB cable | 1 | $10 | Powers the board from the accessory socket |
| HUD reflective windshield film | 1 | $5 | Suppresses the double image from the two glass surfaces |

Schematic and full wiring tables are in [`hardware/`](hardware/) and [`docs/BUILD_GUIDE.md`](docs/BUILD_GUIDE.md).

<p align="center">
  <img src="hardware/schematic.png" width="700" alt="Schematic">
</p>

---

## Firmware

Built with PlatformIO. Open [`firmware/`](firmware/) in VS Code, click Build, click Upload. The STM32Cube HAL and the ARM toolchain are fetched automatically, so there is no CubeMX step and no `.ioc` file to maintain.

Footprint is about 15 KB of flash and 2 KB of RAM.

Everything worth changing lives in a block of defines at the top of `src/main.c`:

| Define | Default | Purpose |
|--------|---------|---------|
| `HUD_MIRROR` | `1` | Mirror the digits for windshield reading. Set to `0` for bench testing |
| `COMMON_CATHODE` | `0` | `0` for common anode plus PNP drivers, `1` for common cathode plus NPN |
| `BENCH_TEST` | `0` | Loopback self-test, described below |
| `DEBUG_UART` | `1` | Status output over the ST-Link virtual COM port |
| `REQUEST_MS` | `100` | ECU poll interval |
| `DATA_TIMEOUT_MS` | `1000` | How long before falling back to `--` |

### Bench self-test

Setting `BENCH_TEST` to `1` puts the CAN controller into silent loopback mode, where it receives its own transmissions without driving the bus, and has the firmware impersonate an ECU by emitting a well-formed response to PID 0x0D with a speed value that ramps from 0 to 99 mph and repeats.

That exercises the clock configuration, the multiplex timer, both digit drivers, all seven segments, the CAN peripheral, the acceptance filter, the receive interrupt, the frame parser, the unit conversion, and the render path. Everything except the transceiver and the car. It needs no vehicle, no OBD cable, and no transceiver connected.

It is also a better wiring test than a static lamp pattern, because sweeping through every digit lights every segment in every combination, so a swapped pair of segment wires shows up as garbled output.

### Diagnostics

The firmware assumes it will be debugged in a parked car with no debugger attached, so it reports through the display itself.

At power-up both digits light every segment for 0.8 seconds, which makes a dead segment or a broken wire obvious before any CAN troubleshooting starts.

Faults appear on the display as `E` followed by a code, and CAN faults are non-fatal: the firmware retries every three seconds, so connecting the transceiver or switching on the ignition recovers without a reset.

| Code | Meaning |
|------|---------|
| `E2` | CAN peripheral init failed |
| `E3` | CAN filter configuration failed |
| `E4` | CAN could not start because the receive line never went idle, most often an unpowered transceiver |
| `E9` | SysTick is not incrementing |

With `DEBUG_UART` enabled, a status line goes out over the same USB cable used for flashing, at 115200 baud:

```
=== HUD speedometer ===
clock: MSI trimmed by LSE (best)
CAN: up, waiting for ECU...
tx=120 rx=118  kmh=97  mph=60  OK
```

The `tx` and `rx` counters separate the three failure modes that all look identical on the display. Requests not going out, requests going out with nothing coming back, and data arriving but rendering wrong are each distinguishable at a glance.

---

## Debugging notes

Two problems during bring-up are worth recording, because both looked like firmware bugs and neither was.

**The display froze on the power-up lamp test.** Both digits sat lit indefinitely and no fault code appeared. The pattern on screen was the startup self-test, which meant execution had stopped somewhere in the first second. The original firmware ran that delay off `HAL_Delay`, so the obvious theory was that SysTick had stopped incrementing, which would also have hung the CAN and UART initialization further down since those spin on the same timebase.

The actual cause was a wiring mistake. The transceiver's ground wire had been landed on the board's RST pin instead of GND, tying the reset line to the ground rail and holding the microcontroller in reset. It was not frozen at all. It was rebooting continuously, replaying the first 0.8 seconds of startup forever.

The misdiagnosis was still worth something. The startup path now runs on a millisecond counter maintained by the multiplex timer interrupt rather than on SysTick, so it cannot hang the same way, and a deliberate check reports `E9` on the display if SysTick ever does stop. A failure that reports itself beats a failure that looks like a freeze.

**Zero volts at the transceiver's supply pin.** Measuring 3V3 to GND at the transceiver read 0 V while the display was clearly powered, which is contradictory until you notice that the ground reference being measured against was the reset line rather than actual ground. Same root cause, different symptom.

The general lesson, which cost an evening: when two symptoms appear at once, look for the single wiring fault that explains both before theorizing about the code.

---

## Installation notes

The display mounts flat on top of the dashboard with the digits facing up and the bottom of the digits toward the windshield. HUD reflective film goes on the glass above it. Without the film you get a visible double image, because the windshield reflects from both its inner and outer surfaces.

Power comes from the 12 V accessory socket through a USB car charger rather than from OBD pin 16. Pin 16 is unswitched on most cars, so drawing from it would slowly flatten the battery while parked. The accessory socket is switched with the ignition on most vehicles, so the HUD powers up and down with the car.

The OBD ground wire still needs to be connected even though power arrives over USB. The CAN transceiver needs a solid ground reference to the vehicle, and the charger's ground path is too indirect for reliable signaling.

Expect the HUD to read one to three mph below the dashboard speedometer. Factory speedometers deliberately over-read, and this displays the underlying bus value.

---

## Safety

This device only transmits standard OBD-II service 01 requests, which are read-only diagnostic queries defined by the same standard every emissions scan tool uses. It does not write to any ECU, does not send UDS or manufacturer-specific commands, and cannot alter vehicle behavior. In bench self-test mode it transmits nothing onto the bus at all.

That said, it physically attaches to a live vehicle bus. Wire it carefully, keep it clear of the pedals and airbag deployment zones, and do not adjust or debug it while driving. Mount the display so the reflection sits low in the field of view rather than in the middle of the road ahead.

Use at your own risk.

---

## Repository layout

```
.
├── firmware/          PlatformIO project (open this folder in VS Code)
│   ├── src/main.c
│   ├── include/stm32l4xx_hal_conf.h
│   └── platformio.ini
├── hardware/          wiring tables, component reasoning, schematic
├── docs/              full build guide
└── images/            photos and demo GIF
```

## Possible extensions

A button to cycle between mph, km/h, coolant temperature (PID 0x05), and RPM divided by 100 (PID 0x0C). An LDR on an ADC input for automatic night dimming. Support for 29-bit extended identifiers, which some trucks and older vehicles use, by requesting on `0x18DB33F1` and accepting the `0x18DAF100` to `0x18DAF1FF` range.

## License

MIT. See [LICENSE](LICENSE).
