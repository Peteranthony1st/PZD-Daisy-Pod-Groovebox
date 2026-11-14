#pragma once
#include <cstdint>
#include <math.h>

// One LFO for a whole synth engine, as on the JP-8000's LFO1 (the Supersaw
// and the Ensemble use it): Triangle, Saw, Square or Sample & Hold, a free
// Rate or a note value synced to the tempo, a Fade (the time after key-on
// to reach full depth -- the engine keeps a fade per voice, see
// FadeStep()), and three bipolar depths, Pitch, Filter and Amp. With Saw,
// a depth below centre runs the ramp the other way.
//
// The engine calls Render() once per block for one value per control step
// (every `step` samples), so every voice moves together and a synced LFO
// stays on the beat.
class SynthLfo
{
  public:
    enum class Wave
    {
        Triangle,
        Saw,
        Square,
        SampleHold,
        kCount
    };
    enum Dest
    {
        kPitch,
        kFilter,
        kAmp,
        kNumDests
    };
    static constexpr int kNumSyncs = 11;

    void Init(float sample_rate, int step)
    {
        sample_rate_ = sample_rate;
        step_        = step;
        SetFade01(fade01_);
    }

    void  SetWave(Wave w) { wave_ = w; }
    Wave  GetWave() const { return wave_; }
    void  SetRate01(float v01) { rate01_ = Clamp01(v01); }
    float GetRate01() const { return rate01_; }
    float GetRateHz() const { return 0.05f * powf(30.f / 0.05f, rate01_); } // 0.05 .. 30 Hz
    void  SetSync(bool on) { sync_ = on; }
    bool  GetSync() const { return sync_; }
    void  SetSyncIndex(int i) { sync_index_ = i < 0 ? 0 : (i >= kNumSyncs ? kNumSyncs - 1 : i); }
    int   GetSyncIndex() const { return sync_index_; }
    static const char* SyncName(int i)
    {
        static const char* const kNames[kNumSyncs] = {"1/32", "1/16T", "1/16", "1/8T", "1/8",   "1/4T",
                                                      "1/4",  "1/2",   "1 bar", "2 bars", "4 bars"};
        return i >= 0 && i < kNumSyncs ? kNames[i] : "?";
    }
    // 0..5 s to full depth after key-on (a squared curve, fine at the short end).
    void SetFade01(float v01)
    {
        fade01_             = Clamp01(v01);
        const float seconds = fade01_ * fade01_ * 5.f;
        const float steps   = seconds * sample_rate_ / (float)step_;
        fade_step_          = steps < 1.f ? 1.f : 1.f / steps;
    }
    float GetFade01() const { return fade01_; }
    float GetFadeSeconds() const { return fade01_ * fade01_ * 5.f; }
    // How far a voice's fade (0..1) moves per control step.
    float FadeStep() const { return fade_step_; }
    // 0.5 = none; below reverses the LFO for that destination.
    void  SetDepth01(int dest, float v01)
    {
        if(dest >= 0 && dest < kNumDests)
            depth01_[dest] = Clamp01(v01);
    }
    float GetDepth01(int dest) const { return depth01_[dest]; }
    // -1..1, from a depth knob.
    float Depth(int dest) const { return depth01_[dest] * 2.f - 1.f; }

    // The tempo clock, once per block: the beat position (the loop's while
    // the looper runs, so a synced LFO lines up with bar 1), and whether
    // that position is the loop's.
    void SetClock(float beat_pos, float bpm, bool locked)
    {
        clock_beat_   = beat_pos;
        clock_bpm_    = bpm;
        clock_locked_ = locked;
        clock_new_    = true;
    }

    // One value (-1..1) per control step for the next n_steps steps.
    void Render(int n_steps, float* out)
    {
        const float inv_sr = 1.f / sample_rate_;
        float       step_inc;
        if(sync_)
        {
            const float cycle = kSyncBeats()[sync_index_];
            step_inc          = clock_bpm_ / 60.f * (float)step_ * inv_sr / cycle;
            if(clock_locked_ && clock_new_)
            {
                // On the loop's grid: the phase is where bar 1 puts it.
                const float p = clock_beat_ / cycle;
                phase_        = p - (float)(int32_t)p;
            }
        }
        else
            step_inc = GetRateHz() * (float)step_ * inv_sr;
        clock_new_ = false;
        for(int s = 0; s < n_steps; s++)
        {
            const float p = phase_;
            float       y;
            switch(wave_)
            {
                case Wave::Triangle: y = p < 0.5f ? 4.f * p - 1.f : 3.f - 4.f * p; break;
                case Wave::Saw: y = 2.f * p - 1.f; break;
                case Wave::Square: y = p < 0.5f ? 1.f : -1.f; break;
                default: y = sh_; break;
            }
            out[s] = y;
            phase_ += step_inc;
            if(phase_ >= 1.f)
            {
                phase_ -= (float)(int32_t)phase_;
                rng_ = rng_ * 1664525u + 1013904223u;
                sh_  = (float)(int32_t)rng_ * (1.f / 2147483648.f);
            }
        }
    }

  private:
    static float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
    // Synced note values, in beats (see SyncName()).
    static const float* kSyncBeats()
    {
        static const float kBeats[kNumSyncs]
            = {0.125f, 1.f / 6.f, 0.25f, 1.f / 3.f, 0.5f, 2.f / 3.f, 1.f, 2.f, 4.f, 8.f, 16.f};
        return kBeats;
    }

    float    sample_rate_ = 48000.f;
    int      step_        = 16;
    Wave     wave_        = Wave::Triangle;
    float    rate01_      = 0.4f;
    bool     sync_        = false;
    int      sync_index_  = 2; // 1/16
    float    fade01_      = 0.f;
    float    fade_step_   = 1.f;
    float    depth01_[kNumDests] = {0.5f, 0.5f, 0.5f};
    float    phase_       = 0.f; // 0..1
    float    sh_          = 0.f; // Sample & Hold's current value
    uint32_t rng_         = 12345;
    float    clock_beat_  = 0.f;
    float    clock_bpm_   = 120.f;
    bool     clock_locked_ = false;
    bool     clock_new_    = false;
};
