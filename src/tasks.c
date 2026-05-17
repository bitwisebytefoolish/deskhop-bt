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
#include "bt_hid_host.h"
#endif

#ifdef CYW43_WL_GPIO_LED_PIN
#include "pico/cyw43_arch.h"
#endif

void task_scheduler(device_t *state, task_t *task) {
    if (!task->exec)
        return;

    uint64_t current_time = time_us_64();

    if (current_time < task->next_run)
        return;

    task->next_run = current_time + task->frequency;
    task->exec(state);
}

/* ================================================== *
 * ==============  Watchdog Functions  ============== *
 * ================================================== */

void kick_watchdog_task(device_t *state) {
    /* Read the timer AFTER duplicating the core1 timestamp,
       so it doesn't get updated in the meantime. */
    uint64_t core1_last_loop_pass = state->core1_last_loop_pass;
    uint64_t current_time         = time_us_64();

    /* If a reboot is requested, we'll stop updating watchdog */
    if (state->reboot_requested)
        return;

    /* If core1 stops updating the timestamp, we'll stop kicking the watchog and reboot */
    if (current_time - core1_last_loop_pass < CORE1_HANG_TIMEOUT_US)
        watchdog_update();
}

/* ================================================== *
 * ===============  USB Device / Host  ============== *
 * ================================================== */

void usb_device_task(device_t *state) {
    tud_task();
}

void usb_host_task(device_t *state) {
    if (tuh_inited())
        tuh_task();
}

/* On CYW43-equipped boards we use the `poll` cyw43_arch variant (see
   CMakeLists), which means we need to pump the cyw43 / BTstack event
   loop ourselves. Without this, the radio sits idle. The threadsafe-
   background variant would run this on a hardware-IRQ context, but
   that allocation hangs on deskhop's existing IRQ/alarm-heavy setup.

   IMPORTANT: the poll variant is single-thread. ALL cyw43_arch_* calls
   are confined to core0 — including this poll and the LED writes via
   led_apply_task. Anything on core1 that wants to change the LED sets
   state->onboard_led_state + state->onboard_led_dirty=true; led_apply_
   task picks up the dirty flag on the next core0 iteration. */

#ifdef CYW43_WL_GPIO_LED_PIN
void cyw43_poll_task(device_t *state) {
    (void)state;
    cyw43_arch_poll();
}
#endif

#ifdef DH_BT_HID_HOST_KBD
/* Thin wrapper so the task scheduler (which always passes device_t *)
 * can drive bt_hid_host_stage_tick().  The tick paints the current BT
 * stage onto the on-board LED as a repeating flash pattern — see the
 * sticky-stage comment in bt_hid_host.c. */
void bt_hid_stage_tick_task(device_t *state) {
    (void)state;
    bt_hid_host_stage_tick();
}
#endif

/* NB: a runtime BOOTSEL-button-poll task was deliberately removed.
   is_bootsel_pressed() (in utils.c) floats QSPI CS for 20 µs with
   interrupts disabled.  Unless the calling function lives in RAM
   (__no_inline_not_in_flash_func), the CPU stalls on the very next
   I-cache miss because CS is dark.  An earlier bootsel_poll_task
   here hung core0 within ~100 ms every boot, tripping the watchdog.

   For developer reflash: hold BOOTSEL while replugging USB — works at
   hardware-bootrom level, no firmware help needed.  For diagnostics:
   the crumb-on-watchdog path in boot_crumb.c reliably captures
   PHASE / DETAIL / CORE0_TASK after any watchdog-triggered reboot. */

mouse_report_t *screensaver_pong(device_t *state) {
    static mouse_report_t report = {0};
    static int dx = 20, dy = 25;

    /* Check if we are bouncing off the walls and reverse direction in that case. */
    if (report.x + dx < MIN_SCREEN_COORD || report.x + dx > MAX_SCREEN_COORD)
        dx = -dx;

    if (report.y + dy < MIN_SCREEN_COORD || report.y + dy > MAX_SCREEN_COORD)
        dy = -dy;

    report.x += dx;
    report.y += dy;

    return &report;
}

mouse_report_t *screensaver_jitter(device_t *state) {
    static mouse_report_t report = {
        .y = JITTER_DISTANCE,
        .mode = RELATIVE,
    };
    report.y = -report.y;

    return &report;
}

/* Have something fun and entertaining when idle. */
void screensaver_task(device_t *state) {
    const uint32_t delays[] = {
        0,        /* DISABLED, unused index 0 */
        5000,     /* PONG, move mouse every 5 ms for a high framerate */
        10000000, /* JITTER, once every 10 sec is more than enough */
    };
    static int last_pointer_move = 0;
    screensaver_t *screensaver = &state->config.output[BOARD_ROLE].screensaver;
    uint64_t inactivity_period = time_us_64() - state->last_activity[BOARD_ROLE];

    /* If we're not enabled, nothing to do here. */
    if (screensaver->mode == DISABLED)
        return;

    /* System is still not idle for long enough to activate or screensaver mode is not supported */
    if (inactivity_period < screensaver->idle_time_us || screensaver->mode > MAX_SS_VAL)
        return;

    /* We exceeded the maximum permitted screensaver runtime */
    if (screensaver->max_time_us
        && inactivity_period > (screensaver->max_time_us + screensaver->idle_time_us))
        return;

    /* If we're the selected output and we can only run on inactive output, nothing to do here. */
    if (screensaver->only_if_inactive && CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        return;

    /* We're active! Now check if it's time to move the cursor yet. */
    if (time_us_32() - last_pointer_move < delays[screensaver->mode])
        return;

    /* Return, if we're not connected or the host is suspended */
    if(!tud_ready()) {
        return;
    }

    mouse_report_t *report;
    switch (screensaver->mode) {
        case PONG:
            report = screensaver_pong(state);
            break;

        case JITTER:
            report = screensaver_jitter(state);
            break;

        default:
            return;
    }

    /* Move mouse pointer */
    queue_mouse_report(report, state);

    /* Update timer of the last pointer move */
    last_pointer_move = time_us_32();
}

/* Periodically emit heartbeat packets */
void heartbeat_output_task(device_t *state) {
    /* If firmware upgrade is in progress, don't touch flash_cs */
    if (state->fw.upgrade_in_progress)
        return;

    if (state->config_mode_active) {
        /* Leave config mode if timeout expired and user didn't click exit */
        if (time_us_64() > state->config_mode_timer)
            reboot();

        /* Keep notifying the user we're still in config mode */
        blink_led(state);
    }

#ifdef DH_DEBUG
    /* Holding the button invokes bootsel firmware upgrade */
    if (is_bootsel_pressed())
        reset_usb_boot(DESKHOP_BOOT_LED_MASK, 0);
#endif

    uart_packet_t packet = {
        .type = HEARTBEAT_MSG,
        .data16 = {
            [0] = state->_running_fw.version,
            [2] = state->active_output,
        },
    };

    queue_try_add(&global_state.uart_tx_queue, &packet);
}


/* Process other outgoing hid report messages. */
void process_hid_queue_task(device_t *state) {
    hid_generic_pkt_t packet;

    if (!queue_try_peek(&state->hid_queue_out, &packet))
        return;

    if (!tud_hid_n_ready(packet.instance))
        return;

    /* ... try sending it to the host, if it's successful */
    bool succeeded = tud_hid_n_report(packet.instance, packet.report_id, packet.data, packet.len);

    /* ... then we can remove it from the queue. Race conditions shouldn't happen [tm] */
    if (succeeded)
        queue_try_remove(&state->hid_queue_out, &packet);
}

/* Task that handles copying firmware from the other device to ours */
void firmware_upgrade_task(device_t *state) {
    if (!state->fw.upgrade_in_progress || !state->fw.byte_done)
        return;

    if (queue_is_full(&state->uart_tx_queue))
        return;

    /* End condition, when reached the process is completed. */
    if (state->fw.address > STAGING_IMAGE_SIZE) {
        state->fw.upgrade_in_progress = 0;
        state->fw.checksum = ~state->fw.checksum;

        /* Checksum mismatch, we wipe the stage 2 bootloader and rely on ROM recovery */
        if(calculate_firmware_crc32() != state->fw.checksum) {
            flash_range_erase((uint32_t)ADDR_FW_RUNNING - XIP_BASE, FLASH_SECTOR_SIZE);
            reset_usb_boot(DESKHOP_BOOT_LED_MASK, 0);
        }

        else {
            state->_running_fw = _firmware_metadata;
            global_state.reboot_requested = true;
        }
    }

    /* If we're on the last element of the current page, page is done - write it. */
    if (TU_U32_BYTE0(state->fw.address) == 0x00) {

        uint32_t page_start_addr = (state->fw.address - 1) & 0xFFFFFF00;
        write_flash_page((uint32_t)ADDR_FW_RUNNING + page_start_addr - XIP_BASE, state->page_buffer);
    }

    request_byte(state, state->fw.address);
}

void packet_receiver_task(device_t *state) {
    uint32_t current_pointer
        = (uint32_t)DMA_RX_BUFFER_SIZE - dma_channel_hw_addr(state->dma_rx_channel)->transfer_count;
    uint32_t delta = get_ptr_delta(current_pointer, state);

    /* If we don't have enough characters for a packet, skip loop and return immediately */
    while (delta >= RAW_PACKET_LENGTH) {
        if (is_start_of_packet(state)) {
            fetch_packet(state);
            process_packet(&state->in_packet, state);
            return;
        }

        /* No packet found, advance to next position and decrement delta */
        state->dma_ptr = NEXT_RING_IDX(state->dma_ptr);
        delta--;
    }
}
