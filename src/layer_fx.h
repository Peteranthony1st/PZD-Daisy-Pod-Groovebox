#pragma once
// Project-local versions of two DaisySP effects the looper layers use, with
// the same algorithms but no libm calls per sample (see fast_math.h).
// Profiled on one layer: DaisySP's Autowah cost +17% CPU (three powf and a
// cosf every sample, per channel) and its Tremolo +4% (a sinf per sample).
#include <cmath>
#include "fast_math.h"

// DaisySP's Autowah (a Faust-generated envelope-following wah), same
// recurrences and constants. Its per-sample control maths simplified:
// 2^(2.3t) / 2^(3-2t) is one 2^(4.3t-3) (no divide), powf(4,t) is 2^(2t),
// and the cosine's argument never exceeds ~0.3 rad, where a short Taylor
// series is exact to float precision.
class LayerAutowah
{
  public:
    void Init(float sample_rate)
    {
        const1_  = 1413.72f / sample_rate;
        const2_  = expf(-100.f / sample_rate);
        const4_  = expf(-10.f / sample_rate);
        wet_dry_ = 100.f;
        level_   = 0.1f;
        wah_     = 0.f;
        rec0_[0] = rec0_[1] = rec0_[2] = 0.f;
        rec1_ = rec2_ = rec3_ = rec4_ = rec5_ = 0.f;
    }
    void SetWah(float wah) { wah_ = wah; }
    void SetLevel(float level) { level_ = level; }
    void SetDryWet(float drywet) { wet_dry_ = drywet; }

    float Process(float in)
    {
        float slow2 = 0.01f * (wet_dry_ * level_);
        float slow3 = (1.f - 0.01f * wet_dry_) + (1.f - wah_);
        float a     = fabsf(in);
        float r3    = const4_ * rec3_ + (1.f - const4_) * a;
        rec3_       = a > r3 ? a : r3;
        rec2_       = const2_ * rec2_ + (1.f - const2_) * rec3_;
        float t     = rec2_ < 1.f ? rec2_ : 1.f;
        float p3    = FastExp2(2.3f * t);
        float t4    = 1.f - const1_ * FastExp2(4.3f * t - 3.f);
        float x     = const1_ * 2.f * p3;
        float x2    = x * x;
        float c     = 1.f + x2 * (-0.5f + x2 * (1.f / 24.f + x2 * (-1.f / 720.f)));
        rec1_       = 0.999f * rec1_ + 0.001f * (-2.f * t4 * c);
        rec4_       = 0.999f * rec4_ + 0.001f * t4 * t4;
        rec5_       = 0.999f * rec5_ + 0.0001f * FastExp2(2.f * t);
        float r0    = -((rec1_ * rec0_[1]) + (rec4_ * rec0_[2])) + slow2 * (rec5_ * in);
        float out   = wah_ * (r0 - rec0_[1]) + slow3 * in;
        rec0_[2]    = rec0_[1];
        rec0_[1]    = r0;
        return out;
    }

  private:
    float const1_, const2_, const4_, wah_, level_, wet_dry_;
    float rec0_[3], rec1_, rec2_, rec3_, rec4_, rec5_;
};

// DaisySP's Tremolo: a sine LFO between 1-depth and 1 (its Oscillator's
// phase handling exactly), with FastSinTurns() for the sinf().
class LayerTremolo
{
  public:
    void Init(float sample_rate)
    {
        sr_recip_ = 1.f / sample_rate;
        phase_    = 0.f;
        SetFreq(1.f);
        SetDepth(1.f);
    }
    void SetFreq(float freq) { phase_inc_ = freq * sr_recip_; }
    void SetDepth(float depth)
    {
        depth = depth < 0.f ? 0.f : (depth > 1.f ? 1.f : depth);
        amp_  = depth * 0.5f;
        dc_   = 1.f - amp_;
    }
    float Process(float in)
    {
        float mod = dc_ + FastSinTurns(phase_) * amp_;
        phase_ += phase_inc_;
        if(phase_ > 1.f)
            phase_ -= 1.f;
        return in * mod;
    }

  private:
    float sr_recip_, phase_, phase_inc_, amp_, dc_;
};
