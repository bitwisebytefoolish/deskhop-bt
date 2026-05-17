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
#pragma once

#include "structs.h"

/*==============================================================================
 *  Core Task Scheduling
 *==============================================================================*/

 void task_scheduler(device_t *, task_t *);

/*==============================================================================
 *  Individual Task Functions
 *==============================================================================*/

void firmware_upgrade_task(device_t *);
void heartbeat_output_task(device_t *);
void kick_watchdog_task(device_t *);
void led_blinking_task(device_t *);
void packet_receiver_task(device_t *);
void process_hid_queue_task(device_t *);
void process_kbd_queue_task(device_t *);
void process_mouse_queue_task(device_t *);
void process_uart_tx_task(device_t *);
void screensaver_task(device_t *);
void usb_device_task(device_t *);
void usb_host_task(device_t *);

#ifdef DH_BT_HID_HOST_KBD
void bt_hid_stage_tick_task(device_t *);
#endif

#ifdef CYW43_WL_GPIO_LED_PIN
void cyw43_poll_task(device_t *);
/* The cyw43_arch poll variant is single-thread. All cyw43_arch_* calls
   are confined to core0: cyw43_poll_task here, and led_apply_task
   (declared in misc.h) which is the only path that issues
   cyw43_arch_gpio_put for the onboard LED. core1 paths that want to
   change the LED set state->onboard_led_state + onboard_led_dirty=true. */
#endif
