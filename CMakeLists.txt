cmake_minimum_required(VERSION 3.13)

## Version Configuration.
## Define e.g. 0.72 as major 0 and minor 72
set(VERSION_MAJOR 0)
set(VERSION_MINOR 77)

## Release Type Selection
option(DH_DEBUG "Build a debug version" OFF)
option(DH_DEBUG_CDC_FLASH "Enable CDC command to trigger bootloader mode" OFF)

## Hardware Configuration
set(DP_PIN_DEFAULT 14 CACHE STRING "Default USB D+ Pin Number")
set(PIO_USE_TINYUSB 1 CACHE STRING "Make TinyUSB Manage the PIO USB Port")
set(PICO_BOARD "pico" CACHE STRING "Target board: 'pico' (RP2040) or 'pico2_w' (RP2350 + CYW43439). See issue #2.")

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
set(PICO_COPY_TO_RAM 1)

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

  tinyusb_device
  tinyusb_host
  pico_multicore
  pico_unique_id
  Pico-PIO-USB
)

## On boards with the CYW43439 module (Pico W, Pico 2 W) the on-board LED is
## a virtual GPIO inside the wireless module rather than a real MCU pin.
## Reaching it requires the cyw43_arch library. Link the "none" variant here
## (LED-only, no Wi-Fi or BT) — issue #4 will swap to a BT-capable variant.
if(PICO_CYW43_SUPPORTED)
    list(APPEND COMMON_LINK_LIBRARIES pico_cyw43_arch_none)
endif()
set(binary deskhop)

## Disk Image Configuration
# This assembles disk.S, then updates the elf section in post-build
# With the disk FAT image binary in /disk/disk.img
#
# The disk-image, firmware-metadata, and firmware-staging sections rely on the
# custom flash layout in misc/memory_map.ld, which currently only targets RP2040.
# For RP2350 / Pico 2 W builds these features are disabled until the linker
# script is ported. Tracked in issue #14.

set(DISK_ASM "${CMAKE_CURRENT_LIST_DIR}/disk/disk.S")
set(DISK_BIN "${CMAKE_CURRENT_LIST_DIR}/disk/disk.img")
set_property(SOURCE ${DISK_ASM} APPEND PROPERTY COMPILE_OPTIONS "-x" "assembler-with-cpp")

if(PICO_PLATFORM STREQUAL "rp2040")
    add_executable(${binary} ${DISK_ASM})
else()
    add_executable(${binary})
    message(STATUS "Custom flash layout disabled for ${PICO_PLATFORM} — web-config disk and OTA firmware exchange unavailable on this target (issue #14).")
endif()

target_sources(${binary} PUBLIC ${COMMON_SOURCES})
target_compile_definitions(${binary} 
    PRIVATE
    VERSION_MAJOR=${VERSION_MAJOR}
    VERSION_MINOR=${VERSION_MINOR}
    PIO_USB_USE_TINYUSB=${PIO_USE_TINYUSB}
    PIO_USB_DP_PIN_DEFAULT=${DP_PIN_DEFAULT}
    __disk_file_path__="${DISK_BIN}"
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
pico_enable_stdio_usb(${binary} 0)
pico_enable_stdio_uart(${binary} 0)

if(PICO_PLATFORM STREQUAL "rp2040")
    pico_set_linker_script(${binary} ${CMAKE_SOURCE_DIR}/misc/memory_map.ld)
endif()

## Build other file formats as well
pico_add_extra_outputs(${binary})

# For internal firmware use, to simplify comparison: 0.72 -> 172
math(EXPR FIRMWARE_VERSION "${VERSION_MAJOR} * 1000 + ${VERSION_MINOR} + 100")

## Post-Build Commands
## CRC32 / .section_metadata update relies on the custom flash layout in
## misc/memory_map.ld (RP2040 only). On RP2350 / Pico 2 W we fall back to
## pico_add_extra_outputs() above for .bin / .uf2 generation, without the
## firmware-metadata CRC patch. Tracked in issue #14.
if(PICO_PLATFORM STREQUAL "rp2040")
    # SDK 2.x replaces the standalone `elf2uf2` binary with picotool's `uf2 convert`
    # subcommand. The picotool CMake target is imported by pico_add_extra_outputs above,
    # so $<TARGET_FILE:picotool> resolves to the right executable at build time.
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

