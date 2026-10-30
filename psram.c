#include "psram.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"

// Provided by psram_text.ld
extern char __psram_text_start[];
extern char __psram_text_end[];
extern char __psram_text_load[];

/*
 * Everything that touches QMI direct mode must run from SRAM and must not
 * call into flash: while direct mode is enabled, XIP fetches stall.
 * (Anything that would call into flash -- clock_get_hz(), 64-bit division
 * helpers from libgcc -- is done BEFORE entering direct mode.)
 */

// Read the PSRAM ID. Handles APS6404-style parts (KGD 0x5D).
static size_t __no_inline_not_in_flash_func(psram_detect)(void) {
    size_t size = 0;

    qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;

    // Wait for the last XIP transfer's cooldown to expire before the first
    // direct-mode operation.
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }

    // Exit QPI mode in case we were initialised before (soft reset, etc.)
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                        (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
                        0xf5u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    // Read ID (0x9F) as plain SPI
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0, eid = 0;
    for (int i = 0; i < 7; i++) {
        qmi_hw->direct_tx = (i == 0) ? 0x9fu : 0xffu;
        while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)) {
        }
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
        }
        uint8_t rx = (uint8_t)qmi_hw->direct_rx;
        if (i == 5) kgd = rx;
        else if (i == 6) eid = rx;
    }
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    if (kgd == 0x5D) {
        size = 1024 * 1024;
        uint8_t size_id = eid >> 5;
        if (eid == 0x26 || size_id == 2) size *= 8;      // 8 MiB
        else if (size_id == 0)           size *= 2;      // 2 MiB
        else if (size_id == 1)           size *= 4;      // 4 MiB
    }
    return size;
}

static size_t __no_inline_not_in_flash_func(psram_setup)(uint32_t divisor, uint32_t rxdelay,
                                                         uint32_t max_select,
                                                         uint32_t min_deselect) {
    size_t size = psram_detect();
    if (!size) return 0;

    // Direct mode, CS1 asserted automatically per transfer
    qmi_hw->direct_csr = 10u << QMI_DIRECT_CSR_CLKDIV_LSB |
                         QMI_DIRECT_CSR_EN_BITS |
                         QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }

    // Switch the PSRAM into QPI mode (command 0x35)
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }

    // Memory-mapped timing for window M1 (APS6404)
    qmi_hw->m[1].timing =
        1u << QMI_M1_TIMING_COOLDOWN_LSB |
        QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
        max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
        min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
        rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
        divisor << QMI_M1_TIMING_CLKDIV_LSB;

    // Reads: 0xEB "fast read quad", 6 dummy cycles, everything quad width
    qmi_hw->m[1].rfmt =
        QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB |
        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB |
        QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB |
        6u << QMI_M0_RFMT_DUMMY_LEN_LSB;
    qmi_hw->m[1].rcmd = 0xEB;

    // Writes: 0x38 "quad write"
    qmi_hw->m[1].wfmt =
        QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_WFMT_ADDR_WIDTH_LSB |
        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_WFMT_DATA_WIDTH_LSB |
        QMI_M0_WFMT_PREFIX_LEN_VALUE_8   << QMI_M0_WFMT_PREFIX_LEN_LSB;
    qmi_hw->m[1].wcmd = 0x38;

    // Leave direct mode; XIP resumes
    qmi_hw->direct_csr = 0;

    // Allow writes through the M1 window (default is read-only)
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

    return size;
}

size_t psram_init(unsigned int cs_pin) {
    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    // ---- all flash-resident work happens before direct mode ----
    const uint32_t clock_hz = clock_get_hz(clk_sys);
    const uint32_t max_psram_hz = 133000000u;

    uint32_t divisor = (clock_hz + max_psram_hz - 1) / max_psram_hz;
    if (divisor == 1 && clock_hz > 100000000u) divisor = 2;

    uint32_t rxdelay = divisor;
    if (clock_hz / divisor > 100000000u) rxdelay += 1;

    // Max select time <= 8 us, in units of 64 system clocks: 8us*f/64 = f/8e6
    uint32_t max_select = clock_hz / 8000000u;

    // Min deselect >= 18 ns, in system clocks, minus what the divider already gives
    int32_t min_deselect = (int32_t)((18u * (clock_hz / 1000000u) + 999u) / 1000u)
                           - (int32_t)((divisor + 1) / 2);
    if (min_deselect < 0) min_deselect = 0;

    // An interrupt handler running from flash would stall in direct mode
    uint32_t irq_state = save_and_disable_interrupts();
    size_t size = psram_setup(divisor, rxdelay, max_select, (uint32_t)min_deselect);
    restore_interrupts(irq_state);
    return size;
}

bool psram_load_code(size_t psram_size) {
    const size_t len = (size_t)(__psram_text_end - __psram_text_start);
    if (len == 0) return true;  // nothing placed in .psram_text

    // Does the image fit?
    if ((uintptr_t)__psram_text_end - PSRAM_BASE_CACHED > psram_size) return false;

    // Write through the UNCACHED alias: data goes straight to PSRAM and no
    // XIP cache lines are involved, so instruction fetches via the cached
    // alias will not see stale data.
    void *dst = (void *)((uintptr_t)__psram_text_start +
                         (PSRAM_BASE_UNCACHED - PSRAM_BASE_CACHED));
    memcpy(dst, __psram_text_load, len);

    // Drop any lines the XIP cache still holds for this range (e.g. code loaded
    // there earlier), so the cached alias sees the new contents. The range must
    // be whole cache lines.
    const uintptr_t line_mask = XIP_CACHE_LINE_SIZE - 1;
    const uintptr_t first = ((uintptr_t)__psram_text_start - XIP_BASE) & ~line_mask;
    const uintptr_t last = ((uintptr_t)__psram_text_end - XIP_BASE + line_mask) & ~line_mask;
    xip_cache_invalidate_range(first, last - first);

    __dsb();
    __isb();

    // Verify via the cached alias (the one the code will execute from)
    return memcmp(__psram_text_start, __psram_text_load, len) == 0;
}
