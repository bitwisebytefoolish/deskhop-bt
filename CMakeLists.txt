cmake_minimum_required(VERSION 3.13)

## Version Configuration.
## Define e.g. 0.72 as major 0 and minor 72
##
## deskhop-bt 1.1.0 (release tag v1.1.0-bt): adds the SSD1306 OLED + button
## device-management UI on board A (issue #22) on top of 1.0.0.  The UI is
## compiled in by default for pico2_w (DH_OLED_UI) but is fully optional at
## RUNTIME — if no panel ACKs on the I2C bus, the UI task no-ops and the
## firmware behaves exactly like 1.0.0.  So this build is a strict superset:
## flash it whether or not an OLED is wired.
##
## v1.0.0-bt stays the "lite" release for anyone who wants the BT host with
## none of the OLED/button code compiled in.
##
## 1.0.0 (v1.0.0-bt, "lite"): first numbered release of the Bluetooth fork.
## Inherits upstream 0.77; adds Phase 0 board bring-up, BT Classic + BLE
## HOGP HID host on board A (Pico 2 W), multi-device bonding, persistent
## bonds, cold-boot reconnect (PR #38), fork docs (PR #39).
##
## 1.1.0 (v1.1.0-bt, OLED): adds the SSD1306 OLED + button UI (#22 Phase
## 1/2) — always-on status screen, menu navigation, device-type icons.
##
## 1.1.1 (v1.1.1-bt): #22 Phase 3 — pair-new flow with countdown, forget
## device / forget all wired to real bond removal, persistent device-name
## metadata (TLV), opt-in pairing for OLED builds. Plus review fixes:
## forget clears the active list immediately, single-cell marquee ellipsis,
## full-length GAP device names (read-by-handle, not Read-By-Type).
##
## 1.2.0 (v1.2.0-bt): OLED UI polish — button-glyph hints (○ SELECT, ↑/↓
## UP/DOWN), "- free" empty-slot label, +2 px name padding, no menu
## underline, and a ping-pong "scanning" dot in the pair-new flow.
##
## FIRMWARE_VERSION (used in OTA compatibility checks) computes as
## MAJOR*1000 + MINOR*10 + PATCH + 100 = 1120 here — strictly greater than
## 1.1.1's 1111, so the upstream A/B OTA version comparison still orders
## correctly.  (The formula gained the *10 / +PATCH terms when the patch
## component was introduced; pre-1.1.1 releases predate it and keep their
## already-flashed values — only the ordering matters for OTA.)
set(VERSION_MAJOR 1)
set(VERSION_MINOR 2)
set(VERSION_PATCH 0)

## Release Type Selection
option(DH_DEBUG "Build a debug version" OFF)
option(DH_DEBUG_CDC_FLASH "Enable CDC command to trigger bootloader mode" OFF)

## Hardware Configuration
set(DP_PIN_DEFAULT 14 CACHE STRING "Default USB D+ Pin Number")
set(PIO_USE_TINYUSB 1 CACHE STRING "Make TinyUSB Manage the PIO USB Port")
set(PICO_BOARD "pico" CACHE STRING "Target board: 'pico' (RP2040) or 'pico2_w' (RP2350 + CYW43439). See issue #2.")

## Optional build-time override of board_autoprobe. Set to 0 (A) or 1 (B)
## to skip the GP18 probe and hard-code the role. Useful when a carrier
## hasn't been modified with the GP18→GND bridge on socket A (see
## src/include/pinout.h::BOARD_ROLE_DETECT_PIN), or for forcing a known
## role during bring-up. Unset / empty = run the autoprobe normally.
set(FORCE_BOARD_ROLE "" CACHE STRING "Force board_role at build time: 0=A, 1=B, empty=autoprobe")
if (NOT "${FORCE_BOARD_ROLE}" STREQUAL "")
    add_definitions(-DFORCE_BOARD_ROLE=${FORCE_BOARD_ROLE})
endif()

## Pico SDK Configuration
set(PICO_SDK_FETCH_FROM_GIT off)
set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-sdk)
set(SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
# SDK 2.x moved pico_sdk_import.cmake from the SDK root into external/.
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

## Project Setup
project(deskhop_project C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

## PICO_COPY_TO_RAM is an RP2040-era speed optimisation that runs all .text
## from SRAM after copying it out of flash at boot. On RP2350 the XIP cache
## is fast enough that copy-to-RAM hurts more than it helps — particularly
## because the cyw43-driver firmware blob (~240 KB) is .rodata and gets
## dragged into RAM, blowing the budget on Pico W and consuming most of
## the Pi Pico 2 W's 512 KB SRAM. Apply only to known RP2040 boards.
if(PICO_BOARD STREQUAL "pico" OR PICO_BOARD STREQUAL "pico_w")
    set(PICO_COPY_TO_RAM 1)
endif()

## Compiler Flags
## NOTE: -mcpu / -mtune are intentionally not set here — the SDK toolchain file
## (selected by PICO_COMPILER) already sets platform-appropriate flags:
##   rp2040 -> pico_arm_cortex_m0plus_gcc -> -mcpu=cortex-m0plus
##   rp2350 -> pico_arm_cortex_m33_gcc    -> -mcpu=cortex-m33
## We APPEND project-specific flags to preserve those toolchain-set values.
## Plain `set(CMAKE_C_FLAGS "...")` would wipe them out and break the build.
string(APPEND CMAKE_C_FLAGS " -Ofast -Wall -fstack-usage")

## Initialize Pico SDK
pico_sdk_init()

## PIO USB Library Setup
set(PICO_PIO_USB_DIR ${CMAKE_CURRENT_LIST_DIR}/Pico-PIO-USB)

add_library(Pico-PIO-USB STATIC
  ${PICO_PIO_USB_DIR}/src/pio_usb.c
  ${PICO_PIO_USB_DIR}/src/pio_usb_host.c
  ${PICO_PIO_USB_DIR}/src/usb_crc.c
)
pico_generate_pio_header(Pico-PIO-USB ${PICO_PIO_USB_DIR}/src/usb_tx.pio)
pico_generate_pio_header(Pico-PIO-USB ${PICO_PIO_USB_DIR}/src/usb_rx.pio)

target_link_libraries(Pico-PIO-USB PRIVATE
  pico_stdlib
  pico_multicore
  hardware_pio
  hardware_dma
)
target_include_directories(Pico-PIO-USB PRIVATE ${PICO_PIO_USB_DIR})

## Source Files
set(COMMON_SOURCES
  ${SRC_DIR}/usb_descriptors.c
  ${SRC_DIR}/defaults.c
  ${SRC_DIR}/constants.c
  ${SRC_DIR}/protocol.c
  ${SRC_DIR}/hid_parser.c
  ${SRC_DIR}/hid_report.c
  ${SRC_DIR}/utils.c
  ${SRC_DIR}/handlers.c
  ${SRC_DIR}/setup.c
  ${SRC_DIR}/keyboard.c
  ${SRC_DIR}/mouse.c
  ${SRC_DIR}/tasks.c
  ${SRC_DIR}/led.c
  ${SRC_DIR}/uart.c
  ${SRC_DIR}/usb.c
  ${SRC_DIR}/main.c
  ${SRC_DIR}/ramdisk.c
  ${SRC_DIR}/boot_crumb.c
  ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/dcd_pio_usb.c
  ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

## Include Directories
set(COMMON_INCLUDES
  ${SRC_DIR}/include
  ${PICO_PIO_USB_DIR}/src
)

## Library Dependencies
set(COMMON_LINK_LIBRARIES
  pico_stdlib
  hardware_flash
  hardware_uart
  hardware_gpio
  hardware_pio
  hardware_dma
  hardware_i2c

  tinyusb_device
  tinyusb_host
  pico_multicore
  pico_unique_id
  Pico-PIO-USB
)

## On boards with the CYW43439 module (Pico W, Pico 2 W) the on-board LED is
## a virtual GPIO inside the wireless module rather than a real MCU pin,
## reachable via cyw43_arch_gpio_*. The Bluetooth radio also sits on the
## same module and is driven through BTstack via cyw43_arch.
##
## We link the `poll` variant rather than `threadsafe_background`. The
## threadsafe-background async context allocates a hardware alarm and a
## user IRQ at init time; in firmware that already uses hardware alarms
## and IRQs heavily (deskhop has DMA-driven UART, watchdog, custom task
## scheduler) that allocation hangs indefinitely inside cyw43_arch_init.
## Switching to poll sidesteps the issue. The trade-off is that we have
## to call cyw43_arch_poll() in our task loop ourselves — see
## src/tasks.c. (See https://github.com/phieri/viking-bio-matter/pull/75
## for the same finding from another project.)
##
## BTstack HID host (issue #6): enabled by default on CYW43 builds.
## Override with -DDH_BT_HID_HOST_KBD=0 to disable (e.g. for a minimal
## cyw43-LED-only build). Issue #9 (mouse) will add DH_BT_HID_HOST_MOUSE
## as an orthogonal flag following the same pattern.
if(PICO_CYW43_SUPPORTED)
    list(APPEND COMMON_LINK_LIBRARIES pico_cyw43_arch_poll)

    if(NOT DEFINED DH_BT_HID_HOST_KBD)
        set(DH_BT_HID_HOST_KBD 1)
    endif()

    if(DH_BT_HID_HOST_KBD)
        ## pico_btstack_cyw43: one-stop Pico W glue — wires run-loop to the
        ## cyw43_arch async-context, inits the HCI transport, and sets up TLV
        ## flash storage. Pulls in pico_btstack_base, pico_btstack_flash_bank,
        ## pico_btstack_run_loop_async_context, and pico_btstack_hci_transport_cyw43.
        ## pico_btstack_classic adds hid_host.c, l2cap, sdp, and ssp on top.
        ## pico_btstack_ble adds gatt_client.c, sm.c, le_device_db_tlv.c, and
        ## the HIDS GATT-service client — required for BLE HID host (#29).
        ## Both transports run simultaneously on the shared CYW43 radio.
        list(APPEND COMMON_LINK_LIBRARIES
            pico_btstack_cyw43
            pico_btstack_classic
            pico_btstack_ble
        )
        ## btstack_config.h is in src/include/bt/ so BTstack finds it.
        list(APPEND COMMON_INCLUDES ${CMAKE_SOURCE_DIR}/src/include/bt)
    endif()
endif()
set(binary deskhop)

## Disk Image Configuration
# Assembles disk.S into .section_disk, then the post-build step patches
# the firmware CRC into .section_metadata. Both sections are provided by
# misc/memory_map.ld (rp2040) and misc/memory_map_rp2350.ld (rp2350).

set(DISK_ASM "${CMAKE_CURRENT_LIST_DIR}/disk/disk.S")
set(DISK_BIN "${CMAKE_CURRENT_LIST_DIR}/disk/disk.img")
set_property(SOURCE ${DISK_ASM} APPEND PROPERTY COMPILE_OPTIONS "-x" "assembler-with-cpp")

add_executable(${binary} ${DISK_ASM})

target_sources(${binary} PUBLIC ${COMMON_SOURCES})
if(DH_BT_HID_HOST_KBD)
    target_sources(${binary} PRIVATE
        ${SRC_DIR}/bt_hid_host.c
        ${SRC_DIR}/bt_hid_host_le.c
    )
endif()

## OLED + button UI (issue #22).  Phase 1: status screen on board A,
## SSD1306 over I2C0 (GP0/GP1).  Buttons reserved in pinout.h for
## Phase 2.  Default ON for pico2_w (only board with BT host); OFF
## for pico (RP2040 — no BT, no UI).  Safe with the panel absent —
## oled_init returns false, ui_render_task no-ops.
if(NOT DEFINED DH_OLED_UI)
    if(PICO_BOARD STREQUAL "pico2_w")
        set(DH_OLED_UI 1)
    else()
        set(DH_OLED_UI 0)
    endif()
endif()
if(DH_OLED_UI)
    target_sources(${binary} PRIVATE
        ${SRC_DIR}/oled.c
        ${SRC_DIR}/ui.c
        ${SRC_DIR}/buttons.c
    )
endif()
## bt_events.c compiles unconditionally — bt_hid_host*.c reference it
## inside #ifdef DH_OLED_UI guards, but keeping the symbol always-defined
## (publish becomes a no-op when no consumer exists) avoids link-time
## conditionals on every call site.
target_sources(${binary} PRIVATE ${SRC_DIR}/bt_events.c)
target_compile_definitions(${binary}
    PRIVATE
    VERSION_MAJOR=${VERSION_MAJOR}
    VERSION_MINOR=${VERSION_MINOR}
    VERSION_PATCH=${VERSION_PATCH}
    PIO_USB_USE_TINYUSB=${PIO_USE_TINYUSB}
    PIO_USB_DP_PIN_DEFAULT=${DP_PIN_DEFAULT}
    # We use cyw43_arch only for the LED and Bluetooth — not Wi-Fi.
    # Setting CYW43_LWIP=0 keeps cyw43.h from pulling in lwip/netif.h,
    # which we don't ship lwipopts.h for. Revisit if we ever need Wi-Fi.
    $<$<BOOL:${PICO_CYW43_SUPPORTED}>:CYW43_LWIP=0>
    # BT HID host keyboard flag — propagated to source so #ifdef guards work.
    $<$<BOOL:${DH_BT_HID_HOST_KBD}>:DH_BT_HID_HOST_KBD>
    # OLED UI flag — propagated to source so #ifdef guards work.
    $<$<BOOL:${DH_OLED_UI}>:DH_OLED_UI>
    __disk_file_path__="${DISK_BIN}"
    # Double core0 stack (SCRATCH_Y) from 2KB to 4KB.  cyw43_arch_init +
    # BTstack init have a deeper frame chain than the default 2KB accommodates.
    # SCRATCH_Y is 4KB and contains nothing else, so this costs nothing.
    # PICO_CORE1_STACK_SIZE stays at 2KB (SCRATCH_X has 64 B of boot_crumb_data).
    PICO_STACK_SIZE=0x1000
    PICO_CORE1_STACK_SIZE=0x800
    # Debug UART on UART1 / GP4 (TX) / GP5 (RX) @ 115200 8N1.
    # UART0 is reserved for the cross-board protocol at 3.6864 Mbps —
    # would look like binary garbage on a debug terminal.  pico_enable_
    # stdio_uart below routes printf / log_error to this UART.  A USB-
    # to-TTL adapter (e.g. DSD Tech SH-V09C in 3.3 V mode) on GP4 →
    # adapter RX captures output.  See issue #26.
    PICO_DEFAULT_UART=1
    PICO_DEFAULT_UART_TX_PIN=4
    PICO_DEFAULT_UART_RX_PIN=5
    PICO_DEFAULT_UART_BAUD_RATE=115200
)

## Support building a debug version
if (DH_DEBUG)
  add_definitions(-DDH_DEBUG)  
endif()

if (DH_DEBUG_CDC_FLASH)
  add_definitions(-DDH_DEBUG_CDC_FLASH)
endif()
  
target_include_directories(${binary} PUBLIC ${COMMON_INCLUDES})
target_link_libraries(${binary} PUBLIC ${COMMON_LINK_LIBRARIES})

## Configure Pico Library
## stdio_usb stays disabled — would conflict with deskhop's USB device
## descriptors (already a composite HID device with multiple interfaces).
## stdio_uart routes printf + BTstack log_error to UART1 / GP4 / GP5 —
## see PICO_DEFAULT_UART* defines above.  Issue #26.
pico_enable_stdio_usb(${binary} 0)
pico_enable_stdio_uart(${binary} 1)

## Custom flash layout linker script — per-platform variant so both the
## RP2040 and RP2350 builds get the deskhop sections (.section_disk,
## .section_metadata, .section_staging, .section_config).
if(PICO_PLATFORM STREQUAL "rp2040")
    pico_set_linker_script(${binary} ${CMAKE_SOURCE_DIR}/misc/memory_map.ld)
elseif(PICO_PLATFORM MATCHES "^rp2350")
    pico_set_linker_script(${binary} ${CMAKE_SOURCE_DIR}/misc/memory_map_rp2350.ld)
else()
    message(WARNING "No custom linker script for PICO_PLATFORM=${PICO_PLATFORM}; using SDK default. Web-config disk and OTA features will be unavailable.")
endif()

## Build other file formats as well
pico_add_extra_outputs(${binary})

# For internal firmware use, to simplify comparison: 0.72 -> 172.
# MAJOR*1000 + MINOR*10 + PATCH + 100 keeps releases monotonically ordered
# now that a PATCH component exists (e.g. 1.1.1 -> 1111 > 1.1.0's 1101).
if(NOT DEFINED VERSION_PATCH)
    set(VERSION_PATCH 0)
endif()
math(EXPR FIRMWARE_VERSION "${VERSION_MAJOR} * 1000 + ${VERSION_MINOR} * 10 + ${VERSION_PATCH} + 100")

## Post-Build Commands
## CRC32 / .section_metadata update — now runs on both rp2040 and rp2350
## (both linker scripts provide .section_metadata). SDK 2.x replaces the
## standalone `elf2uf2` binary with picotool's `uf2 convert` subcommand;
## $<TARGET_FILE:picotool> resolves at build time to the picotool imported
## by pico_add_extra_outputs above.
if(PICO_PLATFORM STREQUAL "rp2040" OR PICO_PLATFORM MATCHES "^rp2350")
    add_custom_command(
        TARGET ${binary} POST_BUILD
        COMMAND python3 ${CMAKE_SOURCE_DIR}/misc/crc32.py ${binary}.bin ${binary}.crc ${FIRMWARE_VERSION}
        COMMAND ${CMAKE_OBJCOPY} --update-section .section_metadata=${binary}.crc ${binary}.elf
        COMMAND ${CMAKE_OBJCOPY} -O binary ${binary}.elf ${binary}.bin
        COMMAND $<TARGET_FILE:picotool> uf2 convert ${binary}.elf -t elf ${binary}.uf2
        COMMENT "Update CRC32 section to match the actual binary"
    )
endif()

## Linker Options
target_link_options(${binary} PRIVATE
  -Xlinker
  --print-memory-usage
)

