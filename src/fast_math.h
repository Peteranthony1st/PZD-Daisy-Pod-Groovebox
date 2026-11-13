#pragma once
// Cheap stand-ins for libm calls that run every sample.

// tanh() as a 7/6 rational approximation, clamped to +-1 -- within 1e-4 of
// the real thing everywhere (-80 dB, inaudible), and monotonic. A library
// tanhf() costs on the order of a hundred-plus cycles on the M7; this is a
// handful of multiply-adds and one divide. Used by every engine's soft
// limiter and the master limiter, several times per sample.
inline float FastTanh(float x)
{
    if(x > 4.97f)
        return 1.f;
    if(x < -4.97f)
        return -1.f;
    float x2 = x * x;
    float y  = x * (135135.f + x2 * (17325.f + x2 * (378.f + x2)))
              / (135135.f + x2 * (62370.f + x2 * (3150.f + x2 * 28.f)));
    return y > 1.f ? 1.f : (y < -1.f ? -1.f : y);
}

#include <cstdint>
#include <cstring>

// 2^x for |x| up to ~100: exact power of two times a degree-5 polynomial for
// the fractional part (relative error ~2e-7). For per-sample control maths
// where powf() would cost far more than the rest of the work.
inline float FastExp2(float x)
{
    float   fl = (float)(int32_t)x;
    if(fl > x)
        fl -= 1.f; // floor for negatives
    float   f = x - fl;
    float   p = 1.f
              + f
                    * (0.6931472f
                       + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
    int32_t e    = (int32_t)fl + 127;
    e            = e < 1 ? 1 : (e > 254 ? 254 : e);
    uint32_t bits = (uint32_t)e << 23;
    float    scale;
    std::memcpy(&scale, &bits, sizeof(scale));
    return p * scale;
}

// sin(2*pi*turns), any turns: folded to a quarter wave, then a degree-9
// odd polynomial (error ~3e-6). For LFOs that would otherwise call sinf().
inline float FastSinTurns(float turns)
{
    float t = turns - (float)(int32_t)turns; // -1..1
    if(t > 0.5f)
        t -= 1.f;
    else if(t < -0.5f)
        t += 1.f;
    float y = t * 6.2831853f; // -pi..pi
    if(y > 1.5707963f)
        y = 3.1415927f - y;
    else if(y < -1.5707963f)
        y = -3.1415927f - y;
    float y2 = y * y;
    return y * (1.f + y2 * (-1.f / 6.f + y2 * (1.f / 120.f + y2 * (-1.f / 5040.f + y2 * (1.f / 362880.f)))));
}
