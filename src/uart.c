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

/* ================================================== *
 * ===============  Sending Packets  ================ *
 * ================================================== */

/* Takes a packet as uart_packet_t struct, adds preamble, checksum and encodes it to a raw array. */
void write_raw_packet(uint8_t *dst, uart_packet_t *packet) {
    uint8_t pkt[RAW_PACKET_LENGTH] = {[0] = START1,
                                      [1] = START2,
                                      [2] = packet->type,
                                      /* [3-10] is data, defaults to 0 */
                                      [11] = calc_checksum(packet->data, PACKET_DATA_LENGTH)};

    memcpy(&pkt[START_LENGTH + TYPE_LENGTH], packet->data, PACKET_DATA_LENGTH);
    memcpy(dst, &pkt, RAW_PACKET_LENGTH);
}

/* Schedule packet for sending to the other box */
void queue_packet(const uint8_t *data, enum packet_type_e packet_type, int length) {
    uart_packet_t packet = {.type = packet_type};
    memcpy(packet.data, data, length);

    queue_try_add(&global_state.uart_tx_queue, &packet);

    /* DIAGNOSTIC: per-type TX counters (slot 9). Kbd in low 16, mouse in
       high 16. Other packet types not counted — those go through send_value
       and we're focused on the relay path. */
    if (packet_type == KEYBOARD_REPORT_MSG)
        boot_crumb_inc_low16(BOOT_CRUMB_SLOT_UART_TX);
    else if (packet_type == MOUSE_REPORT_MSG)
        boot_crumb_inc_high16(BOOT_CRUMB_SLOT_UART_TX);
}

/* Sends just one byte of a certain packet type to the other box. */
void send_value(const uint8_t value, enum packet_type_e packet_type) {
    queue_packet(&value, packet_type, sizeof(uint8_t));
}

/* Process outgoing config report messages. */
void process_uart_tx_task(device_t *state) {
    uart_packet_t packet = {0};

    if (dma_channel_is_busy(state->dma_tx_channel)) {
        /* DIAGNOSTIC: slot 14 high = DMA still busy from previous send.
           If this saturates while TX counters keep climbing, DMA is stuck. */
        boot_crumb_inc_high16(BOOT_CRUMB_SLOT_TX_DMA);
        return;
    }

    if (!queue_try_remove(&state->uart_tx_queue, &packet))
        return;

    write_raw_packet(uart_txbuf, &packet);
    dma_channel_transfer_from_buffer_now(state->dma_tx_channel, uart_txbuf, RAW_PACKET_LENGTH);

    /* DIAGNOSTIC: slot 14 low = DMA kickoff count. Pairs with high 16
       (DMA-busy skips). If low keeps climbing but RX counter on the
       other board stays flat, the UART line / receiver is the suspect. */
    boot_crumb_inc_low16(BOOT_CRUMB_SLOT_TX_DMA);
}

/* ================================================== *
 * ===============  Parsing Packets  ================ *
 * ================================================== */

const uart_handler_t uart_handler[] = {
    /* Core functions */
    {.type = KEYBOARD_REPORT_MSG, .handler = handle_keyboard_uart_msg},
    {.type = MOUSE_REPORT_MSG, .handler = handle_mouse_abs_uart_msg},
    {.type = OUTPUT_SELECT_MSG, .handler = handle_output_select_msg},

    /* Box control */
    {.type = MOUSE_ZOOM_MSG, .handler = handle_mouse_zoom_msg},
    {.type = KBD_SET_REPORT_MSG, .handler = handle_set_report_msg},
    {.type = SWITCH_LOCK_MSG, .handler = handle_switch_lock_msg},
    {.type = SYNC_BORDERS_MSG, .handler = handle_sync_borders_msg},
    {.type = FLASH_LED_MSG, .handler = handle_flash_led_msg},
    {.type = GAMING_MODE_MSG, .handler = handle_toggle_gaming_msg},
    {.type = CONSUMER_CONTROL_MSG, .handler = handle_consumer_control_msg},
    {.type = SCREENSAVER_MSG, .handler = handle_screensaver_msg},

    /* Config */
    {.type = WIPE_CONFIG_MSG, .handler = handle_wipe_config_msg},
    {.type = SAVE_CONFIG_MSG, .handler = handle_save_config_msg},
    {.type = REBOOT_MSG, .handler = handle_reboot_msg},
    {.type = GET_VAL_MSG, .handler = handle_api_msgs},
    {.type = GET_ALL_VALS_MSG, .handler = handle_api_read_all_msg},
    {.type = SET_VAL_MSG, .handler = handle_api_msgs},

    /* Firmware */
    {.type = REQUEST_BYTE_MSG, .handler = handle_request_byte_msg},
    {.type = RESPONSE_BYTE_MSG, .handler = handle_response_byte_msg},
    {.type = FIRMWARE_UPGRADE_MSG, .handler = handle_fw_upgrade_msg},

    {.type = HEARTBEAT_MSG, .handler = handle_heartbeat_msg},
    {.type = PROXY_PACKET_MSG, .handler = handle_proxy_msg},
    {.type = DUMP_CRUMB_MSG, .handler = handle_dump_crumb_msg},
};

void process_packet(uart_packet_t *packet, device_t *state) {
    if (!verify_checksum(packet)) {
        /* DIAGNOSTIC: slot 15 high = checksum failures. If this climbs but
           legit RX counters stay flat, the UART line has signal integrity
           or alignment issues (not "no traffic"). */
        boot_crumb_inc_high16(BOOT_CRUMB_SLOT_LINK_DIAG);
        return;
    }

    for (int i = 0; i < ARRAY_SIZE(uart_handler); i++) {
        if (uart_handler[i].type == packet->type) {
            uart_handler[i].handler(packet, state);
            return;
        }
    }
}
