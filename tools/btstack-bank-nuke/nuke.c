/*
 * btstack_bank_nuke.uf2 — wipes the deskhop-bt BTstack flash bank
 * (PICO_FLASH_SIZE_BYTES - 12288 .. PICO_FLASH_SIZE_BYTES) on a Pico 2 W
 * carrier, then reboots back into BOOTSEL.
 *
 * Erases the LAST 12 KB of flash:
 *   BTstack TLV bank  — 8 KB at PICO_FLASH_SIZE_BYTES - 12288
 *   deskhop config    — 4 KB at PICO_FLASH_SIZE_BYTES - 4096
 * Layout matches the deskhop linker scripts; see
 * /Users/major/git-repos/deskhop-bt/misc/memory_map_rp2350.ld and
 * /Users/major/git-repos/deskhop-bt/src/include/bt/btstack_config.h
 * (PICO_FLASH_BANK_STORAGE_OFFSET).
 *
 * The deskhop firmware itself sits at the start of flash and is not
 * touched.  Wired carrier mods (GP18→GND etc.) are hardware-level and
 * not affected by flash erase.
 */

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

/* Erase the last 12 KB of flash: 8 KB BTstack bank + 4 KB config. */
#define NUKE_OFFSET (PICO_FLASH_SIZE_BYTES - 4096 - 8192)
#define NUKE_SIZE   (4096 + 8192)

/* Sanity: must be sector-aligned (4 KB sectors on RP2350 W25Q* flash). */
_Static_assert((NUKE_OFFSET % FLASH_SECTOR_SIZE) == 0, "offset misaligned");
_Static_assert((NUKE_SIZE   % FLASH_SECTOR_SIZE) == 0, "size misaligned");

/* Watchdog scratch markers so external tools (or a serial monitor on the
 * deskhop's subsequent boot) can tell the nuke firmware actually ran. */
#define SCRATCH_NUKE_RAN    0xC0FFEEEEu
#define SCRATCH_NUKE_DONE   0xC0FFEEDDu

int main(void) {
    /* Stamp "ran" before doing anything risky.  If the erase hangs or
     * panics, this marker survives a watchdog reset and tells us the
     * tool at least booted. */
    watchdog_hw->scratch[7] = SCRATCH_NUKE_RAN;

    /* flash_range_erase needs IRQs disabled around it AND access to flash
     * for instruction fetch is also gated — the standard pico-sdk
     * idiom is to run from RAM via __not_in_flash_func.  flash_range_erase
     * is already marked that way internally; we just call it from this
     * main() (compiled flash-resident).  The XIP cache will hold the
     * instructions, and save_and_disable_interrupts is the only IRQ
     * gating needed for a single-core RP2350 boot context. */
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(NUKE_OFFSET, NUKE_SIZE);
    restore_interrupts(ints);

    watchdog_hw->scratch[7] = SCRATCH_NUKE_DONE;

    /* Reboot directly to BOOTSEL so the user can drop the real deskhop
     * firmware on next.  (LED mask = 0, disable_interface_mask = 0.) */
    reset_usb_boot(0, 0);

    /* Unreachable — reset_usb_boot doesn't return. */
    while (1) tight_loop_contents();
}
