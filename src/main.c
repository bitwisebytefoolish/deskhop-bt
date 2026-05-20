/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */
#include "main.h"
#include "boot_crumb.h"
#include "ui.h"              /* ui_render_task — no-op stub on non-OLED builds */

#include <stdio.h>           /* printf via pico_stdio_uart — see #26 */
#include "pico/stdlib.h"     /* stdio_init_all() */

/* Override the SDK's weak isr_hardfault (bkpt → LOCKUP) so we can stamp
 * a crumb before the watchdog fires.  Must NOT be naked — we need the
 * compiler to emit a proper stack frame so boot_crumb_set_phase works. */
void isr_hardfault(void) {
    boot_crumb_data[BOOT_CRUMB_SLOT_PHASE]  = 0xFFu;
    boot_crumb_data[BOOT_CRUMB_SLOT_DETAIL] = 0xDEADF001u;
    while (1) tight_loop_contents();
}

/*********  Global Variables  **********/
device_t global_state     = {0};
device_t *device          = &global_state;

firmware_metadata_t _firmware_metadata __attribute__((section(".section_metadata"))) = {
    .version = 0x0001,
};

/* ================================================== *
 * ==============  Main Program Loops  ============== *
 * ================================================== */

int main(void) {
    /* If the previous run armed crumbs and just rebooted from a watchdog
       timeout, jump into BOOTSEL so picotool can read the crumbs. */
    boot_crumb_data[5] = 0xAAAAAAAAu;
    boot_crumb_check_and_maybe_reenter_bootsel();
    boot_crumb_data[5] = 0xBBBBBBBBu;
    boot_crumb_set_phase(PHASE_ENTER_MAIN);

    /* Bring up stdio_uart (UART1 / GP4 / 115200) as early as possible so
     * any subsequent printf or BTstack log_error reaches the wire before
     * slower init steps could potentially fail silently.  See issue #26.
     * Returns true if at least one stdio driver initialised — we don't
     * gate on this; if the UART isn't physically connected, prints fan
     * out to nowhere harmlessly. */
    stdio_init_all();
    printf("\n\n=== deskhop-bt boot (FW %u.%u, role=%s) ===\n",
           VERSION_MAJOR, VERSION_MINOR,
#ifdef DH_BT_HID_HOST_KBD
           "A (BT-host)"
#else
           "B (output)"
#endif
    );

    static task_t tasks_core0[] = {
        [0] = {.exec = &usb_device_task,          .frequency = _TOP()},      // .-> USB device task, needs to run as often as possible
        [1] = {.exec = &kick_watchdog_task,       .frequency = _HZ(30)},     // | Verify core1 is still running and if so, reset watchdog timer
        [2] = {.exec = &process_kbd_queue_task,   .frequency = _HZ(2000)},   // | Check if there were any keypresses and send them
        [3] = {.exec = &process_mouse_queue_task, .frequency = _HZ(2000)},   // | Check if there were any mouse movements and send them
        [4] = {.exec = &process_hid_queue_task,   .frequency = _HZ(1000)},   // | Check if there are any packets to send over vendor link
        [5] = {.exec = &process_uart_tx_task,     .frequency = _TOP()},      // | Check if there are any packets to send over UART
        [6] = {.exec = &led_apply_task,           .frequency = _HZ(100)},    // | Apply pending onboard-LED changes (drains state->onboard_led_dirty; cyw43_arch_gpio_put on Pico 2 W, gpio_put on Pico)
#ifdef DH_BT_HID_HOST_KBD
        [7] = {.exec = &bt_hid_stage_tick_task,   .frequency = _HZ(100)},    // | Drive the sticky BT-stage LED indicator (continuous "N flashes, pause" pattern). 100 Hz matches led_apply_task so phase boundaries land within ~10 ms of their nominal time.
#endif
#ifdef CYW43_WL_GPIO_LED_PIN
        [8] = {.exec = &cyw43_poll_task,          .frequency = _TOP()},      // | Pump cyw43 / BTstack event loop (poll variant) — Pico W / 2 W only
#endif
#ifdef DH_OLED_UI
        [9] = {.exec = &ui_render_task,           .frequency = _HZ(30)},     // | OLED UI render — drains bt_events queue, redraws on dirty signature (#22)
#endif
        /* Note: runtime BOOTSEL-button polling intentionally NOT added here.
           is_bootsel_pressed() floats QSPI CS for 20 µs with IRQs disabled;
           the calling code must live in RAM (__no_inline_not_in_flash_func)
           or the CPU will stall on the next I-cache miss while CS is dark.
           Earlier attempt with bootsel_poll_task hung core0 in <100 ms,
           tripping the 3 s watchdog every boot.  For reflash, hold BOOTSEL
           while replugging USB — hardware path, no firmware needed.       */
    };                                                                       // `----- then go back and repeat forever
    const int NUM_TASKS = ARRAY_SIZE(tasks_core0);

    // Wait for the board to settle
    sleep_ms(10);

    // Initial board setup
    initial_setup(device);

    boot_crumb_set_phase(PHASE_BEFORE_SET_ACTIVE);
    // Initial state, A is the default output
    set_active_output(device, OUTPUT_A);
    boot_crumb_set_phase(PHASE_AFTER_SET_ACTIVE);

    boot_crumb_set_phase(PHASE_MAIN_LOOP_FIRST_ITER);
    uint16_t core0_hb = 0;
    uint32_t core0_tick = 0;
    while (true) {
        for (int i = 0; i < NUM_TASKS; i++) {
            boot_crumb_data[BOOT_CRUMB_SLOT_CORE0_TASK] = BOOT_CRUMB_CORE0_TASK_TAG | (uint32_t)i;
            task_scheduler(device, &tasks_core0[i]);
        }
        boot_crumb_data[BOOT_CRUMB_SLOT_CORE0_TASK] = BOOT_CRUMB_CORE0_TASK_TAG | 0xFFFFu;
        if ((++core0_tick & 0xFFFF) == 0) {
            boot_crumb_core0_hb(++core0_hb);
            boot_crumb_set_phase(PHASE_MAIN_LOOP_RUNNING);
        }
    }
}

void core1_main() {
    /* Required so flash_safe_execute() on core0 can coordinate flash writes
     * (e.g. BTstack TLV bank erase/program) without hanging forever. */
    multicore_lockout_victim_init();

    static task_t tasks_core1[] = {
        /* Slot 0: USB host task — disabled on board A when DH_BT_HID_HOST_KBD
         * replaces the wired-USB keyboard socket with BTstack.  task_scheduler
         * skips entries with exec == NULL so the zeroed slot is harmless. */
#ifndef DH_BT_HID_HOST_KBD
        [0] = {.exec = &usb_host_task,           .frequency = _TOP()},       // .-> USB host task, needs to run as often as possible
#endif
        [1] = {.exec = &packet_receiver_task,    .frequency = _TOP()},       // | Receive data over serial from the other board
        [2] = {.exec = &led_blinking_task,       .frequency = _HZ(30)},      // | Check if LED needs blinking
        [3] = {.exec = &screensaver_task,        .frequency = _HZ(120)},     // | Handle "screensaver" movements
        [4] = {.exec = &firmware_upgrade_task,   .frequency = _HZ(4000)},    // | Send firmware to the other board if needed
        [5] = {.exec = &heartbeat_output_task,   .frequency = _HZ(1)},       // | Output periodic heartbeats
    };                                                                       // `----- then go back and repeat forever
    const int NUM_TASKS = ARRAY_SIZE(tasks_core1);

    while (true) {
        // Update the timestamp, so core0 can figure out if we're dead
        device->core1_last_loop_pass = time_us_64();

        for (int i = 0; i < NUM_TASKS; i++) {
            task_scheduler(device, &tasks_core1[i]);
        }
    }
}
/* =======  End of Main Program Loops  ======= */
