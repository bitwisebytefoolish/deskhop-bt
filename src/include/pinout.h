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

/*==============================================================================
 *  Board Roles
 *==============================================================================*/

 #define BOARD_ROLE (global_state.board_role)
#define OTHER_ROLE (BOARD_ROLE == OUTPUT_A ? OUTPUT_B : OUTPUT_A)

/*==============================================================================
 *  GPIO Pins (LED, USB)
 *==============================================================================*/

#define GPIO_LED_PIN   25 // LED is connected to pin 25 on a PICO
#define PIO_USB_DP_PIN 14 // D+ is pin 14, D- is pin 15

/* Bitmask passed to reset_usb_boot() to flash the on-board LED while in
   BOOTSEL mode. On the original Pico this is (1 << GPIO 25). On Pico W /
   Pico 2 W the LED is a virtual GPIO inside the CYW43 module — the
   bootloader cannot reach it from a regular GPIO bitmask, so we pass 0
   and the device enters BOOTSEL without a blink indicator. */
#ifdef PICO_DEFAULT_LED_PIN
#define DESKHOP_BOOT_LED_MASK (1u << PICO_DEFAULT_LED_PIN)
#else
#define DESKHOP_BOOT_LED_MASK 0u
#endif

/*==============================================================================
 *  Serial Pins
 *==============================================================================*/

/* GP12 / GP13, Pins 16 (TX), 17 (RX) on the Pico board */
#define BOARD_A_RX 13
#define BOARD_A_TX 12

/* GP16 / GP17, Pins 21 (TX), 22 (RX) on the Pico board */
#define BOARD_B_RX 17
#define BOARD_B_TX 16

#define SERIAL_RX_PIN (global_state.board_role == OUTPUT_A ? BOARD_A_RX : BOARD_B_RX)
#define SERIAL_TX_PIN (global_state.board_role == OUTPUT_A ? BOARD_A_TX : BOARD_B_TX)
