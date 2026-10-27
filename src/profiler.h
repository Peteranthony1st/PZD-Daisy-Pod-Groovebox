#pragma once
// Per-stage CPU profiling for the audio callback -- the Cortex-M7's own
// cycle counter (DWT->CYCCNT, one read is a single load) timing each
// engine, the looper, the shared delay and reverb, and everything else,
// as a share of one block's time budget. Shown on Global:Mixer (Button1
// toggles it). Measuring only: nothing here changes what's processed.
#include <cstddef>
#include <cstdint>
#include "stm32h7xx.h"

namespace Prof
{
enum Stage
{
    kGrains,
    kDexed,
    kEnsemble,
    kSampler,
    kLooper,
    kDelay,
    kReverb,
    kOther, // the whole callback minus the stages above
    kNumStages,
};

// 0..1 of a block's budget, like g_cpu_avg_load/g_cpu_peak_load:
// a smoothed average, and the highest single block since the last reset
// (reset together with the CPU peak, see main.cpp).
extern float g_avg[kNumStages];
extern float g_peak[kNumStages];

inline uint32_t Now()
{
    return DWT->CYCCNT;
}

// Starts the cycle counter (harmless if already running).
inline void Init()
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55; // unlock -- required on the M7
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

// One block's cycles per stage (kOther is filled in here from total).
void EndBlock(uint32_t cycles[kNumStages], uint32_t total, size_t size, float sample_rate);
void ResetPeaks();
} // namespace Prof
