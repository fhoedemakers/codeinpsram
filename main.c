#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"

#include "psram.h"

#ifndef PSRAM_CS_PIN
#define PSRAM_CS_PIN 47
#endif

#define ITERATIONS 10000000u
#define BIG_PASSES 200u
#define DISPATCHES 1000000u

typedef uint32_t (*bench_fn)(uint32_t);

// Same body three times, living in three different places.
static inline __attribute__((always_inline)) uint32_t bench_body(uint32_t n) {
    uint32_t x = 12345;
    for (uint32_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    return x;
}

// Large variant: one pass is 4096 xorshift steps of straight-line code. Each step
// is three 32-bit EOR.W instructions, so one pass is 48 KiB of code -- three times
// the 16 KiB XIP cache. Every pass misses the cache and fetches from flash/PSRAM.
#define XS1    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
#define XS4    XS1 XS1 XS1 XS1
#define XS16   XS4 XS4 XS4 XS4
#define XS64   XS16 XS16 XS16 XS16
#define XS256  XS64 XS64 XS64 XS64
#define XS1024 XS256 XS256 XS256 XS256
#define XS4096 XS1024 XS1024 XS1024 XS1024

static inline __attribute__((always_inline)) uint32_t big_body(uint32_t n) {
    uint32_t x = 12345;
    for (uint32_t i = 0; i < n; i++) {
        XS4096
    }
    return x;
}

// Random dispatch: a switch over 4096 small handlers, selected by the low bits of
// the xorshift state -- the access pattern of an interpreter or emulator core.
// Each handler is one xorshift step plus an add of its own number (so the compiler
// cannot merge handlers), about 20 bytes. With the 16 KiB jump table that is about
// 96 KiB, so most dispatches jump to code that is not in the XIP cache.
// The jump table holds absolute addresses, so unlike the other benchmarks this
// one cannot be run through the uncached alias.
#define H1(k)    case (k): XS1 x += (k); break;
#define H4(k)    H1(4 * (k)) H1(4 * (k) + 1) H1(4 * (k) + 2) H1(4 * (k) + 3)
#define H16(k)   H4(4 * (k)) H4(4 * (k) + 1) H4(4 * (k) + 2) H4(4 * (k) + 3)
#define H64(k)   H16(4 * (k)) H16(4 * (k) + 1) H16(4 * (k) + 2) H16(4 * (k) + 3)
#define H256(k)  H64(4 * (k)) H64(4 * (k) + 1) H64(4 * (k) + 2) H64(4 * (k) + 3)
#define H1024(k) H256(4 * (k)) H256(4 * (k) + 1) H256(4 * (k) + 2) H256(4 * (k) + 3)
#define H4096    H1024(0) H1024(1) H1024(2) H1024(3)

static inline __attribute__((always_inline)) uint32_t dispatch_body(uint32_t n) {
    uint32_t x = 12345;
    for (uint32_t i = 0; i < n; i++) {
        switch (x & 4095u) {
            H4096
        }
    }
    return x;
}

// Runs from PSRAM (VMA 0x11xxxxxx, image stored in flash)
__psram_func __attribute__((noinline))
uint32_t bench_psram(uint32_t n) { return bench_body(n); }

// Runs from flash (XIP)
__attribute__((noinline))
uint32_t bench_flash(uint32_t n) { return bench_body(n); }

// Runs from SRAM
uint32_t __no_inline_not_in_flash_func(bench_ram)(uint32_t n) { return bench_body(n); }

__psram_func __attribute__((noinline))
uint32_t big_psram(uint32_t n) { return big_body(n); }

__attribute__((noinline))
uint32_t big_flash(uint32_t n) { return big_body(n); }

uint32_t __no_inline_not_in_flash_func(big_ram)(uint32_t n) { return big_body(n); }

__psram_func __attribute__((noinline))
uint32_t dispatch_psram(uint32_t n) { return dispatch_body(n); }

__attribute__((noinline))
uint32_t dispatch_flash(uint32_t n) { return dispatch_body(n); }

uint32_t __no_inline_not_in_flash_func(dispatch_ram)(uint32_t n) { return dispatch_body(n); }

// The same function through the uncached XIP alias (0x14xxxxxx flash, 0x15xxxxxx
// PSRAM): every instruction fetch goes to the QMI. Only valid for code that uses
// nothing but PC-relative branches, which holds for bench_* and big_*.
static bench_fn uncached(bench_fn fn) {
    return (bench_fn)((uintptr_t)fn + (XIP_NOCACHE_NOALLOC_BASE - XIP_BASE));
}

static void run(const char *name, bench_fn fn, uint32_t n) {
    fn(1);  // warm up (fills the XIP cache for flash/PSRAM variants, if the code fits)
    uint32_t t0 = time_us_32();
    uint32_t r = fn(n);
    uint32_t dt = time_us_32() - t0;
    printf("%-11s @ %p  result=%08x  %u us\n", name, (void *)fn, (unsigned)r, (unsigned)dt);
}

static bool in_psram(bench_fn fn, size_t psram_size) {
    uintptr_t a = (uintptr_t)fn;
    return a >= PSRAM_BASE_CACHED && a < PSRAM_BASE_CACHED + psram_size;
}

// QMI settings of one memory window: M0 is the flash (set up by the bootrom),
// M1 the PSRAM (set up by psram_init()). Both windows share the register layout.
static void print_qmi(const char *name, unsigned int m) {
    const uint32_t t = qmi_hw->m[m].timing;
    unsigned int clkdiv = (t & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
    if (clkdiv == 0) clkdiv = 256;  // 0 encodes a divisor of 256
    printf("%-5s (QMI M%u): SCK %u MHz (clk_sys/%u), rxdelay %u, cooldown %u, pagebreak %u, "
           "rcmd %04x, rfmt %08x\n",
           name, m, (unsigned)(clock_get_hz(clk_sys) / clkdiv / 1000000u), clkdiv,
           (unsigned)((t & QMI_M0_TIMING_RXDELAY_BITS) >> QMI_M0_TIMING_RXDELAY_LSB),
           (unsigned)((t & QMI_M0_TIMING_COOLDOWN_BITS) >> QMI_M0_TIMING_COOLDOWN_LSB),
           (unsigned)((t & QMI_M0_TIMING_PAGEBREAK_BITS) >> QMI_M0_TIMING_PAGEBREAK_LSB),
           (unsigned)qmi_hw->m[m].rcmd, (unsigned)qmi_hw->m[m].rfmt);
}

int main(void) {
    stdio_init_all();
    // while (!stdio_usb_connected()) {  // wait for a terminal
    //     sleep_ms(100);
    // }
    sleep_ms(200);
    for (int i = 0; i < 15; i++) {
        printf("Waiting for uart connection %d...\n", i);
        sleep_ms(100);
    }
    // 1) We are running from flash/SRAM here. Bring PSRAM up.
    size_t psram_size = psram_init(PSRAM_CS_PIN);
    if (!psram_size) {
        printf("No PSRAM detected on GPIO%d\n", PSRAM_CS_PIN);
        goto halt;
    }
    printf("PSRAM: %u KiB\n", (unsigned)(psram_size / 1024));
    printf("clk_sys: %u MHz\n", (unsigned)(clock_get_hz(clk_sys) / 1000000u));
    print_qmi("flash", 0);
    print_qmi("psram", 1);

    // 2) Copy the .psram_text image flash -> PSRAM
    if (!psram_load_code(psram_size)) {
        printf("Copying code to PSRAM failed (does not fit or verify error)\n");
        goto halt;
    }

    // 3) Now it is safe to call __psram_func functions
    printf("Code in PSRAM is live\n");

    printf("\nSmall loop (fits in the XIP cache), %u iterations:\n", ITERATIONS);
    run("flash", bench_flash, ITERATIONS);
    run("sram", bench_ram, ITERATIONS);
    run("psram", bench_psram, ITERATIONS);

    printf("\nLarge code (48 KiB per pass, 3x the XIP cache), %u passes:\n", BIG_PASSES);
    run("flash", big_flash, BIG_PASSES);
    run("sram", big_ram, BIG_PASSES);
    run("psram", big_psram, BIG_PASSES);

    printf("\nRandom dispatch (4096 handlers, ~96 KiB), %u dispatches:\n", DISPATCHES);
    run("flash", dispatch_flash, DISPATCHES);
    run("sram", dispatch_ram, DISPATCHES);
    run("psram", dispatch_psram, DISPATCHES);

    // Last, so the results above are already printed should this fault.
    printf("\nUncached alias (no XIP cache, every fetch goes to the QMI):\n");
    run("small flash", uncached(bench_flash), ITERATIONS / 10);
    run("small psram", uncached(bench_psram), ITERATIONS / 10);
    run("large flash", uncached(big_flash), BIG_PASSES);
    run("large psram", uncached(big_psram), BIG_PASSES);

    printf("\nbench_psram    in PSRAM window: %s\n", in_psram(bench_psram, psram_size) ? "yes" : "NO");
    printf("big_psram      in PSRAM window: %s\n", in_psram(big_psram, psram_size) ? "yes" : "NO");
    printf("dispatch_psram in PSRAM window: %s\n", in_psram(dispatch_psram, psram_size) ? "yes" : "NO");

halt:
    while (true) {
        tight_loop_contents();
    }
}
