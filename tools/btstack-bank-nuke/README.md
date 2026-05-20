# btstack-bank-nuke

Tiny standalone firmware that wipes the deskhop-bt BTstack flash bank
(8 KB at `PICO_FLASH_SIZE_BYTES - 4096 - 8192`) and the deskhop config
sector (4 KB at end of flash), then reboots back to BOOTSEL.

## Why this exists

Sometimes `picotool erase --range 0x103FD000 0x10400000` doesn't take —
permission issues, picotool version quirks, or the device wasn't in
BOOTSEL when the command ran. When the deskhop boots with leftover
bonds, BLE peripherals get into a confused multi-device state and
nothing is observable to be cleanly "fresh."

This firmware sidesteps picotool entirely:
1. Loaded via the BOOTSEL mass-storage drive (drag-drop the .uf2)
2. Runs `flash_safe_execute(flash_range_erase, ...)` from CPU side
3. Stamps a `0xC0FFEE` marker into watchdog scratch so you can tell
   it actually ran
4. Calls `reset_usb_boot(0, 0)` to return to BOOTSEL

Then you flash the real `deskhop.uf2` onto the now-clean board, and
the BTstack flash bank is guaranteed `0xFF`-filled.

## Build

```bash
cmake -S tools/btstack-bank-nuke -B tools/btstack-bank-nuke/build \
      -DPICO_BOARD=pico2_w
cmake --build tools/btstack-bank-nuke/build -j
# Output: tools/btstack-bank-nuke/build/btstack_bank_nuke.uf2
```

## Use

1. Hold BOOTSEL on board A, plug in USB
2. Drag-drop `btstack_bank_nuke.uf2` onto the `RPI-RP2` drive that appears
3. Wait ~1 second; the board reboots back to BOOTSEL
4. Drag-drop the real `deskhop.uf2` onto the drive

After this, the deskhop's first-boot log should show `LE device DB has
0/32 bonded entries`.

## Safety

Erases ONLY the last 12 KB of flash (BTstack bank + deskhop config).
The deskhop firmware itself sits much earlier in flash and is not touched.
Wired board-role autoprobe (carrier-mod GP18→GND on socket A) is hardware,
not flash — also unaffected.
