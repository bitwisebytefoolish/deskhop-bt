# pcb/v2.0 — Design decisions log

Locked decisions for the BT-master + LCD + buttons carrier ([#25](https://github.com/bitwisebytefoolish/deskhop-bt/issues/25)). Capture date: 2026-05-30. One row per decision: **what was chosen**, **what alternatives were considered**, **why this one**.

## Mechanical

### Board outline
- **Chosen**: 100 × 100 mm (2-host variant), 100 × 150 mm (3-host variant)
- **Alternatives**: "as small as components allow" (~80 × 80 mm packed); match a specific off-the-shelf enclosure
- **Why**: 100 × 100 mm stays in JLCPCB's $2 fab tier. Comfortable spacing for the OLED + 3 buttons + 1 USB-C + 1 Pico. The 3-host variant just adds a row for the extra Pico + USB-C. We can always shrink later; we can't easily un-cramp.

### OLED placement
- **Chosen**: top face, landscape orientation
- **Alternatives**: front face vertical (looks like a small monitor); short-edge mount
- **Why**: standard desk-gadget layout. User looks down at the unit. Simplest enclosure window cutout, no daughter board or 90° riser needed.

### Buttons
- **Chosen**: 3× Omron B3F-1000 through-hole tactile, stacked vertically to the right of the OLED — UP / SELECT / DOWN
- **Alternatives**: SMT tactile (3 × 6 mm); slide DIP switch for navigation
- **Why**: user has a stash of B3F buttons on hand (no sourcing delay). Through-hole survives hand-soldering after JLC PCBA does the SMT. Vertical stack with SELECT in the middle reads as a natural up/select/down DPad column.

### USB-C placement
- **Chosen**: all USB-C receptacles on one edge (the rear)
- **Alternatives**: master front + host-PCs rear (input → output flow); USB-Cs on three faces (most ergonomic but most enclosure complexity)
- **Why**: simplest cable management — all cables exit one side. Matches typical desk placement where the unit sits with one edge toward the cable run.

### Mounting
- **Chosen**: 4× M3 corner holes, 5 mm edge inset
- **Alternatives**: M2.5; rubber feet only (no holes); different pattern
- **Why**: M3 is standard, works with both standoffs and case posts. 5 mm inset gives copper-to-edge clearance without wasting board area. Doesn't preclude rubber feet for desktop placement.

### Layer count
- **Chosen**: 2-layer PCB
- **Alternatives**: 4-layer
- **Why**: only low-speed signals + USB 2.0 + I2C on this carrier. 4-layer's improved signal integrity isn't needed and ~2.5× the fab cost. The production board ([#53](https://github.com/bitwisebytefoolish/deskhop-bt/issues/53)) goes 4-layer for RF integrity around the raw CYW43439, but this dev-board carrier doesn't.

## Electrical

### Power topology
- **Chosen**: each Pico powered from its own USB-C VBUS — master from its USB-C, each output peer from its host-PC USB-C
- **Alternatives**: all Picos powered from master USB-C only; jumper-selectable per-Pico source
- **Why**: mirrors deskhop's existing 2-board topology (each Pico's USB powers itself). Avoids 500 mA × N draw from a single USB-C. Clean electrical isolation — host PC always sees its own Pico as a normal USB device. No cross-Pico ground-loop concerns.

### USB-C VBUS protection
- **Chosen**: USBLC6-2P6 TVS diode array + 0.5 A polyfuse (1 A trip) on each USB-C input
- **Alternatives**: USBLC6 only (no polyfuse); no external protection
- **Why**: USBLC6 protects D+/D−/VBUS against ESD. Polyfuse protects against PC USB-port abuse and accidental shorts during bring-up. ~$1 per port — cheap insurance for a hand-assembled prototype.

### UART idle pullups
- **Chosen**: 10 kΩ pullup to 3.3 V on each UART line (3 resistors total, one per UART pair)
- **Alternatives**: none (rely on Pico TX driving high when idle)
- **Why**: during the brief Pico reset/boot window, TX is high-impedance, and a transient low on the bus could be misread as a start bit by a peer that's already running. 10 kΩ pulls the bus to a defined idle level. Three resistor BOM lines is negligible.

### GP18 role-detect (master)
- **Chosen**: 2-pin 0.1″ pin header with a shunt jumper. Shunt installed = OUTPUT_A; shunt removed = OUTPUT_B (autoprobe sees pin floating)
- **Alternatives**: 3-pin header with shunt position selecting GND vs floating; DIP switch; permanent trace to GND
- **Why**: same form factor as classic motherboard jumpers. Cheap, unambiguous. Replaces the Phase 0 solder bridge with something that's reversible and field-serviceable. Output-peer Picos don't need this — role determined by enumeration ([#23](https://github.com/bitwisebytefoolish/deskhop-bt/issues/23)).

### Per-output activity LED
- **Chosen**: small SMT LED + current-limit resistor adjacent to each host-PC USB-C, driven from a free GPIO on the master, lit when that output is the active one
- **Alternatives**: rely solely on OLED status display
- **Why**: glanceable cue — user can see at a hardware level which physical port is "live." Doesn't replace the OLED, complements it. 1 GPIO + 1 LED + 1 resistor per output (1 or 2 LEDs total depending on variant); GPIO budget has plenty of headroom.

## Display

### Display module
- **Chosen**: Hosyond SSD1306 0.96″ OLED — Amazon listing [B09T6SJBV5](https://www.amazon.com/dp/B09T6SJBV5) (white variant), I2C address 0x3C
- **Module dimensions**: 27 × 27 × 4 mm (confirmed from product listing)
- **Pin order on the 4-pin header (left to right)**: **GND, VCC, SCL, SDA** — note GND-first, not VCC-first. The carrier's I2C header footprint must match this.
- **Resolution**: 128 × 64, monochrome white
- **Why**: user already specified Hosyond in [#22](https://github.com/bitwisebytefoolish/deskhop-bt/issues/22). 4-pin I2C is the simplest possible display integration. 5-pack on Amazon is cheap and ships fast.

### I2C pullups
- **Chosen**: 4.7 kΩ pullups to 3.3 V on the carrier **unless** physical inspection of the module confirms onboard pullups (then omit)
- **Why**: Hosyond's listing doesn't explicitly call out pullups; the 27 × 27 mm form-factor SSD1306 modules from this category usually include 4.7 kΩ onboard, but it's not guaranteed. Designing the carrier with optional unpopulated pullup footprints (DNI by default, populated if module arrives without them) is the safest path. Verify with a multimeter on the first module out of the bag during bring-up — measure SDA-to-VCC and SCL-to-VCC; should read ~4.7 kΩ if onboard pullups are present, open circuit if absent.
- **Bring-up note**: parallel pullups (carrier + module) halve effective resistance to ~2.4 kΩ, which violates the SSD1306 rise-time spec at higher I2C speeds. Populate carrier pullups *or* trust module pullups, never both.

### Mounting holes
- **Chosen**: TBD — verify from physical inspection of the first delivered module
- **Why**: the listing doesn't specify mounting hole positions or diameter. Hosyond's 27 × 27 mm form factor typically has 4 corner M2 mounting holes (~2.2 mm diameter) at ~22 × 22 mm center-to-center spacing, but this varies between batches. **Next session should design the OLED footprint with both (a) standoff mount holes and (b) backup adhesive-mount pad in case the module's holes don't match.**

## Manufacturing

### USB-C connector
- **Chosen**: GCT USB4105-GF-A (USB 2.0 receptacle, mid-mount, surface mount + through-hole hybrid)
- **LCSC / JLC PCBA part number**: **C3020560**
- **Availability (verified 2026-05-30)**: 1,164 units in stock at JLC, $1.0306 per unit at 1+, **Extended** library tier
- **Extended-part fee**: +$0.03/piece special component fee + ~$3 one-time per-Extended-part setup charge. For a 2-host carrier (2 USB-Cs × 5 boards = 10) the math is ~$0.30 + $3 = $3.30 extra; for 3-host (3 × 5 = 15) it's ~$0.45 + $3 = $3.45 extra. Total assembly delta vs a Basic part is well under $10.
- **Why**: USB4105-GF-A is widely stocked at LCSC, available through JLC PCBA, and used in many open-source Pico-class projects. Mid-mount hybrid layout simplifies hand-rework relative to fully-SMT USB-C parts. Connector body is mechanically robust enough for repeated mating.
- **Alternative if availability tightens**: Korean Hroparts Electronics TYPE-C-31-M-12 (LCSC C165948) — Basic library part, ~$0.30, slightly thinner profile, also 12-pin USB 2.0. Footprint differs; substitution requires layout rework.

### Fab + assembly
- **Chosen**: JLCPCB fab + JLC PCBA for SMT parts; through-hole hand-soldered
- **Alternatives**: hand-solder everything; PCBWay / NextPCB / OSHPark
- **Why**: JLCPCB is the price leader for low-qty prototype PCBA. SMT parts here (USB-C, USBLC6, decoupling caps, current-limit resistors, LEDs) are tedious by hand but trivial for JLC's PCBA service. ~$30–50 total for 5 boards including assembly is well below the value of the time saved.

### Quantity
- **Chosen**: 5 boards of each variant (10 total — JLC minimum order × 2 variants)
- **Alternatives**: 5 of just one variant; 10+ for community / kit distribution
- **Why**: covers prototyping, two finished builds for the user, and a few spares for community give-aways. Within JLC's $50 budget for the full run. Larger batches would require panelization and test-jig planning.

## Tooling

### Schematic + layout
- **Chosen**: KiCad 10.0.3 (release build)
- **Why**: most recent KiCad release as of decision date; matches the user's installed version. Existing v1.x/v2.0 KiCad projects in this directory open cleanly.

### Enclosure
- **Chosen**: OpenSCAD or CadQuery (final choice during session — recommend OpenSCAD if no preference)
- **Why**: parametric model is reproducible and version-controllable. 250 × 250 × 250 mm bed handles either variant comfortably with room for ventilation, button caps, and OLED window detail.
