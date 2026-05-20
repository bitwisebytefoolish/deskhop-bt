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

/* ================================================== *
 * =============  Initial Board Setup  ============== *
 * ================================================== */

#include "main.h"
#include "boot_crumb.h"
#include "ui.h"              /* ui_init — no-op stub on non-OLED builds */
#include "buttons.h"         /* buttons_init — no-op stub on non-OLED builds */

#include <stdio.h>           /* printf — routed to stdio_uart on UART1 / GP4, see #26 */
#include "pico/stdlib.h"     /* stdio_init_all() — re-call after set_sys_clock_khz */

/* CYW43_WL_GPIO_LED_PIN is defined by the board header on boards that carry
   the CYW43439 module (Pico W, Pico 2 W). Same discriminator used in led.c.
   The C-level PICO_CYW43_SUPPORTED isn't a thing — it's a CMake-only var. */
#ifdef CYW43_WL_GPIO_LED_PIN
#include "pico/cyw43_arch.h"
#endif

#ifdef DH_BT_HID_HOST_KBD
#include "bt_hid_host.h"
#include "bt_hid_host_le.h"   /* BLE peer of bt_hid_host — see #29 */
#endif

/* ================================================== *
 * Perform initial UART setup
 * ================================================== */

void serial_init() {
    /* Set up our UART with a default baudrate. */
    uart_init(SERIAL_UART, SERIAL_BAUDRATE);

    /* Set UART flow control CTS/RTS. We don't have these - turn them off.*/
    uart_set_hw_flow(SERIAL_UART, false, false);

    /* Set our data format */
    uart_set_format(SERIAL_UART, SERIAL_DATA_BITS, SERIAL_STOP_BITS, SERIAL_PARITY);

    /* Turn of CRLF translation */
    uart_set_translate_crlf(SERIAL_UART, false);

    /* We do want FIFO, will help us have fewer interruptions */
    uart_set_fifo_enabled(SERIAL_UART, true);

    /* Set the RX/TX pins, they differ based on the device role (A or B, check schematics) */
    gpio_set_function(SERIAL_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(SERIAL_RX_PIN, GPIO_FUNC_UART);
}

/* ================================================== *
 * PIO USB configuration, D+ pin 14, D- pin 15
 * ================================================== */

void pio_usb_host_config(device_t *state) {
    /* tuh_configure() must be called before tuh_init() */
    static pio_usb_configuration_t config = PIO_USB_DEFAULT_CONFIG;
    config.pin_dp                         = PIO_USB_DP_PIN_DEFAULT;

#ifdef CYW43_WL_GPIO_LED_PIN
    /* On boards with the CYW43439 wireless module (Pico W, Pico 2 W), the
       cyw43-driver claims a state machine from pio0 during cyw43_arch_init()
       — which has already run by the time we get here. Pico-PIO-USB's default
       config hardcodes pio0 sm0/sm1/sm2, which collides with that.
       RP2350 has three PIO blocks (pio0, pio1, pio2); pio2 is always free
       for us, so put Pico-PIO-USB there. */
    config.pio_tx_num = 2;
    config.pio_rx_num = 2;
#endif

    /* Board B is always report mode, board A is default-boot if configured */
    if (state->board_role == OUTPUT_B || ENFORCE_KEYBOARD_BOOT_PROTOCOL == 0)
        tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &config);

    /* Initialize and configure TinyUSB Host */
    tuh_init(1);
}

/* ================================================== *
 * Board Autoprobe Routine
 * ================================================== */

/* Probing algorithm logic:
    - RX pin is driven by the digital isolator IC
    - IF we are board A, it will be connected to pin 13
      and it will drive it either high or low at any given time
    - Before uart setup, enable it as an input
    - Go through a probing sequence of 8 values and pull either up or down
      to that value
    - Read out the value on the RX pin
    - If the entire sequence of values match, we are definitely floating
      so IC is not connected on BOARD_A_RX, and we're BOARD B
*/
/* Detect socket role by reading BOARD_ROLE_DETECT_PIN with an internal
   pull-up enabled. Carrier mod required: bridge that pin to its adjacent
   GND pin on socket A only; socket B leaves the pin floating. The
   internal ~50 kΩ pull-up easily wins against a floating pin (reads
   high → socket B) but cannot fight a hard external GND (reads low →
   socket A). Replaces the original GP13 pull-toggle probe which on
   RP2350 carriers is overpowered by the digital isolator's idle-high
   bias on both sockets. */
int board_autoprobe(void) {
    gpio_init(BOARD_ROLE_DETECT_PIN);
    gpio_set_dir(BOARD_ROLE_DETECT_PIN, GPIO_IN);
    gpio_pull_up(BOARD_ROLE_DETECT_PIN);
    sleep_us(200);  /* settling for the 50 kΩ pull against any board parasitics */
    bool reads_high = gpio_get(BOARD_ROLE_DETECT_PIN);
    gpio_disable_pulls(BOARD_ROLE_DETECT_PIN);

#ifdef FORCE_BOARD_ROLE
    /* Build-time override (0 = A, 1 = B). Useful when the carrier doesn't
       have the GP18→GND bridge yet, or for forcing a known role during
       bring-up. The probe above still runs so the pin is properly
       quiesced regardless. */
    (void)reads_high;
    return FORCE_BOARD_ROLE;
#else
    return reads_high ? OUTPUT_B : OUTPUT_A;
#endif
}


/* ================================================== *
 * Check if we should boot in configuration mode or not
 * ================================================== */

bool is_config_mode_active(device_t *state) {
    /* Watchdog registers survive reboot (RP2040 datasheet section 2.8.1.1) */
    bool is_active = (watchdog_hw->scratch[5] == MAGIC_WORD_1 &&
                      watchdog_hw->scratch[6] == MAGIC_WORD_2);

    /* Remove, so next reboot it's no longer active */
    if (is_active)
        watchdog_hw->scratch[5] = 0;

    reset_config_timer(state);

    return is_active;
}


/* ================================================== *
 * Configure DMA for reliable UART transfers
 * ================================================== */
const uint8_t* uart_buffer_pointers[1] = {uart_rxbuf};
uint8_t uart_rxbuf[DMA_RX_BUFFER_SIZE] __attribute__((aligned(DMA_RX_BUFFER_SIZE))) ;
uint8_t uart_txbuf[DMA_TX_BUFFER_SIZE] __attribute__((aligned(DMA_TX_BUFFER_SIZE))) ;

static void configure_tx_dma(device_t *state) {
    state->dma_tx_channel = dma_claim_unused_channel(true);

    dma_channel_config tx_config = dma_channel_get_default_config(state->dma_tx_channel);
    channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_8);

    /* Writing uart (always write the same address, but source addr changes as we read) */
    channel_config_set_read_increment(&tx_config, true);
    channel_config_set_write_increment(&tx_config, false);

    // channel_config_set_ring(&tx_config, false, 4);
    channel_config_set_dreq(&tx_config, DREQ_UART0_TX);

    /* Configure, but don't start immediately. We'll do this each time the outgoing
       packet is ready and we copy it to the buffer */
    dma_channel_configure(
        state->dma_tx_channel,
        &tx_config,
        &uart0_hw->dr,
        uart_txbuf,
        0,
        false
    );
}

static void configure_rx_dma(device_t *state) {
    /* Find an empty channel, store it for later reference */
    state->dma_rx_channel = dma_claim_unused_channel(true);
    state->dma_control_channel = dma_claim_unused_channel(true);

    dma_channel_config config = dma_channel_get_default_config(state->dma_rx_channel);
    dma_channel_config control_config = dma_channel_get_default_config(state->dma_control_channel);

    channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
    channel_config_set_transfer_data_size(&control_config, DMA_SIZE_32);

    // The read address is the address of the UART data register which is constant
    channel_config_set_read_increment(&config, false);
    channel_config_set_read_increment(&control_config, false);

    // Read into a ringbuffer with 1024 (2^10) elements
    channel_config_set_write_increment(&config, true);
    channel_config_set_write_increment(&control_config, false);

    channel_config_set_ring(&config, true, 10);

    // The UART signals when data is avaliable
    channel_config_set_dreq(&config, DREQ_UART0_RX);

    channel_config_set_chain_to(&config, state->dma_control_channel);

    dma_channel_configure(
        state->dma_rx_channel,
        &config,
        uart_rxbuf,
        &uart0_hw->dr,
        DMA_RX_BUFFER_SIZE,
        false);

    dma_channel_configure(
        state->dma_control_channel,
        &control_config,
        &dma_hw->ch[state->dma_rx_channel].al2_write_addr_trig,
        uart_buffer_pointers,
        1,
        false);

    dma_channel_start(state->dma_control_channel);
}


/* ================================================== *
 * Perform initial board/usb setup
 * ================================================== */
int board;

void initial_setup(device_t *state) {
    boot_crumb_set_phase(PHASE_ENTER_INITIAL_SETUP);

    /* Enable watchdog early with a generous 8 s timeout so any hang during
       init triggers a watchdog reboot, SRAM survives, and the auto-BOOTSEL
       handler at the top of main() catches it with the crumb intact.
       Each step below kicks the watchdog so the 8 s window resets.
       The matching watchdog_enable at the end resets the timer to the
       production timeout. REVERT BOTH ONCE THE CRASH IS ROOT-CAUSED. */
    watchdog_enable(8000, WATCHDOG_PAUSE_ON_DEBUG);

    /* PIO USB requires a clock multiple of 12 MHz, setting to 120 MHz */
    set_sys_clock_khz(120000, true);
    /* Re-init stdio_uart: the UART divisor configured in main()'s call to
     * stdio_init_all() was computed against the bootrom default clock
     * (~150 MHz on RP2350).  After the clock change the divisor is wrong
     * by ~25%, turning printf output into garbage.  stdio_uart_init
     * (called transitively) recomputes the divisor for the new clock.
     * Boot banner from main() already went out cleanly at the original
     * clock; everything from here on uses the new (correct) divisor. */
    stdio_init_all();
    printf("[setup] sys_clock=120MHz, stdio_uart re-init\n");
    watchdog_update();
    boot_crumb_set_phase(PHASE_AFTER_SET_SYS_CLOCK);

    /* Search the persistent storage sector in flash for valid config or use defaults */
    load_config(state);
    watchdog_update();
    boot_crumb_set_phase(PHASE_AFTER_LOAD_CONFIG);

    /* Initialise the on-board LED (platform-specific — direct GPIO on the
       original Pico, CYW43 virtual GPIO on Pi Pico W / 2 W). On the CYW43
       boards this is a no-op; the virtual GPIO is brought up by
       cyw43_arch_init(), called later in this function. */
    deskhop_led_init();
    watchdog_update();
    boot_crumb_set_phase(PHASE_AFTER_LED_INIT);

    /* Check if we should boot in configuration mode or not */
    state->config_mode_active = is_config_mode_active(state);
    watchdog_update();
    boot_crumb_set_phase(PHASE_AFTER_CONFIG_MODE_CHECK);

    /* Detect which board we're running on */
    state->board_role = board_autoprobe();
    watchdog_update();
    boot_crumb_set_phase(PHASE_AFTER_BOARD_AUTOPROBE);

    /* Initialize and configure UART */
    serial_init();
    watchdog_update();
    boot_crumb_set_phase(PHASE_AFTER_SERIAL_INIT);

    /* Initialize keyboard and mouse queues */
    queue_init(&state->kbd_queue, sizeof(hid_keyboard_report_t), KBD_QUEUE_LENGTH);
    queue_init(&state->mouse_queue, sizeof(mouse_report_t), MOUSE_QUEUE_LENGTH);

    /* Initialize generic HID packet queue */
    queue_init(&state->hid_queue_out, sizeof(hid_generic_pkt_t), HID_QUEUE_LENGTH);

    /* Initialize UART queue */
    queue_init(&state->uart_tx_queue, sizeof(uart_packet_t), UART_QUEUE_LENGTH);
    watchdog_update();
    boot_crumb_set_phase(PHASE_AFTER_QUEUES);

    /* Setup RP2040 Core 1 */
    boot_crumb_set_phase(PHASE_BEFORE_CORE1_LAUNCH);
    multicore_reset_core1();
    multicore_launch_core1(core1_main);
    watchdog_update();
    boot_crumb_set_phase(PHASE_AFTER_CORE1_LAUNCH);

    /* Initialize and configure TinyUSB Device. Must run before cyw43_arch_init()
       on Pico 2 W — see comment below for the enumeration-window rationale. */
    boot_crumb_set_phase(PHASE_BEFORE_TUD_INIT);
    tud_init(BOARD_TUD_RHPORT);
    watchdog_update();
    boot_crumb_set_phase(PHASE_AFTER_TUD_INIT);

    /* Initialize and configure TinyUSB Host — disabled on board A when the
     * BTstack BT HID host replaces the wired-USB keyboard socket. */
#ifndef DH_BT_HID_HOST_KBD
    boot_crumb_set_phase(PHASE_BEFORE_TUH_INIT);
    pio_usb_host_config(state);
    watchdog_update();
    boot_crumb_set_phase(PHASE_AFTER_TUH_INIT);
#endif

    /* Initialize and configure DMA */
    configure_tx_dma(state);
    configure_rx_dma(state);
    watchdog_update();
    boot_crumb_set_phase(PHASE_AFTER_DMA);

#ifdef CYW43_WL_GPIO_LED_PIN
    /* Initialise the CYW43439 wireless module — deferred to here on purpose.
       cyw43_arch_init() blocks the CPU for hundreds of ms loading the radio
       firmware blob over SPI. If it runs before tud_init(), the host PC's
       USB enumeration window expires while we're stuck in firmware load and
       the device never appears. Running it after tud_init+tuh_init means
       enumeration starts on time and the radio comes up after.
       The `poll` cyw43_arch variant is used (see CMakeLists); it requires
       periodic cyw43_arch_poll() calls — handled by cyw43_poll_task in
       src/tasks.c. */
    boot_crumb_set_phase(PHASE_BEFORE_CYW43_INIT);
    boot_crumb_set_detail(0xC9430001u); /* about to call cyw43_arch_init */
    int cyw_rc = cyw43_arch_init();
    boot_crumb_set_detail(0xC9430002u); /* returned from cyw43_arch_init */
    watchdog_update();
    if (cyw_rc != 0) {
        /* If radio init fails there's nothing useful we can do — sit on it so
           the watchdog reboots us, rather than running with a half-up device.
           Stash cyw_rc into DETAIL so the dump reveals what went wrong. */
        boot_crumb_set_detail(0xC9430000u | (uint32_t)((uint8_t)cyw_rc));
        while (1) tight_loop_contents();
    }
    boot_crumb_set_phase(PHASE_AFTER_CYW43_INIT);

#ifdef DH_BT_HID_HOST_KBD
    /* BTstack HID host init — must follow cyw43_arch_init() since it binds
       the run-loop to the cyw43 async-context.  Must precede watchdog_enable()
       since btstack_cyw43_init() may take up to a few ms.
       Both statics outlive this function (no stack aliasing risk). */
    boot_crumb_set_phase(PHASE_BEFORE_BT_HID_INIT);
    static bt_hid_state_t  bt_hid_state;
    static hid_interface_t bt_kbd_iface;
    static hid_interface_t bt_mouse_iface;

    bt_hid_state.keyboard_connected   = &state->keyboard_connected;
    bt_hid_state.kbd_iface            = &bt_kbd_iface;
    bt_hid_state.kbd_itf              = 0;
    bt_hid_state.mouse_iface          = &bt_mouse_iface;
    bt_hid_state.mouse_itf            = 1;
    bt_hid_state.parse_descriptor     = parse_report_descriptor;
    bt_hid_state.process_report       = process_keyboard_report;
    bt_hid_state.process_mouse_report = process_mouse_report;
    bt_hid_state.blinks_left          = &state->blinks_left;
    bt_hid_state.last_led_change      = &state->last_led_change;
    bt_hid_state.onboard_led_state    = &state->onboard_led_state;
    bt_hid_state.onboard_led_dirty    = &state->onboard_led_dirty;

    /* Mouse iface: same BOOT-mode shortcut.  extract_report_values in
     * mouse.c casts raw_report straight to hid_mouse_report_t (5 bytes)
     * when protocol == HID_PROTOCOL_BOOT, so no descriptor parsing
     * needed on the BT path. */
    bt_mouse_iface.protocol = 0; /* HID_PROTOCOL_BOOT */

    /* Pre-populate iface->protocol = 0 (HID_PROTOCOL_BOOT) so
     * extract_kbd_data dispatches to _extract_kbd_boot, the fixed-format
     * 8-byte parser that doesn't depend on parse_report_descriptor having
     * correctly populated kb->key_array etc.  bt_hid_host.c also asks
     * BTstack to negotiate HID_PROTOCOL_MODE_BOOT with the keyboard so
     * the keyboard actually sends boot-format reports.  Avoid switching
     * to REPORT mode until process_keyboard_report's descriptor-based
     * path is verified working for the BT path (#8 in the phase plan). */
    bt_kbd_iface.protocol = 0; /* HID_PROTOCOL_BOOT */

    /* Init order matters: bt_hid_host_init() calls l2cap_init() FIRST.
     * sm_init() (inside bt_hid_host_le_init) registers an L2CAP signaling
     * handler and assumes l2cap_init has already run — calling sm_init
     * first leaves the SM in a half-initialised state and silent BLE
     * pairing failures result (8BitDo connects but never receives our
     * pairing request).  Match the order in BTstack's hog_boot_host_demo:
     *   l2cap_init → sm_init → gatt_client_init → register handlers.
     *
     * hci_power_control() (called at the end of bt_hid_host_init) is
     * non-blocking — the chip takes ~1 s to bring the radio up, so
     * BTSTACK_EVENT_STATE → HCI_STATE_WORKING fires well after the
     * subsequent bt_hid_host_le_init() finishes registering its handlers. */
    bt_hid_host_init(&bt_hid_state);
    bt_hid_host_le_init(&bt_hid_state);
    watchdog_update();
    boot_crumb_set_phase(PHASE_AFTER_BT_HID_INIT);

    /* OLED + UI bring-up (issue #22).  Probes the I2C bus for the
     * SSD1306; if nothing ACKs, ui_init returns false and the
     * ui_render_task no-ops at runtime — safe to call unconditionally.
     * Done AFTER bt_hid_host_init so the boot splash can paint over
     * any noise the panel might be holding from a previous session,
     * and so the UI is ready to receive events as soon as BT comes up. */
    ui_init();
    /* Button GPIOs (issue #22 Phase 2).  Init unconditionally — if no
     * buttons are wired, the pull-ups keep the lines high and the
     * GPIO IRQ never fires.  Safe even when the OLED itself is
     * absent: button events accumulate in their ring but the UI
     * task is a no-op so they're harmlessly discarded. */
    buttons_init();
#endif
#endif

    /* Load the current firmware info */
    state->_running_fw = _firmware_metadata;

    /* Update the core1 initial pass timestamp before enabling the watchdog */
    state->core1_last_loop_pass = time_us_64();

    boot_crumb_set_phase(PHASE_BEFORE_WATCHDOG_ENABLE);
    watchdog_enable(WATCHDOG_TIMEOUT, WATCHDOG_PAUSE_ON_DEBUG);
    boot_crumb_set_phase(PHASE_AFTER_WATCHDOG_ENABLE);
}

/* ==========  End of Initial Board Setup  ========== */
