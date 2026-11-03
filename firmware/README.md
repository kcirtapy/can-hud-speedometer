# HUD Speedometer firmware

Complete PlatformIO project for the Nucleo-L432KC. Open this folder, click
Build, click Upload. No CubeMX, no `.ioc`, no manual toolchain setup.

```
firmware/
├── platformio.ini                 board + toolchain settings
├── include/
│   └── stm32l4xx_hal_conf.h       which HAL modules to build
└── src/
    └── main.c                     all the firmware
```

---

## 1. One-time setup

1. Install **VS Code**.
2. In VS Code: Extensions (Ctrl/Cmd-Shift-X), search **PlatformIO IDE**,
   Install. First launch downloads the ARM toolchain automatically; it takes
   a few minutes and only happens once.
3. Restart VS Code when it asks.

Windows only: if the board isn't detected later, install the **ST-LINK USB
driver** from st.com (search "STSW-LINK009"). macOS and Linux need nothing
else. On Linux, if upload fails with a permissions error, install
PlatformIO's udev rules:
`curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules && sudo udevadm control --reload-rules && sudo udevadm trigger`

## 2. Build and flash

1. **File > Open Folder** and select this `firmware` folder. Open the folder
   itself, not the repository root and not an individual file: PlatformIO
   looks for `platformio.ini` at the root of whatever you open.
2. Wait for the status bar to finish "Configuring project" the first time.
   It downloads the STM32Cube HAL for this chip, which takes a few minutes,
   once.
3. Plug the Nucleo into your computer with the micro-USB cable.
4. Click the **✓** (Build) icon in the status bar at the bottom. Expect
   `SUCCESS` and roughly 15 KB flash / 2 KB RAM used.
5. Click the **→** (Upload) icon. The board runs the new firmware
   immediately and on every power-up from then on.

## 3. What you should see

On power-up the display runs a **lamp test**: both digits show `88` (every
segment lit) for 0.8 s. Use it to spot a dead segment or a wiring mistake.

Then:
- `--` means no CAN data. Normal on the bench, and normal until the
  ignition is on.
- A number means it's reading the ECU.
- `E` followed by a digit is a fault code, see section 6.

The digits are **mirrored** by default, for reading in the windshield. Hold
the display up to a mirror and it should read normally. To bench-test in
normal orientation, set `HUD_MIRROR` to `0` near the top of `main.c` and
re-upload.

If the display stays on `88` forever with no fault code, the board is almost
certainly resetting in a loop rather than frozen. Check that nothing is
wired to the RST pin, which sits right beside GND on the Nucleo headers.

## 4. Bench self-test, no car needed

Set `BENCH_TEST` to `1` (and `HUD_MIRROR` to `0`) and upload. With nothing
connected but the display, it should show the lamp test, then sweep the
speed from 0 to 99 and repeat, about 16 seconds per cycle.

This mode puts the CAN controller in silent loopback, where it receives its
own transmissions without driving the bus, and has the firmware impersonate
the ECU with a well-formed reply to PID 0x0D. A working sweep validates the
clock setup, the timer, the multiplexing, both digit drivers, all seven
segments, the CAN peripheral, the acceptance filter, the receive interrupt,
the frame parsing, the km/h to mph conversion, and the rendering. It does
not test the transceiver or the OBD wiring.

Set `BENCH_TEST` back to `0` and `HUD_MIRROR` back to `1` before installing
in the car. In bench mode the firmware ignores the real bus and shows a fake
sweeping speed.

## 5. The serial monitor, your main debugging tool

The firmware prints a status line every second over the same USB cable.
Click the **plug icon** (Serial Monitor) in the status bar. The baud rate is
115200.

```
=== HUD speedometer ===
clock: MSI trimmed by LSE (best)
CAN: up, waiting for ECU...
tx=10 rx=0  kmh=0  mph=0  no data
tx=20 rx=0  kmh=0  mph=0  no data
```

Reading it:

| What you see | What it means |
|---|---|
| `tx` climbing, `rx=0` | Requests are going out, nothing is coming back. Ignition off, TX and RX swapped at the transceiver, CANH/CANL not on OBD pins 6 and 14, the OBD ground wire missing, or a car that uses 29-bit IDs |
| `rx` climbing, sensible `kmh` | Working |
| `CAN FAULT` | CAN could not start. The display shows the code, and it retries every 3 s |
| `clock: HSI16 fallback` | The LSE crystal didn't start. HSI16 is marginal for CAN timing, so expect intermittent gaps in `rx` |
| The banner repeating over and over | The board is resetting. Check the RST pin wiring and the power supply |
| Nothing at all | Wrong COM port, or `DEBUG_UART` set to 0 |

Setting `DEBUG_UART` to `0` once everything works is optional; it costs
nothing to leave on.

## 6. Fault codes

Faults after the display is running appear on the display itself. CAN faults
are not fatal: the firmware retries every 3 seconds, so connecting the
transceiver or switching on the ignition recovers without a reset.

| Display | Meaning |
|---------|---------|
| `E2` | CAN peripheral init failed |
| `E3` | CAN filter configuration failed |
| `E4` | CAN could not start because the receive line never went idle, most often an unpowered transceiver |
| `E9` | SysTick is not incrementing |

If startup fails before the display is running, the onboard green LED (LD3)
blinks a code instead, repeating with a pause:

| Blinks | Meaning |
|--------|---------|
| 1 | Clock configuration failed |
| 5 | Timer init failed |

## 7. Settings at the top of main.c

| Define | Default | Meaning |
|--------|---------|---------|
| `HUD_MIRROR` | `1` | Mirror digits for the windshield. `0` for direct reading |
| `COMMON_CATHODE` | `0` | `0` = common anode + PNP drivers (the LTD-4608JR). `1` = common cathode + NPN drivers |
| `DEBUG_UART` | `1` | Serial status output |
| `BENCH_TEST` | `0` | Loopback self-test, see section 4 |
| `REQUEST_MS` | `100` | ECU poll interval, ms |
| `DATA_TIMEOUT_MS` | `1000` | How long before falling back to `--` |
| `LAMP_TEST_MS` | `800` | Duration of the power-up `88` test |

## 8. Adding other readouts later

Any OBD-II PID works the same way: change `0x0D` in `send_request()` and the
matching check in the receive callback.

| PID | Value | Conversion |
|-----|-------|-----------|
| `0x0D` | Speed | `data[3]` km/h |
| `0x0C` | Engine RPM | `(data[3]*256 + data[4]) / 4`, divide by 100 to fit 2 digits |
| `0x05` | Coolant temp | `data[3] - 40` °C |
| `0x2F` | Fuel level | `data[3] * 100 / 255` % |
| `0x0F` | Intake air temp | `data[3] - 40` °C |
