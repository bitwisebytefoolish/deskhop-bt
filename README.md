# DeskHop-BT — Wireless Bluetooth KVM

Fast two-PC keyboard / mouse switching with wireless Bluetooth peripherals. No dongles, no host-side software, no copy-paste leakage between machines.

> This is a **fork** of the original [hrvach/deskhop](https://github.com/hrvach/deskhop) that replaces the wired USB-host side with a Bluetooth HID host on Pi Pico 2 W. Wireless BT keyboards and mice (BT Classic *and* BLE) drive the deskhop switch directly. The board-to-PC side, the PCB, the case, and most of the upstream UX (hotkeys, mouse cursor-jump, web config, gaming mode) are unchanged.
>
> **See [BLUETOOTH.md](BLUETOOTH.md)** for the deep dive: build / flash / pair / LED feedback / tested peripherals / migration from upstream.

![DeskHop case and board](img/case_and_board_s.png)

------

## What this fork adds

- **Bluetooth Classic HID host** — pairs with classic BT keyboards (Apple, Logitech, Keychron in Classic mode).
- **BLE HID-over-GATT (HOGP) host** — pairs with modern BLE-only mice and keyboards (8BitDo, BLE mice, BLE numpads).
- **Up to 4 simultaneous bonded peripherals** on one chip. Mix keyboards, mice, and keypads freely.
- **Persistent bonds** stored in flash — devices reconnect automatically on cold boot.
- **Cheaper output peer** — board B can be a stock Raspberry Pi Pico (RP2040); only board A needs a radio.
- **No host-side software** — your PCs see the deskhop as a normal USB HID composite device, exactly as before.

## What stays the same (inherited from upstream)

- Cursor-jump-between-screens behavior via absolute mouse coordinates.
- Active-output toggle via mouse drag or `Left Ctrl + Caps Lock`.
- Web configuration UI (Chromium-only) via `Left Ctrl + Right Shift + C + O`.
- Mouse slowdown, switch lock, lock both PCs, gaming mode, screensaver.
- Galvanic isolation between outputs via the ADuM1201 / ISO7721DR digital isolator.
- 3D-printable snap-fit case, identical PCB footprint.
- Cross-platform: Linux, macOS, Windows.

[![Open Source Hardware Logo](img/oshw.svg)](https://certification.oshwa.org/de000149.html)
*OSHW certification belongs to the upstream DeskHop project; this fork inherits the hardware design.*

## How it works

The device sits between your Bluetooth peripherals and your two host PCs. Board A pairs with the BT keyboard / mouse / keypad over the air and emits their HID reports onto an internal UART link. Board B receives those reports and emits them as USB HID to its locally attached PC. Active-output state decides which board's USB endpoint actually delivers a given report — and the seamless cursor-drag-between-monitors trick is preserved end-to-end.

```
┌────────────────────────┐   ADuM1201 /      ┌────────────────────────┐
│  Board A (Pico 2 W)    │   ISO7721DR       │  Board B (RP2040 or    │
│                        │   isolated UART   │  Pico 2 W)             │
│  - BT Classic HID host │ <───────────────> │                        │
│  - BLE HOGP host       │                   │  - USB HID device only │
│  - USB HID device      │                   │  - No radio used       │
│  - Up to 4 bonded peers│                   │                        │
└─────────┬──────────────┘                   └─────────┬──────────────┘
          │ USB                                        │ USB
          ▼                                            ▼
      Host PC A                                    Host PC B
```

See [BLUETOOTH.md](BLUETOOTH.md) for the full architecture rationale, build instructions, and tested-peripherals matrix.

## Mouse

To get the mouse cursor to magically jump across, the mouse HID report descriptor was changed to use absolute coordinates and the relative motion deltas (now sourced over BLE HOGP, not wired USB) accumulate internally, keeping accurate tally of position.

When the cursor tries to leave the active monitor in the direction of the other monitor, deskhop keeps the Y coordinate, swaps max X for min X, and flips outputs. The cursor seamlessly reappears at the same height on the other monitor — even though the input was wireless and is now traveling across two boards and an isolated UART link.

![DeskHop Mouse Demo](img/deskhop-demo.gif)

<p align="center">Dragging the mouse from Mac to Linux automatically switches outputs.</p>

The actual switch happens the instant one arrow stops moving and the other starts.

## Keyboard

Board A acts as a Bluetooth HID host — bonded keyboards are connected over BT Classic or BLE HOGP and their reports are demuxed, then forwarded down the UART link to board B (or kept locally on board A, depending on active output). The upstream hotkey-detection logic runs against the incoming reports just as it would for a wired keyboard — the BT layer is transparent to everything downstream.

The classic deskhop hotkey (`Left Ctrl + Caps Lock`, default — fully remappable in the web UI) still toggles the active output. Keyboard LEDs round-trip from the host PC back to the BT peripheral if the peripheral supports the standard LED output report.

![DeskHop Typing Demo](img/demo-typing.gif)

## Getting started

### Quick path

1. **Hardware**: a deskhop carrier PCB (v1.0 or v1.1) with **a Pi Pico 2 W in socket A** and either a Pi Pico (RP2040) or Pi Pico 2 W in socket B. One small carrier modification on socket A — bridge GP18 to the adjacent GND — see [BLUETOOTH.md](BLUETOOTH.md#carrier-modification-one-time).
2. **Toolchain**: `arm-none-eabi-gcc`, CMake, Python 3.
3. **Build** the two firmware variants:
   ```shell
   git submodule update --init --recursive
   cmake -S . -B build-pico2w -DPICO_BOARD=pico2_w        # board A (BT host)
   cmake --build build-pico2w -j
   cmake -S . -B build-pico   -DPICO_BOARD=pico           # board B (RP2040 output peer)
   cmake --build build-pico -j
   ```
4. **Flash** each board via BOOTSEL (hold button, plug USB, drag-drop the matching `.uf2`).
5. **Pair** — power both boards together, put each BT peripheral into pairing mode; the LED on board A confirms each pair. See [BLUETOOTH.md → Pairing flow](BLUETOOTH.md#pairing-flow).

### Full walkthrough

See **[BLUETOOTH.md](BLUETOOTH.md)** for:

- Detailed toolchain setup per OS
- Carrier modification photo / diagram
- `FORCE_BOARD_ROLE` build-time override if you don't want to solder
- LED feedback states (sticky-stage indicator with 1-7 stages + error codes)
- Tested peripherals matrix
- Wiping bonds (factory reset, picotool + nuke-firmware fallback)
- Migration checklist for users coming from upstream wired deskhop
- Where to file issues (upstream vs fork triage)

### Docker build

An alternative reproducible build via Docker:

```shell
docker-compose -f misc/docker.yml run --rm build_container
```

To rebuild the disk image, see `disk/create.sh` (requires `dosfstools`). To rebuild the web config UI, see `webconfig/render.py` (requires Jinja2).

## Upgrading firmware

Flash each board via BOOTSEL drag-drop as in [Getting started](#getting-started). **Bluetooth bonds persist across firmware upgrades** — the BTstack TLV bank lives in a reserved flash region that's untouched by the firmware update path.

To **wipe bonds** as part of an upgrade (for testing, or before handing the device to someone else), see [BLUETOOTH.md → Wiping bonds](BLUETOOTH.md#wiping-bonds-factory-reset).

The upstream config-mode hotkey **`Left Ctrl + Right Shift + C + O`** still opens the web configuration UI on the side your keyboard is plugged into.

## Misc features

### Mouse slowdown

Ever tried to move that YT video slider to a specific position but your mouse moves too jumpy and you're suddenly playing "Operation" all over again? **Press right CTRL + right ALT** to toggle slow-mouse mode. Press again to restore normal speed.

### Switch lock

Lock yourself to one screen with **`Right CTRL + K`**. Stops you accidentally leaving the current screen. Toggle off with the same combo.

### Lock both screens

Lock both computers at once with **`Right CTRL + L`**. Configure the OS for each output in the web config first, since lock-shortcut differs per OS.

### Gaming mode

If your game doesn't like absolute mouse mode, toggle **gaming mode** with **`Left CTRL + Right Shift + G`**. You're locked to the current screen and the mouse acts as a standard relative mouse — also useful for VMs.

### Screensaver

Optional bouncy-pong-ball screensaver after a configurable inactivity window. Off by default. Useful workaround for buggy USB docks that won't resume video from standby.

## Hardware

[The circuit](schematics/DeskHop_v1.1.pdf) is based on two Raspberry Pi Pico form-factor boards. For this fork **board A must be a Pi Pico 2 W** (RP2350 + CYW43439 radio); board B can be a stock Pi Pico (RP2040) or another Pico 2 W. The Picos are connected over UART through an Analog Devices ADuM1201 dual-channel digital isolator or the pin-compatible TI ISO7721DR (preferred — cheaper, faster, better specs).

While Picos normally don't have dual USB, the [Pico-PIO-USB project](https://github.com/sekigon-gonnoc/Pico-PIO-USB) implements USB via the RP2040 / RP2350 programmable IO, so each board can present a USB HID device to its host PC.

The Pi Pico 2 W is a physical drop-in on the existing carrier PCB — GPIO 23/24/25 (where CYW43 lives internally on the 2 W) aren't used by the carrier traces. See issue [#1](https://github.com/bitwisebytefoolish/deskhop-bt/issues/1) for the full pin-by-pin audit.

## PCB

To keep things simple for DIY builds, traces stay on one side and parts count is minimized.

![PCB Image](img/plocica2.png)

USB D+/D- differential lines should be identical in length, but they are slightly asymmetrical on purpose to counter the length difference on the corresponding GPIO traces on the Pico board itself, so the overall lengths match.

Zd (differential impedance) is aimed at 90 Ω (~107 measured — close enough). PCB thickness is designed for 1.6 mm so the case snap-fit works as expected.

Two PCB versions exist (no major user-facing differences):

- **v1.0** — easier to solder and assemble.
- **v1.1** — adds ESD protection (TPD4E1U06DBVR), USB VBUS caps, silkscreen orientation markings, alignment holes for header-clone boards, and USB 27 Ω resistors. Slightly harder to hand-solder due to small SMD parts. TVS can in theory be omitted (not advised) and it will still work.

## Case

The [snap-fit case](case/) uses ~33 g of filament and prints in a couple of hours. PCB is held by pegs (horizontal) and snap-fit lugs (vertical) — no screws. The lid snap-fits with a screwdriver slot for opening; recessed top-markings can be filled with crayon for contrast.

![DeskHop with 3D Printed Case](img/deskhop-case.gif)

USB connectors are offset from the case side, so slightly larger holes will let cables reach in.

## Bill of materials

For the BT fork, the only change vs. upstream is swapping one Pi Pico for a Pi Pico 2 W on socket A. The upstream BoM otherwise applies unchanged. See the upstream [hrvach/deskhop README](https://github.com/hrvach/deskhop) for the canonical parts list and links.

Additional steps for the BT fork:

- Make the PCB ([Gerber](pcb/), 1.6 mm thickness)
- 3D print the case ([STL files](case/), ~33 g filament)
- Solder a wire bridge from GP18 to the adjacent GND on socket A (BT-fork carrier mod — see [BLUETOOTH.md](BLUETOOTH.md#carrier-modification-one-time))

## Assembly guide

If you've soldered before, the deskhop PCB is straightforward. If you haven't, the upstream video walkthrough still applies — the BT fork doesn't change the PCB or assembly procedure (just the chip on board A and the carrier-mod wire bridge):

[![PCB Assembly Guide](img/yt-video-s.jpg)](https://www.youtube.com/watch?v=LxI9NYi_oOU)

After soldering, clean flux from the PCB with isopropyl alcohol and a brush.

*(Note: the video covers PCB v1.0; v1.1 is very similar.)*

## Usage guide

### Keyboard shortcuts

*Config:*

- `Left Ctrl + Right Shift + C + O` — enter config (web UI) mode
- `Right Shift + F12 + D` — remove flash config
- `Right Shift + F12 + Y` — save screen-switch offset

*Usage:*

- `Right Ctrl + Right Alt` — toggle slower mouse mode
- `Right Ctrl + K` — lock / unlock mouse desktop switching
- `Right Ctrl + L` — lock both outputs (set OS per output in web config first)
- `Left Ctrl + Right Shift + G` — toggle gaming mode (lock to screen, relative mouse)
- `Left Ctrl + Right Shift + S` — enable screensaver
- `Left Ctrl + Right Shift + X` — disable screensaver
- `Left Ctrl + Caps Lock` — toggle active output

*(Some keyboards don't send both shifts simultaneously, which is why the config-mode chord uses Left Ctrl instead of Left Shift. If a chord doesn't work, try the equivalent in the web config.)*

### Switch cursor height calibration

Optional but handy if your screens differ in size or aren't perfectly aligned. Park the mouse on the larger screen at the height of the smaller one (see image below) and press `Right Shift + F12 + Y`. The LED (and Caps Lock) flash to confirm.

![Border height difference](img/border_top_s.png)

Repeat for the bottom border if needed. Calibration persists in flash.

### Multiple screens per output

Windows and macOS misbehave with multiple screens + absolute positioning. Workarounds exist (still experimental) — set the OS per output and the total screen count in the web config. Main screens go in the middle, secondaries on the edges.

![Multiple screens per output](img/deskhop-scr.png)

### Web configuration mode

1. Press `Left Ctrl + Right Shift + C + O` — the side your keyboard is plugged into reboots and enters config mode (LED blinks).
2. A USB drive "DESKHOP" appears with a single `config.htm`.
3. Open `config.htm` with Chromium or Chrome (Firefox lacks WebHID).
4. Click connect, allow the device to pair.
5. Configure options, click save.
6. Click "exit" in the menu to leave config mode.

![Web Config](img/config-page-big.png)

<details closed>
  <summary>Linux doesn't see device? Click here.</summary>

You probably need to tweak `/dev` permissions or add a udev rules file:

`/etc/udev/rules.d/99-deskhop.rules`:

```plain
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="c000", GROUP="plugdev", MODE="0660"
```

Make sure your user is in the `plugdev` group.

</details>

**Q: Why not just a website?**
**A:** Loading JavaScript from a remote location that interacts with your input devices is a security risk. The config page is local-only; nothing loads externally. The page is self-decompressing (small flash budget) but fully open source — rebuild it yourself from `webconfig/` if you want.

### Functional verification

When you connect a new USB peripheral or a Bluetooth peripheral pairs, the board flashes its LED and tells the other board to do the same — quick sanity check that USB / UART are alive end-to-end.

For Bluetooth pairing specifically, the LED on board A also encodes the current BT subsystem stage (1-7 flashes + error code on failure). See [BLUETOOTH.md → LED feedback states](BLUETOOTH.md#led-feedback-states).

## Security and Safety

Some features are deliberately missing to make the device safer. The threat model assumes your host PCs are untrusted; deskhop should never become a back-channel between them.

- **No copy-paste or any information sharing between systems** — prevents data leakage.
- **No WebHID device management without explicit user consent.** No inbound connectivity from output PCs except the standard keyboard-LED output report, hard-limited to 1 byte.
- **No firmware-upgrade trigger from the output PCs.** Only deliberate user action via a keyboard shortcut can do that.
- **No keyboard/mouse custom endpoints exposed.** Peripheral vulnerabilities are firewalled from the host PCs.
- **No input history retained.**
- **No device-initiated keystrokes**, for any reason. Only what you type / trigger comes out.
- **Outputs are physically separated and galvanically isolated** at ≥2 kV via the digital isolator.
- **All inter-board UART packets are fixed-length**; config options that cross the link are a short whitelist, mostly read-only. Cross-board firmware upgrades can be disabled in config.
- **No WiFi networking stack.** We link `pico_cyw43_arch_poll` — the no-lwIP variant — so the firmware has no IP / DHCP / WiFi station code paths at all. Only BTstack uses the CYW43 radio. The chip is *capable* of WiFi, but no code path brings it up; you can audit the `cyw43_arch_init` call site in `src/setup.c` and the `pico_cyw43_arch_poll` line in `CMakeLists.txt` to confirm.
- **Bluetooth uses LE Secure Connections** (modern BLE) and **SSP just-works** (Classic). Bonded link keys are stored in a flash region separate from configuration data; no plaintext key transmission. Bonds can be wiped with a single `picotool erase` or the bundled nuke firmware — see [BLUETOOTH.md](BLUETOOTH.md#wiping-bonds-factory-reset).
- **Configuration mode auto-disables** after a period of inactivity.
- **All code is open source**, no binary blobs, thoroughly commented. Audit before flashing.

This still doesn't guarantee anything, but it's a reasonable set of ground rules. If you have a use case that needs WiFi-off-by-default to be provable, the CYW43 boot path is in `src/setup.c` (search for `cyw43_arch_init`) — inspectable and trivially auditable.

## FAQ

1. **I just have two Picos, can I do without a PCB and isolator?**

   For the **upstream wired build**: yes, an isolator is recommended but it'll work without one. For the **BT fork**: you also need board A to be a Pi Pico 2 W, not a regular Pico — the BLE stack won't fit on RP2040.

1. **What happens if I have two different resolutions on my monitors?**

   The mouse moves in abstract coordinate space; each computer figures out how that maps to its physical screen. Just works.

1. **Where can I buy it?**

   Not for sale by this fork's maintainer. The upstream project has an Elecrow listing — see [hrvach/deskhop](https://github.com/hrvach/deskhop). Note that the BT fork hasn't been productized; you'd build the upstream-spec PCB and substitute a Pico 2 W on board A.

1. **When the active screen is changed via the mouse, does the keyboard follow (and vice versa)?**

   Yes — the goal is to feel like one machine.

1. **Will this work with my Bluetooth keyboard / mouse?**

   See the [tested peripherals matrix in BLUETOOTH.md](BLUETOOTH.md#tested-peripherals). If your device works, great; if it doesn't, file an issue with the LED status code and we'll triage. Modern BLE-only peripherals (post-2017 mice, 8BitDo keyboards) and most BT Classic keyboards should work.

1. **Will this work with USB combo dongles (Logitech Unifying, etc.)?**

   The BT fork doesn't use the wired USB-host path on board A — it's repurposed for the BT radio. If you want to use a Unifying receiver on board A, you want the upstream wired deskhop, not this fork. You *can* plug a wired keyboard or Unifying dongle into board B's USB-A port, but it'll only drive board B's local PC (no input routing).

1. **I have issues with build or compilation.**

   Check the upstream [Troubleshooting Wiki](https://github.com/hrvach/deskhop/wiki/Troubleshooting). For BT-fork-specific issues, [open an issue here](https://github.com/bitwisebytefoolish/deskhop-bt/issues/new).

## Software Alternatives

If a deskhop doesn't suit your situation, software KVM options:

- [Barrier](https://github.com/debauchee/barrier) — free, open source
- [Input Leap](https://github.com/input-leap/input-leap) — free, open source
- [Synergy](https://symless.com/synergy) — commercial
- [Mouse Without Borders](https://www.microsoft.com/en-us/garage/wall-of-fame/mouse-without-borders/) — free, Windows only
- [Universal Control](https://support.apple.com/en-my/HT212757) — free, Apple ecosystem only

## Shortcomings

- **Windows 10** broke HID absolute-coordinates behavior in KB5003637; you can't use more than 1 screen on Windows without the experimental workaround. Inherited from upstream.
- **macOS** has multi-screen quirks with absolute positioning. Inherited from upstream.
- **Two boards are still required.** Single-board mode (one Pico 2 W driving both host PCs) isn't implemented; tracked as a future direction in [#23](https://github.com/bitwisebytefoolish/deskhop-bt/issues/23).
- **Both PCs must be powered for the device to work** — each board is bus-powered from its host PC. Most desktops keep USB power on even when shut down, but some don't. Workaround: USB hub on the powered side.
- **No physical pairing trigger / forget-device UX** on board A yet. The current behavior is "any BT peripheral in pairing mode auto-pairs while a slot is free." Per-device management UX is deferred to the LCD-UI work in [#22](https://github.com/bitwisebytefoolish/deskhop-bt/issues/22).
- **Advanced keyboards** with knobs, sliders, or per-key custom hardware may see unsupported features ignored.
- **Limited tested-peripherals matrix.** See [BLUETOOTH.md](BLUETOOTH.md#tested-peripherals) for what's verified; file an issue with your hardware to extend it.

## Upstream relationship

This fork tracks [hrvach/deskhop](https://github.com/hrvach/deskhop). Bug fixes from upstream are pulled periodically. Bluetooth-specific work is developed here; if upstream is interested, subsets can be proposed as PRs.

The original DeskHop project is maintained by **hrvach** with significant contributions from the open-source community — the wired KVM design, PCB / case / firmware foundation, web config UI, mouse-jump trick, and hotkey UX all come from upstream. This fork's contribution is the Bluetooth host layer, the multi-device bond storage / reconnect logic, and the RP2040-output-peer architecture pivot. **Credit for the underlying KVM design belongs to the upstream project.**

If you're considering deskhop and don't need wireless input, use the upstream project directly — it has a larger user base, mature documentation, and the maintainer accepts PRs.

## Sponsor / donate

The original DeskHop project's maintainer suggests donations go to **[Doctors Without Borders](https://donate.doctorswithoutborders.org/secure/donate)** rather than to any individual. This fork's maintainer endorses that.

[![Donate to Doctors Without Borders, with PayPal](https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif)](https://donate.doctorswithoutborders.org/secure/donate)

## Disclaimer

Anyone building this project understands and acknowledges that neither this fork's maintainers nor the upstream maintainers are liable for any injuries, damages, or other consequences. Your safety is your responsibility — solder carefully, don't get electrocuted, and have fun.

Happy switchin'!
