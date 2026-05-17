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

#ifdef DH_BT_HID_HOST_KBD
/* While the BT debug indicator (bt_hid_host.c stage_tick) is driving the
 * on-board LED, gate every other LED writer in this file so they don't
 * race.  Set by bt_hid_host's set_stage() the first time it leaves IDLE.
 * No header-include needed — this is just a one-line cross-module flag. */
extern volatile bool bt_hid_host_owns_led;
#endif

/* ==================================================== *
 * ========== Platform-specific LED I/O       ========== *
 * ==================================================== *
 *
 * On the original Pi Pico, GPIO 25 is wired directly to the on-board LED
 * and can be driven with the standard hardware/gpio routines.
 *
 * On the Pi Pico W and Pi Pico 2 W, GPIO 25 is repurposed as WL_CS — the
 * SPI chip-select line to the CYW43439 wireless module. The on-board LED
 * is instead exposed as a virtual GPIO inside the CYW43 module, accessed
 * via cyw43_arch_gpio_put / cyw43_arch_gpio_get. Driving GPIO 25 directly
 * on these boards would assert chip-select on the radio and corrupt any
 * Bluetooth or Wi-Fi traffic in flight.
 *
 * The board headers define CYW43_WL_GPIO_LED_PIN only on boards with the
 * CYW43 module, so we use that as the compile-time discriminator.
 *
 * Note: the CYW43 path requires cyw43_arch_init() to have been called
 * before any of these helpers fire — wired up in issue #4.
 */

#ifdef CYW43_WL_GPIO_LED_PIN
#include "pico/cyw43_arch.h"
/* IMPORTANT: deskhop_led_put / deskhop_led_get must ONLY be called from
   core0. The cyw43 poll-arch variant is NOT multi-core safe — earlier
   attempts to serialize with a recursive_mutex still hung the driver
   when a single cyw43 call internally waited for state owned by the
   other core (boot crumbs showed core0 stuck in cyw43_poll_task while
   core1 was stuck in led_blinking_task / usb_host_task). The fix is
   architectural: only core0 ever issues cyw43_arch_* calls. core1
   paths that want to change the onboard LED set state->onboard_led_state
   and state->onboard_led_dirty=true; led_apply_task (in src/tasks.c,
   wired into core0 task list in main.c) picks up the dirty flag and
   issues the cyw43 call. */
static inline void deskhop_led_put(bool v) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, v);
}
static inline bool deskhop_led_get(void)   {
    return cyw43_arch_gpio_get(CYW43_WL_GPIO_LED_PIN);
}
#else
static inline void deskhop_led_put(bool v) { gpio_put(GPIO_LED_PIN, v); }
static inline bool deskhop_led_get(void)   { return gpio_get(GPIO_LED_PIN); }
#endif

void deskhop_led_init(void) {
#ifndef CYW43_WL_GPIO_LED_PIN
    gpio_init(GPIO_LED_PIN);
    gpio_set_dir(GPIO_LED_PIN, GPIO_OUT);
#endif
    /* On CYW43 boards the LED virtual GPIO is set up by cyw43_arch_init() — #4. */
}

/* ==================================================== *
 * ========== Update pico and keyboard LEDs  ========== *
 * ==================================================== */

void set_keyboard_leds(uint8_t requested_led_state, device_t *state) {
    static uint8_t new_led_value;

    new_led_value = requested_led_state;
    if (state->keyboard_connected) {
        tuh_hid_set_report(state->kbd_dev_addr,
                           state->kbd_instance,
                           0,
                           HID_REPORT_TYPE_OUTPUT,
                           &new_led_value,
                           sizeof(uint8_t));
    }
}

void restore_leds(device_t *state) {
#ifdef DH_BT_HID_HOST_KBD
    /* Defer to bt_hid_host's stage indicator while it's driving the LED. */
    if (bt_hid_host_owns_led)
        return;
#endif
    /* Light up on-board LED if current board is active output. Only flag
       the change as dirty; the actual cyw43 write happens on core0 in
       led_apply_task. The RP2040 (non-CYW43) path picks up the dirty
       flag the same way for code-path symmetry. */
    bool target = (state->active_output == BOARD_ROLE);
    if (state->onboard_led_state != target) {
        state->onboard_led_state = target;
        state->onboard_led_dirty = true;
    }

    /* Light up appropriate keyboard leds (if it's connected locally).
       tuh_hid_set_report is a USB host op; safe to invoke from any core
       — it queues the control transfer for the USB host stack to drain. */
    if (state->keyboard_connected) {
        uint8_t leds = state->keyboard_leds[state->active_output];
        set_keyboard_leds(leds, state);
    }
}

/* Logical LED toggle. Updates the requested state + dirty flag; the actual
   cyw43 write is owned by core0 (led_apply_task). Called from
   led_blinking_task on core1 — no longer touches cyw43 directly. */
uint8_t toggle_led(device_t *state) {
    uint8_t new_led_state = state->onboard_led_state ? 0 : 1;
    state->onboard_led_state = (bool)new_led_state;
    state->onboard_led_dirty = true;
    return new_led_state;
}

void blink_led(device_t *state) {
#ifdef DH_BT_HID_HOST_KBD
    /* Defer to bt_hid_host's stage indicator while it's driving the LED.
     * Without this, e.g. handle_flash_led_msg from peer-board UART pings
     * sets blinks_left=5 → wakes led_blinking_task → starts toggling the
     * LED on top of our pattern → user sees corrupted flash counts. */
    if (bt_hid_host_owns_led)
        return;
#endif
    /* Since LEDs might be ON previously, we go OFF, ON, OFF, ON, OFF.
       Safe on CYW43 boards now that toggle_led no longer touches cyw43
       directly — led_apply_task on core0 issues the actual writes. */
    state->blinks_left     = 5;
    state->last_led_change = time_us_32();
}

void led_blinking_task(device_t *state) {
    const int blink_interval_us = 80000; /* 80 ms off, 80 ms on */
    static uint8_t leds;

    /* If there is no more blinking to be done, exit immediately */
    if (state->blinks_left == 0)
        return;

    /* We have some blinks left to do, check if they are due, exit if not */
    if ((time_us_32()) - state->last_led_change < blink_interval_us)
        return;

    /* Toggle the logical LED state. cyw43 write is deferred to core0
       (led_apply_task) — this task can run on core1 safely. */
    uint8_t new_led_state = toggle_led(state);

    /* Also keyboard leds (if it's connected locally) since on-board leds are not visible */
    leds = new_led_state * 0x07; /* Numlock, capslock, scrollock */

    if (state->keyboard_connected)
        set_keyboard_leds(leds, state);

    /* Decrement the counter and update the last-changed timestamp */
    state->blinks_left--;
    state->last_led_change = time_us_32();

    /* Restore LEDs in the last pass */
    if (state->blinks_left == 0)
        restore_leds(state);
}

/* Core0 task: apply pending LED state changes via cyw43. All cyw43_arch_*
   calls are confined to core0 — see comment above deskhop_led_put. Runs
   in the core0 task list (main.c). Fires at _HZ(100) — fast enough that
   LED transitions track visually but slow enough to not starve the
   USB device task. */
void led_apply_task(device_t *state) {
    if (!state->onboard_led_dirty)
        return;
    deskhop_led_put(state->onboard_led_state ? 1 : 0);
    state->onboard_led_dirty = false;
}
