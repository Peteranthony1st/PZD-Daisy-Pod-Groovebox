#pragma once
// A project-local version of DaisySP's Tremolo for the looper layers, with
// the same algorithm but no libm call per sample (see fast_math.h) --
// profiled on one layer, DaisySP's own cost +4% CPU (a sinf per sample).
#include <cmath>
#include "fast_math.h"

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
