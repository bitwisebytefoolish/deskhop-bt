/*
 * Gamepad report routing (#24).  Mirrors the keyboard/mouse pipeline:
 * a report produced on the BT-host board is either queued to the local
 * USB gamepad interface (if this board is the active output) or sent
 * over UART to the output peer as a GAMEPAD_REPORT_MSG.
 */

#pragma once

#include "structs.h"

/* Queue a gamepad report for local USB delivery (output peer side). */
void queue_gamepad_report(gamepad_report_t *report, device_t *state);

/* Core0 task: drain the gamepad queue to tud_hid. */
void process_gamepad_queue_task(device_t *state);

/* Route a gamepad report: local queue if this board is the active
 * output, else over UART to the peer. */
void send_gamepad(gamepad_report_t *report, device_t *state);
