#pragma once
#include <math.h>
#include <stddef.h>

// The shared delay bus's settings, in one place so the screen (Ui), the
// live audio (main.cpp) and WAV export (PerformanceStore::ExportWav())
// can't disagree.
namespace SharedDelay
{
constexpr size_t kMaxSamples  = 72000; // 1.5s @ 48kHz
constexpr float  kMinMs       = 20.f;
constexpr float  kMaxMs       = (float)kMaxSamples / 48000.f * 1000.f;
constexpr float  kMaxFeedback = 0.9f; // below 1.0 so the cross-feedback can't self-oscillate

// Time knob -> milliseconds: exponential, so the short slapback end gets
// more of the knob's travel (same curve shape the ADSR times use).
inline float TimeMs(float time01)
{
    time01 = time01 < 0.f ? 0.f : time01 > 1.f ? 1.f : time01;
    return kMinMs * powf(kMaxMs / kMinMs, time01);
}

// Below this the bus is skipped entirely ("Off") -- matches the screen's
// own 0% rounding, see main.cpp's delay_bypassed.
inline bool IsOff(float time01) { return time01 < 0.005f; }
} // namespace SharedDelay
