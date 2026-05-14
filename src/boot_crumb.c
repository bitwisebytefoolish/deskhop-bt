/*
 * boot_crumb.c — see boot_crumb.h for design notes.
 *
 * On crash detection, the crumb snapshot is mirrored from SRAM to a 4 KB
 * reserved flash sector BEFORE calling reset_usb_boot(). The RP2350
 * `rom_reboot(REBOOT_TYPE_BOOTSEL)` path empirically wipes all SRAM
 * (including SCRATCH_X) when transitioning into the bootrom, so SRAM
 * alone is not sufficient. Flash survives any reset short of an erase.
 */

#include "boot_crumb.h"

#include <string.h>
#include <hardware/flash.h>
#include <hardware/sync.h>
#include <hardware/watchdog.h>
#include <pico/bootrom.h>

/* Backing storage in SCRATCH_X NOLOAD section (see linker script). Survives
   the run that wrote it, but NOT a reset_usb_boot()-triggered BOOTSEL
   transition — that's why we also mirror to flash on crash detection. */
volatile uint32_t boot_crumb_data[BOOT_CRUMB_NUM_SLOTS]
    __attribute__((section(".scratch_x_noinit"), used));

/* Linker-provided pointer to the start of the 4 KB FLASH_CRUMB region.
   Defined in misc/memory_map.ld and misc/memory_map_rp2350.ld via
   PROVIDE(_boot_crumb_flash = ORIGIN(FLASH_CRUMB)). */
extern const uint8_t _boot_crumb_flash[];

/* Snapshot the current SRAM crumb data into the reserved flash sector.
   Runs with interrupts off because pico-sdk's flash erase/program pauses
   XIP and reads/writes the flash chip directly via the SSI peripheral.
   Marked not-in-flash so it runs from RAM (otherwise the act of reading
   the next instruction would deadlock against the paused XIP). */
static void __no_inline_not_in_flash_func(boot_crumb_save_to_flash)(void) {
    uint32_t flash_offs = (uint32_t)_boot_crumb_flash - 0x10000000u;

    /* Pad to FLASH_PAGE_SIZE (256). We only use 32 bytes of crumb data;
       the rest of the page is zero so a partial read doesn't see stale
       bits from a previous program. */
    uint8_t buf[FLASH_PAGE_SIZE] = {0};
    memcpy(buf, (const void *)boot_crumb_data, sizeof(boot_crumb_data));

    uint32_t saved_ints = save_and_disable_interrupts();
    flash_range_erase(flash_offs, FLASH_SECTOR_SIZE);
    flash_range_program(flash_offs, buf, FLASH_PAGE_SIZE);
    restore_interrupts(saved_ints);
}

void boot_crumb_check_and_maybe_reenter_bootsel(void) {
    /* TRACER (slot[6] is never zeroed by this function). 0xC0DE0001 = we
       reached the check at least; 0xC0DE0002 = we took the BOOTSEL path.
       Useful for distinguishing "firmware never reached main" from
       "post-BOOTSEL SRAM wipe" when reading via picotool. */
    boot_crumb_data[6] = 0xC0DE0001u;

    if (boot_crumb_data[BOOT_CRUMB_SLOT_MAGIC] == BOOT_CRUMB_MAGIC_ARMED
        && watchdog_caused_reboot()) {
        boot_crumb_data[6] = 0xC0DE0002u;
        boot_crumb_data[BOOT_CRUMB_SLOT_MAGIC] = BOOT_CRUMB_MAGIC_CAPTURED;

        /* Mirror SRAM crumbs to flash BEFORE rebooting into BOOTSEL —
           the bootrom transition wipes SRAM, so this is our only chance
           to preserve the snapshot for picotool to read post-BOOTSEL. */
        boot_crumb_save_to_flash();

        reset_usb_boot(0, 0);
        while (1) tight_loop_contents();
    }

    /* Cold boot, clean reset, or previous boot already captured — arm
       fresh crumbs for THIS run. PHASE starts at ENTER_MAIN so that any
       observed crash after this function returns yields a non-zero phase.
       Also zero the runtime telemetry counters so they start from 0
       each session (otherwise SCRATCH_X retains stale values from the
       previous run if it didn't go through a power cycle). */
    boot_crumb_data[BOOT_CRUMB_SLOT_MAGIC]          = BOOT_CRUMB_MAGIC_ARMED;
    boot_crumb_data[BOOT_CRUMB_SLOT_PHASE]          = PHASE_ENTER_MAIN;
    boot_crumb_data[BOOT_CRUMB_SLOT_DETAIL]         = 0;
    boot_crumb_data[BOOT_CRUMB_SLOT_HEARTBEAT]      = 0;
    boot_crumb_data[BOOT_CRUMB_SLOT_TUD_LIFECYCLE]  = 0;
    boot_crumb_data[BOOT_CRUMB_SLOT_UART_TX]        = 0;
    boot_crumb_data[BOOT_CRUMB_SLOT_UART_RX]        = 0;
    boot_crumb_data[BOOT_CRUMB_SLOT_QUEUE_DROPS]    = 0;
    boot_crumb_data[BOOT_CRUMB_SLOT_STATE_SNAPSHOT] = 0;
    boot_crumb_data[BOOT_CRUMB_SLOT_RELAY_BRANCH]   = 0;
    boot_crumb_data[BOOT_CRUMB_SLOT_TX_DMA]         = 0;
    boot_crumb_data[BOOT_CRUMB_SLOT_LINK_DIAG]      = 0;
}

void boot_crumb_dump_to_bootsel(void) {
    /* Force a watchdog reboot — boot_crumb_check_and_maybe_reenter_bootsel
       at the top of main() will see ARMED magic + watchdog_caused_reboot,
       mirror crumbs to flash, and drop into BOOTSEL. We funnel through
       reboot rather than calling flash_range_erase directly here because
       this can be invoked from core1 (hotkey handler runs in core1's
       USB-host task chain). Flash erase stalls XIP, which would deadlock
       core0 if it happens to be fetching from flash at the moment. The
       boot-time path runs on core0 only before core1 is launched, so
       it's the safe place to touch flash. */
    watchdog_reboot(0, 0, 1);
    while (1) tight_loop_contents();
}
