# codeinpsram

Executes code from external QSPI PSRAM on the RP2350 and measures instruction-fetch
performance from PSRAM, flash and SRAM.

## Background

The RP2350 QSPI memory interface (QMI) has two chip selects, each with its own
memory-mapped window:

| Window | Device | Cached address | Uncached alias |
|---|---|---|---|
| M0 (CS0) | Boot flash | `0x10000000` | `0x14000000` |
| M1 (CS1) | PSRAM | `0x11000000` | `0x15000000` |

Both cached windows go through the same 16 KiB XIP cache (8-byte lines). Both lie in
the Cortex-M33 code region, so the processor can execute directly from PSRAM. PSRAM is
volatile and is not configured at reset, so code must be copied into it at run time,
after the QMI has been set up for the PSRAM device.

## Hardware requirements

- An RP2350 board with APS6404-compatible QSPI PSRAM. Detection accepts devices that
  report the known-good-die value `0x5D` and derives the size (up to 8 MiB) from the
  device ID.
- The PSRAM chip select connected to a GPIO that supports the `XIP_CS1` function:
  GPIO 0, 8, 19 or 47 (47 on RP2350B only).
- A serial terminal on UART0 (TX GPIO 0, RX GPIO 1, 115200 baud). If the PSRAM chip
  select is on GPIO 0, the UART must be moved to other pins.

The project is configured for the Pimoroni Pico Plus 2, which has its PSRAM chip select
on GPIO 47. For another board, change `PICO_BOARD` and `PSRAM_CS_PIN` in
`CMakeLists.txt`.

## Building

```sh
export PICO_SDK_PATH=/path/to/pico-sdk        # tested with Pico SDK 2.2.0
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make -C build -j$(nproc)
```

This produces `build/psram_exec.uf2`. Copy it to the board in BOOTSEL mode, or use
`picotool load -x build/psram_exec.uf2`.

The results below were obtained with a Debug build (`-Og`). A Release build (`-O3`,
the SDK default when no build type is given) also works, but compiling `main.c` then
takes several minutes because of the 4096-case `switch` in the random-dispatch
benchmark. The generated code also differs (for example, the dispatch function is
89,420 instead of 97,840 bytes), so Release timings are not directly comparable with
the results below.

The link step reports two warnings:

```
psram_text.ld:24: warning: memory region `FLASH' not declared
pico_flash_region.ld:1: warning: redeclaration of memory region `FLASH'
```

These are expected and do not affect the output. `psram_text.ld` uses `INSERT`, which
only works when the inserting script is read before the main SDK linker script. At that
point the `FLASH` region has not been declared yet. The linker creates a placeholder
region, and the SDK's `MEMORY` block later fills in that same region. The resulting
placement is correct. Removing the warnings would require a full copy of the SDK linker
script, which would then have to be kept in step with SDK releases.

## Files

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Build configuration; adds `psram_text.ld` to the link |
| `psram_text.ld` | Linker script fragment that defines the `.psram_text` section |
| `psram.h`, `psram.c` | PSRAM initialisation and code loader |
| `main.c` | Benchmarks |

## How it works

### 1. Placement at link time

`__psram_func` (defined in `psram.h`) places a function in the section `.psram_text`.
`psram_text.ld` gives this section two addresses:

- **Run address (VMA):** `0x11000000`, the start of the PSRAM window. All references to
  the function resolve to this address.
- **Load address (LMA):** flash, directly after `.rodata`. This is where the bytes are
  stored in the UF2 image.

This is the same mechanism the SDK uses for initialised data (`.data`). The C runtime
copies `.data` before `main()`, but it cannot copy `.psram_text` because PSRAM is not
usable at that point. The application performs this copy itself. The linker script
exports `__psram_text_start`, `__psram_text_end` and `__psram_text_load` for that
purpose.

### 2. PSRAM initialisation: `psram_init()`

1. Assigns the chip-select GPIO to `XIP_CS1`.
2. Calculates the QMI timing from `clk_sys`: clock divider (PSRAM clock at most
   133 MHz), receive delay, maximum chip-select time (8 µs) and minimum deselect time
   (18 ns). This runs from flash, before the steps below.
3. Disables interrupts and calls `psram_setup()`, which runs from SRAM. While the QMI is
   in direct mode, memory-mapped accesses stall, so no code may be fetched from flash.
   - `psram_detect()` sends `0xF5` (exit quad mode, in case of a soft reset), then
     reads the device ID with `0x9F` and derives the size from it.
   - Switches the device to quad mode (`0x35`).
   - Programs window M1: timing, quad read `0xEB` with 6 wait cycles, quad write `0x38`.
   - Leaves direct mode and enables writes through the M1 window.

The function returns the PSRAM size in bytes, or 0 if no supported device was found.

### 3. Loading: `psram_load_code()`

1. Checks that `.psram_text` fits in the detected PSRAM.
2. Copies the section from its flash load address to PSRAM through the uncached alias
   (`0x15000000`), so the data is written directly to the device.
3. Invalidates the XIP cache lines for the range, so that no stale lines from an
   earlier load remain.
4. Issues `DSB` and `ISB`, so the writes complete before instructions are fetched.
5. Compares the copy with the flash image through the cached address `0x11000000`, the
   address the processor executes from.

### 4. Execution

After `psram_load_code()` returns `true`, functions marked `__psram_func` are called like
any other function.

## Using the mechanism in other code

```c
#include "psram.h"

__psram_func __attribute__((noinline))
void my_function(void) { /* ... */ }

size_t size = psram_init(PSRAM_CS_PIN);
if (size && psram_load_code(size)) {
    my_function();
}
```

Constraints:

- No `__psram_func` function may be called before `psram_load_code()` has succeeded.
- `psram_init()` must be called again after `clk_sys` changes, because the QMI timing
  depends on it. It must not be called while either core executes from, or accesses,
  PSRAM.
- Flash erase and program operations take the QMI out of memory-mapped mode. Code in
  PSRAM must not run during these operations, which is the same rule that applies to
  code in flash. The SDK's flash functions preserve the PSRAM (M1) settings.
- PSRAM code and flash code share the XIP cache, and each evicts the other's lines.

## Benchmarks

Each benchmark body is an `always_inline` function that is instantiated three times:
in flash (an ordinary function), in SRAM (`__no_inline_not_in_flash_func`) and in PSRAM
(`__psram_func`). Each measurement calls the function once with `n = 1` as a warm-up,
which loads code that fits into the XIP cache. It then times one call with
`time_us_32()`. All copies of a benchmark must report the same `result` value. A match
confirms that the copy in PSRAM executed correctly.

| Benchmark | Code size per copy | Workload | What it measures |
|---|---|---|---|
| Small loop | 30 bytes | xorshift loop, 10,000,000 iterations | Execution from the XIP cache |
| Large code | 49,174 bytes | 4096 xorshift steps without branches, 200 passes | Sequential fetch throughput; the code is three times the cache size |
| Random dispatch | 97,840 bytes | `switch` with 4096 handlers selected pseudo-randomly, 1,000,000 dispatches | Non-sequential fetches, similar to an interpreter or emulator core |
| Uncached alias | as above | Small loop (1,000,000 iterations) and large code, flash and PSRAM copies, called through `0x14000000`/`0x15000000` | Fetches without the XIP cache |

The jump table of the random-dispatch function contains absolute addresses. That
function is therefore not run through the uncached alias, because the first dispatch
would branch back into the cached window.

Before the benchmarks, the program prints `clk_sys` and the QMI settings of the flash
(M0) and PSRAM (M1) windows.

## Results

Conditions: `clk_sys` 150 MHz, PSRAM clock 75 MHz (`clk_sys`/2), 8 MiB PSRAM detected,
Debug build (`-Og`), Pico SDK 2.2.0, arm-none-eabi-gcc 13.2.1.

These results were recorded before the random-dispatch and uncached benchmarks and the
QMI settings output were added. Results for those have not been recorded yet. The code
of the two measured benchmarks has not changed since.

### Small loop

| Location | Address | Time |
|---|---|---|
| Flash | `0x10000275` | 666,668 µs |
| SRAM | `0x20000111` | 666,666 µs |
| PSRAM | `0x11000001` | 666,666 µs |

666,666 µs for 10,000,000 iterations is 10.0 clock cycles per iteration in all three
locations. (Addresses are odd because Thumb function pointers have bit 0 set.) Once the
loop is in the XIP cache, execution from flash or PSRAM is as fast as from SRAM.

### Large code

| Location | Total | Per pass | Code throughput | Relative to SRAM |
|---|---|---|---|---|
| SRAM | 32,774 µs | 164 µs | not memory-bound | 1.0 |
| PSRAM | 307,750 µs | 1,539 µs | 32.0 MB/s | 9.4 |
| Flash | 435,250 µs | 2,176 µs | 22.6 MB/s | 13.3 |

Throughput is 49,174 bytes per pass divided by the time per pass (1 MB = 10^6 bytes).

- **SRAM.** 164 µs per pass is 24,580 cycles for 12,288 `EOR.W` instructions: 2 cycles
  per instruction. This figure is set by the processor, not by the memory. The small
  loop agrees with it: 3 × 2 cycles for the `EOR.W` instructions plus 4 cycles for the
  counter, compare and branch give 10 cycles. Each `EOR.W` shifts the result of the
  previous one, which probably prevents them from overlapping.
- **PSRAM.** The theoretical maximum at 75 MHz on four data lines is 37.5 MB/s, so
  32.0 MB/s is 85% of it. This requires the QMI to extend the transfer in progress on
  sequential cache misses, instead of sending command, address and wait cycles again.
  It does so within the cooldown period that `psram_setup()` enables (`COOLDOWN = 1`).
  The remaining overhead most likely comes from transfers being split at 1 KiB page
  boundaries (`PAGEBREAK`) and after 8 µs (`MAX_SELECT`).
- **Flash.** Flash is slower than PSRAM in this configuration. On the RP2350 the SDK
  does not embed a boot stage 2 by default (`PICO_EMBED_XIP_SETUP=0`). The flash read
  mode and clock divider are therefore the ones the boot ROM selected while scanning
  the flash device. The measured throughput is consistent with a divider of 3 (50 MHz,
  25 MB/s theoretical). The QMI settings line printed by the current version of the
  program shows the actual divider.

### Conclusions

- Code that fits in the XIP cache runs from PSRAM and flash at the same speed as from
  SRAM.
- Code that does not fit is limited by QSPI throughput. For straight-line code at
  150 MHz, execution from PSRAM was 9.4 times and from flash 13.3 times slower than
  from SRAM.
- Straight-line code is the most favourable case for streaming. Code with frequent
  jumps to locations outside the cache restarts a transfer on every jump and is
  expected to be slower; the random-dispatch benchmark measures this case.

## License

This project is licensed under the MIT License; see [LICENSE](LICENSE).

`psram.c` is based on `ports/rp2/rp2_psram.c` from the
[MicroPython](https://github.com/micropython/micropython) project, copyright (c) 2025
Phil Howard, Mike Bell and Kirk D. Benell, which is also licensed under the MIT
License. The original copyright notice is retained in that file.
