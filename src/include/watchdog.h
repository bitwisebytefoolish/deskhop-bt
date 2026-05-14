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

#include <pico.h>           /* pulls in the board header so CYW43_WL_GPIO_LED_PIN is visible to the #ifdef below */
#include <hardware/watchdog.h>

/* RP2040 build: 500 ms is plenty — the original deskhop budget.
   Pi Pico 2 W / RP2350 + CYW43439 build: bump to 2000 ms because the
   cyw43-driver's own ioctl path can stall the caller for ~500 ms
   (CYW43_IOCTL_TIMEOUT_US internally). With a 500 ms watchdog those
   timers race and the watchdog wins, which we observed empirically as
   "Pico 2 W reboots in a loop, no enumeration" during the autoprobe
   debug session. 2000 ms gives the cyw43 stack room to time out and
   recover naturally. */
#ifdef CYW43_WL_GPIO_LED_PIN
#define WATCHDOG_TIMEOUT        2000
#else
#define WATCHDOG_TIMEOUT        500
#endif
#define WATCHDOG_PAUSE_ON_DEBUG 1                       // When using a debugger, disable watchdog
#define CORE1_HANG_TIMEOUT_US   WATCHDOG_TIMEOUT * 1000 // Convert to microseconds

#define MAGIC_WORD_1 0xdeadf00f // When these are set, we'll boot to configuration mode
#define MAGIC_WORD_2 0x00c0ffee
