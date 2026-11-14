#pragma once
#include <cstdint>

// The Ensemble's, Organ's and Supersaw's chorus: daisysp::Chorus's own maths
// (a triangle-LFO-swept delay line, 0.75's 6 ms delay, 0.2 feedback, the
// same Depth and Rate ranges), made properly stereo and cheaper.
//
// daisysp::Chorus runs two engines panned 25/75, but every setter these
// engines used gives both the same values and they start alike, so they
// always computed the same signal: its "stereo" output was mono (left =
// right). Here the right side's voice sweeps the same delay the opposite
// way (the classic stereo chorus, as on the Juno and the JP-8000), each side
// at the old mono output's level. Heard side by side on all three engines,
// stereo was much better (DESIGN.md, "Supersaw").
//
// Cheaper too: the delay lines wrap with a compare instead of three `%`s
// per sample, and it's all inline, so the engines' sample loops call
// nothing. SetStereo(false) gives the old mono output, to rounding.
class FastChorus
{
  public:
    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        for(int i = 0; i < kLength; i++)
            line_[i] = 0.f;
        write_     = 0;
        feedback_  = 0.2f;
        delay_     = (0.1f + 0.75f * 7.9f) * 0.001f * sample_rate_; // daisysp's SetDelay(.75)
        lfo_amp_   = 0.f;
        lfo_phase_ = 0.f;
        lfo_freq_  = 0.f;
        for(int i = 0; i < kLength; i++)
            line_r_[i] = 0.f;
        SetLfoFreq(0.3f);
        SetLfoDepth(0.9f);
    }

    // 0..0.93 of the delay, as daisysp's.
    void SetLfoDepth(float depth)
    {
        depth    = depth < 0.f ? 0.f : (depth > 0.93f ? 0.93f : depth);
        lfo_amp_ = depth * delay_;
    }

    // Hz. Keeps the triangle's current direction, as daisysp's does.
    void SetLfoFreq(float hz)
    {
        float f = 4.f * hz / sample_rate_;
        f *= lfo_freq_ < 0.f ? -1.f : 1.f;
        lfo_freq_ = f < -0.25f ? -0.25f : (f > 0.25f ? 0.25f : f);
    }

    void SetStereo(bool on) { stereo_ = on; }
    bool GetStereo() const { return stereo_; }

    // One sample in, the chorus out on both sides: the same mono signal,
    // or with SetStereo(), the right from its own opposite-LFO voice.
    inline void Process(float in, float& l, float& r)
    {
        const uint32_t w = write_;
        l                = Process(in);
        if(!stereo_)
        {
            r = l;
            return;
        }
        // The right voice: the same delay swept the other way (-phase),
        // its own line so its feedback stays its own.
        r = Voice(in, line_r_, w, -lfo_phase_) * 0.5f; // the left's level
    }

    // One sample in, the mono chorus out.
    inline float Process(float in)
    {
        lfo_phase_ += lfo_freq_;
        if(lfo_phase_ > 1.f)
        {
            lfo_phase_ = 1.f - (lfo_phase_ - 1.f);
            lfo_freq_  = -lfo_freq_;
        }
        else if(lfo_phase_ < -1.f)
        {
            lfo_phase_ = -1.f - (lfo_phase_ + 1.f);
            lfo_freq_  = -lfo_freq_;
        }
        const float y = Voice(in, line_, write_, lfo_phase_);
        write_        = write_ == 0 ? kLength - 1 : write_ - 1;
        // daisysp: the engine's (in + out) / 2, then its two-engine pan and
        // gain_frac 0.5, which with both engines equal is another / 2.
        return y * 0.5f;
    }

  private:
    static constexpr int kLength = 2400; // 50 ms at 48 kHz, as daisysp's

    // One delay voice at LFO position phase, writing at w: (in + out) / 2.
    inline float Voice(float in, float* line, uint32_t w, float phase)
    {
        const float d    = phase * lfo_amp_ + delay_;
        int32_t     di   = (int32_t)d;
        const float frac = d - (float)di;
        if((uint32_t)di >= (uint32_t)kLength)
            di = kLength - 1;
        uint32_t r0 = w + (uint32_t)di;
        if(r0 >= (uint32_t)kLength)
            r0 -= kLength;
        uint32_t r1 = r0 + 1;
        if(r1 >= (uint32_t)kLength)
            r1 -= kLength;
        const float a   = line[r0];
        const float out = a + (line[r1] - a) * frac;
        line[w]         = in + out * feedback_;
        return (in + out) * 0.5f;
    }

    float    line_[kLength];
    float    line_r_[kLength]; // the right voice, used only in stereo
    bool     stereo_    = true;
    uint32_t write_     = 0;
    float    sample_rate_ = 48000.f;
    float    feedback_  = 0.2f;
    float    delay_     = 0.f;
    float    lfo_amp_   = 0.f;
    float    lfo_phase_ = 0.f;
    float    lfo_freq_  = 0.f;
};
