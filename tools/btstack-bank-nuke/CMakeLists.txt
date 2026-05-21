# btstack-bank-nuke — standalone "wipe BTstack flash bank, reboot to BOOTSEL"
# firmware.  Builds INDEPENDENTLY from the main deskhop project so that nothing
# in this CMakeLists can touch the real firmware's build.
#
# Usage:
#   cmake -S tools/btstack-bank-nuke -B tools/btstack-bank-nuke/build \
#         -DPICO_BOARD=pico2_w
#   cmake --build tools/btstack-bank-nuke/build -j
# Output:
#   tools/btstack-bank-nuke/build/btstack_bank_nuke.uf2
#
# Drag-drop the .uf2 onto the RPI-RP2 mass-storage drive that appears when
# the board is in BOOTSEL mode.  The firmware will erase the last 12 KB of
# flash (BTstack TLV bank + deskhop config sector) and reboot back to BOOTSEL
# — ready for the real deskhop.uf2.

cmake_minimum_required(VERSION 3.13)

# Default to pico2_w when the user doesn't override.  This tool only makes
# sense on a board where deskhop-bt itself runs (currently pico2_w; RP2040
# variants too if someone wants to reuse the layout).
if (NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pico2_w" CACHE STRING "Target board" FORCE)
endif()

# Locate the in-tree pico-sdk (sibling of tools/btstack-bank-nuke).
set(PICO_SDK_FETCH_FROM_GIT off)
set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/../../pico-sdk)

# SDK 2.x ships pico_sdk_import.cmake under external/.
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(btstack_bank_nuke C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(btstack_bank_nuke nuke.c)

target_link_libraries(btstack_bank_nuke
    pico_stdlib
    hardware_flash
    hardware_sync
    hardware_watchdog
)

# No stdio: this firmware is fire-and-forget, no USB CDC, no UART.  Saves
# flash and avoids any risk of the USB stack hanging before the erase runs.
pico_enable_stdio_usb(btstack_bank_nuke 0)
pico_enable_stdio_uart(btstack_bank_nuke 0)

# Produce .uf2 alongside .elf so the user can drag-drop.
pico_add_extra_outputs(btstack_bank_nuke)
