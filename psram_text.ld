/*
 * Extra linker script for code that executes from PSRAM.
 *
 *   VMA (run address):  0x11000000+  (QMI CS1, cached XIP window)
 *   LMA (stored in image): flash, right after .rodata
 *
 * Same idea as .data: stored in flash, copied to its run address at startup.
 * Startup code must do that copy itself (see psram_load_code()), because
 * PSRAM does not exist yet when crt0 runs.
 *
 * Added to the link with -T; INSERT makes ld merge it into the SDK's
 * default script instead of replacing it.
 */
SECTIONS
{
    .psram_text 0x11000000 : ALIGN(4)
    {
        __psram_text_start = .;
        *(.psram_text .psram_text.*)
        . = ALIGN(4);
        __psram_text_end = .;
    } AT> FLASH

    __psram_text_load = LOADADDR(.psram_text);
} INSERT AFTER .rodata;
