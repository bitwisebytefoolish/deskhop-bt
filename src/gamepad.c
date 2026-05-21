/*
 * Gamepad report routing (#24).  See src/include/gamepad.h.
 *
 * Modeled directly on the keyboard pipeline in keyboard.c
 * (process_kbd_queue_task / queue_kbd_report / send_key).
 */

#include "main.h"

void process_gamepad_queue_task(device_t *state) {
    gamepad_report_t report;

    /* Nowhere to send if USB isn't up. */
    if (!state->tud_connected)
        return;

    if (!queue_try_peek(&state->gamepad_queue, &report))
        return;

    if (tud_suspended())
        tud_remote_wakeup();

    /* Gamepad shares the composite HID interface with kbd/mouse. */
    if (!tud_hid_n_ready(ITF_NUM_HID))
        return;

    bool succeeded =
        tud_hid_n_report(ITF_NUM_HID, REPORT_ID_GAMEPAD, &report, sizeof(report));

    if (succeeded)
        queue_try_remove(&state->gamepad_queue, &report);
}

void queue_gamepad_report(gamepad_report_t *report, device_t *state) {
    if (!state->tud_connected)
        return;

    queue_try_add(&state->gamepad_queue, report);
}

void send_gamepad(gamepad_report_t *report, device_t *state) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        queue_gamepad_report(report, state);
        state->last_activity[BOARD_ROLE] = time_us_64();
    } else {
        queue_packet((uint8_t *)report, GAMEPAD_REPORT_MSG, GAMEPAD_REPORT_LENGTH);
    }
}
