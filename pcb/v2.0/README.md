# pcb/v2.0 — BT-master + LCD + buttons carrier

Working directory for the deskhop-bt carrier rework tracked in [#25](https://github.com/bitwisebytefoolish/deskhop-bt/issues/25). This rev:

- Hosts a **Pi Pico 2 W as the BT-master** (replaces one of the v1.x RP2040 sockets) — owns Bluetooth pairing, device management, and input routing.
- Hosts **1 or 2 Pi Pico (RP2040) sockets as output peers**, each driving one host PC over its own USB-C.
- Adds a **Hosyond SSD1306 0.96″ OLED** + **3 tactile buttons** (UP / SELECT / DOWN) for the on-board UI from [#22](https://github.com/bitwisebytefoolish/deskhop-bt/issues/22) (closed).
- Ships in two variants — same schematic family, different physical footprints.

| Variant | PCB outline | Picos | USB-Cs | Hosts addressable |
|---|---|---|---|---|
| 2-host carrier | 100 × 100 mm | 1× Pico 2 W (master) + 1× Pico (output peer) | 2× USB-C (1 master + 1 host-PC) | 2 |
| 3-host carrier | 100 × 150 mm | 1× Pico 2 W (master) + 2× Pico (output peers) | 3× USB-C (1 master + 2 host-PC) | 3 |

Firmware ([#23](https://github.com/bitwisebytefoolish/deskhop-bt/issues/23)) supports up to 14 peers via the v2 protocol; these two carrier variants are the validated hardware realizations.

## Files

| File | What it is |
|---|---|
| `DECISIONS.md` | Locked design decisions + rationale for each. Read before opening KiCad. |
| `DeskHop_Rev1.kicad_pro` | Existing KiCad project (v1.x master+peer, pre-#25). **Starting point**, not the final design. |
| `DeskHop_Rev1.kicad_sch` | Existing schematic. |
| `DeskHop_Rev1.kicad_pcb` | Existing board layout. |
| `Gerber/`, `Gerber_DeskHop.zip` | Gerber output of the existing v1.x layout. |

The schematic + PCB will be reworked in place — same files, new revisions — when the #25 implementation session opens. Existing v1.x design serves as the symbol library, footprint library, and design-rule reference.

## Sibling directories

- `pcb/v1.0/`, `pcb/v1.1/` — earlier revs of the original hrvach/deskhop carrier (point-to-point UART, no BT, no LCD)
- `pcb/v2.0/` *(this dir)* — first revision with a BT-master Pico 2 W + LCD UI
- *Future:* a production-grade carrier with raw RP2040/RP2350 chips (no dev modules) — tracked in [#53](https://github.com/bitwisebytefoolish/deskhop-bt/issues/53)

## Tooling

- **KiCad 10.0.3** (release build)
- **JLCPCB** for fab (2-layer) and PCBA (SMT parts only — through-hole hand-soldered)
- **OpenSCAD or CadQuery** for the parametric enclosure model (250 × 250 × 250 mm 3D-print bed available)

## Reference: existing claimed pins

Firmware-side pins already in use (must not collide with new I/O on this carrier):

| GPIO | Purpose | Source |
|---|---|---|
| GP12 / GP13 | Board A UART TX / RX | `src/include/pinout.h` |
| GP14 / GP15 | PIO USB D+ / D− (output peers only — input host is BT on master) | `src/include/pinout.h` |
| GP16 / GP17 | Board B UART TX / RX | `src/include/pinout.h` |
| GP18 | Role-detect (2-pin jumper to GND on master) | `src/include/pinout.h::BOARD_ROLE_DETECT_PIN` |
| GP23, GP24, GP25, GP29 | CYW43 SPI (master only) | Pico 2 W board header |

**Free for #25's I/O** (I2C, buttons, per-output LEDs): GP0–GP11, GP19–GP22, GP26–GP28. Specific assignments determined during board layout based on physical component placement.
