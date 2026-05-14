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

#ifdef CYW43_WL_GPIO_LED_PIN
#include "pico/mutex.h"
void cyw43_poll_task(device_t *);
/* Serializes calls into the cyw43_arch poll-variant driver. The poll
   variant is documented as NOT multi-core safe — calling cyw43_arch_poll
   from core0 while core1 is inside cyw43_arch_gpio_put (via
   led_blinking_task → restore_leds) deadlocks the driver. All cyw43_arch_*
   call sites in deskhop must enter this mutex first. Definition (with
   auto-init via .mutex_array section) is in src/tasks.c. */
extern recursive_mutex_t cyw43_call_mutex;
#endif
