/*
 * boot_crumb.h — printf-debugging via a NOLOAD SRAM section.
 *
 * No probe? No UART? Use SRAM crumbs instead. The deskhop linker script
 * (misc/memory_map_rp2350.ld) has a `.uninitialized_data (NOLOAD)` section
 * that the C runtime does NOT zero on startup. Values written there survive
 * a watchdog-triggered reset (SRAM contents are retained across a watchdog
 * reset on RP2350; only the CPU/peripherals get reset).
 *
 * The mechanism: every major init step writes a phase tag to boot_crumb_data.
 * If the firmware hangs, the watchdog reboots the chip; the very first thing
 * the firmware does on the next boot is detect "I just crashed" (via the
 * magic value still in our SRAM mirror + watchdog_caused_reboot()) and jump
 * straight into BOOTSEL. The developer then reads the section with:
 *
 *     scripts/read-crumb.sh
 *
 * which extracts the runtime address from the deskhop.elf, then
 * `picotool save -r <addr> <addr+32> file.bin`. picotool's PICOBOOT read can
 * reach SRAM but NOT peripheral registers — that's why we mirror the data
 * into SRAM instead of using watchdog scratch registers directly.
 *
 * Slot layout (boot_crumb_data[i] — each is a uint32_t):
 *   [0] MAGIC      — 0xC0FFEE00 = "armed", 0xDEADBEEF = "crash captured"
 *   [1] PHASE      — last reached boot phase (see enum boot_phase)
 *   [2] DETAIL     — phase-specific detail (e.g. cyw43_arch_init return code)
 *   [3] HEARTBEAT  — packed: upper 16b = core0 hb, lower 16b = core1 hb
 *   [4..7] reserved for future crumbs
 */

#pragma once

#include <stdint.h>

#define BOOT_CRUMB_MAGIC_ARMED    0xC0FFEE00u
#define BOOT_CRUMB_MAGIC_CAPTURED 0xDEADBEEFu

#define BOOT_CRUMB_SLOT_MAGIC     0
#define BOOT_CRUMB_SLOT_PHASE     1
#define BOOT_CRUMB_SLOT_DETAIL    2
#define BOOT_CRUMB_SLOT_HEARTBEAT 3
#define BOOT_CRUMB_SLOT_CORE0_TASK 4   /* 0xCC00xxxx — core0 currently in task xxxx */
#define BOOT_CRUMB_SLOT_CORE1_TASK 7   /* 0xCC01xxxx — core1 currently in task xxxx */

#define BOOT_CRUMB_CORE0_TASK_TAG 0xCC000000u
#define BOOT_CRUMB_CORE1_TASK_TAG 0xCC010000u

/* Backing store lives in the .uninitialized_data NOLOAD section so the C
   runtime doesn't zero it at startup. Definition is in src/boot_crumb.c. */
extern volatile uint32_t boot_crumb_data[8];

enum boot_phase {
    PHASE_ENTER_MAIN              = 0x01,
    PHASE_ENTER_INITIAL_SETUP     = 0x02,
    PHASE_AFTER_SET_SYS_CLOCK     = 0x03,
    PHASE_AFTER_LOAD_CONFIG       = 0x04,
    PHASE_AFTER_LED_INIT          = 0x05,
    PHASE_AFTER_CONFIG_MODE_CHECK = 0x06,
    PHASE_AFTER_BOARD_AUTOPROBE   = 0x07,
    PHASE_AFTER_SERIAL_INIT       = 0x08,
    PHASE_AFTER_QUEUES            = 0x09,
    PHASE_BEFORE_CORE1_LAUNCH     = 0x0A,
    PHASE_AFTER_CORE1_LAUNCH      = 0x0B,
    PHASE_BEFORE_TUD_INIT         = 0x0C,
    PHASE_AFTER_TUD_INIT          = 0x0D,
    PHASE_BEFORE_TUH_INIT         = 0x0E,
    PHASE_AFTER_TUH_INIT          = 0x0F,
    PHASE_AFTER_DMA               = 0x10,
    PHASE_BEFORE_CYW43_INIT       = 0x11,
    PHASE_AFTER_CYW43_INIT        = 0x12,
    PHASE_BEFORE_WATCHDOG_ENABLE  = 0x13,
    PHASE_AFTER_WATCHDOG_ENABLE   = 0x14,
    PHASE_BEFORE_SET_ACTIVE       = 0x15,
    PHASE_AFTER_SET_ACTIVE        = 0x16,
    PHASE_MAIN_LOOP_FIRST_ITER    = 0x17,
    PHASE_MAIN_LOOP_RUNNING       = 0x18,
};

static inline void boot_crumb_set_phase(uint32_t phase) {
    boot_crumb_data[BOOT_CRUMB_SLOT_PHASE] = phase;
}

static inline void boot_crumb_set_detail(uint32_t detail) {
    boot_crumb_data[BOOT_CRUMB_SLOT_DETAIL] = detail;
}

static inline void boot_crumb_core0_hb(uint16_t hb) {
    uint32_t v = boot_crumb_data[BOOT_CRUMB_SLOT_HEARTBEAT];
    boot_crumb_data[BOOT_CRUMB_SLOT_HEARTBEAT] = (v & 0x0000FFFFu) | ((uint32_t)hb << 16);
}

static inline void boot_crumb_core1_hb(uint16_t hb) {
    uint32_t v = boot_crumb_data[BOOT_CRUMB_SLOT_HEARTBEAT];
    boot_crumb_data[BOOT_CRUMB_SLOT_HEARTBEAT] = (v & 0xFFFF0000u) | (uint32_t)hb;
}

/* Call this at the TOP of main(), before anything else (set_sys_clock_khz,
   load_config, etc.). If the previous run armed crumbs and the chip just
   rebooted from a watchdog timeout, jump straight into BOOTSEL — the user
   can dump the crumbs with scripts/read-crumb.sh. */
void boot_crumb_check_and_maybe_reenter_bootsel(void);
