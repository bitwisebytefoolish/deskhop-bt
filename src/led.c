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
#include "tasks.h" /* for cyw43_call_mutex */
/* The poll cyw43_arch variant is NOT multi-core safe. Without locking,
   core0's cyw43_poll_task can race with core1's restore_leds path here
   and deadlock the driver — confirmed by boot crumbs (slot[4] = core0 in
   cyw43_poll_task, slot[7] = core1 in led_blinking_task). The recursive
   mutex (declared extern in tasks.h, defined in tasks.c) serializes all
   cyw43_arch_* access across both cores. The trailing cyw43_arch_poll()
   ensures the GPIO SPI op completes before we drop the lock — otherwise
   a subsequent poll on the other core could pick up a half-completed
   transaction. */
static inline void deskhop_led_put(bool v) {
    recursive_mutex_enter_blocking(&cyw43_call_mutex);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, v);
    cyw43_arch_poll();
    recursive_mutex_exit(&cyw43_call_mutex);
}
static inline bool deskhop_led_get(void)   {
    recursive_mutex_enter_blocking(&cyw43_call_mutex);
    bool v = cyw43_arch_gpio_get(CYW43_WL_GPIO_LED_PIN);
    recursive_mutex_exit(&cyw43_call_mutex);
    return v;
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
    /* Light up on-board LED if current board is active output */
    state->onboard_led_state = (state->active_output == BOARD_ROLE);
    deskhop_led_put(state->onboard_led_state);

    /* Light up appropriate keyboard leds (if it's connected locally) */
    if (state->keyboard_connected) {
        uint8_t leds = state->keyboard_leds[state->active_output];
        set_keyboard_leds(leds, state);
    }
}

uint8_t toggle_led(void) {
    uint8_t new_led_state = deskhop_led_get() ^ 1;
    deskhop_led_put(new_led_state);

    return new_led_state;
}

void blink_led(device_t *state) {
    /* Since LEDs might be ON previously, we go OFF, ON, OFF, ON, OFF */
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

    /* Toggle the LED state */
    uint8_t new_led_state = toggle_led();

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
