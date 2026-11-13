#pragma once
#include <math.h>

// The project's one state-variable filter -- every engine's Filter page,
// the looper layers, the master filter and the import anti-alias filters.
//
// Andrew Simper's trapezoidal ("zero-delay-feedback") SVF, Cytomic 2013:
// https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf
// It replaces daisysp::Svf, Simper's earlier double-sampled design
// (musicdsp.org #92), whose resonance carried a cubic saturation term
// (`band -= drive * band^3`). With a loud input -- Dexed and Ensemble
// filter before their limiters, so a chord can reach +-4 -- or high
// resonance, that term over-corrected and crackled (heard on Ensemble's
// Dark Drone and on Dexed). This one is linear and stable for any input
// level, cutoff and resonance, so there's nothing to crackle, at about
// the same cost (one pass per sample instead of two).
//
// Same interface as daisysp::Svf as used here: SetFreq() in Hz,
// SetRes() 0..1 with the same knob-to-damping curve as before (so the
// knobs feel the same), Process(), Low()/High()/Band().
class TptSvf
{
  public:
    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        ic1eq_ = ic2eq_ = 0.f;
        low_ = band_ = high_ = 0.f;
        fc_  = 1000.f;
        k_   = 2.f;
        Update();
    }

    void SetFreq(float hz)
    {
        // Keep clear of Nyquist, where tan() blows up.
        float max_hz = sample_rate_ * 0.45f;
        fc_          = hz < 10.f ? 10.f : (hz > max_hz ? max_hz : hz);
        Update();
    }

    // 0 = no resonance (k = 2), 1 = maximum. Same res^0.25 curve as the
    // old filter's damping, but stopping at k = 0.05 (Q 20) instead of
    // 0, where a linear filter has no damping left at all.
    void SetRes(float r)
    {
        r  = r < 0.f ? 0.f : (r > 1.f ? 1.f : r);
        k_ = 2.f * (1.f - powf(r, 0.25f));
        if(k_ < kMinDamping)
            k_ = kMinDamping;
        Update();
    }

    inline void Process(float in)
    {
        float v3 = in - ic2eq_;
        float v1 = a1_ * ic1eq_ + a2_ * v3;
        float v2 = ic2eq_ + a2_ * ic1eq_ + a3_ * v3;
        ic1eq_   = 2.f * v1 - ic1eq_;
        ic2eq_   = 2.f * v2 - ic2eq_;
        low_     = v2;
        band_    = v1;
        high_    = in - k_ * v1 - v2;
    }

    inline float Low() const { return low_; }
    inline float High() const { return high_; }
    inline float Band() const { return band_; }

  private:
    static constexpr float kMinDamping = 0.05f;

    void Update()
    {
        float g = tanf(3.14159265f * fc_ / sample_rate_);
        a1_     = 1.f / (1.f + g * (g + k_));
        a2_     = g * a1_;
        a3_     = g * a2_;
    }

    float sample_rate_ = 48000.f;
    float fc_ = 1000.f, k_ = 2.f;
    float a1_ = 0.f, a2_ = 0.f, a3_ = 0.f;
    float ic1eq_ = 0.f, ic2eq_ = 0.f;
    float low_ = 0.f, band_ = 0.f, high_ = 0.f;
};
