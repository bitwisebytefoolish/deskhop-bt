# DeskHop-BT — Bluetooth fork

This is a fork of [hrvach/deskhop](https://github.com/hrvach/deskhop) that replaces the wired USB-host side with a **Bluetooth HID host on Raspberry Pi Pico 2 W**, so wireless keyboards and mice (BT Classic *and* BLE HID-over-GATT) can drive a deskhop KVM without dongles.

The deskhop board-to-PC side is unchanged from upstream — your computers still see a normal USB HID composite device with no host-side software required. All Bluetooth handling lives entirely on the deskhop hardware.

If you're looking for the upstream wired version, see [README.md](README.md).

---

## Table of contents

- [What this fork adds](#what-this-fork-adds)
- [Architecture](#architecture)
- [Hardware required](#hardware-required)
- [Carrier modification (one-time)](#carrier-modification-one-time)
- [Building](#building)
- [Flashing each board](#flashing-each-board)
- [Pairing flow](#pairing-flow)
- [OLED device-management UI](#oled-device-management-ui)
- [LED feedback states](#led-feedback-states)
- [Tested peripherals](#tested-peripherals)
- [Wiping bonds (factory reset)](#wiping-bonds-factory-reset)
- [Known limitations](#known-limitations)
- [Migrating from upstream wired deskhop](#migrating-from-upstream-wired-deskhop)
- [Where to file issues](#where-to-file-issues)

---

## What this fork adds

- **BT Classic HID host** — pairs with classic Bluetooth keyboards (e.g. most Apple, Logitech, Keychron in Classic mode) via BTstack's `hid_host`.
- **BLE HID-over-GATT (HOGP) host** — pairs with modern BLE-only peripherals (8BitDo, Logitech BLE mice, BLE numpads) via BTstack's `hids_client`. Required for modern mice and most 2018+ keyboards that no longer ship Classic.
- **Multi-device support** — up to **4 simultaneous bonded HID devices** on the BT-host board. Mix and match keyboards, mice, keypads in any combination on the same chip.
- **Persistent bonds** — bonds stored in flash via BTstack's TLV bank; devices reconnect automatically on cold boot, no re-pairing required.
- **On-device OLED UI (optional)** — an SSD1306 128×64 OLED + three buttons on board A provide a live status screen and a menu to **pair new devices, forget bonds, list paired devices**, and an **About** screen — no host-side tool or UART cable needed. Persisted friendly names show in the list. The panel is optional at runtime: builds without one fall back to the headless behaviour. See [OLED device-management UI](#oled-device-management-ui).
- **Cheaper output peer** — board B can be a stock Raspberry Pi Pico (RP2040), since only board A needs a radio.

---

## Architecture

```
┌────────────────────────┐   ADuM1201        ┌────────────────────────┐
│      BOARD A           │   isolated UART   │      BOARD B           │
│  Pi Pico 2 W           │ <───────────────> │  Pi Pico (RP2040)      │
│                        │                   │  *or* Pi Pico 2 W      │
│  - BT Classic HID host │                   │                        │
│  - BLE HOGP host       │                   │  - USB HID device only │
│  - USB HID device      │                   │  - No radio used       │
│  - Up to 4 simultaneous│                   │  - Cheap                │
│    bonded peripherals  │                   │                        │
└─────────┬──────────────┘                   └─────────┬──────────────┘
          │ USB                                        │ USB
          ▼                                            ▼
      Host PC A                                    Host PC B
```

- One chip does *all* the Bluetooth work.
- The active-output toggle hotkey routes input to either board's locally attached host PC.
- The 2-board layout matches the upstream wired deskhop PCB. No PCB redesign needed for the BT fork.

---

## Hardware required

| Slot     | Chip                          | Why                                       |
|----------|-------------------------------|-------------------------------------------|
| Board A  | **Pi Pico 2 W** (RP2350+CYW43)| BT radio + flash for bonds                |
| Board B  | Pi Pico (RP2040) **or** Pico 2 W | USB HID device only; cheaper part fine  |
| Carrier  | Existing deskhop PCB (v1.0 / v1.1) | Drop-in; one trace mod required (below) |

The Pi Pico 2 W is a physical drop-in on the existing deskhop carrier — GPIO 23/24/25 (where CYW43 lives internally on the 2 W) are not used by the carrier traces. See issue [#1](https://github.com/bitwisebytefoolish/deskhop-bt/issues/1) for the pin-by-pin hardware audit.

---

## Carrier modification (one-time)

The BT fork's board-role autoprobe needs a way to distinguish socket A from socket B at boot. **Solder a small wire bridge from GP18 (pin 24) to the adjacent GND (pin 23) on socket A only.** Leave socket B unmodified.

```
   Socket A side                Socket B side
   ┌─────────────┐              ┌─────────────┐
   │             │              │             │
   │  ●─GP18 ●   │              │   GP18 ●    │
   │  │       ●  │              │        ●    │
   │  ●─GND  ●   │  (bridge)    │   GND  ●    │  (unmodified)
   │             │              │             │
   └─────────────┘              └─────────────┘
```

Why: the upstream wired autoprobe used GP13's pull-state, but on RP2350 the digital isolator's idle-high output overwhelms the internal pulls on both sockets, so both boards read identical. GP18 is otherwise unused in deskhop. See [`src/include/pinout.h`](src/include/pinout.h) line ~50 for the full rationale.

**Alternative — no soldering required:** pass `-DFORCE_BOARD_ROLE=0` (for A) or `-DFORCE_BOARD_ROLE=1` (for B) to CMake. Useful for bring-up or when you don't want to modify the carrier. You'll need separate builds per side in that case.

---

## Building

### Toolchain

You need a working `arm-none-eabi-gcc` toolchain. The Homebrew formula is broken on macOS as of mid-2026; install the ARM cask instead:

```shell
# macOS
brew install --cask gcc-arm-embedded
# or for Debian/Ubuntu
sudo apt install build-essential cmake gcc-arm-none-eabi libnewlib-arm-none-eabi python3
```

### Submodules

```shell
git submodule update --init --recursive
```

This pulls pinned versions of pico-sdk (2.2.0), tinyusb, btstack, cyw43-driver, lwip, and mbedtls.

### Build board A (BT host)

```shell
cmake -S . -B build-pico2w -DPICO_BOARD=pico2_w
cmake --build build-pico2w -j
# → build-pico2w/deskhop.uf2
```

This enables the [OLED device-management UI](#oled-device-management-ui) by default (`DH_OLED_UI=ON` for `pico2_w`). It's safe to flash whether or not a panel is wired — the UI no-ops if no OLED is detected. To build the headless "lite" variant (no OLED/button code compiled in, always auto-pair), pass `-DDH_OLED_UI=0`. Pre-built `.uf2`s for each variant are attached to every [GitHub release](https://github.com/bitwisebytefoolish/deskhop-bt/releases) (`deskhop-bt-A-pico2w-oled.uf2` for board A; `deskhop-bt-B-pico.uf2` / `-B-pico2w.uf2` for board B).

### Build board B (output peer)

Option 1 — RP2040 (cheapest):

```shell
cmake -S . -B build-pico -DPICO_BOARD=pico
cmake --build build-pico -j
# → build-pico/deskhop.uf2
```

Option 2 — Pico 2 W (if you have spares):

Use the same `build-pico2w` .uf2 from board A; the autoprobe handles role detection.

### Forcing the board role (no carrier mod)

```shell
cmake -S . -B build-A -DPICO_BOARD=pico2_w -DFORCE_BOARD_ROLE=0  # always A
cmake -S . -B build-B -DPICO_BOARD=pico     -DFORCE_BOARD_ROLE=1  # always B
```

---

## Flashing each board

Each board flashes independently. Easiest order: flash board B first, then board A, then power both together.

### Flashing the first time (BOOTSEL drag-drop)

1. Hold the BOOTSEL button on the target board, connect USB → an `RPI-RP2` (RP2040) or `RP2350` mass-storage drive appears.
2. Drag-drop the matching `.uf2` from `build-pico/` (RP2040) or `build-pico2w/` (Pico 2 W) onto the drive.
3. Board reboots itself when the copy completes.

### Subsequent upgrades

The same BOOTSEL drag-drop procedure works for re-flashing. Hold BOOTSEL on the board you want to update, plug it in, drag-drop the new `.uf2`.

The upstream config-mode hotkey **`Left Ctrl + Right Shift + C + O`** also reboots the side your keyboard is plugged into and opens the web configuration UI (for editing settings, not for flashing firmware).

If you want a hotkey that drops a specific board directly into BOOTSEL without pressing the button, see the `fw_upgrade_hotkey_handler_*` table in [`src/keyboard.c`](src/keyboard.c) — the chord is documented inline and may shift as the project converges on a final UX.

---

## Pairing flow

There are two pairing models depending on the build / hardware:

### Auto-pair (lite build, or OLED build with no panel detected)

The BT-host firmware scans / inquires continuously while it has free slots, and **any Bluetooth keyboard / mouse / keypad in pairing mode auto-pairs** the moment its advertisement or inquiry response is seen. Just-works pairing is auto-confirmed; no PIN entry is needed. This is the behaviour of the `v1.0.0-bt` "lite" build, and of the OLED build when no panel is wired (so a headless unit is still usable).

To pair a device:

1. Put the device into its pairing mode (consult the device's manual — typically "hold the Bluetooth button for 3 seconds until it blinks fast", or remove and re-insert batteries).
2. Wait. Within ~5 seconds you should see the on-board LED settle into the "connected" pattern (4 flashes) — see [LED feedback states](#led-feedback-states) below.
3. The device is now bonded and persists across cold boots.

### Opt-in pairing (OLED build with a panel present)

When an OLED panel is detected at boot, pairing is **closed by default** — the host ignores unbonded advertisers and declines their pairing requests, so a forgotten device can't silently re-pair. To add a device you open a temporary pairing window from the menu: **SELECT → Pair new device**. The screen shows a 60-second countdown with a bouncing "scanning" dot; put your device into pairing mode during the window. Bonded devices reconnect at any time regardless of the window. See [OLED device-management UI](#oled-device-management-ui).

### Slot limit

Once 4 devices are bonded and connected, the host stops accepting new pairings until one is forgotten. On an OLED build you forget devices from the menu (**Paired devices → SELECT → Forget device**, or **Forget all bonds**). On a lite build the only way to release a slot is to wipe all bonds — see [Wiping bonds](#wiping-bonds-factory-reset).

If you want to watch the pairing flow live, attach a UART debug cable to the BT-host board (UART1 pins, 115200 8N1) and the firmware prints a per-event trace.

---

## OLED device-management UI

Board A optionally drives a **128×64 SSD1306 OLED** (I2C) plus three buttons — **UP**, **DOWN**, **SELECT** — for live Bluetooth device management with no host tool or UART cable. This is the default for the `pico2_w` build (`DH_OLED_UI=ON`); the `pico` / lite build omits it entirely.

The panel is optional **at runtime**: if no OLED ACKs on the I2C bus at boot, the UI quietly no-ops and the firmware behaves like the lite build (including reverting to [auto-pair](#pairing-flow)).

### Wiring

| OLED pin | Pico 2 W   |
|----------|------------|
| SDA      | GP0 (I2C0) |
| SCL      | GP1 (I2C0) |
| VCC      | 3V3        |
| GND      | GND        |

I2C address `0x3C` (some panels are `0x3D`), 400 kHz fast-mode.

Buttons are momentary push-buttons wired **GPIO → button → GND**, using the RP2350's internal pull-ups (no external resistors needed):

| Button   | Pin  |
|----------|------|
| UP       | GP19 |
| DOWN     | GP20 |
| SELECT   | GP21 |

These GPIOs are otherwise unused on the deskhop carrier, so the typical bring-up is a jumper from the unused header pins to a small breadboard with three tactile switches. Pin assignments live in [`src/include/pinout.h`](src/include/pinout.h).

### Navigation

Button hints are drawn on-screen as glyphs: **○** = SELECT, **↑ / ↓** = UP / DOWN.

- **SELECT** = click (activate / enter); **hold SELECT** (≥ 500 ms) = back / cancel.
- **UP / DOWN** move the `>` cursor.
- Any non-status screen auto-reverts to the status screen after ~10 s of no input.

### Screens

- **Status** (always-on): the active-output letter (**A** / **B**) at large size, a Bluetooth icon that flashes while the radio comes up then goes solid, an "N of M paired" counter, and a row per connected device with a type icon (keyboard / mouse / keypad / generic), its name, and a connection dot. Long names scroll (marquee) with a single-cell "…" overflow hint. Empty slots read "- free".
- **Menu** (SELECT from status): **Paired devices**, **Pair new device**, **Forget all bonds**, **About**.
- **Paired devices**: every bonded device (connected or not) with a connection-status dot. SELECT opens **device info** (full address, status) with a **Forget device** action.
- **Pair new device**: opens a 60-second [opt-in pairing window](#pairing-flow) with a countdown and a bouncing "scanning" dot; shows a "Paired!" splash with the device name on success.
- **Forget all bonds**: confirm-gated wipe of every bond.
- **About**: title, firmware version, build date, author, and fork credit.

### Friendly names

Device names are harvested from advertising data and, for devices that don't advertise a name (e.g. the 8BitDo Retro), read from the **GAP Device Name** characteristic (0x2A00) over GATT after connect. Names are persisted in a flash TLV table so they survive reboots and show in the device list even before reconnect.

---

## LED feedback states

The on-board CYW43 LED on board A doubles as a Bluetooth diagnostic indicator. The pattern is **sticky**: it shows the *latest* state of the BT subsystem and keeps looping until something changes. On an OLED build the screen supersedes the LED for routine status — the LED is most useful on the lite build or for diagnosing a failure code.

### Stage patterns (N flashes, pause, repeat)

| Stage | Meaning                          |
|-------|----------------------------------|
| **1** | Radio up — HCI reached `WORKING` |
| **2** | Peripheral discovered             |
| **3** | Connecting                        |
| **4** | Connected (steady state ✓)        |
| 5     | HID descriptor parsed             |
| 6     | First HID report received         |
| **7** | Connect failed (see status below) |

A healthy idle state is **stage 4** repeating (4 flashes, pause, 4 flashes, pause…). If you see this you're done — peripherals are connected.

### Failure status codes (stage 7)

When stage is 7 (failed), the LED follows a second blink cycle that encodes BTstack's status code:

```
[7 flashes] [1.5 s gap] [N flashes] [2.5 s gap] [7 flashes] [1.5 s gap] …
            ^^^ stage    ^^^ error code
```

Status codes ≥ 5 are blinked in groups of 5 for readability — e.g. code 22 = `[5][5][5][5][2]`.

Common codes:

| Code | Meaning                                         |
|------|-------------------------------------------------|
| 4    | HCI page timeout (peer didn't respond)          |
| 5    | HCI authentication failure                      |
| 6    | HCI PIN/key missing                             |
| 8    | HCI connection timeout                          |
| 12   | BTstack state-machine reentry                   |
| 14   | Rejected for security reasons                   |
| 19   | Remote terminated                               |
| 22   | Local terminated                                |

Inline rationale and the full code path live in [`src/bt_hid_host.c`](src/bt_hid_host.c) — search for `Sticky stage indicator`.

### Disabling the BT indicator

If you want the LED back for the standard upstream uses (active-output indicator etc.), the BT firmware releases the LED when no BT activity is in flight. While any BT state machine is mid-cycle (pairing, connecting, fail-retry loop) it owns the LED; otherwise the upstream LED logic runs as normal.

---

## Tested peripherals

Devices verified working in real hardware as of 2026-05-20. **Status legend:** ✅ = fully working, ⚠️ = working with notes, ❌ = does not work.

### BLE HID-over-GATT (HOGP)

| Device class | Address type | Notes | Status |
|---|---|---|---|
| BLE mouse, descriptor ~97 B (Logitech-class, static random address) | random | Uses `ADV_DIRECT_IND` on reconnect; needs the cold-boot reconnect fix from [#38](https://github.com/bitwisebytefoolish/deskhop-bt/pull/38) | ✅ |
| BLE keyboard, descriptor ~345 B (random address) | random | Uses `ADV_DIRECT_IND` on reconnect | ✅ |
| BLE keypad, descriptor ~210 B (public address) | public | Uses `ADV_IND` (undirected) — straightforward case | ✅ |

Named devices verified: **8BitDo Retro Mechanical Keyboard** (BLE; name resolved over GATT) and **Logitech MX Master** (BLE mouse).

### BT Classic HID

| Device | Notes | Status |
|---|---|---|
| Keychron K7 (Classic mode) | Phase-1 primary test target | ✅ |

(If you have a device that works or doesn't, please file an issue with the device name + behavior + LED status code so we can extend this table.)

### Per-class footprint

Each bonded device consumes:

- ~30 B for the link key
- ~50 B for the BLE identity DB entry (IRK + identity address)
- One slot in the `MAX_NR_HIDS_CLIENTS = 4` runtime pool

The bond DB is sized for 32 entries to absorb peripherals that rotate their IRK every session (a privacy feature on some BLE mice). See [#34](https://github.com/bitwisebytefoolish/deskhop-bt/issues/34) for the diagnostic and [PR #35](https://github.com/bitwisebytefoolish/deskhop-bt/pull/35) for the sizing.

---

## Wiping bonds (factory reset)

Sometimes you want to start from a clean bond DB — testing pairing flow, selling the device, recovering from a confused multi-device state.

### Option 1 — picotool erase (preferred, when it works)

```shell
# Put board A in BOOTSEL (hold button, connect USB)
picotool erase --range 0x103FD000 0x10400000
# This erases the BTstack TLV bank (8 KB) + deskhop config sector (4 KB).
```

Verify with:

```shell
picotool save -r 0x103FD000 0x103FF000 /tmp/bank.bin && xxd /tmp/bank.bin | head -5
# Should show all 0xFF if the erase took effect.
```

### Option 2 — btstack-bank-nuke firmware (sledgehammer fallback)

If `picotool erase` silently doesn't take (we've observed this with picotool 2.2.0 in some BOOTSEL session states), use the bundled purpose-built wiper firmware:

```shell
cmake -S tools/btstack-bank-nuke -B tools/btstack-bank-nuke/build -DPICO_BOARD=pico2_w
cmake --build tools/btstack-bank-nuke/build -j
# → tools/btstack-bank-nuke/build/btstack_bank_nuke.uf2
```

Then:

1. Hold BOOTSEL on board A, connect USB → `RP2350` drive appears.
2. Drag-drop `btstack_bank_nuke.uf2` onto the drive.
3. The firmware erases the last 12 KB of flash and reboots back into BOOTSEL automatically.
4. Drag-drop the real `build-pico2w/deskhop.uf2` to flash a fresh firmware on the now-clean board.

After either method, the next boot should log `LE device DB has 0/32 bonded entries` and all your bonded peripherals will need to be re-paired.

---

## Known limitations

- **Explicit pairing control needs the OLED UI.** On an OLED build with a panel, pairing is opt-in (**Pair new device** opens a timed window) and you can forget devices from the menu — see [OLED device-management UI](#oled-device-management-ui). On the **lite build** (or an OLED build with no panel) there's no pairing trigger or per-device forget: any peripheral in pairing mode auto-bonds while a slot is free, and the only way to release a slot is to wipe all bonds. Tracked in [#7](https://github.com/bitwisebytefoolish/deskhop-bt/issues/7) and [#22](https://github.com/bitwisebytefoolish/deskhop-bt/issues/22).
- **No rename / preferred-reconnect.** The OLED UI lists, pairs, and forgets devices, but per-device rename and a preferred-reconnect choice aren't implemented yet — remaining scope on [#22](https://github.com/bitwisebytefoolish/deskhop-bt/issues/22).
- **REPORT-mode HID descriptor parsing is BOOT-mode-only on Classic.** N-key rollover and media keys may not pass through cleanly on Classic peripherals. BLE HOGP uses report mode and is unaffected. Tracked in the same Phase-1 group.
- **No gamepad support.** Xbox / Switch Pro tracked in [#24](https://github.com/bitwisebytefoolish/deskhop-bt/issues/24).
- **No N-output star topology.** The current 2-board architecture only drives 2 host PCs. 3+ outputs via a shared UART bus is tracked in [#23](https://github.com/bitwisebytefoolish/deskhop-bt/issues/23).
- **Carrier mod required** (or `-DFORCE_BOARD_ROLE`). See [Carrier modification](#carrier-modification-one-time) above.
- **Two boards still required.** Single-board mode where one Pico 2 W drives both host PCs via dual PIO USB is not implemented.

---

## Migrating from upstream wired deskhop

If you're already running `hrvach/deskhop` and want to switch to this fork:

1. **Hardware**: confirm board A is a Pi Pico 2 W. Pi Pico W (the first-gen RP2040+CYW43) is **not** sufficient — the Pico 2 W's RP2350 is required for the BLE stack memory footprint. Board B can stay as your existing RP2040 Pico.
2. **Carrier mod**: solder the GP18→GND bridge on socket A (see above), or build with `-DFORCE_BOARD_ROLE=...`.
3. **Build & flash**: follow [Building](#building) and [Flashing](#flashing-each-board) above. The build sets up parallel `pico2w` and `pico` targets in one CMakeLists.
4. **Pair your peripherals**: power up both boards, then put each BT peripheral into pairing mode in turn. The LED on board A confirms each pairing.
5. **What stays the same**: all upstream hotkeys, the active-output toggle, web config UI, mouse smoothing, gaming mode, lock-both — none of those are touched by the BT fork.
6. **What's gone**: the wired USB host on board A is **not** active in this fork. You can't plug a wired keyboard into board A's USB-A port any more (the PIO-USB host on board A is repurposed; board B's wired-USB-host capability is untouched, so if you want a fallback wired keyboard you can plug it into board B's USB-A port — it'll work but you lose the BT advantage on the keyboard side).

---

## Where to file issues

- Bluetooth pairing / reconnect / device-compatibility bugs: [open an issue](https://github.com/bitwisebytefoolish/deskhop-bt/issues/new) on this fork.
- Wired-deskhop / mouse-smoothing / config UI bugs (anything that also reproduces on upstream `hrvach/deskhop`): file upstream where there's a larger user base to triage.

When filing a BT bug, please include:

- Board A and board B chip types (Pico 2 W / Pico)
- Device name(s) involved
- Stage number + status code from the LED (or the UART debug log if you have one attached)
- Whether the device works when paired directly to a phone / laptop (to isolate device-side bugs from deskhop-bt bugs)
