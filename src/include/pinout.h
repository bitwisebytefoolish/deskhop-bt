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

/* Board-role discriminator pin. The autoprobe enables an internal
   pull-up and reads. Carrier mod: bridge GP18 (pin 24) to the adjacent
   GND (pin 23) on socket A only. Socket A reads low (external GND
   wins); socket B reads high (floats, pull-up wins). Replaces the old
   GP13 pull-toggle autoprobe which was fooled on RP2350 because the
   digital isolator's idle-high output overwhelms the ~50 kΩ internal
   pulls on both sockets (verified 2026-05-14 via the autoprobe DETAIL
   crumb: GP13 and GP17 both read 0xff regardless of pull on both
   boards). GP18 is otherwise unused in deskhop. */
#define BOARD_ROLE_DETECT_PIN 18

#define SERIAL_RX_PIN (global_state.board_role == OUTPUT_A ? BOARD_A_RX : BOARD_B_RX)
#define SERIAL_TX_PIN (global_state.board_role == OUTPUT_A ? BOARD_A_TX : BOARD_B_TX)

/*==============================================================================
 *  OLED + button harness (board A only; BT host UI — see issue #22)
 *
 *  These pins are jumper-wired from board A to a breadboard that carries
 *  a Hosyond 0.96" SSD1306 OLED (128x64, I2C) and three momentary buttons.
 *  Selected from the unused-on-board-A GPIO set so no carrier mod is
 *  required beyond the existing GP18->GND role autoprobe.
 *
 *  Pin selection rationale:
 *   - GP0/GP1 are I2C0's first SDA/SCL pair, at the corner of the Pico
 *     for short jumper runs.
 *   - GP19-21 are three consecutive unused GPIOs on the opposite edge;
 *     a single IRQ callback dispatches all three.
 *   - GP22 is reserved for a future fourth (BACK) button if the 3-button
 *     long-press-back UX proves clunky.
 *
 *  Activated only when DH_OLED_UI is defined at build time (see
 *  CMakeLists.txt).  If the OLED never ACKs at boot, the UI task no-ops
 *  and the existing bt_hid_stage LED indicator remains the source of
 *  truth.  Wiring this on a Pico (RP2040) build is a no-op since the
 *  BT host only runs on the Pico 2 W.
 *==============================================================================*/

#ifdef DH_OLED_UI
/* I2C bus and pins for the SSD1306 panel. */
#define OLED_I2C_INSTANCE  i2c0
#define OLED_I2C_BAUD      400000  /* 400 kHz fast-mode; ~25 ms per full flush */
#define OLED_PIN_SDA       0
#define OLED_PIN_SCL       1
#define OLED_I2C_ADDR      0x3C    /* 7-bit; some panels are 0x3D */

/* Button GPIOs (active-low: external press shorts to GND, internal pull-up). */
#define UI_BUTTON_UP       19
#define UI_BUTTON_DOWN     20
#define UI_BUTTON_SELECT   21
/* GP22 reserved for an optional 4th BACK button (Phase 4). */
#endif
