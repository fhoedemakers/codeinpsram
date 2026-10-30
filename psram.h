#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// QMI CS1 windows
#define PSRAM_BASE_CACHED   0x11000000u   // normal, goes through the XIP cache
#define PSRAM_BASE_UNCACHED 0x15000000u   // bypasses the XIP cache

// Place a function in PSRAM. Its run address is in PSRAM, its load image is in flash.
#define __psram_func __attribute__((section(".psram_text")))

// Detect and initialise PSRAM on the given XIP_CS1-capable GPIO.
// Returns the size in bytes, or 0 if no (supported) PSRAM was found.
// Must be called again if clk_sys changes (QMI timing depends on it).
size_t psram_init(unsigned int cs_pin);

// Copy the .psram_text image from flash into PSRAM and verify it.
// Call after psram_init(), before calling any __psram_func function.
bool psram_load_code(size_t psram_size);
