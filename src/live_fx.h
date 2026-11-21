#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Live FX (Home's Live FX page): Pocket Operator-style performance effects
// on the whole mix, one at a time, held (B2) or latched (B1).
//
// The mix is always written into a short 16-bit stereo recording (the
// ring), so the repeats, reverse, tape stop and scratch can play back what
// just happened, and Throw can echo it -- no extra delay memory, and it
// works with the shared delay off. Every effect starts on the next 1/16 of
// the tempo; letting go crossfades back to the live mix.
class LiveFx
{
  public:
    static constexpr int kEffects = 16;
    enum Effect
    {
        kRepeat4,   // repeat the last 1/4
        kRepeat8,   // 1/8
        kRepeat16,  // 1/16
        kRepeat32,  // 1/32
        kRepeat12,  // 1/12 (triplet 1/8)
        kStutter,   // repeats that get shorter -- K2 Speed
        kStutFade,  // 1/8 repeats dying away -- K2 Fade
        kRptDrop,   // 1/8 repeats sinking in pitch -- K2 Depth
        kReverse,   // the last 1/8..1 beat backwards -- K2 Length
        kTapeStop,  // slows to a halt -- K2 Speed
        kScratch,   // back and forth around now -- K2 Rate
        kLpSweep,   // low-pass sweeping down -- K2 Depth
        kHpSweep,   // high-pass sweeping up -- K2 Depth
        kCrush,     // bits and sample rate -- K2 Amount
        kGate,      // chopped in time -- K2 Rate
        kThrow,     // echoes of what you held -- K2 Feedback
    };
    static const char* ShortName(int e)
    {
        static const char* const k[kEffects] = {"1/4", "1/8",  "1/16", "1/32", "1/12", "Stut",
                                                "Fade", "Drop", "Rev",  "Stop", "Scrch", "LPF",
                                                "HPF", "Crush", "Gate", "Throw"};
        return e >= 0 && e < kEffects ? k[e] : "";
    }
    static const char* FullName(int e)
    {
        static const char* const k[kEffects]
            = {"Repeat 1/4", "Repeat 1/8", "Repeat 1/16", "Repeat 1/32", "Repeat 1/12", "Stutter",
               "Stutter Fade", "Repeat Drop", "Reverse", "Tape Stop", "Scratch", "LP Sweep",
               "HP Sweep", "Crush", "Gate", "Throw"};
        return e >= 0 && e < kEffects ? k[e] : "";
    }
    // K2's name for each effect, nullptr where it has none.
    static const char* ParamName(int e)
    {
        static const char* const k[kEffects]
            = {nullptr, nullptr, nullptr, nullptr, nullptr, "Speed", "Fade", "Depth",
               "Length", "Speed", "Rate", "Depth", "Depth", "Amount", "Rate", "Feedback"};
        return e >= 0 && e < kEffects ? k[e] : nullptr;
    }
    // K2 as shown: a note value for the stepped ones, else a percentage.
    static void ParamText(int e, float v01, char* out, size_t n)
    {
        static const char* const kRevNames[4]  = {"1/8", "1/4", "1/2", "1/1"};
        static const char* const kGateNames[3] = {"1/8", "1/16", "1/32"};
        if(!ParamName(e))
            snprintf(out, n, "--");
        else if(e == kReverse)
            snprintf(out, n, "%s", kRevNames[Step(v01, 4)]);
        else if(e == kGate)
            snprintf(out, n, "%s", kGateNames[Step(v01, 3)]);
        else
            snprintf(out, n, "%d%%", (int)(v01 * 100.f + 0.5f));
    }
    static int Step(float v01, int n)
    {
        int i = (int)(v01 * (float)n);
        return i < 0 ? 0 : (i >= n ? n - 1 : i);
    }

    // Saved with the Performance (its "LFX1" section).
    struct Settings
    {
        int32_t effect = kRepeat8;
        float   param[kEffects];
        Settings()
        {
            for(float& p : param)
                p = 0.5f;
        }
    };

    // ring_frames must be a power of two. The ring is cleared here (it
    // lives in SDRAM, which isn't zeroed at boot).
    void Init(int16_t* ring_l, int16_t* ring_r, uint32_t ring_frames)
    {
        ring_l_ = ring_l;
        ring_r_ = ring_r;
        mask_   = ring_frames - 1;
        memset(ring_l_, 0, ring_frames * sizeof(int16_t));
        memset(ring_r_, 0, ring_frames * sizeof(int16_t));
    }

    // --- The UI side (main loop) --------------------------------------------
    Settings& Edit() { return settings_; }
    const Settings& Get() const { return settings_; }
    // Held or latched. A change of effect while on starts it again.
    void SetEngaged(bool on) { engaged_ = on; }
    bool IsPlaying() const { return state_ == kActive || state_ == kWaiting; }

    // --- The audio side ------------------------------------------------------
    // Once a block: the tempo, and the beat position at the block's start
    // (beats, from the loop's start) when the clock runs.
    void BeginBlock(size_t n, float sr, float bpm, bool clock_running, float beat_pos)
    {
        sr_  = sr;
        spb_ = sr * 60.f / (bpm > 1.f ? bpm : 1.f);
        if(!clock_running)
        {
            free_beat_ += (float)n / spb_;
            if(free_beat_ > 4096.f)
                free_beat_ -= 4096.f;
            beat_pos = free_beat_;
        }
        // Samples from the block's start to the next 1/16.
        const float pos16 = beat_pos * 4.f;
        float       frac  = pos16 - floorf(pos16);
        to_16th_          = frac < 1e-4f ? 0.f : (1.f - frac) * spb_ * 0.25f;

        // Held/latched or not, and which effect. A different effect while
        // one plays fades the old one out first; the new one then starts on
        // the next 1/16, like any start.
        const int want = engaged_ ? settings_.effect : -1;
        if(want < 0)
        {
            if(state_ == kWaiting)
                state_ = kIdle;
            else if(state_ == kActive)
                Release();
        }
        else if(state_ == kActive && want != effect_)
            Release();
        else if(state_ == kIdle)
        {
            effect_    = want;
            state_     = kWaiting;
            wait_left_ = (int32_t)(to_16th_ + 0.5f);
        }
        else if(state_ == kWaiting)
            effect_ = want;
        // The effect's setting, and the sweep's filter, once a block.
        param_ = settings_.param[effect_ >= 0 ? effect_ : 0];
        if(effect_ == kLpSweep || effect_ == kHpSweep)
            UpdateFilter(n);
    }

    // One sample of the mix, in place.
    void Process(float& l, float& r)
    {
        // t counts time; w is the recording's newest frame. The recording
        // pauses while a repeat plays, so what it loops is never written
        // over, however long it's held.
        const uint32_t t = frame_++;
        if(!Frozen())
        {
            ring_l_[rec_ & mask_] = ToPcm(l);
            ring_r_[rec_ & mask_] = ToPcm(r);
            rec_++;
        }
        const uint32_t w = rec_ - 1;

        if(state_ == kWaiting && wait_left_-- <= 0)
            Start(t, w);

        // The dry mix, the effect's output, and how far to blend (the 3 ms
        // fade in, or the 5 ms fade back on release).
        float wl = l, wr = r;
        if(state_ == kActive || state_ == kReleasing)
        {
            Render(t, w, l, r, wl, wr);
            if(state_ == kActive)
            {
                mix_ += mix_in_;
                if(mix_ > 1.f)
                    mix_ = 1.f;
            }
            else if(!drop_return_)
            {
                mix_ -= mix_out_;
                if(mix_ <= 0.f)
                {
                    mix_   = 0.f;
                    state_ = kIdle;
                }
            }
            const float m = mix_;
            const float dl = l, dr = r;
            l = dl + (wl - dl) * m;
            r = dr + (wr - dr) * m;
        }
        // Throw's echoes keep going after it's let go, until they're past.
        if(throw_on_)
            AddThrow(t, w, l, r);
    }

  private:
    enum State
    {
        kIdle,
        kWaiting, // engaged; starts at the next 1/16
        kActive,
        kReleasing,
    };

    // The effects that loop a slice of the past: the recording holds still
    // while they play (and fade out), unless Throw's echoes still need it.
    bool Frozen() const
    {
        return (state_ == kActive || state_ == kReleasing) && !throw_on_
               && effect_ >= kRepeat4 && effect_ <= kReverse;
    }

    static int16_t ToPcm(float x)
    {
        x = x > 1.f ? 1.f : (x < -1.f ? -1.f : x);
        return (int16_t)(x * 32767.f);
    }
    float L(int64_t f) const { return (float)ring_l_[(uint32_t)f & mask_] * (1.f / 32768.f); }
    float R(int64_t f) const { return (float)ring_r_[(uint32_t)f & mask_] * (1.f / 32768.f); }
    // A fractional read, linear between two frames.
    void ReadAt(double pos, float& l, float& r) const
    {
        const int64_t i = (int64_t)floor(pos);
        const float   f = (float)(pos - (double)i);
        l = L(i) + (L(i + 1) - L(i)) * f;
        r = R(i) + (R(i + 1) - R(i)) * f;
    }
    // The ring's oldest usable frame from now -- reads never go further back.
    float MaxLag() const { return (float)(mask_ - 256); }
    float Clamp(float samples) const
    {
        const float m = MaxLag();
        return samples < 1.f ? 1.f : (samples > m ? m : samples);
    }

    void Start(uint32_t t, uint32_t w)
    {
        state_   = kActive;
        t0_      = t;
        w0_      = w;
        mix_     = 0.f;
        mix_in_  = 1.f / (0.003f * sr_);
        mix_out_ = 1.f / (0.005f * sr_);
        // Each effect's starting state.
        const float beat = spb_;
        switch(effect_)
        {
            case kRepeat4: slice_ = Clamp(beat); break;
            case kRepeat8: slice_ = Clamp(beat * 0.5f); break;
            case kRepeat16: slice_ = Clamp(beat * 0.25f); break;
            case kRepeat32: slice_ = Clamp(beat * 0.125f); break;
            case kRepeat12: slice_ = Clamp(beat / 3.f); break;
            case kStutter:
            case kStutFade:
            case kRptDrop: slice_ = Clamp(beat * 0.5f); break;
            case kReverse:
            {
                static const float kBeats[4] = {0.5f, 1.f, 2.f, 4.f}; // 1/8 .. 1/1
                slice_ = Clamp(beat * kBeats[Step(param_, 4)]);
                break;
            }
            default: slice_ = Clamp(beat * 0.5f); break;
        }
        pos_     = 0.f;
        gain_    = 1.f;
        drop_return_ = false;
        rate_    = 1.f;
        read_    = (double)w;
        hold_    = 0;
        crush_l_ = crush_r_ = 0.f;
        f_ic1_[0] = f_ic1_[1] = f_ic2_[0] = f_ic2_[1] = 0.f;
        cutoff_  = effect_ == kHpSweep ? 20.f : 18000.f;
        if(effect_ == kLpSweep || effect_ == kHpSweep)
            UpdateFilter(0);
        if(effect_ == kThrow)
        {
            throw_on_    = true;
            throw_start_ = w;
            throw_end_   = UINT32_MAX;
            throw_d_     = Clamp(beat * 0.75f); // a dotted 1/8
            throw_fb_    = 0.3f + param_ * 0.6f;
        }
    }

    void Release()
    {
        // Drop winds back up the way it slowed down, over as long as it
        // took, before it fades back to the mix.
        if(effect_ == kRptDrop && state_ == kActive)
        {
            const float bars = (float)(frame_ - t0_) / (spb_ * 4.f);
            drop_return_     = true;
            drop_down_       = bars < 1.f ? bars : 1.f;
            drop_up_t_       = frame_;
        }
        state_ = kReleasing;
        if(effect_ == kThrow)
        {
            throw_end_   = rec_ - 1;
            throw_end_t_ = frame_;
        }
    }

    // One repeat's edges, faded over 1 ms so the joins don't click.
    float Edge(float pos, float len) const
    {
        const float f = 0.001f * sr_;
        float       e = pos < f ? pos / f : 1.f;
        const float b = len - pos;
        if(b < f)
            e = e < b / f ? e : b / f;
        return e < 0.f ? 0.f : e;
    }

    // The effect's output for frame t, from the dry sample (dl, dr).
    // t is the time, w the recording's newest frame (see Process()).
    void Render(uint32_t t, uint32_t w, float dl, float dr, float& wl, float& wr)
    {
        switch(effect_)
        {
            case kRepeat4:
            case kRepeat8:
            case kRepeat16:
            case kRepeat32:
            case kRepeat12:
            case kStutter:
            case kStutFade:
            {
                // The slice that ended where it started, played again and
                // again (Stutter: each time shorter; Fade: each quieter).
                const float e = Edge(pos_, slice_) * gain_;
                const int64_t src = (int64_t)w0_ - (int64_t)slice_ + (int64_t)pos_;
                wl = L(src) * e;
                wr = R(src) * e;
                pos_ += 1.f;
                if(pos_ >= slice_)
                {
                    pos_ = 0.f;
                    if(effect_ == kStutter)
                    {
                        const float shrink = 0.92f - 0.37f * param_;
                        const float min    = spb_ / 32.f;
                        slice_ = slice_ * shrink > min ? slice_ * shrink : min;
                    }
                    else if(effect_ == kStutFade)
                        gain_ *= 0.92f - 0.5f * param_;
                }
                break;
            }
            case kRptDrop:
            {
                // A 1/8 repeat whose playback slows: down to 2 octaves over a
                // bar at full Depth.
                const float e = Edge(pos_, slice_);
                ReadAt((double)w0_ - slice_ + pos_, wl, wr);
                wl *= e;
                wr *= e;
                float bars = (float)(t - t0_) / (spb_ * 4.f);
                if(drop_return_)
                {
                    // Let go: the same curve, back up.
                    bars = drop_down_ - (float)(t - drop_up_t_) / (spb_ * 4.f);
                    if(bars <= 0.f)
                    {
                        bars         = 0.f;
                        drop_return_ = false;
                    }
                }
                rate_ = exp2f(-2.f * param_ * (bars < 1.f ? bars : 1.f));
                pos_ += rate_;
                if(pos_ >= slice_)
                    pos_ -= slice_;
                break;
            }
            case kReverse:
            {
                const float e = Edge(pos_, slice_);
                const int64_t src = (int64_t)w0_ - 1 - (int64_t)pos_;
                wl = L(src) * e;
                wr = R(src) * e;
                pos_ += 1.f;
                if(pos_ >= slice_)
                    pos_ = 0.f;
                break;
            }
            case kTapeStop:
            {
                // From full speed to a stop over 2 s (Speed 0) .. 0.1 s.
                const float secs = 2.f - 1.9f * param_;
                rate_ -= 1.f / (secs * sr_);
                if(rate_ <= 0.f)
                {
                    rate_ = 0.f;
                    wl = wr = 0.f;
                    break;
                }
                read_ += rate_;
                if((double)w - read_ > MaxLag())
                    read_ = (double)w - MaxLag();
                ReadAt(read_, wl, wr);
                const float g = sqrtf(rate_); // eases out the very slow end
                wl *= g;
                wr *= g;
                break;
            }
            case kScratch:
            {
                // Back and forth over a 1/16 behind now, 1..8 times a second.
                const float hz  = 1.f + 7.f * param_;
                const float ph  = (float)(t - t0_) / sr_ * hz;
                const float amp = Clamp(spb_ * 0.25f);
                const float lag = 1.f + amp * 0.5f * (1.f - cosf(6.2831853f * ph));
                ReadAt((double)w - lag, wl, wr);
                break;
            }
            case kLpSweep:
            case kHpSweep:
            {
                const float k = kSweepK;
                const float a1 = f_a1_, a2 = f_a2_, a3 = f_a3_;
                float       out[2], in[2] = {dl, dr};
                for(int c = 0; c < 2; c++)
                {
                    const float v3 = in[c] - f_ic2_[c];
                    const float v1 = a1 * f_ic1_[c] + a2 * v3;
                    const float v2 = f_ic2_[c] + a2 * f_ic1_[c] + a3 * v3;
                    f_ic1_[c]      = 2.f * v1 - f_ic1_[c];
                    f_ic2_[c]      = 2.f * v2 - f_ic2_[c];
                    out[c] = effect_ == kLpSweep ? v2 : in[c] - k * v1 - v2;
                }
                wl = out[0];
                wr = out[1];
                break;
            }
            case kCrush:
            {
                // Fewer bits (16 down to 3) and a lower rate (to 1/16).
                const int hold = 1 + (int)(param_ * 15.f);
                if(hold_ <= 0)
                {
                    const float steps = exp2f(15.f - param_ * 13.f);
                    crush_l_ = floorf(dl * steps + 0.5f) / steps;
                    crush_r_ = floorf(dr * steps + 0.5f) / steps;
                    hold_    = hold;
                }
                hold_--;
                wl = crush_l_;
                wr = crush_r_;
                break;
            }
            case kGate:
            {
                // On for the first half of each 1/8, 1/16 or 1/32, from where
                // it started (a 1/16), with 2 ms ramps.
                static const float kDiv[3] = {0.5f, 0.25f, 0.125f};
                const float period = spb_ * kDiv[Step(param_, 3)];
                const float ph     = fmodf((float)(t - t0_), period);
                const float ramp   = 0.002f * sr_;
                const float half   = period * 0.5f;
                float       g;
                if(ph < ramp)
                    g = ph / ramp;
                else if(ph < half)
                    g = 1.f;
                else if(ph < half + ramp)
                    g = 1.f - (ph - half) / ramp;
                else
                    g = 0.f;
                wl = dl * g;
                wr = dr * g;
                break;
            }
            case kThrow:
            default:
                wl = dl;
                wr = dr;
                break;
        }
    }

    // Throw: up to 8 echoes of what played while it was held, a dotted 1/8
    // apart, each Feedback quieter. What it echoes fades in and out over
    // 2 ms at the hold's start and end, so no echo clicks on or off. Ends
    // once the last one is past.
    void AddThrow(uint32_t t, uint32_t w, float& l, float& r)
    {
        const float fade = 0.002f * sr_;
        float       g    = 1.f;
        for(int k = 1; k <= 8; k++)
        {
            g *= throw_fb_;
            const float lag = throw_d_ * (float)k;
            if(lag > MaxLag())
                break;
            const int64_t s = (int64_t)w - (int64_t)lag;
            if(s < (int64_t)throw_start_)
                break; // the rest are further back still
            float w = (float)(s - (int64_t)throw_start_) / fade;
            if(throw_end_ != UINT32_MAX)
            {
                const float e = (float)((int64_t)throw_end_ - s) / fade;
                w             = w < e ? w : e;
            }
            if(w <= 0.f)
                continue;
            w = w > 1.f ? g : g * w;
            l += L(s) * w;
            r += R(s) * w;
        }
        if(throw_end_ != UINT32_MAX && (float)(t - throw_end_t_) > throw_d_ * 8.f + 1.f)
            throw_on_ = false;
    }

    // The sweep: its cutoff glides (over about a 1/16) towards the target --
    // LP from 18 kHz down by up to 7 octaves (about 140 Hz), HP from 20 Hz
    // up by as much -- and the filter's coefficients follow, once a block
    // (tanf() is too dear per sample).
    static constexpr float kSweepK = 1.2f; // a little resonance
    void UpdateFilter(size_t n)
    {
        const float oct    = 1.f + 6.f * param_;
        const float target = effect_ == kLpSweep ? 18000.f * exp2f(-oct) : 20.f * exp2f(oct);
        const float glide  = (float)n / (spb_ * 0.25f);
        cutoff_ += (target - cutoff_) * (glide < 1.f ? glide : 1.f);
        const float fc = cutoff_ < sr_ * 0.45f ? cutoff_ : sr_ * 0.45f;
        const float g  = tanf(3.14159265f * fc / sr_);
        f_a1_          = 1.f / (1.f + g * (g + kSweepK));
        f_a2_          = g * f_a1_;
        f_a3_          = g * f_a2_;
    }

    int16_t* ring_l_ = nullptr;
    int16_t* ring_r_ = nullptr;
    uint32_t mask_   = 0;
    uint32_t frame_  = 0;

    Settings      settings_;
    volatile bool engaged_ = false;

    State    state_     = kIdle;
    int      effect_    = -1;
    int32_t  wait_left_ = 0;
    uint32_t t0_        = 0;
    uint32_t w0_        = 0; // the recording's newest frame at the start
    uint32_t rec_       = 0; // the next frame the recording writes
    float    mix_ = 0.f, mix_in_ = 0.f, mix_out_ = 0.f;

    float sr_ = 48000.f, spb_ = 24000.f, to_16th_ = 0.f, free_beat_ = 0.f, param_ = 0.5f;

    float  slice_ = 1.f, pos_ = 0.f, gain_ = 1.f, rate_ = 1.f;
    double read_  = 0.0;
    int    hold_  = 0;
    float  crush_l_ = 0.f, crush_r_ = 0.f;
    float  f_ic1_[2] = {}, f_ic2_[2] = {};
    float  cutoff_ = 18000.f, f_a1_ = 1.f, f_a2_ = 0.f, f_a3_ = 0.f;

    bool     throw_on_    = false;
    uint32_t throw_start_ = 0, throw_end_ = UINT32_MAX;
    uint32_t throw_end_t_ = 0;
    // Drop winding back up after it's let go (see Release()).
    bool     drop_return_ = false;
    float    drop_down_   = 0.f; // bars it spent slowing down (at most 1)
    uint32_t drop_up_t_   = 0;   // when it was let go // when it was let go (time, not the recording)
    float    throw_d_ = 1.f, throw_fb_ = 0.5f;
};
