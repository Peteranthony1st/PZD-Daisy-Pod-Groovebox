/*
   Copyright 2012 Google Inc.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

class Exp2 {
  public:
    Exp2();

    static void init();
    static bool initDone;

    // Q24 in, Q24 out
    static int32_t lookup(int32_t x);

    // Same table as lookup(), but returns the PLAIN (non-Q24-scaled) value
    // of 2^(x/2^24) -- lookup()'s own final step (`y >> (6 - (x>>24))`)
    // only supports a base-2 exponent up to ~6 (bigger makes that a
    // negative shift, undefined behavior); this supports any exponent from
    // 0 up to ~29 instead, for callers whose own curve runs well past 6
    // (see Dx7Note::compute()'s AMS branch, the one caller so far).
    static uint32_t lookupRaw(int32_t x);
};

#define EXP2_LG_N_SAMPLES 10
#define EXP2_N_SAMPLES (1 << EXP2_LG_N_SAMPLES)

#define EXP2_INLINE

extern int32_t exp2tab[EXP2_N_SAMPLES << 1];

#ifdef EXP2_INLINE
inline
int32_t Exp2::lookup(int32_t x) {
  const int32_t SHIFT = 24 - EXP2_LG_N_SAMPLES;
  int32_t lowbits = x & ((1 << SHIFT) - 1);
  int32_t x_int = (x >> (SHIFT - 1)) & ((EXP2_N_SAMPLES - 1) << 1);
  int32_t dy = exp2tab[x_int];
  int32_t y0 = exp2tab[x_int + 1];

  int32_t y = y0 + (((int64_t)dy * (int64_t)lowbits) >> SHIFT);
  return y >> (6 - (x >> 24));
}

#ifdef EXP2_INLINE
inline
uint32_t Exp2::lookupRaw(int32_t x) {
  const int32_t SHIFT = 24 - EXP2_LG_N_SAMPLES;
  int32_t lowbits = x & ((1 << SHIFT) - 1);
  int32_t x_int = (x >> (SHIFT - 1)) & ((EXP2_N_SAMPLES - 1) << 1);
  int32_t dy = exp2tab[x_int];
  int32_t y0 = exp2tab[x_int + 1];

  // Same Q30 mantissa (2^30..2^31, representing 2^frac) lookup()'s own y
  // is -- scaled here to a plain integer value of 2^(ipart+frac) instead
  // of lookup()'s own Q24-scaled one, since ipart can be well past 6.
  int32_t mant  = y0 + (((int64_t)dy * (int64_t)lowbits) >> SHIFT);
  int32_t ipart = x >> 24;
  return ipart >= 30 ? (uint32_t)mant << (ipart - 30) : (uint32_t)mant >> (30 - ipart);
}
#endif
#endif

class Tanh {
  public:
    Tanh();
    static void init();
    static bool initDone;

    // Q24 in, Q24 out
    static int32_t lookup(int32_t x);
};

#define TANH_LG_N_SAMPLES 10
#define TANH_N_SAMPLES (1 << TANH_LG_N_SAMPLES)

extern int32_t tanhtab[TANH_N_SAMPLES << 1];

inline
int32_t Tanh::lookup(int32_t x) {
  int32_t signum = x >> 31;
  x ^= signum;
  if (x >= (4 << 24)) {
    if (x >= (17 << 23)) {
      return signum ^ (1 << 24);
    }
    int32_t sx = ((int64_t) - 48408812 * (int64_t)x) >> 24;
    return signum ^ ((1 << 24) - 2 * Exp2::lookup(sx));
  } else {
    const int32_t SHIFT = 26 - TANH_LG_N_SAMPLES;
    int32_t lowbits = x & ((1 << SHIFT) - 1);
    int32_t x_int = (x >> (SHIFT - 1)) & ((TANH_N_SAMPLES - 1) << 1);
    int32_t dy = tanhtab[x_int];
    int32_t y0 = tanhtab[x_int + 1];
    int32_t y = y0 + (((int64_t)dy * (int64_t)lowbits) >> SHIFT);
    return y ^ signum;
  }
}
