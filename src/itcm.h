#pragma once

// Places a function in ITCMRAM (the STM32H750's dedicated, zero-
// contention instruction memory) instead of wherever it would otherwise
// land -- QSPIFLASH under this project's BOOT_QSPI setup. Mirrors
// libDaisy's own DSY_QSPI_TEXT/DSY_SDRAM_BSS pattern (per/qspi.h,
// dev/sdram.h), but libDaisy has no ITCM equivalent of its own, so this
// is project-local. Only meaningful with the custom
// src/STM32H750IB_qspi_custom.lds, which is the only linker script with
// a .itcm_text output section wired up -- see that file's header comment
// for the full story (a real, measured audio-callback timing regression
// under plain BOOT_QSPI, not a preemptive optimization).
//
// Reserve this for the real-time call graph only (AudioCallback() and
// everything it calls) -- ITCMRAM is a fixed 64KB, shared by all of it.
#define DSY_ITCM_TEXT __attribute__((section(".itcm_text")))

// Places a zero-initialized variable in DTCMRAM -- the CPU's own 128KB of
// zero-wait data memory, read without going through the 16KB data cache,
// so per-sample engine state can't be evicted by the loop audio streaming
// in from SDRAM. Same idea as DSY_ITCM_TEXT, for data. Two rules:
//   - Startup code only zeroes ordinary .bss, so main.cpp's
//     ZeroDtcmBss() clears this section before any constructor runs.
//   - DMA can't reach DTCM: never put SD/audio/ADC transfer buffers here
//     (see PerformanceStore's own static FIL/DIR comments).
// The stack also lives in DTCMRAM (growing down from the top), so keep
// what goes here to the hot real-time state only.
#define DSY_DTCM_BSS __attribute__((section(".dtcmram_bss")))
