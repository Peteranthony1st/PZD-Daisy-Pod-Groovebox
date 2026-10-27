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
