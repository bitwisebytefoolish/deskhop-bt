/* RP2350 / Pi Pico 2 W variant of deskhop's custom flash layout.
   Mirrors misc/memory_map.ld in spirit (preserves the four deskhop-specific
   sections: .section_disk, .section_metadata, .section_staging,
   .section_config) but uses RP2350-correct memory regions and includes the
   RP2350-specific embedded-block + TLS sections that the SDK 2.x default
   layout expects.

   Address layout (4 MB flash, 512 KB SRAM):

     0x10000000 +---------------------+ FLASH (executable)              1 MB
                |                     |
     0x10100000 +---------------------+ DISK_IMAGE (web-config FAT)    64 KB
     0x10110000 +---------------------+ FW_METADATA (version + CRC)     4 KB
     0x10111000 +---------------------+ FW_STAGING (OTA staging area)   1 MB
                |                     |
     0x103FD000 +---------------------+ FLASH_BTSTACK_BANK (BT bonds)   8 KB
     0x103FF000 +---------------------+ FLASH_CONFIG (persistent cfg)   4 KB
     0x10400000 +---------------------+ end of flash

   FLASH_BTSTACK_BANK reserves space for BTstack's TLV bond storage
   (pico_btstack_flash_bank requires two FLASH_SECTOR_SIZE sectors).
   It is declared (NOLOAD) so it never appears in the .uf2 image; the
   pico_flash_bank library reads/writes this region at runtime via
   PICO_FLASH_BANK_STORAGE_OFFSET (set in CMakeLists.txt to match the
   `__FLASH_TOTAL_SIZE - __CONFIG_STORAGE_LEN - __BTSTACK_BANK_LEN`
   offset below).

   The 1 MB executable region is ~12x the current 83 KB pico build. Of
   that, ~240 KB is taken by the cyw43-driver firmware blob (.rodata) and
   ~30-50 KB by cyw43_arch + driver. That leaves ~700 KB of headroom for
   BTstack (typical Classic + BLE host footprint ~50-100 KB) and the
   hid_host work scheduled for #6 / #9. STAGING matches FLASH size so a
   full firmware replacement fits.
*/

__FLASH_LEN = 1024k;
__DISK_IMAGE_LEN = 64k;
__METADATA_LEN = 4k;
__TOTAL_IMAGE_LENGTH = __FLASH_LEN + __DISK_IMAGE_LEN + __METADATA_LEN;
__STAGING_LEN = 1024k;
__CONFIG_STORAGE_LEN = 4k;
__BTSTACK_BANK_LEN = 8k;
__FLASH_TOTAL_SIZE = 4096k;

MEMORY
{
    FLASH(rx)         : ORIGIN = 0x10000000, LENGTH = __FLASH_LEN
    DISK_IMAGE(rw)    : ORIGIN = 0x10000000 + __FLASH_LEN, LENGTH = __DISK_IMAGE_LEN
    FW_METADATA(rw)   : ORIGIN = 0x10000000 + __FLASH_LEN + __DISK_IMAGE_LEN, LENGTH = __METADATA_LEN
    FW_STAGING(rw)    : ORIGIN = 0x10000000 + __TOTAL_IMAGE_LENGTH, LENGTH = __STAGING_LEN

    FLASH_BTSTACK_BANK(rw) : ORIGIN = 0x10000000 + (__FLASH_TOTAL_SIZE - __CONFIG_STORAGE_LEN - __BTSTACK_BANK_LEN), LENGTH = __BTSTACK_BANK_LEN
    FLASH_CONFIG(rw)  : ORIGIN = 0x10000000 + (__FLASH_TOTAL_SIZE - __CONFIG_STORAGE_LEN), LENGTH = __CONFIG_STORAGE_LEN
    RAM(rwx)          : ORIGIN = 0x20000000, LENGTH = 512k
    SCRATCH_X(rwx)    : ORIGIN = 0x20080000, LENGTH = 4k
    SCRATCH_Y(rwx)    : ORIGIN = 0x20081000, LENGTH = 4k
}

PROVIDE(_staging_metadata = ORIGIN(FW_STAGING) + (__STAGING_LEN - __METADATA_LEN));
PROVIDE(_firmware_metadata = ORIGIN(FW_METADATA));

ENTRY(_entry_point)

SECTIONS
{
    .flash_begin : {
        ADDR_FW_RUNNING = .;
        __flash_binary_start = .;
    } > FLASH

    /* On RP2350 the bootrom enters at the address indicated in IMAGE_DEF,
       not at .boot2 (which is optional and historic). The .embedded_block
       carries that IMAGE_DEF and must appear within the first 4096 bytes. */
    .text : {
        __logical_binary_start = .;
        KEEP (*(.vectors))
        KEEP (*(.binary_info_header))
        __binary_info_header_end = .;
        KEEP (*(.embedded_block))
        __embedded_block_end = .;
        KEEP (*(.reset))
        *(.init)
        *libgcc.a:cmse_nonsecure_call.o
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)
        *(.fini)
        /* Pull all c'tors into .text */
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        /* Followed by destructors */
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        . = ALIGN(4);
        /* preinit data */
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(SORT(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        /* init data */
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        /* finit data */
        PROVIDE_HIDDEN (__fini_array_start = .);
        *(SORT(.fini_array.*))
        *(.fini_array)
        PROVIDE_HIDDEN (__fini_array_end = .);

        *(.eh_frame*)
        . = ALIGN(4);
    } > FLASH

    .boot2 : {
        __boot2_start__ = .;
        *(.boot2)
        __boot2_end__ = .;
    } > FLASH

    ASSERT(__boot2_end__ - __boot2_start__ <= 256,
        "ERROR: Pico second stage bootloader must be no more than 256 bytes in size")

    .rodata : {
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .rodata*)
        *(.srodata*)
        . = ALIGN(4);
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.flashdata*)))
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH
    __exidx_end = .;

    /* Machine inspectable binary information */
    . = ALIGN(4);
    __binary_info_start = .;
    .binary_info :
    {
        KEEP(*(.binary_info.keep.*))
        *(.binary_info.*)
    } > FLASH
    __binary_info_end = .;
    . = ALIGN(4);

    /* Section order mirrors misc/memory_map.ld (the rp2040 variant), which
       is known to play nicely with the post-build objcopy --update-section
       step. .text and .rodata stay in FLASH (no PICO_COPY_TO_RAM on rp2350),
       only .data / .tdata / .scratch_* take an LMA in FLASH. */

    .ram_vector_table (NOLOAD): {
        *(.ram_vector_table)
    } > RAM

    .data : {
        __data_start__ = .;
        *(vtable)

        *(.time_critical*)

        *(.data*)
        *(.sdata*)

        . = ALIGN(4);
        *(.after_data.*)
        . = ALIGN(4);
        /* preinit data */
        PROVIDE_HIDDEN (__mutex_array_start = .);
        KEEP(*(SORT(.mutex_array.*)))
        KEEP(*(.mutex_array))
        PROVIDE_HIDDEN (__mutex_array_end = .);

        *(.jcr)
        . = ALIGN(4);
    } > RAM AT> FLASH

    .tdata : {
        . = ALIGN(4);
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        __tdata_end = .;
    } > RAM AT> FLASH
    PROVIDE(__data_end__ = .);

    __etext = LOADADDR(.data);

    .uninitialized_data (NOLOAD): {
        . = ALIGN(4);
        *(.uninitialized_data*)
    } > RAM

    /* Start and end symbols must be word-aligned */
    .scratch_x : {
        __scratch_x_start__ = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end__ = .;
    } > SCRATCH_X AT > FLASH
    __scratch_x_source__ = LOADADDR(.scratch_x);

    .scratch_y : {
        __scratch_y_start__ = .;
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end__ = .;
    } > SCRATCH_Y AT > FLASH
    __scratch_y_source__ = LOADADDR(.scratch_y);

    .tbss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        __tls_base = .;
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)
        __tls_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __tbss_end = .;
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
        *(COMMON)
        PROVIDE(__global_pointer$ = . + 2K);
        *(.sbss*)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    .heap (NOLOAD):
    {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
        __HeapLimit = .;
    } > RAM

    /* Web-config FAT disk image — exposed to the host as a USB MSC LUN. */
    .section_disk : {
        ADDR_DISK_IMAGE = .;
        KEEP(*(.section_disk))
    } > DISK_IMAGE

    /* Firmware metadata (version + CRC32). Patched in post-build. */
    .section_metadata : {
        ADDR_FW_METADATA = .;
        KEEP(*(.section_metadata));
    } > FW_METADATA

    /* OTA staging area for cross-Pico firmware exchange over UART. */
    .section_staging : {
        ADDR_FW_STAGING = .;
        KEEP(*(.section_staging))
    } > FW_STAGING

    /* Pad metadata region to its full size so the binary footprint is
       consistent. */
    .fill : {
        FILL(0x00);
        . = ORIGIN(FW_METADATA) + LENGTH(FW_METADATA) - 1;
        BYTE(0x00)
        ___ROM_AT = .;
    } > FW_METADATA

    /* Persistent configuration storage at the very end of flash (NOLOAD —
       no content baked into the .uf2; firmware reads/writes at runtime via
       flash_range_erase / flash_range_program). */
    .section_config (NOLOAD) : {
        ADDR_CONFIG = .;
    } > FLASH_CONFIG

    .section_btstack_bank (NOLOAD) : {
        ADDR_BTSTACK_BANK = .;
    } > FLASH_BTSTACK_BANK

    /* by default we put core 0 stack at the end of scratch Y, so that if core 1
       stack is not used then all of SCRATCH_X is free. */
    .stack1_dummy (NOLOAD):
    {
        *(.stack1*)
    } > SCRATCH_X
    .stack_dummy (NOLOAD):
    {
        KEEP(*(.stack*))
    } > SCRATCH_Y

    .flash_end : {
        KEEP(*(.embedded_end_block*))
        __flash_binary_end = .;
    } > FLASH

    /* Pad FLASH region to its full size so DISK_IMAGE starts at a known
       address regardless of executable size. */
    .pad : {
        FILL(0x00)
        . = ADDR_DISK_IMAGE - __flash_binary_end - 1;
        BYTE(0x00)
        KEEP(*(.pad))
    } > FLASH

    /* stack limit is poorly named, but historically is maximum heap ptr */
    __StackLimit = ORIGIN(RAM) + LENGTH(RAM);
    __StackOneTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);
    __StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);
    __StackOneBottom = __StackOneTop - SIZEOF(.stack1_dummy);
    __StackBottom = __StackTop - SIZEOF(.stack_dummy);
    PROVIDE(__stack = __StackTop);

    /* picolibc and LLVM */
    PROVIDE (__heap_start = __end__);
    PROVIDE (__heap_end = __HeapLimit);
    PROVIDE( __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)) );
    PROVIDE( __tls_size_align = (__tls_size + __tls_align - 1) & ~(__tls_align - 1));
    PROVIDE( __arm32_tls_tcb_offset = MAX(8, __tls_align) );

    /* llvm-libc */
    PROVIDE (_end = __end__);
    PROVIDE (__llvm_libc_heap_limit = __HeapLimit);

    /* Check if data + heap + stack exceeds RAM limit */
    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")

    ASSERT( __binary_info_header_end - __logical_binary_start <= 1024, "Binary info must be in first 1024 bytes of the binary")
    ASSERT( __embedded_block_end - __logical_binary_start <= 4096, "Embedded block must be in first 4096 bytes of the binary")
}
