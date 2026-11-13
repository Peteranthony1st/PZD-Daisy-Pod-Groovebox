#pragma once
#include <math.h>
#include <stddef.h>
#include <string.h>
#include "daisysp.h"

// The shared delay bus, in one place so the screen (Ui), the live audio
// (main.cpp) and WAV export (PerformanceStore::ExportWav()) can't
// disagree -- export used to carry its own copy of this code.
namespace SharedDelay
{
constexpr size_t kMaxSamples  = 144000; // 3s @ 48kHz -- room for a synced 1/2 note down to 40 BPM
constexpr float  kMinMs       = 20.f;
constexpr float  kMaxMs       = (float)kMaxSamples / 48000.f * 1000.f;
constexpr float  kMaxFeedback = 0.9f; // below 1.0 so the cross-feedback can't self-oscillate

// Below this the bus is skipped entirely ("Off") -- matches the screen's
// own 0% rounding, see main.cpp's delay_bypassed.
inline bool IsOff(float time01) { return time01 < 0.005f; }

// Tempo-synced note values, shortest first (quarter note = 1 beat).
constexpr int kNumSyncDivisions = 8;
inline float SyncBeats(int idx)
{
    static const float kBeats[kNumSyncDivisions]
        = {0.25f, 1.f / 3.f, 0.5f, 2.f / 3.f, 0.75f, 1.f, 1.5f, 2.f};
    return kBeats[idx < 0 ? 0 : idx >= kNumSyncDivisions ? kNumSyncDivisions - 1 : idx];
}
inline const char* SyncName(int idx)
{
    static const char* const kNames[kNumSyncDivisions]
        = {"1/16", "1/8T", "1/8", "1/4T", "1/8D", "1/4", "1/4D", "1/2"};
    return kNames[idx < 0 ? 0 : idx >= kNumSyncDivisions ? kNumSyncDivisions - 1 : idx];
}
// With Sync on, the Time knob steps through the note values (its very
// bottom is still "Off", same as free time).
inline int SyncIndex(float time01)
{
    int idx = (int)(time01 * kNumSyncDivisions);
    return idx >= kNumSyncDivisions ? kNumSyncDivisions - 1 : idx;
}

// Delay time in ms. Free: exponential from 20ms to 3s, so the short
// slapback end gets more of the knob's travel (same curve shape the ADSR
// times use). Synced: the chosen note value at this BPM, halved until it
// fits the buffer (only a 1/2 note below 40 BPM needs that).
inline float TimeMs(float time01, bool sync, float bpm)
{
    time01 = time01 < 0.f ? 0.f : time01 > 1.f ? 1.f : time01;
    if(!sync)
        return kMinMs * powf(kMaxMs / kMinMs, time01);
    float ms = SyncBeats(SyncIndex(time01)) * 60000.f / (bpm > 1.f ? bpm : 1.f);
    while(ms > kMaxMs)
        ms *= 0.5f;
    return ms;
}

struct Settings
{
    float time01      = 0.f;
    float feedback01  = 0.35f;
    bool  sync        = false;
    float pingpong01  = 0.f; // 0 = echoes follow the source's own pan, 1 = full L/R bounce
    float tone01      = 0.f; // 0 = bright repeats, 1 = each repeat noticeably darker
};

// Two delay lines with crossed feedback (L's echo feeds R and vice
// versa). Ping-Pong only changes what goes IN: at 0 each side gets its
// own input, which is exactly how this bus has always behaved (a centred
// sound echoes on both sides together, a panned one bounces); at 1 the
// input is summed to mono into the left line only, so every echo
// alternates L, R, L, R whatever the pan. Tone is a one-pole low-pass on
// the feedback only -- the first echo stays bright, each repeat after it
// is filtered once more.
class Bus
{
  public:
    // Lives in SDRAM, which isn't zeroed at boot -- clear everything.
    void Init()
    {
        memset((void*)this, 0, sizeof(*this));
        l_.Init();
        r_.Init();
    }

    void Configure(const Settings& s, float bpm, float sample_rate)
    {
        float samples = TimeMs(s.time01, s.sync, bpm) * 0.001f * sample_rate;
        samples = samples < 1.f ? 1.f
                  : samples > (float)kMaxSamples - 1.f ? (float)kMaxSamples - 1.f
                                                        : samples;
        l_.SetDelay(samples);
        r_.SetDelay(samples);
        float fb = s.feedback01 < 0.f ? 0.f : s.feedback01 > 1.f ? 1.f : s.feedback01;
        feedback_ = fb * kMaxFeedback;
        pingpong_ = s.pingpong01 < 0.f ? 0.f : s.pingpong01 > 1.f ? 1.f : s.pingpong01;
        if(s.tone01 < 0.005f)
            tone_coef_ = 1.f; // filter fully open -- repeats untouched
        else
        {
            // 18kHz down to 800Hz across the knob, exponential.
            float fc   = 18000.f * powf(800.f / 18000.f, s.tone01 > 1.f ? 1.f : s.tone01);
            tone_coef_ = 1.f - expf(-6.2831853f * fc / sample_rate);
        }
    }

    // Read both lines before either Write(), so the crossed feedback uses
    // this sample's real output, not a half-updated one.
    inline void Process(float in_l, float in_r, float* out_l, float* out_r)
    {
        float wet_l = l_.Read();
        float wet_r = r_.Read();
        lp_l_ += tone_coef_ * (wet_l - lp_l_);
        lp_r_ += tone_coef_ * (wet_r - lp_r_);
        float mono = (in_l + in_r) * 0.7071f; // ~equal loudness when it all goes one side
        float w_l  = in_l + pingpong_ * (mono - in_l);
        float w_r  = in_r * (1.f - pingpong_);
        l_.Write(w_l + lp_r_ * feedback_);
        r_.Write(w_r + lp_l_ * feedback_);
        *out_l = wet_l;
        *out_r = wet_r;
    }

  private:
    // No default member initializers, on purpose: they'd give Bus a
    // constructor that writes to SDRAM before main() has even switched
    // SDRAM on (global constructors run first) -- a hard fault at boot.
    // Init() clears and sets everything instead.
    daisysp::DelayLine<float, kMaxSamples> l_, r_;
    float feedback_;
    float pingpong_;
    float tone_coef_;
    float lp_l_, lp_r_;
};
} // namespace SharedDelay
