# Build Guide: 2-Digit HUD Speedometer (STM32L432KC + OBD-II CAN)

Reads vehicle speed straight off the car's diagnostic CAN bus and displays
mph, mirrored, so it reads correctly when reflected in the windshield.

---

## 1. Parts List

| # | Part | Qty | ~Price | Notes |
|---|------|-----|--------|-------|
| 1 | Nucleo-L432KC | 1 | $15 | Has a built-in bxCAN controller |
| 2 | Lite-On LTD-4608JR 2-digit display | 1 | $3 | Common anode, super red. Pinout in §3 |
| 3 | SN65HVD230 CAN transceiver breakout | 1 | $2-3 | 3.3 V part, matches the STM32. Do NOT use an MCP2551 (5 V) |
| 4 | OBD-II male connector pigtail (or splitter cable) | 1 | $5-8 | Only 3 wires used: CAN-H, CAN-L, ground. A splitter is nicer because it leaves the port free for a scan tool |
| 5 | USB car charger (name-brand, 1 A+) + micro-USB cable | 1 | $8-12 | This IS the buck converter: it plugs into the 12 V accessory socket. Avoid gas-station-cheap ones, since noisy power shows up as random glitches |
| 6 | Resistors, 150 Ω (100 Ω also works, see the §4 note) | 7 | $1 | Sets segment current: 150 Ω ≈ 9 mA, 100 Ω ≈ 13 mA |
| 7 | PNP transistors, 2N2907 / BC327 / 2N3906 | 2 | $1 | Digit switching. The display is **common anode**, which needs high-side switches, so N-channel parts like the BS170 won't work here |
| 8 | Resistors, 1 kΩ | 2 | $1 | PNP base resistors |
| 9 | HUD reflective windshield film (about 10×15 cm) | 1 | $5 | Kills the double-image ghosting and boosts daytime contrast |
| 10 | Perfboard, stranded hookup wire, header pins | | $3 | Use stranded wire throughout: solid core fatigues under vibration |

**Total beyond the Nucleo and display: roughly $25-35.**

---

## 2. How It Works

1. Every 100 ms the STM32 transmits an OBD-II request on CAN ID `0x7DF`:
   `02 01 0D 00 00 00 00 00` (service 01, PID 0x0D = vehicle speed).
2. The engine ECU replies on `0x7E8` with speed in **km/h** in byte 3.
3. The firmware converts to mph and renders it **mirrored** (segments b and f
   swapped, c and e swapped, digit order reversed) so the windshield
   reflection reads normally.
4. A timer interrupt multiplexes the two digits at 500 Hz each, so no
   driver chip is needed.

This is the same data feeding your speedometer: no GPS lag, works in
tunnels, updates about 10 times per second.

---

## 3. The Display: Lite-On LTD-4608JR

**Common anode**, super-red AlInGaP, 0.4" digits. It is an unusually
efficient display (brightness is specified at just 1 mA test current), so it
is plenty bright at 9 mA per segment. Pin 1 is bottom left viewed from the
front, counting counterclockwise like an IC.

| Display pin | Function |
|-------------|----------|
| 1 | Cathode C |
| 2 | Cathode DP (unused, leave unconnected) |
| 3 | Cathode E |
| 4 | **Common anode, digit 2 (right)** |
| 5 | Cathode D |
| 6 | Cathode F |
| 7 | Cathode G |
| 8 | Cathode B |
| 9 | **Common anode, digit 1 (left)** |
| 10 | Cathode A |

```
 Segment layout:      aaa
                     f   b
                      ggg
                     e   c
                      ddd
```

**Firmware setting**: common anode means `#define COMMON_CATHODE 0` in
`main.c`, which is already the default.

---

## 4. Wiring

### CAN transceiver (SN65HVD230) to Nucleo

| SN65HVD230 | Nucleo-L432KC |
|------------|---------------|
| 3V3        | 3V3           |
| GND        | GND (**not RST**, which sits right beside GND on the headers) |
| CTX (D)    | **D2** (PA12 = CAN1_TX) |
| CRX (R)    | **D10** (PA11 = CAN1_RX) |
| RS (if present) | GND (high-speed mode) |

### SN65HVD230 to OBD-II connector

| SN65HVD230 | OBD-II pin |
|------------|-----------|
| CANH       | Pin 6     |
| CANL       | Pin 14    |

Twist the CANH and CANL wires around each other along their whole run, a
couple of twists per inch. CAN's noise immunity depends on the two wires
picking up identical interference.

**Do not add a 120 Ω termination resistor**: the car's bus is already
terminated. If your breakout has one pre-soldered (some do, marked R2 or RL),
remove it. Measure CANH to CANL with everything unpowered: roughly 120 Ω
means it is fitted.

Insulate every unused pigtail wire individually, especially pin 16, which
carries constant 12 V whenever the connector is plugged in.

### Power

Simple: **12 V accessory socket → USB car charger → micro-USB cable → the
Nucleo's micro-USB port.** The Nucleo runs from USB 5 V and its onboard
regulator powers the display and the transceiver.

This has two hidden benefits. The socket circuit is already fused by the car,
so no inline fuse is needed. And on most cars it switches off with the
ignition, so the HUD powers up and down with the car automatically, with
zero battery drain while parked. OBD pin 16 (constant 12 V) is deliberately
**not** used.

⚠️ One wire you still need: run OBD-II **pin 4 or 5 (ground)** to the
board's GND anyway. The CAN transceiver needs a solid ground reference to
the car, and the USB charger's ground path is too indirect for reliable CAN
signaling.

### Display to Nucleo

Segments, each through a **150 Ω** resistor:

| Display pin | Segment | Through | Nucleo pin | STM32 pin |
|-------------|---------|---------|-----------|-----------|
| 10 | a | 150 Ω | A0 | PA0 |
| 8  | b | 150 Ω | A1 | PA1 |
| 1  | c | 150 Ω | A2 | PA3 |
| 5  | d | 150 Ω | A3 | PA4 |
| 3  | e | 150 Ω | A4 | PA5 |
| 6  | f | 150 Ω | A5 | PA6 |
| 7  | g | 150 Ω | A6 | PA7 |
| 2  | DP | | unconnected | |

Digit drivers, 2× **PNP** (2N2907/BC327/2N3906), one per digit, as high-side
switches for the common-anode pins:

| PNP pin | Connect to |
|---------|-----------|
| Emitter | 3V3 |
| Collector | Display pin 9 (left digit) or pin 4 (right digit) |
| Base | 1 kΩ resistor → STM32 GPIO |

| Digit | Display pin | Nucleo pin | STM32 pin |
|-------|-------------|-----------|-----------|
| LEFT (digit 1) | 9 | D3 | PB0 |
| RIGHT (digit 2) | 4 | D6 | PB1 |

With PNPs, GPIO **low** turns a digit on. The firmware handles this
automatically when `COMMON_CATHODE` is `0`. On the TO-92 package with the
flat face toward you and legs down, the pins are emitter, base, collector
from left to right; check your exact part's datasheet before soldering.

**Resistor note**: 150 Ω gives about 9 mA per segment, 63 mA worst case with
all seven lit, which is comfortable. 100 Ω gives about 13 mA per segment and
more brightness, but 7 × 13 is about 91 mA when displaying "8", right at the
STM32's 100 mA total I/O budget. Workable, but with no margin.

**Why the transistors**: the common-anode pin carries the *sum* of all lit
segment currents, up to about 63 mA. A GPIO is rated 20 mA absolute maximum
and only ±8 mA within spec. Driven directly, its voltage sags and the display
dims, progressively more as more segments light. The PNP feeds the full 3.3 V
rail instead, keeping every digit at full, even brightness.

Leave **D4/D5 unconnected**: on this board they're tied to A4/A5 through
solder bridges, and the firmware leaves them as inputs so they won't
interfere.

---

## 5. Firmware

The firmware is a ready-to-build **PlatformIO project** in
[`firmware/`](../firmware/) for VS Code. See its
[README](../firmware/README.md) for the full walkthrough. Short version:

1. Install VS Code, then the **PlatformIO IDE** extension.
2. **File > Open Folder** and select the `firmware` folder.
3. Plug in the Nucleo, click **Build**, then **Upload**.

No CubeMX, no `.ioc`, no toolchain setup: the extension fetches everything.
Expect about 15 KB flash and 2 KB RAM used.

The firmware prints a status line every second over the same USB cable
(PlatformIO's Serial Monitor, 115200 baud). That output is the fastest way to
tell "the bus isn't connected" from "the display is miswired". CAN faults
show on the display as `E2`, `E3`, `E4` or `E9`.

### Settings at the top of main.c

```c
#define HUD_MIRROR      1   // 1 = mirrored for windshield, 0 = normal (bench test)
#define COMMON_CATHODE  0   // already set for the LTD-4608JR (common anode)
#define DEBUG_UART      1   // serial status output
#define BENCH_TEST      0   // 1 = loopback self-test, no car needed
```

---

## 6. Bench Test Before the Car

1. Set `HUD_MIRROR 0`, flash. On power-up every segment lights for 0.8 s
   (the `88` lamp test), so check for a dead segment. Then it settles to
   `--` because no CAN bus is attached. That proves the display wiring and
   multiplexing.
2. Segment garbled? Re-check the display-pin-to-Nucleo-pin table in §4. The
   LTD-4608JR's segment pins are not in alphabetical order around the
   package.
3. Set `BENCH_TEST 1` as well and flash. With nothing but the display
   connected, it should sweep 0 to 99 and repeat. This checks the whole CAN
   receive path in loopback, and every segment in every combination.
4. Set `BENCH_TEST 0` and `HUD_MIRROR 1`, flash, and hold the display up to a
   mirror. Digits should read correctly *in the mirror*.

Before installing in the car, confirm `BENCH_TEST` is `0` and `HUD_MIRROR` is
`1`. In bench mode the firmware ignores the real bus.

On the transceiver, with the board powered: 3V3 to GND should read 3.3 V, and
CANH and CANL to GND should each sit around 2.3-2.5 V with the OBD side
disconnected (the recessive idle state).

## 7. In the Car

1. Plug the OBD connector in, plug the charger into the accessory socket,
   then switch the ignition ON (the engine can stay off). Within a second the
   display should read `0`.
2. Drive. It should track your speedometer closely. Usually the HUD reads
   1-3 mph *lower*, because factory speedometers deliberately over-read.
3. Mount the display flat on the dash top, digits facing **up**, with the
   *bottom* of the digits toward the windshield. Stick the HUD film on the
   glass above it. Adjust position so the reflection sits low in your view.

---

## 8. Troubleshooting

| Symptom | Likely cause / fix |
|---------|-------------------|
| Stuck on `88`, no fault code | The board is resetting in a loop, not frozen. Check that nothing is wired to RST, especially a ground wire one pin off |
| Always `--`, even ignition on (serial shows `tx` rising, `rx=0`) | TX/RX swapped at the transceiver (the most common mistake): swap the D2/D10 wires |
| Still `--` | The car may use 29-bit CAN IDs (some trucks, some pre-2008 vehicles). The comment in `send_request()` shows what to change |
| `--` and the car is older than about 2004 | It may not use CAN at all (K-line or J1850). This design needs a CAN car; 2008+ US vehicles are guaranteed |
| `E4` on the display | The CAN receive line never goes idle, most often because the transceiver is unpowered. Check 3.3 V at its 3V3 pin |
| Digits swapped or mirrored wrong | Toggle `HUD_MIRROR`, and check the left/right digit wiring (PB0 = physical left) |
| One digit dimmer than the other | PNP base resistor missing or the wrong value on that digit, or a poor solder joint on its emitter to 3V3 |
| Random resets or glitches in the car | A cheap USB charger with noisy output: try a name-brand one, or add 100 µF across the Nucleo's 5V/GND pins |
| Works on the bench, dead in the car | Missing OBD ground wire (pin 4/5 → board GND). USB ground alone isn't enough for CAN |
| Too dim in daylight | Drop the segment resistors toward 100 Ω (see the §4 note). Beyond that, use a MAX7219 driver and run the display at 5 V |
| Reflection shows a double image | You skipped the HUD film. It matters |

---

## 9. Nice Upgrades Later

- A long-press button to switch between mph, km/h, coolant temp (PID 0x05)
  and RPM÷100 (PID 0x0C)
- Auto-dim at night using an LDR on an ADC pin
- Hardwire power with an add-a-fuse tap on a switched circuit, to free up
  the accessory socket
