#pragma once
#include <math.h>

// The one pan law every channel uses -- the 4 loop layers, every engine
// and Bypass. Equal-power (sine/cosine): each side at 0.707 (-3 dB) in
// the centre, full level hard left/right, so a sound keeps the same
// loudness as it moves across. It replaced a linear law (left = 1 - pan,
// right = pan), which put every centred channel at -6 dB -- and twice
// over for a synth recorded into a layer (its own pan, then the
// layer's), which is a big part of why recordings played back quieter
// than they sounded live.
inline void EqualPowerPan(float pan01, float* gain_l, float* gain_r)
{
    pan01   = pan01 < 0.f ? 0.f : (pan01 > 1.f ? 1.f : pan01);
    float a = pan01 * 1.5707963f; // 0..pi/2
    *gain_l = cosf(a);
    *gain_r = sinf(a);
}
